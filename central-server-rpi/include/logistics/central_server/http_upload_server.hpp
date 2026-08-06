#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "logistics/central_server/database.hpp"

namespace logistics::central_server {

struct HttpUploadServerConfig {
    bool enabled{ false };
    int port{ 8080 };
    bool tls_enabled{ false };
    std::filesystem::path tls_certificate;
    std::filesystem::path tls_private_key;
    std::string bearer_token;
    std::filesystem::path upload_root{ "/var/lib/logistics/uploads" };
};

class HttpUploadServer final {
public:
    HttpUploadServer(Database& database, HttpUploadServerConfig config);
    ~HttpUploadServer();
    HttpUploadServer(const HttpUploadServer&) = delete;
    HttpUploadServer& operator=(const HttpUploadServer&) = delete;

    [[nodiscard]] DatabaseStatus Start();
    void Stop();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace logistics::central_server
