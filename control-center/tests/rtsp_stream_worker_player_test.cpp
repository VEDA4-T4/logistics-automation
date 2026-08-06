#include <QApplication>
#include <QMediaPlayer>
#include <QPlaybackOptions>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVideoSink>

#include "logistics/control_center/rtsp_stream_worker.hpp"

namespace {

QByteArray rtspResponse(int cseq, const QByteArray& headers = {}, const QByteArray& body = {}) {
    QByteArray response = "RTSP/1.0 200 OK\r\nCSeq: " + QByteArray::number(cseq) + "\r\n" + headers;
    if (!body.isEmpty()) {
        response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    }
    return response + "\r\n" + body;
}

QByteArray interleavedH264Packet(const QByteArray& payload, quint16 sequence, quint32 timestamp) {
    QByteArray rtp(12, '\0');
    rtp[0] = static_cast<char>(0x80);
    rtp[1] = static_cast<char>(0x80 | 96);
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
    QApplication app(argc, argv);
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) {
        return 1;
    }

    logistics::control_center::RtspStreamWorker worker(1000, 64 * 1024);
    QMediaPlayer player;
    QVideoSink video_sink;
    player.setVideoSink(&video_sink);
    QTimer frame_timer;
    QByteArray request_buffer;
    QTcpSocket* stream_socket = nullptr;
    quint16 sequence = 1;
    quint32 timestamp = 90000;
    QByteArray idr_payload(1400, '\0');
    idr_payload[0] = static_cast<char>(0x65);
    int request_count = 0;
    int exit_code = 4;
    bool stream_ready = false;
    bool player_attached = false;
    bool cleanup_started = false;

    QObject::connect(&server, &QTcpServer::newConnection, &app, [&]() {
        stream_socket = server.nextPendingConnection();
        stream_socket->setParent(&server);
        QObject::connect(stream_socket, &QTcpSocket::readyRead, &app, [&]() {
            request_buffer.append(stream_socket->readAll());
            while (true) {
                const auto request_end = request_buffer.indexOf("\r\n\r\n");
                if (request_end < 0) {
                    return;
                }
                const auto request = request_buffer.left(request_end + 4);
                request_buffer.remove(0, request_end + 4);
                ++request_count;

                if (request.startsWith("DESCRIBE ")) {
                    const auto base =
                        QByteArray("rtsp://127.0.0.1:") + QByteArray::number(server.serverPort()) + "/stream/";
                    const QByteArray sdp =
                        "v=0\r\n"
                        "s=Worker player test\r\n"
                        "m=video 0 RTP/AVP 96\r\n"
                        "a=rtpmap:96 H264/90000\r\n"
                        "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=Z2QAH6zZQFAFuwEBAaQeJEV,aO48gA==\r\n"
                        "a=control:trackID=1\r\n";
                    stream_socket->write(rtspResponse(
                        request_count, "Content-Type: application/sdp\r\nContent-Base: " + base + "\r\n", sdp));
                } else if (request.startsWith("SETUP ")) {
                    stream_socket->write(rtspResponse(request_count,
                                                      "Session: player-session;timeout=60\r\n"
                                                      "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"));
                } else if (request.startsWith("PLAY ")) {
                    stream_socket->write(rtspResponse(request_count, "Session: player-session\r\n") +
                                         interleavedH264Packet(idr_payload, sequence++, timestamp));
                    frame_timer.start(20);
                }
            }
        });
    });
    QObject::connect(&frame_timer, &QTimer::timeout, &app, [&]() {
        timestamp += 3000;
        stream_socket->write(interleavedH264Packet(idr_payload, sequence++, timestamp));
    });
    QObject::connect(worker.stream(), &logistics::control_center::RtspH264Stream::streamReady, &app, [&]() {
        stream_ready = true;
        QPlaybackOptions options;
        options.setPlaybackIntent(QPlaybackOptions::PlaybackIntent::LowLatencyStreaming);
        options.setProbeSize(2048);
        player.setPlaybackOptions(options);
        player.setSourceDevice(worker.stream(), QUrl(QStringLiteral("active-stream.h264")));
        player.play();
        player_attached = player.sourceDevice() == worker.stream();
        if (cleanup_started) {
            return;
        }
        cleanup_started = true;
        QTimer::singleShot(200, &app, [&]() {
            frame_timer.stop();
            player.stop();
            player.setSource({});
            worker.stop();
            exit_code = 0;
            app.quit();
        });
    });
    QTimer::singleShot(3000, &app, [&]() { app.quit(); });

    worker.start(QUrl(QStringLiteral("rtsp://127.0.0.1:%1/stream").arg(server.serverPort())));
    app.exec();

    if (exit_code != 0) {
        if (request_count != 3) {
            return 41;
        }
        if (!stream_ready) {
            return 42;
        }
        if (!player_attached) {
            return 43;
        }
        return exit_code;
    }
    if (request_count != 3) {
        return 5;
    }
    if (!stream_ready) {
        return 6;
    }
    return player_attached ? 0 : 7;
}
