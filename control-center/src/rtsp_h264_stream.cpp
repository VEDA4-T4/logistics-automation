#include "logistics/control_center/rtsp_h264_stream.hpp"

#include <QCryptographicHash>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cstring>

namespace logistics::control_center {
namespace {

constexpr char kAnnexBStartCode[] = "\0\0\0\1";

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

QUrl resolveControlUrl(const QUrl& content_base, const QString& control) {
    if (control == QStringLiteral("*")) {
        return content_base;
    }

    const QUrl control_url(control);
    if (!control_url.isRelative()) {
        return control_url;
    }

    auto resolution_base = content_base;
    auto path = resolution_base.path();
    if (!path.endsWith(QLatin1Char('/'))) {
        path.append(QLatin1Char('/'));
        resolution_base.setPath(path);
    }
    return resolution_base.resolved(control_url);
}

}  // namespace

RtspH264Stream::RtspH264Stream(QObject* parent)
    : QIODevice(parent),
      socket_(new QTcpSocket(this)),
      keep_alive_timer_(new QTimer(this)),
      network_timeout_timer_(new QTimer(this)) {
    keep_alive_timer_->setInterval(20000);
    network_timeout_timer_->setSingleShot(true);

    connect(keep_alive_timer_, &QTimer::timeout, this, &RtspH264Stream::sendKeepAlive);
    connect(network_timeout_timer_, &QTimer::timeout, this,
            [this]() { fail(QStringLiteral("RTSP/TCP 영상 수신 시간이 초과되었습니다.")); });
    connect(socket_, &QTcpSocket::connected, this, [this]() {
        emit diagnosticMessage(QStringLiteral("TCP 연결 성공 · DESCRIBE 요청"));
        network_timeout_timer_->start(network_timeout_ms_);
        sendDescribe();
    });
    connect(socket_, &QTcpSocket::readyRead, this, [this]() {
        network_timeout_timer_->start(network_timeout_ms_);
        receive_buffer_.append(socket_->readAll());
        processIncomingData();
    });
    connect(socket_, &QTcpSocket::disconnected, this, [this]() {
        if (!stopped_) {
            fail(QStringLiteral("RTSP/TCP 영상 연결이 종료되었습니다."));
        }
    });
    connect(socket_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (!stopped_) {
            fail(QStringLiteral("RTSP/TCP 영상 연결 오류: %1").arg(socket_->errorString()));
        }
    });
}

void RtspH264Stream::setNetworkTimeout(int timeout_ms) {
    network_timeout_ms_ = std::clamp(timeout_ms, 100, 60000);
}

void RtspH264Stream::setMaximumBufferSize(qsizetype size_bytes) {
    maximum_buffer_size_ = std::clamp<qsizetype>(size_bytes, 64 * 1024, 16 * 1024 * 1024);
}

void RtspH264Stream::start(const QUrl& stream_url) {
    stop();
    stream_url_ = stream_url;
    username_ = stream_url.userName(QUrl::FullyDecoded);
    password_ = stream_url.password(QUrl::FullyDecoded);
    request_url_ = requestUrlWithoutCredentials(stream_url);
    if (request_url_.scheme().compare(QStringLiteral("rtsp"), Qt::CaseInsensitive) != 0) {
        emit streamError(QStringLiteral("직접 영상 수신기는 RTSP/TCP 주소만 지원합니다."));
        return;
    }

    stopped_ = false;
    failure_reported_ = false;
    open(QIODevice::ReadOnly);
    emit connectionStateChanged(false, QStringLiteral("RTSP/TCP 연결 중"));
    connectToCamera();
}

void RtspH264Stream::stop() {
    stopped_ = true;
    keep_alive_timer_->stop();
    network_timeout_timer_->stop();
    socket_->abort();
    resetSession();
    {
        const QMutexLocker lock(&read_buffer_mutex_);
        read_buffer_.clear();
        read_buffer_ready_.wakeAll();
    }
    close();
}

bool RtspH264Stream::isSequential() const {
    return true;
}

bool RtspH264Stream::atEnd() const {
    const QMutexLocker lock(&read_buffer_mutex_);
    return stopped_ && read_buffer_.isEmpty();
}

qint64 RtspH264Stream::bytesAvailable() const {
    const QMutexLocker lock(&read_buffer_mutex_);
    return static_cast<qint64>(read_buffer_.size()) + QIODevice::bytesAvailable();
}

qint64 RtspH264Stream::readData(char* data, qint64 max_size) {
    if (max_size <= 0) {
        return 0;
    }
    const QMutexLocker lock(&read_buffer_mutex_);
    while (read_buffer_.isEmpty() && !stopped_) {
        read_buffer_ready_.wait(&read_buffer_mutex_);
    }
    const auto size = std::min<qint64>(max_size, read_buffer_.size());
    if (size <= 0) {
        return 0;
    }
    std::memcpy(data, read_buffer_.constData(), static_cast<std::size_t>(size));
    read_buffer_.remove(0, size);
    return size;
}

qint64 RtspH264Stream::writeData(const char*, qint64) {
    return -1;
}

void RtspH264Stream::connectToCamera() {
    resetSession();
    socket_->connectToHost(request_url_.host(), static_cast<quint16>(request_url_.port(554)));
    network_timeout_timer_->start(network_timeout_ms_);
}

void RtspH264Stream::fail(const QString& detail) {
    if (stopped_ || failure_reported_) {
        return;
    }
    failure_reported_ = true;
    stopped_ = true;
    keep_alive_timer_->stop();
    network_timeout_timer_->stop();
    streaming_ = false;
    socket_->abort();
    {
        const QMutexLocker lock(&read_buffer_mutex_);
        read_buffer_ready_.wakeAll();
    }
    emit connectionStateChanged(false, detail);
    emit streamError(detail);
}

void RtspH264Stream::processIncomingData() {
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

void RtspH264Stream::processRtspResponse(const QByteArray& header, const QByteArray& body) {
    auto lines = header.split('\n');
    for (auto& line : lines) {
        line = line.trimmed();
    }
    if (lines.isEmpty()) {
        fail(QStringLiteral("RTSP 응답이 비어 있습니다."));
        return;
    }

    const auto status = responseStatusCode(lines.front());
    const auto headers = parseHeaders(lines);
    emit diagnosticMessage(
        QStringLiteral("%1 응답 · RTSP %2").arg(QString::fromLatin1(pending_request_.method)).arg(status));
    if (status == 401) {
        if (pending_request_.authentication_retried ||
            !parseAuthenticationChallenge(headers.value("www-authenticate"))) {
            fail(QStringLiteral("RTSP 영상 인증에 실패했습니다."));
            return;
        }
        const auto retry = pending_request_;
        sendRequest(retry.method, retry.url, retry.headers, true);
        return;
    }
    if (status < 200 || status >= 300) {
        fail(
            QStringLiteral("RTSP %1 요청 실패: RTSP %2").arg(QString::fromLatin1(pending_request_.method)).arg(status));
        return;
    }

    if (pending_request_.method == "DESCRIBE") {
        QUrl content_base(QString::fromUtf8(headers.value("content-base")));
        if (!content_base.isValid() || content_base.isEmpty()) {
            content_base = request_url_;
        }
        if (!selectH264Track(body, content_base)) {
            fail(QStringLiteral("RTSP 프로파일에 H.264 영상 트랙이 없습니다."));
            return;
        }
        sendSetup();
    } else if (pending_request_.method == "SETUP") {
        session_id_ = headers.value("session").split(';').front().trimmed();
        if (session_id_.isEmpty()) {
            fail(QStringLiteral("RTSP SETUP 응답에 Session이 없습니다."));
            return;
        }
        const auto transport = headers.value("transport");
        if (!transport.toLower().contains("rtp/avp/tcp")) {
            fail(QStringLiteral("카메라가 요청한 RTSP/TCP 전송 방식을 수락하지 않았습니다."));
            return;
        }
        const QRegularExpression interleaved_expression(QStringLiteral("interleaved=(\\d+)-\\d+"),
                                                        QRegularExpression::CaseInsensitiveOption);
        const auto match = interleaved_expression.match(QString::fromLatin1(headers.value("transport")));
        if (match.hasMatch()) {
            video_rtp_channel_ = match.captured(1).toInt();
        }
        sendPlay();
    } else if (pending_request_.method == "PLAY") {
        streaming_ = true;
        failure_reported_ = false;
        keep_alive_timer_->start();
        network_timeout_timer_->start(network_timeout_ms_);
        emit connectionStateChanged(true, QStringLiteral("RTSP/TCP 영상 수신 중"));
    }
}

void RtspH264Stream::processInterleavedPacket(quint8 channel, const QByteArray& packet) {
    if (channel != video_rtp_channel_ || packet.size() < 12) {
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
    const auto sequence = static_cast<quint16>((bytes[2] << 8U) | bytes[3]);
    const auto timestamp = (static_cast<quint32>(bytes[4]) << 24U) | (static_cast<quint32>(bytes[5]) << 16U) |
                           (static_cast<quint32>(bytes[6]) << 8U) | static_cast<quint32>(bytes[7]);

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
            return;
        }
        payload_size -= padding_size;
    }

    if (last_sequence_ >= 0) {
        const auto expected = static_cast<quint16>((last_sequence_ + 1) & 0xFFFF);
        if (sequence != expected) {
            access_unit_.clear();
            fragmented_nal_.clear();
            current_contains_idr_ = false;
            drop_until_idr_ = true;
            emit packetLossDetected(expected, sequence);
        }
    }
    last_sequence_ = sequence;

    if ((!access_unit_.isEmpty() || !fragmented_nal_.isEmpty()) && timestamp != current_timestamp_) {
        access_unit_.clear();
        fragmented_nal_.clear();
        current_contains_idr_ = false;
        drop_until_idr_ = true;
    }
    current_timestamp_ = timestamp;
    if (ready_emitted_) {
        network_timeout_timer_->start(network_timeout_ms_);
    }
    processH264Payload(QByteArray(packet.constData() + offset, payload_size), marker);
}

void RtspH264Stream::processH264Payload(const QByteArray& payload, bool marker) {
    if (payload.isEmpty()) {
        return;
    }

    const auto nal_type = static_cast<quint8>(payload.front()) & 0x1FU;
    if (nal_type >= 1 && nal_type <= 23) {
        appendNalUnit(payload);
    } else if (nal_type == 24) {
        qsizetype offset = 1;
        while (offset + 2 <= payload.size()) {
            const auto size = (static_cast<quint8>(payload[offset]) << 8U) | static_cast<quint8>(payload[offset + 1]);
            offset += 2;
            if (size == 0 || offset + size > payload.size()) {
                access_unit_.clear();
                current_contains_idr_ = false;
                drop_until_idr_ = true;
                break;
            }
            appendNalUnit(QByteArrayView(payload).sliced(offset, size));
            offset += size;
        }
    } else if (nal_type == 28 && payload.size() >= 2) {
        const auto fu_indicator = static_cast<quint8>(payload[0]);
        const auto fu_header = static_cast<quint8>(payload[1]);
        const bool start = (fu_header & 0x80U) != 0U;
        const bool end = (fu_header & 0x40U) != 0U;
        if (start) {
            fragmented_nal_.clear();
            fragmented_nal_.append(static_cast<char>((fu_indicator & 0xE0U) | (fu_header & 0x1FU)));
            fragmented_nal_.append(payload.constData() + 2, payload.size() - 2);
        } else if (!fragmented_nal_.isEmpty()) {
            fragmented_nal_.append(payload.constData() + 2, payload.size() - 2);
        }
        if (end && !fragmented_nal_.isEmpty()) {
            appendNalUnit(fragmented_nal_);
            fragmented_nal_.clear();
        }
    }

    if (marker) {
        if (!fragmented_nal_.isEmpty()) {
            fragmented_nal_.clear();
            drop_until_idr_ = true;
        }
        finishAccessUnit();
    }
}

void RtspH264Stream::appendNalUnit(QByteArrayView nal_unit) {
    if (nal_unit.isEmpty()) {
        return;
    }
    const auto nal_type = static_cast<quint8>(nal_unit.front()) & 0x1FU;
    if (nal_type == 5) {
        current_contains_idr_ = true;
    }
    if (nal_type == 7) {
        sps_.clear();
        sps_.append(kAnnexBStartCode, 4);
        sps_.append(nal_unit.data(), nal_unit.size());
        parameter_sets_ = sps_ + pps_;
    } else if (nal_type == 8) {
        pps_.clear();
        pps_.append(kAnnexBStartCode, 4);
        pps_.append(nal_unit.data(), nal_unit.size());
        parameter_sets_ = sps_ + pps_;
    }
    access_unit_.append(kAnnexBStartCode, 4);
    access_unit_.append(nal_unit.data(), nal_unit.size());
}

void RtspH264Stream::finishAccessUnit() {
    if (!access_unit_.isEmpty()) {
        appendToReadBuffer(access_unit_, current_contains_idr_);
    }
    access_unit_.clear();
    current_contains_idr_ = false;
}

void RtspH264Stream::appendToReadBuffer(const QByteArray& access_unit, bool contains_idr) {
    bool data_appended = false;
    bool start_decoder = false;
    {
        const QMutexLocker lock(&read_buffer_mutex_);
        if (read_buffer_.size() + access_unit.size() > maximum_buffer_size_) {
            read_buffer_.clear();
            drop_until_idr_ = true;
        }
        if (drop_until_idr_ && !contains_idr) {
            return;
        }
        if (drop_until_idr_) {
            read_buffer_.append(parameter_sets_);
            drop_until_idr_ = false;
        }
        read_buffer_.append(access_unit);
        read_buffer_ready_.wakeAll();
        data_appended = true;
        if (!ready_emitted_) {
            ready_emitted_ = true;
            start_decoder = true;
        }
    }
    if (start_decoder) {
        emit streamReady();
    }
    if (data_appended) {
        emit readyRead();
    }
}

void RtspH264Stream::sendRequest(const QByteArray& method, const QUrl& url,
                                 const QList<QPair<QByteArray, QByteArray>>& headers, bool authentication_retried) {
    if (socket_->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    pending_request_ = { method, url, headers, authentication_retried };
    QByteArray request;
    request += method + ' ' + url.toEncoded() + " RTSP/1.0\r\n";
    request += "CSeq: " + QByteArray::number(++cseq_) + "\r\n";
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

void RtspH264Stream::sendDescribe() {
    sendRequest("DESCRIBE", request_url_, { { "Accept", "application/sdp" } });
}

void RtspH264Stream::sendSetup() {
    sendRequest("SETUP", video_track_url_, { { "Transport", "RTP/AVP/TCP;unicast;interleaved=0-1" } });
}

void RtspH264Stream::sendPlay() {
    sendRequest("PLAY", request_url_, { { "Range", "npt=now-" } });
}

void RtspH264Stream::sendKeepAlive() {
    if (streaming_) {
        sendRequest("OPTIONS", request_url_);
    }
}

bool RtspH264Stream::parseAuthenticationChallenge(const QByteArray& challenge) {
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

QByteArray RtspH264Stream::authorizationHeader(const QByteArray& method, const QUrl& url) {
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

bool RtspH264Stream::selectH264Track(const QByteArray& sdp, const QUrl& content_base) {
    parameter_sets_.clear();
    sps_.clear();
    pps_.clear();
    const auto normalized = QString::fromUtf8(sdp).replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    const auto sections = normalized.split(
        QRegularExpression(QStringLiteral("(?=^m=)"), QRegularExpression::MultilineOption), Qt::SkipEmptyParts);
    for (const auto& section : sections) {
        if (!section.startsWith(QStringLiteral("m=video")) ||
            !section.contains(
                QRegularExpression(QStringLiteral("^a=rtpmap:\\d+ H264/90000"),
                                   QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption))) {
            continue;
        }

        const QRegularExpression control_expression(QStringLiteral("^a=control:(.+)$"),
                                                    QRegularExpression::MultilineOption);
        const auto control_match = control_expression.match(section);
        if (!control_match.hasMatch()) {
            continue;
        }
        video_track_url_ = resolveControlUrl(content_base, control_match.captured(1).trimmed());

        const QRegularExpression parameter_expression(QStringLiteral("sprop-parameter-sets=([^;\\r\\n]+)"),
                                                      QRegularExpression::CaseInsensitiveOption);
        const auto parameter_match = parameter_expression.match(section);
        if (parameter_match.hasMatch()) {
            const auto encoded_sets = parameter_match.captured(1).split(QLatin1Char(','));
            for (const auto& encoded : encoded_sets) {
                const auto decoded = QByteArray::fromBase64(encoded.trimmed().toLatin1());
                if (!decoded.isEmpty()) {
                    QByteArray annex_b_parameter;
                    annex_b_parameter.append(kAnnexBStartCode, 4);
                    annex_b_parameter.append(decoded);
                    const auto nal_type = static_cast<quint8>(decoded.front()) & 0x1FU;
                    if (nal_type == 7) {
                        sps_ = annex_b_parameter;
                    } else if (nal_type == 8) {
                        pps_ = annex_b_parameter;
                    }
                }
            }
            parameter_sets_ = sps_ + pps_;
        }
        return video_track_url_.isValid();
    }
    return false;
}

QUrl RtspH264Stream::requestUrlWithoutCredentials(const QUrl& url) const {
    auto result = url;
    result.setUserName({});
    result.setPassword({});
    return result;
}

void RtspH264Stream::resetSession() {
    keep_alive_timer_->stop();
    network_timeout_timer_->stop();
    receive_buffer_.clear();
    access_unit_.clear();
    fragmented_nal_.clear();
    parameter_sets_.clear();
    sps_.clear();
    pps_.clear();
    pending_request_ = {};
    session_id_.clear();
    video_track_url_.clear();
    authentication_type_ = AuthenticationType::None;
    authentication_parameters_.clear();
    nonce_count_ = 0;
    cseq_ = 0;
    current_timestamp_ = 0;
    video_rtp_channel_ = 0;
    last_sequence_ = -1;
    streaming_ = false;
    current_contains_idr_ = false;
    ready_emitted_ = false;
    drop_until_idr_ = true;
}

}  // namespace logistics::control_center
