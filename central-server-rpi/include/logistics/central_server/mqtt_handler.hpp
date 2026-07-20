#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

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

    explicit MqttHandler(DeviceManager& device_manager, Logger logger = {},
                         PersistenceService* persistence_service = nullptr);
    void SetWorkCreatedHandler(WorkCreatedHandler handler);
    void SetQtEventHandler(QtEventHandler handler);
    void SetCommandRouteHandler(MessageRouteHandler handler);
    void SetQtResponseHandler(MessageRouteHandler handler);
    void SetQtStatusHandler(MessageRouteHandler handler);
    void SetQtErrorHandler(MessageRouteHandler handler);

    [[nodiscard]] bool Handle(std::string_view topic, std::string_view payload, std::string_view received_at = {});

private:
    void Log(MqttHandlerLogLevel level, std::string_view message) const;

    DeviceManager& device_manager_;
    Logger logger_;
    PersistenceService* persistence_service_;
    WorkCreatedHandler work_created_handler_;
    QtEventHandler qt_event_handler_;
    MessageRouteHandler command_route_handler_;
    MessageRouteHandler qt_response_handler_;
    MessageRouteHandler qt_status_handler_;
    MessageRouteHandler qt_error_handler_;
};

}  // namespace logistics::central_server
