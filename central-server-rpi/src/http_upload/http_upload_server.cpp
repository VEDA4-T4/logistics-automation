#include "logistics/central_server/http_upload_server.hpp"

#include <microhttpd.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "logistics/central_server/upload_service.hpp"
#include "logistics/contracts/http_upload.hpp"
#include "logistics/contracts/identifier.hpp"

namespace logistics::central_server {
namespace {

namespace contract = contracts::http;

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return input ? contents.str() : std::string{};
}

std::string JsonEscape(std::string_view value) {
    std::string output;
    output.reserve(value.size());
    for (const char character : value) {
        switch (character) {
            case '\\':
                output += "\\\\";
                break;
            case '"':
                output += "\\\"";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(character) >= 0x20U) {
                    output.push_back(character);
                }
                break;
        }
    }
    return output;
}

unsigned int HttpStatus(UploadStatus status) {
    switch (status) {
        case UploadStatus::kCreated:
            return MHD_HTTP_CREATED;
        case UploadStatus::kDuplicate:
            return MHD_HTTP_OK;
        case UploadStatus::kInvalidRequest:
            return MHD_HTTP_BAD_REQUEST;
        case UploadStatus::kNotFound:
            return MHD_HTTP_NOT_FOUND;
        case UploadStatus::kConflict:
            return MHD_HTTP_CONFLICT;
        case UploadStatus::kTooLarge:
            return 413U;
        case UploadStatus::kUnsupportedMediaType:
            return MHD_HTTP_UNSUPPORTED_MEDIA_TYPE;
        case UploadStatus::kChecksumMismatch:
            return 422U;
        case UploadStatus::kStorageError:
            return MHD_HTTP_INTERNAL_SERVER_ERROR;
    }
    return MHD_HTTP_INTERNAL_SERVER_ERROR;
}

std::string ResultJson(const UploadResult& result) {
    if (!result.ok()) {
        return "{\"error\":\"UPLOAD_FAILED\",\"message\":\"" + JsonEscape(result.message) + "\"}";
    }
    return "{\"uploadId\":\"" + JsonEscape(result.upload_id) + "\",\"path\":\"" + JsonEscape(result.path) +
           "\",\"checksum\":\"" + JsonEscape(result.checksum) +
           "\",\"duplicate\":" + (result.status == UploadStatus::kDuplicate ? "true" : "false") + "}";
}

MHD_Result QueueJson(MHD_Connection* connection, unsigned int status, const std::string& body) {
    MHD_Response* response =
        MHD_create_response_from_buffer(body.size(), const_cast<char*>(body.data()), MHD_RESPMEM_MUST_COPY);
    if (response == nullptr) {
        return MHD_NO;
    }
    static_cast<void>(MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, "application/json"));
    const MHD_Result result = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return result;
}

MHD_Result QueueImage(MHD_Connection* connection, const std::filesystem::path& path, std::string_view content_type) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return QueueJson(connection, MHD_HTTP_NOT_FOUND, "{\"error\":\"NOT_FOUND\",\"message\":\"image not found\"}");
    }
    std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input.eof()) {
        return QueueJson(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
                         "{\"error\":\"READ_FAILED\",\"message\":\"cannot read image\"}");
    }
    MHD_Response* response =
        MHD_create_response_from_buffer(body.size(), const_cast<char*>(body.data()), MHD_RESPMEM_MUST_COPY);
    if (response == nullptr) {
        return MHD_NO;
    }
    static_cast<void>(MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, content_type.data()));
    static_cast<void>(
        MHD_add_response_header(response, MHD_HTTP_HEADER_CACHE_CONTROL, "public, max-age=31536000, immutable"));
    const MHD_Result result = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return result;
}

MHD_Result ServeUploadedImage(MHD_Connection* connection, const std::filesystem::path& upload_root,
                              std::string_view url) {
    constexpr std::string_view prefix = "/uploads/images/";
    if (!url.starts_with(prefix)) {
        return QueueJson(connection, MHD_HTTP_NOT_FOUND, "{\"error\":\"NOT_FOUND\",\"message\":\"unknown endpoint\"}");
    }
    const std::string_view filename = url.substr(prefix.size());
    const auto extension_offset = filename.rfind('.');
    if (extension_offset == std::string_view::npos || !contracts::IsValidUuid(filename.substr(0, extension_offset))) {
        return QueueJson(connection, MHD_HTTP_NOT_FOUND, "{\"error\":\"NOT_FOUND\",\"message\":\"image not found\"}");
    }
    const std::string_view extension = filename.substr(extension_offset);
    if (extension == ".jpg") {
        return QueueImage(connection, upload_root / "images" / std::string(filename), "image/jpeg");
    }
    if (extension == ".png") {
        return QueueImage(connection, upload_root / "images" / std::string(filename), "image/png");
    }
    return QueueJson(connection, MHD_HTTP_NOT_FOUND, "{\"error\":\"NOT_FOUND\",\"message\":\"image not found\"}");
}

bool ParseSize(std::string_view text, std::size_t& output) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return false;
    }
    output = static_cast<std::size_t>(value);
    return static_cast<std::uint64_t>(output) == value;
}

struct ConnectionContext {
    explicit ConnectionContext(contract::UploadKind upload_kind) {
        request.kind = upload_kind;
    }

    UploadRequest request;
    MHD_PostProcessor* processor{ nullptr };
    std::string declared_byte_size;
    bool file_received{ false };
    bool too_large{ false };
    bool responded{ false };
};

std::string* TextField(ConnectionContext& context, std::string_view key) {
    if (key == contract::kDeviceIdField)
        return &context.request.device_id;
    if (key == contract::kWorkIdField)
        return &context.request.work_id;
    if (key == contract::kMessageIdField)
        return &context.request.message_id;
    if (key == contract::kCapturedAtField)
        return &context.request.captured_at;
    if (key == contract::kStartedAtField)
        return &context.request.started_at;
    if (key == contract::kEndedAtField)
        return &context.request.ended_at;
    if (key == contract::kChecksumField)
        return &context.request.sha256;
    if (key == contract::kByteSizeField)
        return &context.declared_byte_size;
    return nullptr;
}

MHD_Result ProcessPart(void* context_pointer, MHD_ValueKind, const char* key, const char*, const char* content_type,
                       const char*, const char* data, std::uint64_t offset, std::size_t size) {
    auto& context = *static_cast<ConnectionContext*>(context_pointer);
    if (key == nullptr) {
        return MHD_YES;
    }
    if (std::string_view(key) == contract::kFileField) {
        context.file_received = true;
        if (content_type != nullptr && context.request.mime_type.empty()) {
            context.request.mime_type = content_type;
        }
        const auto maximum = contract::MaximumBytes(context.request.kind);
        if (offset > maximum || size > maximum - static_cast<std::size_t>(offset)) {
            context.too_large = true;
            return MHD_YES;
        }
        const auto required_size = static_cast<std::size_t>(offset) + size;
        if (context.request.bytes.size() < required_size) {
            context.request.bytes.resize(required_size);
        }
        std::copy_n(reinterpret_cast<const std::uint8_t*>(data), size,
                    context.request.bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        return MHD_YES;
    }
    std::string* field = TextField(context, key);
    if (field == nullptr || offset > 4096 || size > 4096 - static_cast<std::size_t>(offset)) {
        return MHD_YES;
    }
    const auto required_size = static_cast<std::size_t>(offset) + size;
    if (field->size() < required_size) {
        field->resize(required_size);
    }
    std::copy_n(data, size, field->begin() + static_cast<std::ptrdiff_t>(offset));
    return MHD_YES;
}

}  // namespace

class HttpUploadServer::Impl final {
public:
    Impl(Database& database, HttpUploadServerConfig config)
        : config_(std::move(config)), service_(database, config_.upload_root) {}

    DatabaseStatus Start() {
        if (!config_.enabled) {
            return DatabaseStatus::Ok();
        }
        if (config_.port <= 0 || config_.port > 65535) {
            return { DatabaseStatusCode::kInvalidArgument, "HTTP upload port is invalid" };
        }
        if (config_.bearer_token.empty()) {
            return { DatabaseStatusCode::kInvalidArgument, "HTTP upload bearer token must not be empty" };
        }
        unsigned int flags = MHD_USE_INTERNAL_POLLING_THREAD;
        if (config_.tls_enabled) {
            certificate_ = ReadTextFile(config_.tls_certificate);
            private_key_ = ReadTextFile(config_.tls_private_key);
            if (certificate_.empty() || private_key_.empty()) {
                return { DatabaseStatusCode::kInvalidArgument, "cannot read HTTP TLS certificate or private key" };
            }
            flags |= MHD_USE_TLS;
            daemon_ = MHD_start_daemon(flags, static_cast<std::uint16_t>(config_.port), nullptr, nullptr,
                                       &HandleRequest, this, MHD_OPTION_HTTPS_MEM_KEY, private_key_.c_str(),
                                       MHD_OPTION_HTTPS_MEM_CERT, certificate_.c_str(), MHD_OPTION_NOTIFY_COMPLETED,
                                       &RequestCompleted, this, MHD_OPTION_END);
        } else {
            daemon_ =
                MHD_start_daemon(flags, static_cast<std::uint16_t>(config_.port), nullptr, nullptr, &HandleRequest,
                                 this, MHD_OPTION_NOTIFY_COMPLETED, &RequestCompleted, this, MHD_OPTION_END);
        }
        if (daemon_ == nullptr) {
            return { DatabaseStatusCode::kIoError, "cannot start HTTP upload server" };
        }
        return DatabaseStatus::Ok();
    }

    void Stop() {
        if (daemon_ != nullptr) {
            MHD_stop_daemon(daemon_);
            daemon_ = nullptr;
        }
    }

private:
    static MHD_Result HandleRequest(void* server_pointer, MHD_Connection* connection, const char* url,
                                    const char* method, const char*, const char* upload_data,
                                    std::size_t* upload_data_size, void** context_pointer) {
        auto& server = *static_cast<Impl*>(server_pointer);
        if (std::string_view(method) == MHD_HTTP_METHOD_GET) {
            return ServeUploadedImage(connection, server.config_.upload_root, url);
        }
        if (std::string_view(method) != MHD_HTTP_METHOD_POST) {
            return QueueJson(connection, MHD_HTTP_METHOD_NOT_ALLOWED,
                             "{\"error\":\"METHOD_NOT_ALLOWED\",\"message\":\"GET or POST is required\"}");
        }

        contract::UploadKind kind;
        if (std::string_view(url) == contract::kImageUploadEndpoint) {
            kind = contract::UploadKind::kImage;
        } else if (std::string_view(url) == contract::kLogUploadEndpoint) {
            kind = contract::UploadKind::kLog;
        } else {
            return QueueJson(connection, MHD_HTTP_NOT_FOUND,
                             "{\"error\":\"NOT_FOUND\",\"message\":\"unknown upload endpoint\"}");
        }

        if (*context_pointer == nullptr) {
            auto* context = new ConnectionContext(kind);
            context->processor = MHD_create_post_processor(connection, 64U * 1024U, &ProcessPart, context);
            if (context->processor == nullptr) {
                delete context;
                return MHD_NO;
            }
            *context_pointer = context;
            return MHD_YES;
        }

        auto& context = *static_cast<ConnectionContext*>(*context_pointer);
        if (*upload_data_size != 0U) {
            static_cast<void>(MHD_post_process(context.processor, upload_data, *upload_data_size));
            *upload_data_size = 0;
            return MHD_YES;
        }
        if (context.responded) {
            return MHD_YES;
        }
        context.responded = true;

        const char* authorization =
            MHD_lookup_connection_value(connection, MHD_HEADER_KIND, contract::kAuthorizationHeader.data());
        if (!server.config_.bearer_token.empty()) {
            const std::string expected = std::string(contract::kBearerPrefix) + server.config_.bearer_token;
            if (authorization == nullptr || std::string_view(authorization) != expected) {
                return QueueJson(connection, MHD_HTTP_UNAUTHORIZED,
                                 "{\"error\":\"UNAUTHORIZED\",\"message\":\"invalid device token\"}");
            }
        }

        const char* idempotency =
            MHD_lookup_connection_value(connection, MHD_HEADER_KIND, contract::kIdempotencyHeader.data());
        if (idempotency == nullptr || context.request.message_id != idempotency) {
            return QueueJson(connection, MHD_HTTP_BAD_REQUEST,
                             "{\"error\":\"INVALID_IDEMPOTENCY_KEY\",\"message\":\"header must equal messageId\"}");
        }
        if (!context.file_received) {
            return QueueJson(connection, MHD_HTTP_BAD_REQUEST,
                             "{\"error\":\"FILE_REQUIRED\",\"message\":\"multipart file is required\"}");
        }
        if (context.too_large) {
            return QueueJson(connection, 413U,
                             "{\"error\":\"FILE_TOO_LARGE\",\"message\":\"upload exceeds size limit\"}");
        }
        std::size_t declared_size = 0;
        if (!ParseSize(context.declared_byte_size, declared_size) || declared_size != context.request.bytes.size()) {
            return QueueJson(connection, 422U,
                             "{\"error\":\"SIZE_MISMATCH\",\"message\":\"byteSize does not match file\"}");
        }

        const UploadResult result = server.service_.Store(context.request);
        return QueueJson(connection, HttpStatus(result.status), ResultJson(result));
    }

    static void RequestCompleted(void*, MHD_Connection*, void** context_pointer, MHD_RequestTerminationCode) {
        auto* context = static_cast<ConnectionContext*>(*context_pointer);
        if (context != nullptr) {
            if (context->processor != nullptr) {
                MHD_destroy_post_processor(context->processor);
            }
            delete context;
            *context_pointer = nullptr;
        }
    }

    HttpUploadServerConfig config_;
    UploadService service_;
    MHD_Daemon* daemon_{ nullptr };
    std::string certificate_;
    std::string private_key_;
};

HttpUploadServer::HttpUploadServer(Database& database, HttpUploadServerConfig config)
    : impl_(std::make_unique<Impl>(database, std::move(config))) {}

HttpUploadServer::~HttpUploadServer() {
    impl_->Stop();
}

DatabaseStatus HttpUploadServer::Start() {
    return impl_->Start();
}

void HttpUploadServer::Stop() {
    impl_->Stop();
}

}  // namespace logistics::central_server
