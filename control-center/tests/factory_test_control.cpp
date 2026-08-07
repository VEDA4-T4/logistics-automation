#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMqttClient>
#include <QMqttTopicName>
#include <QPushButton>
#include <QSettings>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QUuid>
#include <QVBoxLayout>
#include <cassert>

#include "logistics/contracts/mqtt_message.hpp"
#include "logistics/contracts/mqtt_topic.hpp"

namespace {

namespace mqtt = logistics::contracts::mqtt;

QString ConfigPath() {
    const auto configured = qEnvironmentVariable("LOGISTICS_CONTROL_CENTER_CONFIG");
    if (!configured.isEmpty()) {
        return QFileInfo(configured).absoluteFilePath();
    }
    for (const auto& candidate : {
             QCoreApplication::applicationDirPath() + QStringLiteral("/config/control-centor.ini"),
             QCoreApplication::applicationDirPath() + QStringLiteral("/../control-center/config/control-centor.ini"),
             QCoreApplication::applicationDirPath() + QStringLiteral("/../../control-center/config/control-centor.ini"),
             QDir::currentPath() + QStringLiteral("/control-center/config/control-centor.ini"),
         }) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return {};
}

QJsonObject Envelope(const QString& type, const QString& source, const QJsonObject& data) {
    return {
        { QString::fromLatin1(mqtt::kProtocolVersionField), QString::fromLatin1(mqtt::kCurrentProtocolVersion) },
        { QString::fromLatin1(mqtt::kMessageIdField), QUuid::createUuid().toString(QUuid::WithoutBraces) },
        { QString::fromLatin1(mqtt::kMessageTypeField), type },
        { QString::fromLatin1(mqtt::kSourceIdField), source },
        { QString::fromLatin1(mqtt::kTimestampField), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) },
        { QString::fromLatin1(mqtt::kDataField), data },
    };
}

QJsonObject Position(const QString& area, const QString& location) {
    return { { QStringLiteral("area"), area }, { QStringLiteral("location"), location } };
}

class FactoryMockControl final : public QWidget {
public:
    FactoryMockControl() : mqtt_(new QMqttClient(this)) {
        setWindowTitle(QStringLiteral("관제 MQTT 목업 제어"));
        resize(720, 760);

        const auto config_path = ConfigPath();
        QSettings settings(config_path, QSettings::IniFormat);

        auto* root = new QVBoxLayout(this);
        auto* connection_box = new QGroupBox(QStringLiteral("MQTT 연결"), this);
        auto* connection_form = new QFormLayout(connection_box);
        host_ = new QLineEdit(settings.value(QStringLiteral("mqtt/host"), QStringLiteral("127.0.0.1")).toString(),
                              connection_box);
        port_ = new QLineEdit(settings.value(QStringLiteral("mqtt/port"), 1883).toString(), connection_box);
        target_client_ =
            new QLineEdit(settings.value(QStringLiteral("mqtt/client_id"), QStringLiteral("control-center")).toString(),
                          connection_box);
        username_ = new QLineEdit(settings.value(QStringLiteral("mqtt/username")).toString(), connection_box);
        password_ = new QLineEdit(settings.value(QStringLiteral("mqtt/password")).toString(), connection_box);
        password_->setEchoMode(QLineEdit::Password);
        tls_ = new QCheckBox(QStringLiteral("TLS"), connection_box);
        tls_->setChecked(settings.value(QStringLiteral("mqtt/tls_enabled"), false).toBool());
        ca_certificate_ =
            new QLineEdit(settings.value(QStringLiteral("mqtt/ca_certificate")).toString(), connection_box);
        connection_form->addRow(QStringLiteral("Broker"), host_);
        connection_form->addRow(QStringLiteral("Port"), port_);
        connection_form->addRow(QStringLiteral("관제 client ID"), target_client_);
        connection_form->addRow(QStringLiteral("Username"), username_);
        connection_form->addRow(QStringLiteral("Password"), password_);
        connection_form->addRow(tls_, ca_certificate_);
        connect_button_ = new QPushButton(QStringLiteral("연결"), connection_box);
        connect_button_->setObjectName(QStringLiteral("mockConnectButton"));
        connection_status_ = new QLabel(config_path.isEmpty() ? QStringLiteral("설정 파일 없음 · 직접 입력 필요")
                                                              : QStringLiteral("설정: %1").arg(config_path),
                                        connection_box);
        connection_form->addRow(connect_button_, connection_status_);
        root->addWidget(connection_box);

        controls_ = new QGroupBox(QStringLiteral("목업 공정 메시지"), this);
        controls_->setEnabled(false);
        auto* controls_layout = new QVBoxLayout(controls_);
        auto* boundary = new QLabel(
            QStringLiteral("중앙 서버 출력 토픽을 직접 발행하여 실제 관제 수신 경로를 테스트합니다."), controls_);
        boundary->setWordWrap(true);
        controls_layout->addWidget(boundary);
        auto* work_row = new QHBoxLayout();
        work_id_ = new QLineEdit(NewWorkId(), controls_);
        auto* new_work_id = new QPushButton(QStringLiteral("새 UUID"), controls_);
        connect(new_work_id, &QPushButton::clicked, this, [this]() { work_id_->setText(NewWorkId()); });
        work_row->addWidget(new QLabel(QStringLiteral("작업 ID"), controls_));
        work_row->addWidget(work_id_, 1);
        work_row->addWidget(new_work_id);
        controls_layout->addLayout(work_row);

        auto* route_row = new QHBoxLayout();
        departure_ = RouteSelector(controls_);
        destination_ = RouteSelector(controls_);
        route_row->addWidget(new QLabel(QStringLiteral("라인 출발"), controls_));
        route_row->addWidget(departure_);
        route_row->addWidget(new QLabel(QStringLiteral("도착"), controls_));
        route_row->addWidget(destination_);
        route_row->addStretch();
        controls_layout->addLayout(route_row);

        image_url_ = new QLineEdit(controls_);
        image_url_->setPlaceholderText(QStringLiteral("선택: http://.../mock-product.jpg"));
        controls_layout->addWidget(image_url_);

        AddButton(controls_layout, QStringLiteral("1. 전체 장치 온라인/대기"), [this]() { SetAllStates("IDLE"); });
        AddButton(controls_layout, QStringLiteral("2. 작업 생성"), [this]() {
            PublishEvent(QStringLiteral("WORK_CREATED"), QStringLiteral("PI-INPUT-01"), WorkData());
        });
        AddButton(controls_layout, QStringLiteral("3. 투입 컨베이어 상자 이동"),
                  [this]() { PublishStatus(QStringLiteral("PI-INPUT-01"), QStringLiteral("BUSY"), true); });
        AddButton(controls_layout, QStringLiteral("4. 비전 인식 결과"), [this]() {
            PublishStatus(QStringLiteral("PI-VISION-01"), QStringLiteral("VISION_PROCESSING"), true);
            auto data = WorkData({ { QStringLiteral("recognitionStatus"), QStringLiteral("SUCCESS") },
                                   { QStringLiteral("barcode"), QStringLiteral("8801234567890") },
                                   { QStringLiteral("productId"), QStringLiteral("MOCK-PRODUCT-01") },
                                   { QStringLiteral("productName"), QStringLiteral("목업 상품") },
                                   { QStringLiteral("destination"), QString::number(DestinationIndex()) },
                                   { QStringLiteral("confidence"), 0.99 },
                                   { QStringLiteral("message"), QStringLiteral("목업 비전 인식 완료") } });
            if (!image_url_->text().trimmed().isEmpty()) {
                data.insert(QStringLiteral("imageUrl"), image_url_->text().trimmed());
            }
            PublishStatusMessage(QStringLiteral("PRODUCT_INFO"), QStringLiteral("PI-VISION-01"), data);
        });
        AddButton(controls_layout, QStringLiteral("5. 그리퍼 픽업"),
                  [this]() { PublishStatus(QStringLiteral("PI-GRIPPER-01"), QStringLiteral("PICKING"), true); });
        AddButton(controls_layout, QStringLiteral("6. 그리퍼 이송"),
                  [this]() { PublishStatus(QStringLiteral("PI-GRIPPER-01"), QStringLiteral("TRANSFERRING"), true); });
        AddButton(controls_layout, QStringLiteral("7. 목적지 분류"), [this]() {
            PublishEvent(QStringLiteral("DESTINATION_SET"), QStringLiteral("PI-SORTING-01"),
                         WorkData({ { QStringLiteral("command"), QStringLiteral("DESTINATION_SET") },
                                    { QStringLiteral("destination"), QString::number(DestinationIndex()) } }));
            PublishStatus(QStringLiteral("PI-SORTING-01"), QStringLiteral("SORTING"), true);
        });
        AddButton(controls_layout, QStringLiteral("8. 라인트레이서 출발"), [this]() {
            PublishLineStatus(QStringLiteral("FOLLOWING_LINE"), QStringLiteral("DEPARTURE"), departure_->currentText(),
                              QStringLiteral("MOVING"));
        });
        AddButton(controls_layout, QStringLiteral("9. 라인트레이서 도착"), [this]() {
            PublishLineStatus(QStringLiteral("ARRIVED_%1").arg(destination_->currentText()),
                              QStringLiteral("DESTINATION"), destination_->currentText(), QStringLiteral("ARRIVED"));
        });
        AddButton(controls_layout, QStringLiteral("10. 배송 완료"), [this]() {
            PublishEvent(QStringLiteral("WORK_COMPLETED"), QStringLiteral("PI-LT-01"),
                         WorkData({ { QStringLiteral("result"), QStringLiteral("SUCCESS") },
                                    { QStringLiteral("message"), QStringLiteral("목업 배송 완료") } }));
        });
        AddButton(controls_layout, QStringLiteral("비상정지 상태"), [this]() { SetAllStates("EMERGENCY_STOP"); });
        AddButton(controls_layout, QStringLiteral("복구 후 중립 상태"), [this]() { SetAllStates("STOPPED"); });
        AddButton(controls_layout, QStringLiteral("분류 장치 오류"), [this]() {
            PublishError(QStringLiteral("PI-SORTING-01"),
                         { { QStringLiteral("jobId"), WorkId() },
                           { QStringLiteral("errorCode"), QStringLiteral("ERR-MOCK") },
                           { QStringLiteral("errorLevel"), QStringLiteral("ERROR") },
                           { QStringLiteral("currentState"), QStringLiteral("ERROR") },
                           { QStringLiteral("message"), QStringLiteral("목업 분류 오류") } });
        });
        root->addWidget(controls_);

        publish_status_ = new QLabel(QStringLiteral("연결 후 목업 메시지를 발행할 수 있습니다."), this);
        publish_status_->setWordWrap(true);
        root->addWidget(publish_status_);

        connect(connect_button_, &QPushButton::clicked, this, [this]() { ToggleConnection(); });
        connect(mqtt_, &QMqttClient::stateChanged, this, [this](QMqttClient::ClientState state) {
            const bool connected = state == QMqttClient::Connected;
            controls_->setEnabled(connected);
            connect_button_->setText(connected ? QStringLiteral("연결 해제") : QStringLiteral("연결"));
            connection_status_->setText(connected ? QStringLiteral("연결됨 · 목업 발행 가능")
                                        : state == QMqttClient::Connecting ? QStringLiteral("연결 중")
                                                                           : QStringLiteral("연결 안 됨"));
        });
        connect(mqtt_, &QMqttClient::errorChanged, this, [this](QMqttClient::ClientError error) {
            if (error != QMqttClient::NoError) {
                publish_status_->setText(QStringLiteral("MQTT 오류: %1").arg(static_cast<int>(error)));
            }
        });
    }

private:
    static QString NewWorkId() {
        return QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    static QComboBox* RouteSelector(QWidget* parent) {
        auto* selector = new QComboBox(parent);
        selector->addItems({ QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C") });
        return selector;
    }

    template <typename Action>
    void AddButton(QVBoxLayout* layout, const QString& text, Action action) {
        auto* button = new QPushButton(text, controls_);
        connect(button, &QPushButton::clicked, this, action);
        layout->addWidget(button);
    }

    QString WorkId() const {
        return work_id_->text().trimmed();
    }
    int DestinationIndex() const {
        return destination_->currentIndex() + 1;
    }

    QJsonObject WorkData(QJsonObject data = {}) const {
        data.insert(QStringLiteral("workId"), WorkId());
        return data;
    }

    void ToggleConnection() {
        if (mqtt_->state() != QMqttClient::Disconnected) {
            mqtt_->disconnectFromHost();
            return;
        }
        bool port_ok = false;
        const auto port = port_->text().toUShort(&port_ok);
        if (!port_ok || host_->text().trimmed().isEmpty() ||
            !mqtt::IsValidTopicLevel(target_client_->text().trimmed().toStdString())) {
            publish_status_->setText(QStringLiteral("Broker, port, 관제 client ID를 확인하세요."));
            return;
        }
        mqtt_->setHostname(host_->text().trimmed());
        mqtt_->setPort(port);
        mqtt_->setClientId(QStringLiteral("control-center-mock-%1").arg(QCoreApplication::applicationPid()));
        mqtt_->setUsername(username_->text());
        mqtt_->setPassword(password_->text());
        mqtt_->setProtocolVersion(QMqttClient::MQTT_3_1_1);
        if (!tls_->isChecked()) {
            mqtt_->connectToHost();
            return;
        }
        QFile ca_file(ca_certificate_->text().trimmed());
        const auto certificates = ca_file.open(QIODevice::ReadOnly) ? QSslCertificate::fromDevice(&ca_file, QSsl::Pem)
                                                                    : QList<QSslCertificate>{};
        if (certificates.isEmpty()) {
            publish_status_->setText(QStringLiteral("TLS CA 인증서를 읽을 수 없습니다."));
            return;
        }
        auto ssl = QSslConfiguration::defaultConfiguration();
        ssl.addCaCertificates(certificates);
        ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
        ssl.setProtocol(QSsl::TlsV1_2OrLater);
        mqtt_->connectToHostEncrypted(ssl);
    }

    void Publish(const QString& suffix, const QString& type, const QString& source, const QJsonObject& data) {
        if (mqtt_->state() != QMqttClient::Connected) {
            publish_status_->setText(QStringLiteral("MQTT가 연결되지 않았습니다."));
            return;
        }
        if (QUuid::fromString(WorkId()).isNull()) {
            publish_status_->setText(QStringLiteral("작업 ID는 UUID 형식이어야 합니다."));
            return;
        }
        const auto topic = QStringLiteral("qt/%1/%2").arg(target_client_->text().trimmed(), suffix);
        const auto payload = QJsonDocument(Envelope(type, source, data)).toJson(QJsonDocument::Compact);
        const auto message_id = mqtt_->publish(QMqttTopicName(topic), payload, 1, false);
        publish_status_->setText(message_id < 0 ? QStringLiteral("발행 실패 · ACL과 연결 상태를 확인하세요.")
                                                : QStringLiteral("발행 완료 · %1 · %2").arg(topic, type));
    }

    void PublishEvent(const QString& type, const QString& source, const QJsonObject& data) {
        Publish(QStringLiteral("event"), type, source, data);
    }

    void PublishStatusMessage(const QString& type, const QString& source, const QJsonObject& data) {
        Publish(QStringLiteral("status"), type, source, data);
    }

    void PublishError(const QString& source, const QJsonObject& data) {
        Publish(QStringLiteral("error"), QStringLiteral("ERROR_OCCURRED"), source, data);
    }

    void PublishStatus(const QString& source, const QString& state, bool include_work) {
        QJsonObject data{ { QStringLiteral("status"), QStringLiteral("ONLINE") },
                          { QStringLiteral("currentState"), state } };
        if (include_work) {
            data.insert(QStringLiteral("jobId"), WorkId());
        }
        PublishStatusMessage(QStringLiteral("DEVICE_STATUS"), source, data);
    }

    void SetAllStates(const QString& state) {
        for (const auto& device :
             { QStringLiteral("PI-INPUT-01"), QStringLiteral("PI-VISION-01"), QStringLiteral("PI-GRIPPER-01"),
               QStringLiteral("PI-SORTING-01"), QStringLiteral("PI-LT-01") }) {
            PublishStatus(device, state, false);
        }
    }

    void PublishLineStatus(const QString& state, const QString& confirmed_area, const QString& confirmed_location,
                           const QString& movement) {
        auto data = QJsonObject{
            { QStringLiteral("status"), QStringLiteral("ONLINE") },
            { QStringLiteral("currentState"), state },
            { QStringLiteral("jobId"), WorkId() },
            { QStringLiteral("departurePosition"), Position(QStringLiteral("DEPARTURE"), departure_->currentText()) },
            { QStringLiteral("targetPosition"), Position(QStringLiteral("DESTINATION"), destination_->currentText()) },
            { QStringLiteral("confirmedPosition"), Position(confirmed_area, confirmed_location) },
            { QStringLiteral("movementState"), movement },
        };
        PublishStatusMessage(QStringLiteral("DEVICE_STATUS"), QStringLiteral("PI-LT-01"), data);
    }

    QMqttClient* mqtt_;
    QLineEdit* host_;
    QLineEdit* port_;
    QLineEdit* target_client_;
    QLineEdit* username_;
    QLineEdit* password_;
    QCheckBox* tls_;
    QLineEdit* ca_certificate_;
    QPushButton* connect_button_;
    QLabel* connection_status_;
    QGroupBox* controls_;
    QLineEdit* work_id_;
    QComboBox* departure_;
    QComboBox* destination_;
    QLineEdit* image_url_;
    QLabel* publish_status_;
};

}  // namespace

int main(int argc, char* argv[]) {
    const bool self_test = argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--self-test");
    if (self_test) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication application(argc, argv);
    if (self_test) {
        const auto envelope = Envelope(QStringLiteral("WORK_CREATED"), QStringLiteral("PI-INPUT-01"),
                                       { { QStringLiteral("workId"), QStringLiteral("mock-work") } });
        assert(envelope.value(QStringLiteral("messageType")) == QStringLiteral("WORK_CREATED"));
        assert(envelope.value(QStringLiteral("sourceId")) == QStringLiteral("PI-INPUT-01"));
        assert(envelope.value(QStringLiteral("messageId")).toString().size() == 36);
        assert(
            QDateTime::fromString(envelope.value(QStringLiteral("timestamp")).toString(), Qt::ISODateWithMs).isValid());
        assert(QString::fromStdString(mqtt::QtEventTopic("control-center")) ==
               QStringLiteral("qt/control-center/event"));
        return 0;
    }
    FactoryMockControl control;
    control.show();
    return application.exec();
}
