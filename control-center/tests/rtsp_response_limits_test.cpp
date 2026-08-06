#include <QCoreApplication>
#include <QEventLoop>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "logistics/control_center/onvif_rtsp_metadata_client.hpp"
#include "logistics/control_center/rtsp_h264_stream.hpp"

namespace {

QUrl LocalRtspUrl(const QTcpServer& server) {
    return QUrl(QStringLiteral("rtsp://127.0.0.1:%1/stream").arg(server.serverPort()));
}

bool RejectsIncompleteHeaderByDeadline() {
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) {
        return false;
    }

    QEventLoop loop;
    QTimer trickle;
    trickle.setInterval(25);
    QTcpSocket* peer = nullptr;
    QObject::connect(&server, &QTcpServer::newConnection, &loop, [&]() {
        peer = server.nextPendingConnection();
        QObject::connect(peer, &QTcpSocket::readyRead, &loop, [&]() {
            if (peer->readAll().contains("\r\n\r\n")) {
                trickle.start();
            }
        });
    });
    QObject::connect(&trickle, &QTimer::timeout, &loop, [&]() {
        if (peer != nullptr) {
            peer->write("x");
        }
    });

    logistics::control_center::RtspH264Stream client;
    client.setNetworkTimeout(150);
    bool rejected = false;
    QObject::connect(&client, &logistics::control_center::RtspH264Stream::streamError, &loop, [&](const QString&) {
        rejected = true;
        loop.quit();
    });
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    client.start(LocalRtspUrl(server));
    loop.exec();
    client.stop();
    return rejected;
}

bool RejectsOversizedBody() {
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) {
        return false;
    }

    QEventLoop loop;
    QObject::connect(&server, &QTcpServer::newConnection, &loop, [&]() {
        auto* peer = server.nextPendingConnection();
        QObject::connect(peer, &QTcpSocket::readyRead, &loop, [peer]() {
            if (peer->readAll().contains("\r\n\r\n")) {
                peer->write("RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 4194305\r\n\r\n");
            }
        });
    });

    logistics::control_center::OnvifRtspMetadataClient client;
    client.setReconnectInterval(10000);
    bool rejected = false;
    QObject::connect(&client, &logistics::control_center::OnvifRtspMetadataClient::connectionStateChanged, &loop,
                     [&](bool connected, const QString& detail) {
                         if (!connected && detail.contains(QStringLiteral("body"), Qt::CaseInsensitive)) {
                             rejected = true;
                             loop.quit();
                         }
                     });
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    client.start(LocalRtspUrl(server));
    loop.exec();
    client.stop();
    return rejected;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    return RejectsIncompleteHeaderByDeadline() && RejectsOversizedBody() ? 0 : 1;
}
