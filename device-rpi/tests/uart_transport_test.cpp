#include "logistics/device/uart_transport.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using logistics::device::UartIoBackend;
using logistics::device::UartIoResult;
using logistics::device::UartIoStatus;

struct ReadAction {
    UartIoResult result;
    std::vector<std::uint8_t> data;
};

class FakeUartIoBackend final : public UartIoBackend {
public:
    UartIoResult Open(std::string_view device_path) override {
        opened_path = device_path;
        open = open_result.Succeeded();
        return open_result;
    }

    void Close() noexcept override {
        open = false;
        ++close_calls;
    }

    bool IsOpen() const noexcept override {
        return open;
    }

    UartIoResult Read(std::span<std::uint8_t> buffer) override {
        ++read_calls;
        assert(!read_actions.empty());
        ReadAction action = std::move(read_actions.front());
        read_actions.pop_front();
        if (action.result.Succeeded()) {
            assert(action.data.size() == action.result.bytes_transferred);
            assert(action.data.size() <= buffer.size());
            std::copy(action.data.begin(), action.data.end(), buffer.begin());
        }
        return action.result;
    }

    UartIoResult Write(std::span<const std::uint8_t> data) override {
        ++write_calls;
        assert(!write_actions.empty());
        UartIoResult result = write_actions.front();
        write_actions.pop_front();
        if (result.Succeeded()) {
            assert(result.bytes_transferred <= data.size());
            written.insert(written.end(), data.begin(),
                           data.begin() + static_cast<std::ptrdiff_t>(result.bytes_transferred));
        }
        return result;
    }

    UartIoResult WaitReadable(std::chrono::milliseconds timeout) override {
        ++wait_readable_calls;
        last_read_timeout = timeout;
        assert(!read_wait_actions.empty());
        const UartIoResult result = read_wait_actions.front();
        read_wait_actions.pop_front();
        return result;
    }

    UartIoResult WaitWritable(std::chrono::milliseconds timeout) override {
        ++wait_writable_calls;
        last_write_timeout = timeout;
        assert(!write_wait_actions.empty());
        const UartIoResult result = write_wait_actions.front();
        write_wait_actions.pop_front();
        return result;
    }

    UartIoResult open_result{ UartIoStatus::kSuccess, 0, 0 };
    bool open{};
    int close_calls{};
    int read_calls{};
    int write_calls{};
    int wait_readable_calls{};
    int wait_writable_calls{};
    std::string opened_path;
    std::chrono::milliseconds last_read_timeout{};
    std::chrono::milliseconds last_write_timeout{};
    std::deque<ReadAction> read_actions;
    std::deque<UartIoResult> write_actions;
    std::deque<UartIoResult> read_wait_actions;
    std::deque<UartIoResult> write_wait_actions;
    std::vector<std::uint8_t> written;
};

struct Fixture {
    Fixture() {
        auto owned_backend = std::make_unique<FakeUartIoBackend>();
        backend = owned_backend.get();
        transport = std::make_unique<logistics::device::UartTransport>(std::move(owned_backend));
    }

    FakeUartIoBackend* backend{};
    std::unique_ptr<logistics::device::UartTransport> transport;
};

void TestOpenAndClose() {
    Fixture fixture;
    assert(fixture.transport->Open());
    assert(fixture.transport->IsOpen());
    assert(fixture.backend->opened_path == "/dev/vedauart");
    assert(fixture.transport->DevicePath() == "/dev/vedauart");

    fixture.transport->Close();
    assert(!fixture.transport->IsOpen());
    assert(fixture.transport->DevicePath().empty());
}

void TestOpenFailureIsReported() {
    Fixture fixture;
    fixture.backend->open_result = { UartIoStatus::kDisconnected, 0, 19 };
    assert(!fixture.transport->Open());
    assert(!fixture.transport->IsOpen());
    assert(fixture.transport->LastStatus() == UartIoStatus::kDisconnected);
    assert(fixture.transport->LastError() == 19);
}

void TestNonBlockingReadWouldBlock() {
    Fixture fixture;
    assert(fixture.transport->Open());
    fixture.backend->read_actions.push_back({ { UartIoStatus::kWouldBlock, 0, 11 }, {} });
    std::array<std::uint8_t, 8> buffer{};

    const UartIoResult result = fixture.transport->Read(buffer);

    assert(result.status == UartIoStatus::kWouldBlock);
    assert(fixture.backend->wait_readable_calls == 0);
    assert(fixture.transport->IsOpen());
}

void TestReadWaitsForPartialData() {
    Fixture fixture;
    assert(fixture.transport->Open());
    fixture.backend->read_actions.push_back({ { UartIoStatus::kWouldBlock, 0, 11 }, {} });
    fixture.backend->read_wait_actions.push_back({ UartIoStatus::kSuccess, 0, 0 });
    fixture.backend->read_actions.push_back({ { UartIoStatus::kSuccess, 3, 0 }, { 0xaa, 0x01, 0x02 } });
    std::array<std::uint8_t, 8> buffer{};

    const UartIoResult result = fixture.transport->Read(buffer, std::chrono::milliseconds{ 20 });

    assert(result.Succeeded());
    assert(result.bytes_transferred == 3);
    assert((std::array<std::uint8_t, 3>{ buffer[0], buffer[1], buffer[2] } ==
            std::array<std::uint8_t, 3>{ 0xaa, 0x01, 0x02 }));
    assert(fixture.backend->wait_readable_calls == 1);
}

void TestReadTimeout() {
    Fixture fixture;
    assert(fixture.transport->Open());
    fixture.backend->read_actions.push_back({ { UartIoStatus::kWouldBlock, 0, 11 }, {} });
    fixture.backend->read_wait_actions.push_back({ UartIoStatus::kTimeout, 0, 0 });
    std::array<std::uint8_t, 8> buffer{};

    const UartIoResult result = fixture.transport->Read(buffer, std::chrono::milliseconds{ 20 });

    assert(result.status == UartIoStatus::kTimeout);
    assert(fixture.transport->IsOpen());
}

void TestWriteAllHandlesPartialWrites() {
    Fixture fixture;
    assert(fixture.transport->Open());
    fixture.backend->write_actions.push_back({ UartIoStatus::kSuccess, 2, 0 });
    fixture.backend->write_actions.push_back({ UartIoStatus::kSuccess, 3, 0 });
    const std::array<std::uint8_t, 5> frame{ 0xaa, 0x01, 0x40, 0x00, 0x55 };

    const UartIoResult result = fixture.transport->WriteAll(frame, std::chrono::milliseconds{ 20 });

    assert(result.Succeeded());
    assert(result.bytes_transferred == frame.size());
    assert(fixture.backend->written == std::vector<std::uint8_t>(frame.begin(), frame.end()));
    assert(fixture.backend->write_calls == 2);
}

void TestWriteAllWaitsAfterWouldBlock() {
    Fixture fixture;
    assert(fixture.transport->Open());
    fixture.backend->write_actions.push_back({ UartIoStatus::kWouldBlock, 0, 11 });
    fixture.backend->write_wait_actions.push_back({ UartIoStatus::kSuccess, 0, 0 });
    fixture.backend->write_actions.push_back({ UartIoStatus::kSuccess, 3, 0 });
    const std::array<std::uint8_t, 3> frame{ 0xaa, 0x01, 0x55 };

    const UartIoResult result = fixture.transport->WriteAll(frame, std::chrono::milliseconds{ 20 });

    assert(result.Succeeded());
    assert(fixture.backend->wait_writable_calls == 1);
    assert(fixture.backend->written == std::vector<std::uint8_t>(frame.begin(), frame.end()));
}

void TestWriteTimeoutPreservesProgress() {
    Fixture fixture;
    assert(fixture.transport->Open());
    fixture.backend->write_actions.push_back({ UartIoStatus::kSuccess, 2, 0 });
    fixture.backend->write_actions.push_back({ UartIoStatus::kWouldBlock, 0, 11 });
    fixture.backend->write_wait_actions.push_back({ UartIoStatus::kTimeout, 0, 0 });
    const std::array<std::uint8_t, 4> frame{ 0xaa, 0x01, 0x02, 0x55 };

    const UartIoResult result = fixture.transport->WriteAll(frame, std::chrono::milliseconds{ 20 });

    assert(result.status == UartIoStatus::kTimeout);
    assert(result.bytes_transferred == 2);
    assert(fixture.transport->IsOpen());
}

void TestDisconnectClosesTransport() {
    Fixture fixture;
    assert(fixture.transport->Open());
    fixture.backend->read_actions.push_back({ { UartIoStatus::kDisconnected, 0, 19 }, {} });
    std::array<std::uint8_t, 8> buffer{};

    const UartIoResult result = fixture.transport->Read(buffer);

    assert(result.status == UartIoStatus::kDisconnected);
    assert(!fixture.transport->IsOpen());
    assert(fixture.transport->DevicePath().empty());
    assert(fixture.transport->LastError() == 19);
}

}  // namespace

int main() {
    TestOpenAndClose();
    TestOpenFailureIsReported();
    TestNonBlockingReadWouldBlock();
    TestReadWaitsForPartialData();
    TestReadTimeout();
    TestWriteAllHandlesPartialWrites();
    TestWriteAllWaitsAfterWouldBlock();
    TestWriteTimeoutPreservesProgress();
    TestDisconnectClosesTransport();
    return 0;
}
