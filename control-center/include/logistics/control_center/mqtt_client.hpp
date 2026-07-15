#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

#include "logistics/contracts/mqtt_message.hpp"

class QMqttClient;
class QTimer;

namespace logistics::control_center {

struct MqttClientConfig {
    QString host;
    QString client_id;
    QString username;
    QString password;
    int port{ 1883 };
    int reconnect_interval_ms{ 3000 };
    int keep_alive_seconds{ 30 };
};

class MqttClient final : public QObject {
    Q_OBJECT

public:
    enum class ConnectionState {
        Disconnected,
        Connecting,
        Connected,
        Reconnecting,
        Error,
    };
    Q_ENUM(ConnectionState)

    explicit MqttClient(MqttClientConfig config, QObject* parent = nullptr);

    void start();
    void stop();
    [[nodiscard]] qint32 publishCommand(logistics::contracts::mqtt::ControlCommand command,
                                        const QString& target_device_id, const QString& component_id = {});

signals:
    void connectionStateChanged(ConnectionState state, const QString& detail);
    void messageReceived(const QString& topic, const QJsonObject& envelope);
    void messageRejected(const QString& topic, const QString& reason);
    void errorOccurred(const QString& detail);
    void commandPublished(qint32 message_id, const QString& request_id,
                          logistics::contracts::mqtt::ControlCommand command);

private:
    void connectToBroker();
    void scheduleReconnect();
    void subscribeRequiredTopics();
    void handleMessage(const QByteArray& payload, const QString& topic);
    [[nodiscard]] QString errorDescription(int error) const;

    MqttClientConfig config_;
    QMqttClient* client_{ nullptr };
    QTimer* reconnect_timer_{ nullptr };
    bool stopping_{ false };
};

}  // namespace logistics::control_center
