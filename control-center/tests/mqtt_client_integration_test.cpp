#include <QCoreApplication>
#include <QTimer>
#include <iostream>

#include "logistics/control_center/mqtt_client.hpp"

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: mqtt_client_integration_test <host> <port>\n";
        return 2;
    }

    bool port_is_valid = false;
    const auto port = QString::fromLocal8Bit(argv[2]).toInt(&port_is_valid);
    if (!port_is_valid || port <= 0 || port > 65535) {
        std::cerr << "invalid port\n";
        return 2;
    }

    QCoreApplication application(argc, argv);
    logistics::control_center::MqttClient client({ .host = QString::fromLocal8Bit(argv[1]),
                                                   .client_id = QStringLiteral("control-center-integration"),
                                                   .username = {},
                                                   .password = {},
                                                   .ca_certificate = {},
                                                   .port = port,
                                                   .reconnect_interval_ms = 1000,
                                                   .keep_alive_seconds = 10,
                                                   .tls_enabled = false });

    bool command_was_published = false;
    QObject::connect(
        &client, &logistics::control_center::MqttClient::connectionStateChanged, &application,
        [&client, &command_was_published](auto state, const QString&) {
            if (state != logistics::control_center::MqttClient::ConnectionState::Connected || command_was_published) {
                return;
            }
            command_was_published = client.publishCommand(logistics::contracts::mqtt::ControlCommand::kStatusRequest,
                                                          QStringLiteral("TEST-DEVICE")) >= 0;
        });
    QObject::connect(&client, &logistics::control_center::MqttClient::messageReceived, &application,
                     [&application, &command_was_published](const QString& topic, const QJsonObject&) {
                         if (command_was_published && topic == QStringLiteral("qt/control-center-integration/status")) {
                             application.exit(0);
                         }
                     });
    QObject::connect(&client, &logistics::control_center::MqttClient::messageRejected, &application,
                     [](const QString&, const QString& reason) {
                         std::cerr << "message rejected: " << reason.toStdString() << '\n';
                     });

    QTimer::singleShot(15000, &application, [&application]() { application.exit(1); });
    client.start();
    return application.exec();
}
