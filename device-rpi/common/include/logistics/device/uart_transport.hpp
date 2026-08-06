#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace logistics::device {

enum class UartIoStatus {
    kSuccess,
    kWouldBlock,
    kTimeout,
    kDisconnected,
    kNotOpen,
    kInvalidArgument,
    kIoError,
};

struct UartIoResult {
    UartIoStatus status{ UartIoStatus::kIoError };
    std::size_t bytes_transferred{};
    int error_code{};

    [[nodiscard]] bool Succeeded() const noexcept {
        return status == UartIoStatus::kSuccess;
    }
};

/*
 * Platform seam used by UartTransport. Production uses the POSIX backend;
 * tests inject a fake backend so timeout and partial-I/O behavior can be
 * verified without /dev/vedauart hardware.
 */
class UartIoBackend {
public:
    virtual ~UartIoBackend() = default;

    [[nodiscard]] virtual UartIoResult Open(std::string_view device_path) = 0;
    virtual void Close() noexcept = 0;
    [[nodiscard]] virtual bool IsOpen() const noexcept = 0;
    [[nodiscard]] virtual UartIoResult Read(std::span<std::uint8_t> buffer) = 0;
    [[nodiscard]] virtual UartIoResult Write(std::span<const std::uint8_t> data) = 0;
    [[nodiscard]] virtual UartIoResult WaitReadable(std::chrono::milliseconds timeout) = 0;
    [[nodiscard]] virtual UartIoResult WaitWritable(std::chrono::milliseconds timeout) = 0;
};

[[nodiscard]] std::unique_ptr<UartIoBackend> CreatePlatformUartIoBackend();

class UartTransport final {
public:
    static constexpr std::string_view kDefaultDevicePath{ "/dev/vedauart" };

    UartTransport();
    explicit UartTransport(std::unique_ptr<UartIoBackend> backend);
    ~UartTransport();

    UartTransport(const UartTransport&) = delete;
    UartTransport& operator=(const UartTransport&) = delete;
    UartTransport(UartTransport&&) = delete;
    UartTransport& operator=(UartTransport&&) = delete;

    [[nodiscard]] bool Open(std::string_view device_path = kDefaultDevicePath);
    void Close() noexcept;
    [[nodiscard]] bool IsOpen() const noexcept;

    [[nodiscard]] UartIoResult Read(std::span<std::uint8_t> buffer,
                                    std::chrono::milliseconds timeout = std::chrono::milliseconds{ 0 });
    [[nodiscard]] UartIoResult WriteAll(std::span<const std::uint8_t> data, std::chrono::milliseconds timeout);

    [[nodiscard]] UartIoStatus LastStatus() const noexcept;
    [[nodiscard]] int LastError() const noexcept;
    [[nodiscard]] const std::string& DevicePath() const noexcept;

private:
    [[nodiscard]] UartIoResult Record(UartIoResult result) noexcept;
    [[nodiscard]] UartIoResult RecordAndDisconnect(UartIoResult result) noexcept;

    std::unique_ptr<UartIoBackend> backend_;
    std::string device_path_;
    UartIoStatus last_status_{ UartIoStatus::kNotOpen };
    int last_error_{};
};

}  // namespace logistics::device
