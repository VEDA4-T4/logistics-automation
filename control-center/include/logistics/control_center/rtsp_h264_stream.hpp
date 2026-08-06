#pragma once

#include <QByteArray>
#include <QHash>
#include <QIODevice>
#include <QList>
#include <QMutex>
#include <QPair>
#include <QUrl>
#include <QWaitCondition>
#include <atomic>

class QTcpSocket;
class QTimer;

namespace logistics::control_center {

class RtspH264Stream final : public QIODevice {
    Q_OBJECT

public:
    explicit RtspH264Stream(QObject* parent = nullptr);

    void setNetworkTimeout(int timeout_ms);
    void setMaximumBufferSize(qsizetype size_bytes);
    void start(const QUrl& stream_url);
    void stop();

    [[nodiscard]] bool isSequential() const override;
    [[nodiscard]] bool atEnd() const override;
    [[nodiscard]] qint64 bytesAvailable() const override;

signals:
    void connectionStateChanged(bool connected, const QString& detail);
    void streamReady();
    void streamError(const QString& detail);
    void packetLossDetected(quint16 expected_sequence, quint16 received_sequence);
    void diagnosticMessage(const QString& message);

protected:
    qint64 readData(char* data, qint64 max_size) override;
    qint64 writeData(const char* data, qint64 max_size) override;

private:
    struct Request {
        QByteArray method;
        QUrl url;
        QList<QPair<QByteArray, QByteArray>> headers;
        bool authentication_retried{ false };
    };

    enum class AuthenticationType {
        None,
        Basic,
        Digest,
    };

    void connectToCamera();
    void fail(const QString& detail);
    void processIncomingData();
    void processRtspResponse(const QByteArray& header, const QByteArray& body);
    void processInterleavedPacket(quint8 channel, const QByteArray& packet);
    void processH264Payload(const QByteArray& payload, bool marker);
    void appendNalUnit(QByteArrayView nal_unit);
    void finishAccessUnit();
    void appendToReadBuffer(const QByteArray& access_unit, bool contains_idr);
    void sendRequest(const QByteArray& method, const QUrl& url,
                     const QList<QPair<QByteArray, QByteArray>>& headers = {}, bool authentication_retried = false);
    void sendDescribe();
    void sendSetup();
    void sendPlay();
    void sendKeepAlive();
    [[nodiscard]] bool parseAuthenticationChallenge(const QByteArray& challenge);
    [[nodiscard]] QByteArray authorizationHeader(const QByteArray& method, const QUrl& url);
    [[nodiscard]] bool selectH264Track(const QByteArray& sdp, const QUrl& content_base);
    [[nodiscard]] QUrl requestUrlWithoutCredentials(const QUrl& url) const;
    void resetSession();

    QTcpSocket* socket_{ nullptr };
    QTimer* keep_alive_timer_{ nullptr };
    QTimer* network_timeout_timer_{ nullptr };
    QUrl stream_url_;
    QUrl request_url_;
    QUrl video_track_url_;
    QByteArray receive_buffer_;
    QByteArray read_buffer_;
    QByteArray access_unit_;
    QByteArray fragmented_nal_;
    QByteArray parameter_sets_;
    QByteArray sps_;
    QByteArray pps_;
    Request pending_request_;
    QByteArray session_id_;
    QString username_;
    QString password_;
    AuthenticationType authentication_type_{ AuthenticationType::None };
    QHash<QByteArray, QByteArray> authentication_parameters_;
    mutable QMutex read_buffer_mutex_;
    QWaitCondition read_buffer_ready_;
    quint32 nonce_count_{ 0 };
    quint32 cseq_{ 0 };
    quint32 current_timestamp_{ 0 };
    int video_rtp_channel_{ 0 };
    int last_sequence_{ -1 };
    int network_timeout_ms_{ 3000 };
    qsizetype maximum_buffer_size_{ 2 * 1024 * 1024 };
    std::atomic_bool stopped_{ true };
    bool streaming_{ false };
    bool awaiting_response_{ false };
    bool failure_reported_{ false };
    bool ready_emitted_{ false };
    bool current_contains_idr_{ false };
    bool drop_until_idr_{ true };
};

}  // namespace logistics::control_center
