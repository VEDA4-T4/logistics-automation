#include "logistics/control_center/onvif_rtsp_metadata_client.hpp"

#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace {

QByteArray rtspResponse(int cseq, const QByteArray& headers = {}, const QByteArray& body = {}) {
    QByteArray response = "RTSP/1.0 200 OK\r\nCSeq: " + QByteArray::number(cseq) + "\r\n" + headers;
    if (!body.isEmpty()) {
        response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    }
    return response + "\r\n" + body;
}

QByteArray interleavedMetadataPacket(const QByteArray& xml) {
    QByteArray rtp(12, '\0');
    rtp[0] = static_cast<char>(0x80);
    rtp[1] = static_cast<char>(0x80 | 107);
    rtp[2] = 0;
    rtp[3] = 1;
    rtp.append(xml);

    QByteArray packet;
    packet.append('$');
    packet.append('\0');
    packet.append(static_cast<char>((rtp.size() >> 8) & 0xFF));
    packet.append(static_cast<char>(rtp.size() & 0xFF));
    return packet + rtp;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) {
        return 1;
    }

    logistics::control_center::OnvifRtspMetadataClient client;
    QByteArray request_buffer;
    int request_count = 0;
    bool received = false;

    QObject::connect(&server, &QTcpServer::newConnection, &app, [&]() {
        auto* socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, &app, [&, socket]() {
            request_buffer.append(socket->readAll());
            while (true) {
                const auto request_end = request_buffer.indexOf("\r\n\r\n");
                if (request_end < 0) {
                    break;
                }
                const auto request = request_buffer.left(request_end + 4);
                request_buffer.remove(0, request_end + 4);
                ++request_count;

                if (request.startsWith("DESCRIBE ")) {
                    const auto base =
                        QByteArray("rtsp://127.0.0.1:") + QByteArray::number(server.serverPort()) + "/stream/";
                    const QByteArray sdp =
                        "v=0\r\n"
                        "s=ONVIF metadata test\r\n"
                        "m=application 0 RTP/AVP 107\r\n"
                        "a=rtpmap:107 vnd.onvif.metadata/90000\r\n"
                        "a=control:trackID=1\r\n";
                    socket->write(rtspResponse(request_count,
                                               "Content-Type: application/sdp\r\nContent-Base: " + base + "\r\n", sdp));
                } else if (request.startsWith("SETUP ") && request.contains("/stream/trackID=1 ")) {
                    socket->write(rtspResponse(request_count,
                                               "Session: test-session;timeout=60\r\n"
                                               "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"));
                } else if (request.startsWith("PLAY ") && request.contains("Session: test-session")) {
                    const QByteArray xml =
                        "<tt:MetadataStream xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
                        "<tt:VideoAnalytics><tt:Frame UtcTime=\"2026-07-08T00:32:39.591Z\"/>"
                        "</tt:VideoAnalytics></tt:MetadataStream>";
                    socket->write(rtspResponse(request_count, "Session: test-session\r\n") +
                                  interleavedMetadataPacket(xml));
                } else {
                    app.exit(2);
                }
            }
        });
    });

    QObject::connect(&client, &logistics::control_center::OnvifRtspMetadataClient::metadataReceived, &app,
                     [&](const QByteArray& xml) {
                         received = xml.contains("<tt:MetadataStream") && xml.contains("<tt:Frame ");
                         app.quit();
                     });
    QTimer::singleShot(3000, &app, [&]() { app.exit(3); });

    client.start(QUrl(QStringLiteral("rtsp://user:password@127.0.0.1:%1/stream").arg(server.serverPort())));
    const auto exit_code = app.exec();
    return exit_code == 0 && received && request_count == 3 ? 0 : 1;
}
