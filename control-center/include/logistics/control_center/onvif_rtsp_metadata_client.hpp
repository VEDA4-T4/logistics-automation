#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QUrl>

class QTcpSocket;
class QTimer;

namespace logistics::control_center {

class OnvifRtspMetadataClient final : public QObject {
    Q_OBJECT

public:
    explicit OnvifRtspMetadataClient(QObject* parent = nullptr);

    void setReconnectInterval(int interval_ms);
    void start(const QUrl& stream_url);
    void stop();

signals:
    void metadataReceived(const QByteArray& xml);
    void connectionStateChanged(bool connected, const QString& detail);
    void diagnosticMessage(const QString& message);

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
    void scheduleReconnect(const QString& detail);
    void processIncomingData();
    void processRtspResponse(const QByteArray& header, const QByteArray& body);
    void processInterleavedPacket(quint8 channel, const QByteArray& packet);
    void sendRequest(const QByteArray& method, const QUrl& url,
                     const QList<QPair<QByteArray, QByteArray>>& headers = {}, bool authentication_retried = false);
    void sendDescribe();
    void sendSetup(const QUrl& track_url);
    void sendPlay();
    void sendKeepAlive();
    [[nodiscard]] bool parseAuthenticationChallenge(const QByteArray& challenge);
    [[nodiscard]] QByteArray authorizationHeader(const QByteArray& method, const QUrl& url);
    [[nodiscard]] bool selectMetadataTrack(const QByteArray& sdp, const QUrl& content_base);
    [[nodiscard]] QUrl requestUrlWithoutCredentials(const QUrl& url) const;
    void resetSession();

    QTcpSocket* socket_{ nullptr };
    QTimer* reconnect_timer_{ nullptr };
    QTimer* keep_alive_timer_{ nullptr };
    QTimer* response_timeout_timer_{ nullptr };
    QUrl stream_url_;
    QUrl request_url_;
    QUrl metadata_track_url_;
    QByteArray receive_buffer_;
    QByteArray metadata_buffer_;
    Request pending_request_;
    QByteArray session_id_;
    QString username_;
    QString password_;
    QString last_state_detail_;
    AuthenticationType authentication_type_{ AuthenticationType::None };
    QHash<QByteArray, QByteArray> authentication_parameters_;
    quint32 nonce_count_{ 0 };
    int metadata_rtp_channel_{ 0 };
    int reconnect_interval_ms_{ 3000 };
    int last_sequence_{ -1 };
    bool stopped_{ true };
    bool streaming_{ false };
};

}  // namespace logistics::control_center
