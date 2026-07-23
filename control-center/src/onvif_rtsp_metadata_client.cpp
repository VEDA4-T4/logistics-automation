#include "logistics/control_center/onvif_rtsp_metadata_client.hpp"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <algorithm>

namespace logistics::control_center {
namespace {

constexpr qsizetype kMaximumMetadataDocumentSize = 4 * 1024 * 1024;

QByteArray hashHex(QByteArrayView value, QCryptographicHash::Algorithm algorithm) {
    return QCryptographicHash::hash(value, algorithm).toHex();
}

QHash<QByteArray, QByteArray> parseHeaders(const QList<QByteArray>& lines) {
    QHash<QByteArray, QByteArray> headers;
    for (qsizetype index = 1; index < lines.size(); ++index) {
        const auto separator = lines[index].indexOf(':');
        if (separator <= 0) {
            continue;
        }
        headers.insert(lines[index].left(separator).trimmed().toLower(), lines[index].mid(separator + 1).trimmed());
    }
    return headers;
}

int responseStatusCode(const QByteArray& status_line) {
    const auto parts = status_line.split(' ');
    if (parts.size() < 2) {
        return 0;
    }
    bool valid = false;
    const auto status = parts[1].toInt(&valid);
    return valid ? status : 0;
}

}  // namespace

OnvifRtspMetadataClient::OnvifRtspMetadataClient(QObject* parent)
    : QObject(parent),
      socket_(new QTcpSocket(this)),
      reconnect_timer_(new QTimer(this)),
      keep_alive_timer_(new QTimer(this)) {
    reconnect_timer_->setSingleShot(true);
    keep_alive_timer_->setInterval(20000);

    connect(reconnect_timer_, &QTimer::timeout, this, &OnvifRtspMetadataClient::connectToCamera);
    connect(keep_alive_timer_, &QTimer::timeout, this, &OnvifRtspMetadataClient::sendKeepAlive);
    connect(socket_, &QTcpSocket::connected, this, [this]() {
        emit diagnosticMessage(QStringLiteral("TCP 연결 성공 · DESCRIBE 요청"));
        sendDescribe();
    });
    connect(socket_, &QTcpSocket::readyRead, this, [this]() {
        receive_buffer_.append(socket_->readAll());
        processIncomingData();
    });
    connect(socket_, &QTcpSocket::disconnected, this, [this]() {
        if (!stopped_) {
            scheduleReconnect(QStringLiteral("ONVIF 메타데이터 연결이 종료되었습니다."));
        }
    });
    connect(socket_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (!stopped_) {
            scheduleReconnect(QStringLiteral("ONVIF 메타데이터 연결 오류: %1").arg(socket_->errorString()));
        }
    });
}

void OnvifRtspMetadataClient::setReconnectInterval(int interval_ms) {
    reconnect_interval_ms_ = std::max(interval_ms, 100);
}

void OnvifRtspMetadataClient::start(const QUrl& stream_url) {
    stop();
    stream_url_ = stream_url;
    username_ = stream_url.userName(QUrl::FullyDecoded);
    password_ = stream_url.password(QUrl::FullyDecoded);
    request_url_ = requestUrlWithoutCredentials(stream_url);
    if (request_url_.scheme().compare(QStringLiteral("rtsp"), Qt::CaseInsensitive) != 0) {
        last_state_detail_ = QStringLiteral("ONVIF 메타데이터 수신기는 현재 RTSP/TCP만 지원합니다.");
        emit connectionStateChanged(false, last_state_detail_);
        return;
    }
    stopped_ = false;
    last_state_detail_.clear();
    connectToCamera();
}

void OnvifRtspMetadataClient::stop() {
    stopped_ = true;
    reconnect_timer_->stop();
    keep_alive_timer_->stop();
    socket_->abort();
    resetSession();
}

void OnvifRtspMetadataClient::connectToCamera() {
    if (stopped_ || !request_url_.isValid()) {
        return;
    }
    resetSession();
    const auto port = request_url_.port(554);
    socket_->connectToHost(request_url_.host(), static_cast<quint16>(port));
    const auto detail = QStringLiteral("ONVIF 메타데이터 연결 중");
    if (detail != last_state_detail_) {
        last_state_detail_ = detail;
        emit connectionStateChanged(false, detail);
    }
}

void OnvifRtspMetadataClient::scheduleReconnect(const QString& detail) {
    if (stopped_ || reconnect_timer_->isActive()) {
        return;
    }
    keep_alive_timer_->stop();
    streaming_ = false;
    resetSession();
    if (detail != last_state_detail_) {
        last_state_detail_ = detail;
        emit connectionStateChanged(false, detail);
    }
    reconnect_timer_->start(reconnect_interval_ms_);
    socket_->abort();
}

void OnvifRtspMetadataClient::processIncomingData() {
    while (!receive_buffer_.isEmpty()) {
        if (receive_buffer_.startsWith("\r\n")) {
            receive_buffer_.remove(0, 2);
            continue;
        }

        if (static_cast<quint8>(receive_buffer_.front()) == '$') {
            if (receive_buffer_.size() < 4) {
                return;
            }
            const auto channel = static_cast<quint8>(receive_buffer_[1]);
            const auto packet_size =
                (static_cast<quint8>(receive_buffer_[2]) << 8U) | static_cast<quint8>(receive_buffer_[3]);
            if (receive_buffer_.size() < 4 + packet_size) {
                return;
            }
            const auto packet = receive_buffer_.mid(4, packet_size);
            receive_buffer_.remove(0, 4 + packet_size);
            processInterleavedPacket(channel, packet);
            continue;
        }

        const auto header_end = receive_buffer_.indexOf("\r\n\r\n");
        if (header_end < 0) {
            return;
        }
        const auto header = receive_buffer_.left(header_end);
        const auto lines = header.split('\n');
        const auto headers = parseHeaders(lines);
        bool content_length_valid = false;
        const auto content_length = headers.value("content-length").toInt(&content_length_valid);
        const auto body_size = content_length_valid ? content_length : 0;
        if (receive_buffer_.size() < header_end + 4 + body_size) {
            return;
        }
        const auto body = receive_buffer_.mid(header_end + 4, body_size);
        receive_buffer_.remove(0, header_end + 4 + body_size);
        processRtspResponse(header, body);
    }
}

void OnvifRtspMetadataClient::processRtspResponse(const QByteArray& header, const QByteArray& body) {
    auto lines = header.split('\n');
    for (auto& line : lines) {
        line = line.trimmed();
    }
    if (lines.isEmpty()) {
        scheduleReconnect(QStringLiteral("ONVIF RTSP 응답이 비어 있습니다."));
        return;
    }

    const auto status = responseStatusCode(lines.front());
    const auto headers = parseHeaders(lines);
    emit diagnosticMessage(
        QStringLiteral("%1 응답 · RTSP %2").arg(QString::fromLatin1(pending_request_.method)).arg(status));
    if (status == 401) {
        emit diagnosticMessage(QStringLiteral("인증 요청 수신 · Authorization 재시도"));
        if (pending_request_.authentication_retried ||
            !parseAuthenticationChallenge(headers.value("www-authenticate"))) {
            scheduleReconnect(QStringLiteral("ONVIF RTSP 인증에 실패했습니다."));
            return;
        }
        const auto retry = pending_request_;
        sendRequest(retry.method, retry.url, retry.headers, true);
        return;
    }
    if (status < 200 || status >= 300) {
        scheduleReconnect(QStringLiteral("ONVIF RTSP %1 요청 실패: RTSP %2")
                              .arg(QString::fromLatin1(pending_request_.method))
                              .arg(status));
        return;
    }

    if (pending_request_.method == "DESCRIBE") {
        emit diagnosticMessage(
            QStringLiteral("SDP 수신 (%1 bytes)\n%2").arg(body.size()).arg(QString::fromUtf8(body).trimmed()));
        QUrl content_base(QString::fromUtf8(headers.value("content-base")));
        if (!content_base.isValid() || content_base.isEmpty()) {
            content_base = request_url_;
        }
        if (!selectMetadataTrack(body, content_base)) {
            scheduleReconnect(QStringLiteral("RTSP 프로파일에 비압축 ONVIF 메타데이터 트랙이 없습니다."));
            return;
        }
        emit diagnosticMessage(QStringLiteral("ONVIF 메타데이터 트랙 선택 · SETUP 요청"));
        sendSetup(metadata_track_url_);
    } else if (pending_request_.method == "SETUP") {
        session_id_ = headers.value("session").split(';').front().trimmed();
        if (session_id_.isEmpty()) {
            scheduleReconnect(QStringLiteral("ONVIF RTSP SETUP 응답에 Session이 없습니다."));
            return;
        }
        const QRegularExpression interleaved_expression(QStringLiteral("interleaved=(\\d+)-\\d+"),
                                                        QRegularExpression::CaseInsensitiveOption);
        const auto match = interleaved_expression.match(QString::fromLatin1(headers.value("transport")));
        if (match.hasMatch()) {
            metadata_rtp_channel_ = match.captured(1).toInt();
        }
        emit diagnosticMessage(QStringLiteral("메타데이터 RTP 채널 %1 설정 · PLAY 요청").arg(metadata_rtp_channel_));
        sendPlay();
    } else if (pending_request_.method == "PLAY") {
        streaming_ = true;
        last_state_detail_ = QStringLiteral("ONVIF 메타데이터 수신 중");
        emit connectionStateChanged(true, last_state_detail_);
        keep_alive_timer_->start();
    }
}

void OnvifRtspMetadataClient::processInterleavedPacket(quint8 channel, const QByteArray& packet) {
    if (channel != metadata_rtp_channel_ || packet.size() < 12) {
        return;
    }

    const auto* bytes = reinterpret_cast<const quint8*>(packet.constData());
    if ((bytes[0] >> 6U) != 2U) {
        return;
    }
    const auto csrc_count = bytes[0] & 0x0FU;
    const bool has_extension = (bytes[0] & 0x10U) != 0U;
    const bool has_padding = (bytes[0] & 0x20U) != 0U;
    const bool marker = (bytes[1] & 0x80U) != 0U;
    const auto sequence = static_cast<int>((bytes[2] << 8U) | bytes[3]);
    qsizetype offset = 12 + 4 * csrc_count;
    if (packet.size() < offset) {
        return;
    }
    if (has_extension) {
        if (packet.size() < offset + 4) {
            return;
        }
        const auto extension_words = static_cast<quint16>((bytes[offset + 2] << 8U) | bytes[offset + 3]);
        offset += 4 + static_cast<qsizetype>(extension_words) * 4;
    }
    auto payload_size = packet.size() - offset;
    if (payload_size <= 0) {
        return;
    }
    if (has_padding) {
        const auto padding_size = static_cast<quint8>(packet.back());
        if (padding_size == 0 || padding_size > payload_size) {
            metadata_buffer_.clear();
            return;
        }
        payload_size -= padding_size;
    }

    if (last_sequence_ >= 0 && sequence != ((last_sequence_ + 1) & 0xFFFF)) {
        metadata_buffer_.clear();
    }
    last_sequence_ = sequence;
    metadata_buffer_.append(packet.constData() + offset, payload_size);
    if (metadata_buffer_.size() > kMaximumMetadataDocumentSize) {
        metadata_buffer_.clear();
        return;
    }
    if (marker) {
        if (!metadata_buffer_.trimmed().isEmpty()) {
            emit metadataReceived(metadata_buffer_);
        }
        metadata_buffer_.clear();
    }
}

void OnvifRtspMetadataClient::sendRequest(const QByteArray& method, const QUrl& url,
                                          const QList<QPair<QByteArray, QByteArray>>& headers,
                                          bool authentication_retried) {
    if (socket_->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    pending_request_ = { method, url, headers, authentication_retried };
    static quint32 cseq = 0;
    QByteArray request;
    request += method + ' ' + url.toEncoded() + " RTSP/1.0\r\n";
    request += "CSeq: " + QByteArray::number(++cseq) + "\r\n";
    request += "User-Agent: Logistics-Control-Center/1.0\r\n";
    if (!session_id_.isEmpty() && method != "DESCRIBE" && method != "SETUP") {
        request += "Session: " + session_id_ + "\r\n";
    }
    const auto authorization = authorizationHeader(method, url);
    if (!authorization.isEmpty()) {
        request += "Authorization: " + authorization + "\r\n";
    }
    for (const auto& [name, value] : headers) {
        request += name + ": " + value + "\r\n";
    }
    request += "\r\n";
    socket_->write(request);
}

void OnvifRtspMetadataClient::sendDescribe() {
    sendRequest("DESCRIBE", request_url_, { { "Accept", "application/sdp" } });
}

void OnvifRtspMetadataClient::sendSetup(const QUrl& track_url) {
    sendRequest("SETUP", track_url, { { "Transport", "RTP/AVP/TCP;unicast;interleaved=0-1" } });
}

void OnvifRtspMetadataClient::sendPlay() {
    sendRequest("PLAY", request_url_, { { "Range", "npt=now-" } });
}

void OnvifRtspMetadataClient::sendKeepAlive() {
    if (streaming_) {
        sendRequest("OPTIONS", request_url_);
    }
}

bool OnvifRtspMetadataClient::parseAuthenticationChallenge(const QByteArray& challenge) {
    authentication_parameters_.clear();
    const auto trimmed = challenge.trimmed();
    if (trimmed.left(5).compare("Basic", Qt::CaseInsensitive) == 0) {
        authentication_type_ = AuthenticationType::Basic;
        return !username_.isEmpty();
    }
    if (trimmed.left(6).compare("Digest", Qt::CaseInsensitive) != 0) {
        return false;
    }

    authentication_type_ = AuthenticationType::Digest;
    const QRegularExpression parameter_expression(
        QStringLiteral(R"regex(([A-Za-z0-9_-]+)\s*=\s*(?:"([^"]*)"|([^,\s]+)))regex"));
    auto matches = parameter_expression.globalMatch(QString::fromUtf8(trimmed.mid(6)));
    while (matches.hasNext()) {
        const auto match = matches.next();
        const auto key = match.captured(1).toLatin1().toLower();
        const auto value = !match.captured(2).isNull() ? match.captured(2) : match.captured(3);
        authentication_parameters_.insert(key, value.toLatin1());
    }
    nonce_count_ = 0;
    return !username_.isEmpty() && authentication_parameters_.contains("realm") &&
           authentication_parameters_.contains("nonce");
}

QByteArray OnvifRtspMetadataClient::authorizationHeader(const QByteArray& method, const QUrl& url) {
    if (authentication_type_ == AuthenticationType::None) {
        return {};
    }
    if (authentication_type_ == AuthenticationType::Basic) {
        return "Basic " + (username_ + QLatin1Char(':') + password_).toUtf8().toBase64();
    }

    const auto realm = authentication_parameters_.value("realm");
    const auto nonce = authentication_parameters_.value("nonce");
    const auto opaque = authentication_parameters_.value("opaque");
    const auto algorithm_name = authentication_parameters_.value("algorithm", "MD5");
    const auto upper_algorithm = algorithm_name.toUpper();
    const auto algorithm = upper_algorithm.startsWith("SHA-256") ? QCryptographicHash::Sha256 : QCryptographicHash::Md5;
    const auto uri = url.toEncoded();
    const auto cnonce = QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-')).toLatin1();
    auto ha1 = hashHex(username_.toUtf8() + ':' + realm + ':' + password_.toUtf8(), algorithm);
    if (upper_algorithm.endsWith("-SESS")) {
        ha1 = hashHex(ha1 + ':' + nonce + ':' + cnonce, algorithm);
    }
    const auto ha2 = hashHex(method + ':' + uri, algorithm);

    auto qop = authentication_parameters_.value("qop");
    if (qop.contains(',')) {
        const auto options = qop.split(',');
        qop.clear();
        for (const auto& option : options) {
            if (option.trimmed() == "auth") {
                qop = "auth";
                break;
            }
        }
    }
    QByteArray response;
    QByteArray nc;
    if (qop == "auth") {
        nc = QByteArray::number(++nonce_count_, 16).rightJustified(8, '0');
        response = hashHex(ha1 + ':' + nonce + ':' + nc + ':' + cnonce + ":auth:" + ha2, algorithm);
    } else {
        qop.clear();
        response = hashHex(ha1 + ':' + nonce + ':' + ha2, algorithm);
    }

    QByteArray header = "Digest username=\"" + username_.toUtf8() + "\", realm=\"" + realm + "\", nonce=\"" + nonce +
                        "\", uri=\"" + uri + "\", response=\"" + response + "\", algorithm=" + algorithm_name;
    if (!opaque.isEmpty()) {
        header += ", opaque=\"" + opaque + '"';
    }
    if (!qop.isEmpty()) {
        header += ", qop=auth, nc=" + nc + ", cnonce=\"" + cnonce + '"';
    }
    return header;
}

bool OnvifRtspMetadataClient::selectMetadataTrack(const QByteArray& sdp, const QUrl& content_base) {
    const auto normalized = QString::fromUtf8(sdp).replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    const auto sections = normalized.split(
        QRegularExpression(QStringLiteral("(?=^m=)"), QRegularExpression::MultilineOption), Qt::SkipEmptyParts);
    for (const auto& section : sections) {
        if (!section.startsWith(QStringLiteral("m=application")) ||
            !section.contains(QStringLiteral("vnd.onvif.metadata"), Qt::CaseInsensitive)) {
            continue;
        }
        if (section.contains(QStringLiteral("vnd.onvif.metadata.gzip"), Qt::CaseInsensitive) ||
            section.contains(QStringLiteral("vnd.onvif.metadata.exi"), Qt::CaseInsensitive)) {
            continue;
        }
        const QRegularExpression control_expression(QStringLiteral("^a=control:(.+)$"),
                                                    QRegularExpression::MultilineOption);
        const auto match = control_expression.match(section);
        if (!match.hasMatch()) {
            continue;
        }
        const auto control = match.captured(1).trimmed();
        if (control == QStringLiteral("*")) {
            metadata_track_url_ = content_base;
        } else {
            const QUrl control_url(control);
            if (control_url.isRelative()) {
                auto resolution_base = content_base;
                auto path = resolution_base.path();
                if (!path.endsWith(QLatin1Char('/'))) {
                    path.append(QLatin1Char('/'));
                    resolution_base.setPath(path);
                }
                metadata_track_url_ = resolution_base.resolved(control_url);
            } else {
                metadata_track_url_ = control_url;
            }
        }
        return metadata_track_url_.isValid();
    }
    return false;
}

QUrl OnvifRtspMetadataClient::requestUrlWithoutCredentials(const QUrl& url) const {
    auto result = url;
    result.setUserName({});
    result.setPassword({});
    return result;
}

void OnvifRtspMetadataClient::resetSession() {
    keep_alive_timer_->stop();
    receive_buffer_.clear();
    metadata_buffer_.clear();
    pending_request_ = {};
    session_id_.clear();
    metadata_track_url_.clear();
    authentication_type_ = AuthenticationType::None;
    authentication_parameters_.clear();
    nonce_count_ = 0;
    metadata_rtp_channel_ = 0;
    last_sequence_ = -1;
    streaming_ = false;
}

}  // namespace logistics::control_center
