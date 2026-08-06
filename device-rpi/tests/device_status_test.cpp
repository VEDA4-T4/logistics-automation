#include "logistics/device/device_status.hpp"

#include <cassert>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

namespace device = logistics::device;
namespace mqtt = logistics::contracts::mqtt;

void TestStatusSnapshotTracksDeviceState() {
    device::DeviceStatus status("PI-01");

    auto snapshot = status.Snapshot();
    assert(snapshot.device_id == "PI-01");
    assert(snapshot.connection_state == mqtt::ConnectionState::kOffline);
    assert(snapshot.current_state == "IDLE");
    assert(!snapshot.job_id.has_value());
    assert(!snapshot.error_code.has_value());
    assert(!snapshot.uart_connected);
    assert(status.IsForDevice("PI-01"));
    assert(!status.IsForDevice("PI-02"));

    status.SetConnectionState(mqtt::ConnectionState::kOnline);
    status.SetCurrentState("RUNNING");
    status.SetJobId(std::string("JOB-01"));
    status.SetErrorCode(std::string("ERR-SENSOR"));
    status.SetUartConnected(true);
    status.MarkCommunication("2026-07-16T06:00:00Z");

    snapshot = status.Snapshot();
    assert(snapshot.connection_state == mqtt::ConnectionState::kOnline);
    assert(snapshot.current_state == "RUNNING");
    assert(snapshot.job_id == std::optional<std::string>("JOB-01"));
    assert(snapshot.error_code == std::optional<std::string>("ERR-SENSOR"));
    assert(snapshot.last_communication_timestamp == "2026-07-16T06:00:00Z");
    assert(snapshot.uart_connected);
}

void TestInvalidStatusValuesAreRejected() {
    bool rejected = false;
    try {
        static_cast<void>(device::DeviceStatus("PI/01"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    device::DeviceStatus status("PI-01");
    rejected = false;
    try {
        status.SetConnectionState(mqtt::ConnectionState::kUnknown);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    rejected = false;
    try {
        status.SetCurrentState("");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

}  // namespace

int main() {
    TestStatusSnapshotTracksDeviceState();
    TestInvalidStatusValuesAreRejected();
    return 0;
}
