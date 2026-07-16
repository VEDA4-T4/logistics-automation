#pragma once

#include <QJsonObject>
#include <QString>

#include "logistics/contracts/mqtt_message.hpp"

namespace logistics::control_center {

struct CommandResponse {
    bool is_valid{ false };
    QString error;
    QString request_id;
    logistics::contracts::mqtt::ControlCommand command{ logistics::contracts::mqtt::ControlCommand::kUnknown };
    logistics::contracts::mqtt::CommandResult result{ logistics::contracts::mqtt::CommandResult::kUnknown };
    QString error_code;
    QString message;
};

[[nodiscard]] CommandResponse ParseCommandResponse(const QJsonObject& envelope);

}  // namespace logistics::control_center
