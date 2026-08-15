#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "logistics/central_server/sensor_detection.hpp"

namespace logistics::contracts::mqtt {
struct MqttMessage;
}

namespace logistics::central_server {

class DeviceManager;
class PersistenceService;

enum class MqttHandlerLogLevel : std::uint8_t {
    kInfo,
    kError,
};

class MqttHandler final {
public:
    using Logger = std::function<void(MqttHandlerLogLevel level, std::string_view message)>;
    using WorkCreatedHandler = std::function<bool(std::string_view device_id, std::string_view work_id)>;
    using QtEventHandler = std::function<bool(const contracts::mqtt::MqttMessage& message)>;
    using MessageRouteHandler = std::function<bool(const contracts::mqtt::MqttMessage& message)>;
    using ProcessMessageHandler = std::function<bool(const contracts::mqtt::MqttMessage& message)>;

    explicit MqttHandler(DeviceManager& device_manager, Logger logger = {},
                         PersistenceService* persistence_service = nullptr, std::string default_destination = {},
                         SensorDetectionConfig sensor_detection = {});
    void SetWorkCreatedHandler(WorkCreatedHandler handler);
    void SetQtEventHandler(QtEventHandler handler);
    void SetCommandRouteHandler(MessageRouteHandler handler);
    void SetQtResponseHandler(MessageRouteHandler handler);
    void SetQtStatusHandler(MessageRouteHandler handler);
    void SetQtErrorHandler(MessageRouteHandler handler);
    void SetProcessMessageGuard(ProcessMessageHandler handler);
    void SetProcessMessageHandler(ProcessMessageHandler handler);
    void SetProcessEpoch(std::string process_epoch, bool reject_legacy_work_messages);

    [[nodiscard]] bool Handle(std::string_view topic, std::string_view payload, std::string_view received_at = {},
                              int qos = 1, bool retained = false);
    [[nodiscard]] bool ReplayPendingReceivedEvents(std::size_t limit = 100);
    [[nodiscard]] bool CheckHeartbeatTimeouts(std::string_view checked_at = {});
    [[nodiscard]] bool ReplayDeviceStatuses(std::string_view target_device_id, std::string_view replayed_at = {});

private:
    [[nodiscard]] bool HandleMessage(std::string_view topic, std::string_view payload, std::string_view received_at,
                                     int qos, bool retained, bool replaying);
    void Log(MqttHandlerLogLevel level, std::string_view message) const;

    DeviceManager& device_manager_;
    Logger logger_;
    PersistenceService* persistence_service_;
    std::string default_destination_;
    SensorDetector sensor_detector_;
    WorkCreatedHandler work_created_handler_;
    QtEventHandler qt_event_handler_;
    MessageRouteHandler command_route_handler_;
    MessageRouteHandler qt_response_handler_;
    MessageRouteHandler qt_status_handler_;
    MessageRouteHandler qt_error_handler_;
    ProcessMessageHandler process_message_guard_;
    ProcessMessageHandler process_message_handler_;
    std::string process_epoch_;
    bool reject_legacy_work_messages_{ false };
    std::uint64_t timeout_message_sequence_{};
    std::uint64_t replay_message_sequence_{};
};

}  // namespace logistics::central_server
