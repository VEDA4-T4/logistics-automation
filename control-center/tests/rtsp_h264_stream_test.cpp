#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include "logistics/control_center/rtsp_stream_worker.hpp"

namespace {

QByteArray rtspResponse(int cseq, const QByteArray& headers = {}, const QByteArray& body = {}) {
    QByteArray response = "RTSP/1.0 200 OK\r\nCSeq: " + QByteArray::number(cseq) + "\r\n" + headers;
    if (!body.isEmpty()) {
        response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    }
    return response + "\r\n" + body;
}

QByteArray interleavedH264Packet(const QByteArray& payload, quint16 sequence, quint32 timestamp, bool marker) {
    QByteArray rtp(12, '\0');
    rtp[0] = static_cast<char>(0x80);
    rtp[1] = static_cast<char>((marker ? 0x80 : 0x00) | 96);
    rtp[2] = static_cast<char>((sequence >> 8) & 0xFF);
    rtp[3] = static_cast<char>(sequence & 0xFF);
    rtp[4] = static_cast<char>((timestamp >> 24) & 0xFF);
    rtp[5] = static_cast<char>((timestamp >> 16) & 0xFF);
    rtp[6] = static_cast<char>((timestamp >> 8) & 0xFF);
    rtp[7] = static_cast<char>(timestamp & 0xFF);
    rtp.append(payload);

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

    logistics::control_center::RtspStreamWorker worker(3000, 2 * 1024 * 1024);
    auto* stream = worker.stream();
    if (stream->thread() == QThread::currentThread()) {
        return 5;
    }
    QByteArray request_buffer;
    QByteArray decoded_stream;
    int request_count = 0;
    bool forced_tcp = false;
    bool packet_loss_detected = false;

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
                        "s=H264 TCP test\r\n"
                        "m=video 0 RTP/AVP 96\r\n"
                        "a=rtpmap:96 H264/90000\r\n"
                        "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=Z2QAH6zZQFAFuwEBAaQeJEU=,aO48gA==\r\n"
                        "a=control:trackID=1\r\n";
                    socket->write(rtspResponse(request_count,
                                               "Content-Type: application/sdp\r\nContent-Base: " + base + "\r\n", sdp));
                } else if (request.startsWith("SETUP ") && request.contains("/stream/trackID=1 ")) {
                    forced_tcp = request.contains("Transport: RTP/AVP/TCP;unicast;interleaved=0-1");
                    socket->write(rtspResponse(request_count,
                                               "Session: video-session;timeout=60\r\n"
                                               "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"));
                } else if (request.startsWith("PLAY ") && request.contains("Session: video-session")) {
                    socket->write(rtspResponse(request_count, "Session: video-session\r\n") +
                                  interleavedH264Packet(QByteArray::fromHex("419a10"), 1, 90000, true) +
                                  interleavedH264Packet(QByteArray::fromHex("419a20"), 3, 93000, true) +
                                  interleavedH264Packet(QByteArray::fromHex("7c858884"), 4, 96000, false) +
                                  interleavedH264Packet(QByteArray::fromHex("7c4521a0"), 5, 96000, true));
                } else {
                    app.exit(2);
                }
            }
        });
    });

    QObject::connect(stream, &QIODevice::readyRead, &app, [&]() {
        decoded_stream.append(stream->read(stream->bytesAvailable()));
        if (decoded_stream.contains(QByteArray::fromHex("0000000165888421a0"))) {
            app.quit();
        }
    });
    QObject::connect(stream, &logistics::control_center::RtspH264Stream::streamError, &app,
                     [&](const QString&) { app.exit(3); });
    QObject::connect(
        stream, &logistics::control_center::RtspH264Stream::packetLossDetected, &app,
        [&](quint16 expected, quint16 received) { packet_loss_detected = expected == 2 && received == 3; });
    QTimer::singleShot(3000, &app, [&]() { app.exit(4); });

    worker.start(QUrl(QStringLiteral("rtsp://user:password@127.0.0.1:%1/stream").arg(server.serverPort())));
    const auto exit_code = app.exec();
    worker.stop();

    const auto has_sps = decoded_stream.contains(QByteArray::fromHex("000000016764001facd9405005bb010101a41e2445"));
    const auto has_pps = decoded_stream.contains(QByteArray::fromHex("0000000168ee3c80"));
    const auto has_idr = decoded_stream.contains(QByteArray::fromHex("0000000165888421a0"));
    return exit_code == 0 && request_count == 3 && forced_tcp && packet_loss_detected && has_sps && has_pps && has_idr
               ? 0
               : 1;
}
