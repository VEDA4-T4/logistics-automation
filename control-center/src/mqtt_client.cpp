#include "logistics/control_center/mqtt_client.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMqttClient>
#include <QMqttSubscription>
#include <QMqttTopicFilter>
#include <QMqttTopicName>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTimer>
#include <QUuid>
#include <array>
#include <string>
#include <string_view>
#include <utility>

#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/control_center/mqtt_message_validator.hpp"

namespace logistics::control_center {
namespace {

namespace mqtt = logistics::contracts::mqtt;

QString ToQString(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

}  // namespace

MqttClient::MqttClient(MqttClientConfig config, QObject* parent)
    : QObject(parent), config_(std::move(config)), client_(new QMqttClient(this)), reconnect_timer_(new QTimer(this)) {
    client_->setHostname(config_.host);
    client_->setPort(static_cast<quint16>(config_.port));
    client_->setClientId(config_.client_id);
    client_->setUsername(config_.username);
    client_->setPassword(config_.password);
    client_->setProtocolVersion(QMqttClient::MQTT_3_1_1);
    client_->setCleanSession(true);
    client_->setKeepAlive(static_cast<quint16>(config_.keep_alive_seconds));

    reconnect_timer_->setSingleShot(true);
    reconnect_timer_->setInterval(config_.reconnect_interval_ms);
    connect(reconnect_timer_, &QTimer::timeout, this, &MqttClient::connectToBroker);

    connect(client_, &QMqttClient::stateChanged, this, [this](QMqttClient::ClientState state) {
        switch (state) {
            case QMqttClient::Disconnected:
                if (!stopping_) {
                    scheduleReconnect();
                } else {
                    emit connectionStateChanged(ConnectionState::Disconnected, QStringLiteral("연결 해제"));
                }
                break;
            case QMqttClient::Connecting:
                emit connectionStateChanged(ConnectionState::Connecting,
                                            QStringLiteral("%1:%2 연결 중").arg(config_.host).arg(config_.port));
                break;
            case QMqttClient::Connected:
                reconnect_timer_->stop();
                emit connectionStateChanged(ConnectionState::Connected,
                                            QStringLiteral("%1:%2 연결됨").arg(config_.host).arg(config_.port));
                subscribeRequiredTopics();
                break;
        }
    });

    connect(client_, &QMqttClient::errorChanged, this, [this](QMqttClient::ClientError error) {
        if (error == QMqttClient::NoError) {
            return;
        }
        const auto description = errorDescription(static_cast<int>(error));
        emit connectionStateChanged(ConnectionState::Error, description);
        emit errorOccurred(description);
    });

    connect(client_, &QMqttClient::messageReceived, this,
            [this](const QByteArray& payload, const QMqttTopicName& topic) { handleMessage(payload, topic.name()); });
    if (auto* application = QCoreApplication::instance(); application != nullptr) {
        connect(application, &QCoreApplication::aboutToQuit, this, &MqttClient::stop);
    }
}

void MqttClient::start() {
    stopping_ = false;
    connectToBroker();
}

void MqttClient::stop() {
    stopping_ = true;
    reconnect_timer_->stop();
    if (client_->state() == QMqttClient::Disconnected) {
        return;
    }

    client_->disconnectFromHost();
    if (QCoreApplication::instance() == nullptr) {
        return;
    }

    QEventLoop disconnect_loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(client_, &QMqttClient::stateChanged, &disconnect_loop, [&disconnect_loop](QMqttClient::ClientState state) {
        if (state == QMqttClient::Disconnected) {
            disconnect_loop.quit();
        }
    });
    connect(&timeout, &QTimer::timeout, &disconnect_loop, &QEventLoop::quit);
    timeout.start(500);
    if (client_->state() != QMqttClient::Disconnected) {
        disconnect_loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
}

qint32 MqttClient::publishCommand(mqtt::ControlCommand command, const QString& target_device_id,
                                  const QString& component_id) {
    const auto target_text = target_device_id.toStdString();
    const auto component_text = component_id.toStdString();
    if (command == mqtt::ControlCommand::kUnknown || !mqtt::IsValidTopicLevel(target_text) ||
        (!component_id.isEmpty() && !mqtt::IsValidTopicLevel(component_text))) {
        emit errorOccurred(QStringLiteral("발행할 관제 명령의 값이 잘못되었습니다."));
        return -1;
    }
    if (client_->state() != QMqttClient::Connected) {
        emit errorOccurred(QStringLiteral("MQTT 연결 전에는 관제 명령을 발행할 수 없습니다."));
        return -1;
    }

    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject data{
        { QString::fromLatin1(mqtt::kRequestIdField), request_id },
        { QStringLiteral("command"), ToQString(mqtt::ToString(command)) },
        { QString::fromLatin1(mqtt::kTargetDeviceIdField), target_device_id },
    };
    if (!component_id.isEmpty()) {
        data.insert(QString::fromLatin1(mqtt::kComponentIdField), component_id);
    }

    const auto message_type = command == mqtt::ControlCommand::kEmergencyStop ? mqtt::MessageType::kEmergencyStop
                                                                              : mqtt::MessageType::kControlCommand;
    const QJsonObject envelope{
        { QString::fromLatin1(mqtt::kProtocolVersionField), QString::fromLatin1(mqtt::kCurrentProtocolVersion) },
        { QString::fromLatin1(mqtt::kMessageIdField), QUuid::createUuid().toString(QUuid::WithoutBraces) },
        { QString::fromLatin1(mqtt::kMessageTypeField), ToQString(mqtt::ToString(message_type)) },
        { QString::fromLatin1(mqtt::kSourceIdField), config_.client_id },
        { QString::fromLatin1(mqtt::kTimestampField), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) },
        { QString::fromLatin1(mqtt::kDataField), data },
    };

    const auto request_topic = mqtt::QtRequestTopic(config_.client_id.toStdString());
    const auto message_id = client_->publish(QMqttTopicName(ToQString(request_topic)),
                                             QJsonDocument(envelope).toJson(QJsonDocument::Compact), 1, false);
    if (message_id < 0) {
        emit errorOccurred(QStringLiteral("관제 명령 발행에 실패했습니다."));
        return -1;
    }

    emit commandPublished(message_id, request_id, command);
    return message_id;
}

void MqttClient::connectToBroker() {
    if (stopping_ || client_->state() != QMqttClient::Disconnected) {
        return;
    }

    emit connectionStateChanged(ConnectionState::Connecting,
                                QStringLiteral("%1:%2 연결 중").arg(config_.host).arg(config_.port));

    if (!config_.tls_enabled) {
        client_->connectToHost();
        return;
    }

    QFile ca_file(config_.ca_certificate);
    if (!ca_file.open(QIODevice::ReadOnly)) {
        const auto detail = QStringLiteral("MQTT CA 인증서를 읽을 수 없습니다: %1").arg(config_.ca_certificate);
        emit connectionStateChanged(ConnectionState::Error, detail);
        emit errorOccurred(detail);
        return;
    }

    const auto ca_certificates = QSslCertificate::fromDevice(&ca_file, QSsl::Pem);
    if (ca_certificates.isEmpty()) {
        const auto detail = QStringLiteral("MQTT CA 인증서 형식이 올바르지 않습니다: %1").arg(config_.ca_certificate);
        emit connectionStateChanged(ConnectionState::Error, detail);
        emit errorOccurred(detail);
        return;
    }

    auto ssl_configuration = QSslConfiguration::defaultConfiguration();
    ssl_configuration.addCaCertificates(ca_certificates);
    ssl_configuration.setPeerVerifyMode(QSslSocket::VerifyPeer);
    ssl_configuration.setProtocol(QSsl::TlsV1_2OrLater);

    client_->connectToHostEncrypted(ssl_configuration);
}
void MqttClient::scheduleReconnect() {
    if (stopping_) {
        return;
    }
    emit connectionStateChanged(ConnectionState::Reconnecting,
                                QStringLiteral("연결 안 됨 · %1초 후 재연결")
                                    .arg(static_cast<double>(config_.reconnect_interval_ms) / 1000.0, 0, 'g', 3));
    if (!reconnect_timer_->isActive()) {
        reconnect_timer_->start();
    }
}

void MqttClient::subscribeRequiredTopics() {
    const auto client_id = config_.client_id.toStdString();
    const std::array topics = {
        ToQString(mqtt::QtResponseTopic(client_id)), ToQString(mqtt::QtStatusTopic(client_id)),
        ToQString(mqtt::QtEventTopic(client_id)),    ToQString(mqtt::QtErrorTopic(client_id)),
        ToQString(mqtt::kServerStatusTopic),         ToQString(mqtt::kServerHeartbeatTopic),
    };

    for (const auto& topic : topics) {
        auto* subscription = client_->subscribe(QMqttTopicFilter(topic), 1);
        if (subscription == nullptr) {
            emit errorOccurred(QStringLiteral("MQTT 토픽 구독 요청 실패: %1").arg(topic));
            continue;
        }
        connect(subscription, &QMqttSubscription::stateChanged, this,
                [this, subscription, topic](QMqttSubscription::SubscriptionState state) {
                    if (state == QMqttSubscription::Error) {
                        emit errorOccurred(
                            QStringLiteral("MQTT 토픽 구독 실패: %1 (%2)").arg(topic, subscription->reason()));
                    }
                });
    }
}

void MqttClient::handleMessage(const QByteArray& payload, const QString& topic) {
    const auto result = MqttMessageValidator::Validate(topic, payload, config_.client_id);
    if (!result.is_valid) {
        emit messageRejected(topic, result.error);
        return;
    }
    emit messageReceived(topic, result.envelope);
}

QString MqttClient::errorDescription(int error) const {
    switch (static_cast<QMqttClient::ClientError>(error)) {
        case QMqttClient::InvalidProtocolVersion:
            return QStringLiteral("MQTT protocol version이 거부되었습니다.");
        case QMqttClient::IdRejected:
            return QStringLiteral("MQTT client_id가 거부되었습니다.");
        case QMqttClient::ServerUnavailable:
            return QStringLiteral("MQTT broker를 사용할 수 없습니다.");
        case QMqttClient::BadUsernameOrPassword:
            return QStringLiteral("MQTT 사용자명 또는 비밀번호가 잘못되었습니다.");
        case QMqttClient::NotAuthorized:
            return QStringLiteral("MQTT 연결 권한이 없습니다.");
        case QMqttClient::TransportInvalid:
            return QStringLiteral("MQTT transport 연결에 실패했습니다.");
        case QMqttClient::ProtocolViolation:
            return QStringLiteral("MQTT protocol 위반이 발생했습니다.");
        case QMqttClient::Mqtt5SpecificError:
            return QStringLiteral("MQTT 5 오류가 발생했습니다.");
        case QMqttClient::UnknownError:
            return QStringLiteral("알 수 없는 MQTT 오류가 발생했습니다.");
        case QMqttClient::NoError:
            break;
    }
    return QStringLiteral("MQTT 오류가 발생했습니다.");
}

}  // namespace logistics::control_center
