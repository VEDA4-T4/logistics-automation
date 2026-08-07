#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <memory>

#include "logistics/control_center/mqtt_client.hpp"

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) {
        return 1;
    }

    QByteArray received;
    bool connack_sent = false;
    QTcpSocket* peer = nullptr;
    QObject::connect(&server, &QTcpServer::newConnection, &application, [&]() {
        peer = server.nextPendingConnection();
        QObject::connect(peer, &QTcpSocket::readyRead, &application, [&]() {
            received.append(peer->readAll());
            if (!connack_sent) {
                connack_sent = true;
                peer->write(QByteArray::fromHex("20020000"));
            }
        });
    });

    auto client = std::make_unique<logistics::control_center::MqttClient>(
        logistics::control_center::MqttClientConfig{ .host = QStringLiteral("127.0.0.1"),
                                                     .client_id = QStringLiteral("graceful-disconnect-test"),
                                                     .username = {},
                                                     .password = {},
                                                     .ca_certificate = {},
                                                     .port = server.serverPort(),
                                                     .reconnect_interval_ms = 100,
                                                     .keep_alive_seconds = 10 });
    QObject::connect(client.get(), &logistics::control_center::MqttClient::connectionStateChanged, &application,
                     [&](logistics::control_center::MqttClient::ConnectionState state, const QString&) {
                         if (state == logistics::control_center::MqttClient::ConnectionState::Connected) {
                             application.quit();
                         }
                     });

    QTimer::singleShot(3000, &application, [&application]() { application.exit(2); });
    client->start();
    const auto exit_code = application.exec();
    client.reset();
    if (peer != nullptr && peer->waitForReadyRead(1000)) {
        received.append(peer->readAll());
    }
    return exit_code == 0 && received.contains(QByteArray::fromHex("e000")) ? 0 : 1;
}
