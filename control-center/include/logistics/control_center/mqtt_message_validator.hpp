#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace logistics::control_center {

struct MqttValidationResult {
    bool is_valid{ false };
    QString error;
    QJsonObject envelope;
};

class MqttMessageValidator final {
public:
    [[nodiscard]] static MqttValidationResult Validate(const QString& topic, const QByteArray& payload,
                                                       const QString& client_id);
};

}  // namespace logistics::control_center
