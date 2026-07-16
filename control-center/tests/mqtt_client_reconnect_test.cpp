#include <QCoreApplication>
#include <QTimer>

#include "logistics/control_center/mqtt_client.hpp"

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    logistics::control_center::MqttClient client({ .host = QStringLiteral("127.0.0.1"),
                                                   .client_id = QStringLiteral("control-center-reconnect-test"),
                                                   .username = {},
                                                   .password = {},
                                                   .port = 1,
                                                   .reconnect_interval_ms = 100,
                                                   .keep_alive_seconds = 10 });

    bool reconnect_was_scheduled = false;
    QObject::connect(&client, &logistics::control_center::MqttClient::connectionStateChanged, &application,
                     [&application, &reconnect_was_scheduled](
                         logistics::control_center::MqttClient::ConnectionState state, const QString&) {
                         if (state == logistics::control_center::MqttClient::ConnectionState::Reconnecting) {
                             reconnect_was_scheduled = true;
                         } else if (state == logistics::control_center::MqttClient::ConnectionState::Connecting &&
                                    reconnect_was_scheduled) {
                             application.exit(0);
                         }
                     });

    QTimer::singleShot(5000, &application, [&application]() { application.exit(1); });
    client.start();
    return application.exec();
}
