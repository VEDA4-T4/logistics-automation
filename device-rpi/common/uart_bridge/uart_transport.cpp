#include "logistics/device/uart_transport.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <limits>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace logistics::device {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] bool IsConnectionError(int error_code) noexcept {
    return error_code == ENODEV || error_code == EPIPE || error_code == EBADF || error_code == ENXIO ||
           error_code == ENOENT;
}

[[nodiscard]] std::chrono::milliseconds RemainingTime(Clock::time_point deadline) {
    const auto now = Clock::now();
    if (now >= deadline) {
        return std::chrono::milliseconds{ 0 };
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return std::max(remaining, std::chrono::milliseconds{ 1 });
}

#ifndef _WIN32

class PosixUartIoBackend final : public UartIoBackend {
public:
    ~PosixUartIoBackend() override {
        Close();
    }

    UartIoResult Open(std::string_view device_path) override {
        Close();
        const std::string path(device_path);
        for (;;) {
            descriptor_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
            if (descriptor_ >= 0) {
                return { UartIoStatus::kSuccess, 0, 0 };
            }
            const int error_code = errno;
            if (error_code == EINTR) {
                continue;
            }
            return { IsConnectionError(error_code) ? UartIoStatus::kDisconnected : UartIoStatus::kIoError, 0,
                     error_code };
        }
    }

    void Close() noexcept override {
        if (descriptor_ < 0) {
            return;
        }
        const int descriptor = std::exchange(descriptor_, -1);
        static_cast<void>(::close(descriptor));
    }

    bool IsOpen() const noexcept override {
        return descriptor_ >= 0;
    }

    UartIoResult Read(std::span<std::uint8_t> buffer) override {
        if (!IsOpen()) {
            return { UartIoStatus::kNotOpen, 0, EBADF };
        }
        for (;;) {
            const ssize_t count = ::read(descriptor_, buffer.data(), buffer.size());
            if (count > 0) {
                return { UartIoStatus::kSuccess, static_cast<std::size_t>(count), 0 };
            }
            if (count == 0) {
                return { UartIoStatus::kDisconnected, 0, 0 };
            }
            const int error_code = errno;
            if (error_code == EINTR) {
                continue;
            }
            if (error_code == EAGAIN || error_code == EWOULDBLOCK) {
                return { UartIoStatus::kWouldBlock, 0, error_code };
            }
            return { IsConnectionError(error_code) ? UartIoStatus::kDisconnected : UartIoStatus::kIoError, 0,
                     error_code };
        }
    }

    UartIoResult Write(std::span<const std::uint8_t> data) override {
        if (!IsOpen()) {
            return { UartIoStatus::kNotOpen, 0, EBADF };
        }
        for (;;) {
            const ssize_t count = ::write(descriptor_, data.data(), data.size());
            if (count > 0) {
                return { UartIoStatus::kSuccess, static_cast<std::size_t>(count), 0 };
            }
            if (count == 0) {
                return { UartIoStatus::kDisconnected, 0, 0 };
            }
            const int error_code = errno;
            if (error_code == EINTR) {
                continue;
            }
            if (error_code == EAGAIN || error_code == EWOULDBLOCK) {
                return { UartIoStatus::kWouldBlock, 0, error_code };
            }
            return { IsConnectionError(error_code) ? UartIoStatus::kDisconnected : UartIoStatus::kIoError, 0,
                     error_code };
        }
    }

    UartIoResult WaitReadable(std::chrono::milliseconds timeout) override {
        return Wait(POLLIN, timeout);
    }

    UartIoResult WaitWritable(std::chrono::milliseconds timeout) override {
        return Wait(POLLOUT, timeout);
    }

private:
    UartIoResult Wait(short events, std::chrono::milliseconds timeout) {
        if (!IsOpen()) {
            return { UartIoStatus::kNotOpen, 0, EBADF };
        }
        const auto deadline = Clock::now() + timeout;
        for (;;) {
            pollfd descriptor{ descriptor_, events, 0 };
            const auto bounded_timeout =
                std::min<std::int64_t>(std::max<std::int64_t>(timeout.count(), 0), std::numeric_limits<int>::max());
            const int result = ::poll(&descriptor, 1, static_cast<int>(bounded_timeout));
            if (result == 0) {
                return { UartIoStatus::kTimeout, 0, 0 };
            }
            if (result < 0) {
                const int error_code = errno;
                if (error_code == EINTR) {
                    timeout = RemainingTime(deadline);
                    if (timeout.count() == 0) {
                        return { UartIoStatus::kTimeout, 0, 0 };
                    }
                    continue;
                }
                return { IsConnectionError(error_code) ? UartIoStatus::kDisconnected : UartIoStatus::kIoError, 0,
                         error_code };
            }
            if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                return { UartIoStatus::kDisconnected, 0, ENODEV };
            }
            if ((descriptor.revents & events) != 0) {
                return { UartIoStatus::kSuccess, 0, 0 };
            }
            return { UartIoStatus::kIoError, 0, EIO };
        }
    }

    int descriptor_{ -1 };
};

#else

class UnsupportedUartIoBackend final : public UartIoBackend {
public:
    UartIoResult Open(std::string_view) override {
        return { UartIoStatus::kIoError, 0, ENOTSUP };
    }
    void Close() noexcept override {}
    bool IsOpen() const noexcept override {
        return false;
    }
    UartIoResult Read(std::span<std::uint8_t>) override {
        return { UartIoStatus::kNotOpen, 0, ENOTSUP };
    }
    UartIoResult Write(std::span<const std::uint8_t>) override {
        return { UartIoStatus::kNotOpen, 0, ENOTSUP };
    }
    UartIoResult WaitReadable(std::chrono::milliseconds) override {
        return { UartIoStatus::kNotOpen, 0, ENOTSUP };
    }
    UartIoResult WaitWritable(std::chrono::milliseconds) override {
        return { UartIoStatus::kNotOpen, 0, ENOTSUP };
    }
};

#endif

}  // namespace

std::unique_ptr<UartIoBackend> CreatePlatformUartIoBackend() {
#ifndef _WIN32
    return std::make_unique<PosixUartIoBackend>();
#else
    return std::make_unique<UnsupportedUartIoBackend>();
#endif
}

UartTransport::UartTransport() : UartTransport(CreatePlatformUartIoBackend()) {}

UartTransport::UartTransport(std::unique_ptr<UartIoBackend> backend) : backend_(std::move(backend)) {
    if (backend_ == nullptr) {
        backend_ = CreatePlatformUartIoBackend();
    }
}

UartTransport::~UartTransport() {
    Close();
}

bool UartTransport::Open(std::string_view device_path) {
    Close();
    if (device_path.empty()) {
        static_cast<void>(Record({ UartIoStatus::kInvalidArgument, 0, EINVAL }));
        return false;
    }

    UartIoResult result = backend_->Open(device_path);
    if (!result.Succeeded()) {
        static_cast<void>(Record(result));
        return false;
    }
    device_path_ = device_path;
    static_cast<void>(Record(result));
    return true;
}

void UartTransport::Close() noexcept {
    backend_->Close();
    device_path_.clear();
}

bool UartTransport::IsOpen() const noexcept {
    return backend_->IsOpen();
}

UartIoResult UartTransport::Read(std::span<std::uint8_t> buffer, std::chrono::milliseconds timeout) {
    if (!IsOpen()) {
        return Record({ UartIoStatus::kNotOpen, 0, EBADF });
    }
    if (buffer.empty() || timeout.count() < 0) {
        return Record({ UartIoStatus::kInvalidArgument, 0, EINVAL });
    }

    const auto deadline = Clock::now() + timeout;
    for (;;) {
        UartIoResult result = backend_->Read(buffer);
        if (result.status == UartIoStatus::kSuccess) {
            if (result.bytes_transferred == 0) {
                return RecordAndDisconnect({ UartIoStatus::kDisconnected, 0, result.error_code });
            }
            return Record(result);
        }
        if (result.status != UartIoStatus::kWouldBlock) {
            return result.status == UartIoStatus::kDisconnected || result.status == UartIoStatus::kIoError
                       ? RecordAndDisconnect(result)
                       : Record(result);
        }
        if (timeout.count() == 0) {
            return Record(result);
        }

        const auto remaining = RemainingTime(deadline);
        if (remaining.count() == 0) {
            return Record({ UartIoStatus::kTimeout, 0, 0 });
        }
        result = backend_->WaitReadable(remaining);
        if (!result.Succeeded()) {
            return result.status == UartIoStatus::kDisconnected || result.status == UartIoStatus::kIoError
                       ? RecordAndDisconnect(result)
                       : Record(result);
        }
    }
}

UartIoResult UartTransport::WriteAll(std::span<const std::uint8_t> data, std::chrono::milliseconds timeout) {
    if (!IsOpen()) {
        return Record({ UartIoStatus::kNotOpen, 0, EBADF });
    }
    if (timeout.count() < 0) {
        return Record({ UartIoStatus::kInvalidArgument, 0, EINVAL });
    }
    if (data.empty()) {
        return Record({ UartIoStatus::kSuccess, 0, 0 });
    }

    const auto deadline = Clock::now() + timeout;
    std::size_t total_written = 0;
    while (total_written < data.size()) {
        UartIoResult result = backend_->Write(data.subspan(total_written));
        if (result.status == UartIoStatus::kSuccess) {
            if (result.bytes_transferred == 0 || result.bytes_transferred > data.size() - total_written) {
                return RecordAndDisconnect({ UartIoStatus::kDisconnected, total_written, result.error_code });
            }
            total_written += result.bytes_transferred;
            continue;
        }
        if (result.status != UartIoStatus::kWouldBlock) {
            result.bytes_transferred = total_written;
            return result.status == UartIoStatus::kDisconnected || result.status == UartIoStatus::kIoError
                       ? RecordAndDisconnect(result)
                       : Record(result);
        }

        const auto remaining = RemainingTime(deadline);
        if (remaining.count() == 0) {
            return Record({ UartIoStatus::kTimeout, total_written, 0 });
        }
        result = backend_->WaitWritable(remaining);
        if (!result.Succeeded()) {
            result.bytes_transferred = total_written;
            return result.status == UartIoStatus::kDisconnected || result.status == UartIoStatus::kIoError
                       ? RecordAndDisconnect(result)
                       : Record(result);
        }
    }
    return Record({ UartIoStatus::kSuccess, total_written, 0 });
}

UartIoStatus UartTransport::LastStatus() const noexcept {
    return last_status_;
}

int UartTransport::LastError() const noexcept {
    return last_error_;
}

const std::string& UartTransport::DevicePath() const noexcept {
    return device_path_;
}

UartIoResult UartTransport::Record(UartIoResult result) noexcept {
    last_status_ = result.status;
    last_error_ = result.error_code;
    return result;
}

UartIoResult UartTransport::RecordAndDisconnect(UartIoResult result) noexcept {
    backend_->Close();
    device_path_.clear();
    return Record(result);
}

}  // namespace logistics::device
