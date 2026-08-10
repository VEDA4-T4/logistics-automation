#include "logistics/control_center/operations_dashboard_state.hpp"

#include <QJsonValue>
#include <algorithm>
#include <string>

#include "logistics/contracts/device.hpp"
#include "logistics/contracts/mqtt_codec.hpp"

namespace logistics::control_center {
namespace {

namespace mqtt = logistics::contracts::mqtt;

constexpr qsizetype kMaximumRememberedMessages = 2048;
constexpr qsizetype kMaximumRetiredWorksPerProcess = 512;
constexpr qsizetype kMaximumRememberedDestinations = 512;
constexpr qsizetype kMaximumRetiredDeviceSessions = 512;

QString StringValue(const QJsonObject& object, const char* key) {
    const auto value = object.value(QString::fromLatin1(key));
    return value.isString() ? value.toString().trimmed() : QString{};
}

struct LineTracerPositionSnapshot {
    std::optional<LineTracerPositionStatus> departure;
    std::optional<LineTracerPositionStatus> target;
    std::optional<LineTracerPositionStatus> confirmed;
    QString movement_state;
};

bool ParseLineTracerPosition(const QJsonValue& value, LineTracerPositionStatus& output) {
    if (!value.isObject()) {
        return false;
    }
    const auto position = value.toObject();
    output.area = StringValue(position, "area").toUpper();
    output.location = StringValue(position, "location").toUpper();
    return (output.area == QStringLiteral("DEPARTURE") || output.area == QStringLiteral("DESTINATION")) &&
           (output.location == QStringLiteral("A") || output.location == QStringLiteral("B") ||
            output.location == QStringLiteral("C"));
}

bool ParseLineTracerPositionSnapshot(const QJsonObject& data, std::optional<LineTracerPositionSnapshot>& output,
                                     QString& error) {
    const auto departure = data.value(QStringLiteral("departurePosition"));
    const auto target = data.value(QStringLiteral("targetPosition"));
    const auto confirmed = data.value(QStringLiteral("confirmedPosition"));
    const auto movement = data.value(QStringLiteral("movementState"));
    if (departure.isUndefined() && target.isUndefined() && confirmed.isUndefined() && movement.isUndefined()) {
        output.reset();
        return true;
    }
    if (departure.isNull() && target.isNull() && confirmed.isNull() && movement.isNull()) {
        output = LineTracerPositionSnapshot{};
        return true;
    }

    LineTracerPositionStatus parsed_departure;
    LineTracerPositionStatus parsed_target;
    LineTracerPositionStatus parsed_confirmed;
    const auto movement_state = movement.isString() ? movement.toString().trimmed().toUpper() : QString{};
    if (!ParseLineTracerPosition(departure, parsed_departure) || !ParseLineTracerPosition(target, parsed_target) ||
        !ParseLineTracerPosition(confirmed, parsed_confirmed) ||
        (movement_state != QStringLiteral("IDLE") && movement_state != QStringLiteral("MOVING") &&
         movement_state != QStringLiteral("ARRIVED"))) {
        error = QStringLiteral("라인트레이서 위치 스냅샷이 올바르지 않습니다.");
        return false;
    }
    output = LineTracerPositionSnapshot{ .departure = parsed_departure,
                                         .target = parsed_target,
                                         .confirmed = parsed_confirmed,
                                         .movement_state = movement_state };
    return true;
}

QDateTime ParseTimestamp(const QJsonObject& envelope) {
    const auto value = envelope.value(QString::fromLatin1(mqtt::kTimestampField)).toString();
    auto timestamp = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!timestamp.isValid()) {
        timestamp = QDateTime::fromString(value, Qt::ISODate);
    }
    return timestamp;
}

bool IsDashboardMessage(mqtt::MessageType type) {
    switch (type) {
        case mqtt::MessageType::kHeartbeat:
        case mqtt::MessageType::kBoxDetected:
        case mqtt::MessageType::kWorkCreated:
        case mqtt::MessageType::kWorkCompleted:
        case mqtt::MessageType::kPositionDetected:
        case mqtt::MessageType::kBarcodeDetected:
        case mqtt::MessageType::kProductImage:
        case mqtt::MessageType::kProductInfo:
        case mqtt::MessageType::kDestinationSet:
        case mqtt::MessageType::kDeviceStatus:
        case mqtt::MessageType::kErrorOccurred:
        case mqtt::MessageType::kEmergencyStop:
        case mqtt::MessageType::kCommandResponse:
        case mqtt::MessageType::kSensorStatus:
            return true;
        default:
            return false;
    }
}

bool IsDeviceMessage(mqtt::MessageType type) {
    return type == mqtt::MessageType::kHeartbeat || type == mqtt::MessageType::kDeviceStatus ||
           type == mqtt::MessageType::kErrorOccurred;
}

struct DeviceMessageOrder {
    enum class Stream {
        kApplication,
        kTransport,
        kWill,
    };

    Stream stream;
    QString session_id;
    quint64 sequence;
    int phase;
};

std::optional<DeviceMessageOrder> ParseDeviceMessageOrder(const QString& message_id, const QString& source_id,
                                                          mqtt::MessageType type) {
    const auto application_prefix = source_id + QStringLiteral("-MSG-");
    const auto status_prefix = QStringLiteral("STATUS-") + application_prefix;
    const auto offline_prefix = QStringLiteral("OFFLINE-") + application_prefix;
    const auto will_prefix = QStringLiteral("WILL-") + application_prefix;
    QString prefix;
    DeviceMessageOrder::Stream stream;
    int phase = 0;
    if (message_id.startsWith(application_prefix)) {
        prefix = application_prefix;
        stream = DeviceMessageOrder::Stream::kApplication;
    } else if (type == mqtt::MessageType::kDeviceStatus && message_id.startsWith(status_prefix)) {
        prefix = status_prefix;
        stream = DeviceMessageOrder::Stream::kTransport;
    } else if (type == mqtt::MessageType::kDeviceStatus && message_id.startsWith(offline_prefix)) {
        prefix = offline_prefix;
        stream = DeviceMessageOrder::Stream::kTransport;
        phase = 1;
    } else if (type == mqtt::MessageType::kDeviceStatus && message_id.startsWith(will_prefix)) {
        prefix = will_prefix;
        stream = DeviceMessageOrder::Stream::kWill;
    } else {
        return std::nullopt;
    }

    const auto separator = message_id.lastIndexOf(QLatin1Char('-'));
    if (separator <= prefix.size()) {
        return std::nullopt;
    }

    const auto sequence_text = message_id.mid(separator + 1);
    if (sequence_text.isEmpty() ||
        !std::all_of(sequence_text.cbegin(), sequence_text.cend(), [](const QChar character) {
            return character >= QLatin1Char('0') && character <= QLatin1Char('9');
        })) {
        return std::nullopt;
    }

    bool valid = false;
    const auto sequence = sequence_text.toULongLong(&valid);
    return valid
               ? std::optional<DeviceMessageOrder>{ { stream, message_id.mid(prefix.size(), separator - prefix.size()),
                                                      sequence, phase } }
               : std::nullopt;
}

QString WorkIdFor(mqtt::MessageType type, const QJsonObject& data) {
    switch (type) {
        case mqtt::MessageType::kWorkCreated:
        case mqtt::MessageType::kWorkCompleted:
        case mqtt::MessageType::kPositionDetected:
        case mqtt::MessageType::kBarcodeDetected:
        case mqtt::MessageType::kProductImage:
        case mqtt::MessageType::kProductInfo:
        case mqtt::MessageType::kDestinationSet:
            return StringValue(data, "workId");
        default:
            return {};
    }
}

QString StageFor(mqtt::MessageType type) {
    switch (type) {
        case mqtt::MessageType::kBoxDetected:
            return QStringLiteral("상품 감지");
        case mqtt::MessageType::kWorkCreated:
            return QStringLiteral("상품 투입");
        case mqtt::MessageType::kPositionDetected:
            return QStringLiteral("상품 위치 인식");
        case mqtt::MessageType::kBarcodeDetected:
            return QStringLiteral("바코드 인식");
        case mqtt::MessageType::kProductImage:
            return QStringLiteral("상품 이미지 처리");
        case mqtt::MessageType::kProductInfo:
            return QStringLiteral("상품 이동 준비");
        case mqtt::MessageType::kDestinationSet:
            return QStringLiteral("목적지 분류");
        case mqtt::MessageType::kWorkCompleted:
            return QStringLiteral("배송 완료");
        default:
            return {};
    }
}

contracts::DeviceStateMeaning StateMeaning(const ProcessUnitStatus& process, const QString& current_state) {
    const auto role = contracts::DeviceRoleFromString(process.key.toStdString());
    return role.has_value() ? contracts::DeviceStateMeaningFor(*role, current_state.trimmed().toUpper().toStdString())
                            : contracts::DeviceStateMeaning::kUnknown;
}

bool IsOperationalWaitingState(const QString& current_state) {
    return current_state.trimmed().compare(QStringLiteral("WAITING_FOR_PRODUCT"), Qt::CaseInsensitive) == 0;
}

QString SensorMeasurementForCurrentState(const QString& current_state) {
    const auto state = current_state.trimmed().toUpper();
    if (!state.startsWith(QStringLiteral("SENSOR_"))) {
        return {};
    }
    if (state.endsWith(QStringLiteral("_CLEAR"))) {
        return QStringLiteral("CLEAR");
    }
    if (state.endsWith(QStringLiteral("_DETECTED"))) {
        return QStringLiteral("DETECTED");
    }
    if (state.endsWith(QStringLiteral("_FAULT"))) {
        return QStringLiteral("FAULT");
    }
    return {};
}

std::optional<int> SensorIdForCurrentState(const QString& current_state) {
    const auto state = current_state.trimmed().toUpper();
    if (!state.startsWith(QStringLiteral("SENSOR_"))) {
        return std::nullopt;
    }
    const auto separator = state.indexOf(QLatin1Char('_'), 7);
    if (separator < 0) {
        return std::nullopt;
    }
    bool valid = false;
    const auto sensor_id = state.mid(7, separator - 7).toInt(&valid);
    return valid && sensor_id > 0 ? std::optional<int>{ sensor_id } : std::nullopt;
}

bool HasFaultedSensor(const ProcessUnitStatus& process) {
    return std::any_of(process.sensors.cbegin(), process.sensors.cend(), [](const SensorUnitStatus& sensor) {
        return sensor.measurement_status.compare(QStringLiteral("FAULT"), Qt::CaseInsensitive) == 0;
    });
}

void ResetSensors(ProcessUnitStatus& process) {
    for (auto& sensor : process.sensors) {
        sensor.measurement_status = QStringLiteral("UNKNOWN");
        sensor.distance_cm = -1;
        sensor.updated_at = {};
    }
}

void UpdateSensor(ProcessUnitStatus& process, int sensor_id, const QString& measurement_status, int distance_cm,
                  const QDateTime& timestamp) {
    auto sensor = std::find_if(process.sensors.begin(), process.sensors.end(),
                               [sensor_id](const SensorUnitStatus& item) { return item.sensor_id == sensor_id; });
    if (sensor == process.sensors.end()) {
        process.sensors.append({
            .sensor_id = sensor_id,
            .display_name = QStringLiteral("S%1").arg(sensor_id),
            .measurement_status = QStringLiteral("UNKNOWN"),
            .distance_cm = -1,
            .updated_at = {},
        });
        sensor = std::prev(process.sensors.end());
        std::sort(process.sensors.begin(), process.sensors.end(),
                  [](const SensorUnitStatus& left, const SensorUnitStatus& right) {
                      return left.sensor_id < right.sensor_id;
                  });
        sensor = std::find_if(process.sensors.begin(), process.sensors.end(),
                              [sensor_id](const SensorUnitStatus& item) { return item.sensor_id == sensor_id; });
    }
    sensor->measurement_status = measurement_status;
    if (distance_cm >= 0) {
        sensor->distance_cm = distance_cm == 0xffff ? -1 : distance_cm;
    }
    sensor->updated_at = timestamp;
}

bool IsBusy(const ProcessUnitStatus& process) {
    if (process.current_state == QStringLiteral("배송 완료") ||
        process.current_state.compare(QStringLiteral("COMPLETED"), Qt::CaseInsensitive) == 0) {
        return false;
    }
    const auto meaning = StateMeaning(process, process.current_state);
    return !process.work_id.isEmpty() ||
           (meaning != contracts::DeviceStateMeaning::kUnknown && meaning != contracts::DeviceStateMeaning::kIdle &&
            meaning != contracts::DeviceStateMeaning::kStopped && meaning != contracts::DeviceStateMeaning::kCompleted);
}

QString CommandStage(const QString& command) {
    if (command == QStringLiteral("START"))
        return QStringLiteral("공정 시작");
    if (command == QStringLiteral("STOP"))
        return QStringLiteral("공정 정지");
    if (command == QStringLiteral("RESTART"))
        return QStringLiteral("공정 재시작");
    if (command == QStringLiteral("INITIALIZE"))
        return QStringLiteral("공정 초기화");
    if (command == QStringLiteral("STATUS_REQUEST"))
        return QStringLiteral("공정 상태 확인");
    if (command == QStringLiteral("EMERGENCY_STOP"))
        return QStringLiteral("비상정지");
    if (command == QStringLiteral("RECOVERY"))
        return QStringLiteral("복구");
    return {};
}

}  // namespace

QList<ProcessDefinition> DefaultProcessDefinitions() {
    return {
        { QString::fromLatin1(kInputProcessKey), QStringLiteral("투입 컨베이어"), QStringLiteral("PI-INPUT-01") },
        { QString::fromLatin1(kVisionProcessKey), QStringLiteral("비전 처리"), QStringLiteral("PI-VISION-01") },
        { QString::fromLatin1(kGripperProcessKey), QStringLiteral("그리퍼 이송"), QStringLiteral("PI-GRIPPER-01") },
        { QString::fromLatin1(kSortingProcessKey), QStringLiteral("분류 컨베이어"), QStringLiteral("PI-SORTING-01") },
        { QString::fromLatin1(kLineTracerProcessKey), QStringLiteral("라인트레이서"), QStringLiteral("PI-LT-01") },
    };
}

OperationsDashboardState::OperationsDashboardState() {
    configureProcesses(DefaultProcessDefinitions());
}

void OperationsDashboardState::configureProcesses(const QList<ProcessDefinition>& definitions) {
    process_runtime_.clear();
    process_snapshots_.clear();
    process_index_by_device_.clear();
    process_index_by_key_.clear();
    destination_by_work_id_.clear();
    destination_work_order_.clear();

    for (const auto& definition : definitions) {
        ProcessRuntime runtime;
        runtime.status.key = definition.key;
        runtime.status.display_name = definition.display_name;
        runtime.status.device_id = definition.device_id;
        if (definition.key == QString::fromLatin1(kInputProcessKey)) {
            runtime.status.sensors.append({
                .sensor_id = 1,
                .display_name = QStringLiteral("S1"),
                .measurement_status = QStringLiteral("UNKNOWN"),
                .distance_cm = -1,
                .updated_at = {},
            });
        } else if (definition.key == QString::fromLatin1(kSortingProcessKey)) {
            for (int sensor_id = 1; sensor_id <= 3; ++sensor_id) {
                runtime.status.sensors.append({
                    .sensor_id = sensor_id,
                    .display_name = QStringLiteral("S%1").arg(sensor_id),
                    .measurement_status = QStringLiteral("UNKNOWN"),
                    .distance_cm = -1,
                    .updated_at = {},
                });
            }
        }
        const auto index = process_runtime_.size();
        process_runtime_.append(runtime);
        process_index_by_device_.insert(definition.device_id, index);
        process_index_by_key_.insert(definition.key, index);
    }
    overall_ = {};
    processed_message_ids_.clear();
    processed_message_order_.clear();
    last_completion_at_ = {};
    last_completion_detail_.clear();
    command_override_.reset();
    command_override_stage_.clear();
    command_override_detail_.clear();
    publishProcessSnapshots();
}

void OperationsDashboardState::markMqttConnectedAwaitingStatus(const QDateTime& timestamp) {
    resetForMqttTransition(QStringLiteral("상태 수신 대기"), QStringLiteral("MQTT 연결됨 · 노드 상태 수신 대기"),
                           timestamp);
}

void OperationsDashboardState::markMqttDisconnected(const QDateTime& timestamp) {
    resetForMqttTransition(QStringLiteral("DISCONNECTED"), QStringLiteral("MQTT 연결 끊김"), timestamp);
}

bool OperationsDashboardState::expireStaleProcesses(const QDateTime& timestamp) {
    if (!timestamp.isValid()) {
        return false;
    }

    const auto stale_after =
        std::chrono::duration_cast<std::chrono::milliseconds>(mqtt::kHeartbeatOfflineAfter).count();
    bool changed = false;
    for (auto& runtime : process_runtime_) {
        auto& process = runtime.status;
        if (!runtime.last_received_at.isValid() ||
            (process.connection_state != mqtt::ConnectionState::kOnline &&
             process.connection_state != mqtt::ConnectionState::kDelayed &&
             process.connection_state != mqtt::ConnectionState::kReconnecting) ||
            runtime.last_received_at.msecsTo(timestamp) < stale_after) {
            continue;
        }

        process.connection_state = mqtt::ConnectionState::kOffline;
        process.current_state = QStringLiteral("DISCONNECTED");
        process.work_id.clear();
        process.destination.clear();
        process.work_completed = false;
        process.error_code = QStringLiteral("ERR-HEARTBEAT-TIMEOUT");
        process.updated_at = timestamp;
        process.has_error = true;
        process.has_warning = false;
        ResetSensors(process);
        changed = true;
    }
    if (!changed) {
        return false;
    }

    updateOverall(timestamp);
    publishProcessSnapshots();
    return true;
}

DashboardUpdateResult OperationsDashboardState::applyEnvelope(const QJsonObject& envelope, const QDateTime& received_at,
                                                              const bool apply_command_to_overall) {
    const auto type_text = envelope.value(QString::fromLatin1(mqtt::kMessageTypeField)).toString();
    const auto type = mqtt::MessageTypeFromString(type_text.toStdString());
    if (!IsDashboardMessage(type)) {
        return {};
    }

    DashboardUpdateResult result{ .handled = true, .applied = false, .error = {} };
    const auto message_id = envelope.value(QString::fromLatin1(mqtt::kMessageIdField)).toString().trimmed();
    const auto source_id = envelope.value(QString::fromLatin1(mqtt::kSourceIdField)).toString().trimmed();
    const auto data_value = envelope.value(QString::fromLatin1(mqtt::kDataField));
    const auto timestamp = ParseTimestamp(envelope);
    const auto effective_received_at = received_at.isValid() ? received_at.toUTC() : timestamp;
    const auto protocol_version = envelope.value(QString::fromLatin1(mqtt::kProtocolVersionField)).toString().trimmed();
    if (protocol_version != QString::fromLatin1(mqtt::kCurrentProtocolVersion) ||
        !mqtt::IsValidTopicLevel(message_id.toStdString()) || !mqtt::IsValidTopicLevel(source_id.toStdString()) ||
        !data_value.isObject() || !timestamp.isValid()) {
        result.error = QStringLiteral("대시보드 메시지 envelope가 올바르지 않습니다.");
        return result;
    }
    if (processed_message_ids_.contains(message_id)) {
        return result;
    }

    const auto data = data_value.toObject();
    if (type == mqtt::MessageType::kSensorStatus) {
        const auto process_index = processIndexForDevice(source_id);
        if (process_index < 0) {
            return result;
        }
        const auto sensor_id_value = data.value(QStringLiteral("sensorId"));
        const auto distance_value = data.value(QStringLiteral("distanceCm"));
        const auto measurement_status = StringValue(data, "measurementStatus").toUpper();
        const auto sensor_id = sensor_id_value.toInt();
        const auto distance_cm = distance_value.toInt();
        if (!sensor_id_value.isDouble() || sensor_id_value.toDouble() != sensor_id || sensor_id <= 0 ||
            !distance_value.isDouble() || distance_value.toDouble() != distance_cm || distance_cm < 0 ||
            (measurement_status != QStringLiteral("CLEAR") && measurement_status != QStringLiteral("DETECTED") &&
             measurement_status != QStringLiteral("FAULT"))) {
            result.error =
                QStringLiteral("센서 상태 메시지의 sensorId, measurementStatus 또는 distanceCm이 올바르지 않습니다.");
            return result;
        }

        auto& process = process_runtime_[process_index];
        UpdateSensor(process.status, sensor_id, measurement_status, distance_cm, timestamp);
        process.last_received_at = effective_received_at;
        if (measurement_status == QStringLiteral("FAULT")) {
            process.status.error_code = QStringLiteral("ERR-SENSOR");
            process.status.has_error = true;
            process.status.has_warning = false;
        } else if (process.status.error_code.compare(QStringLiteral("ERR-SENSOR"), Qt::CaseInsensitive) == 0 &&
                   !HasFaultedSensor(process.status)) {
            process.status.error_code.clear();
            process.status.has_error = false;
        }
        updateOverall(timestamp);
        publishProcessSnapshots();
        rememberMessage(message_id);
        result.applied = true;
        return result;
    }

    if (IsDeviceMessage(type)) {
        const auto process_index = processIndexForDevice(source_id);
        if (process_index < 0) {
            return result;
        }

        auto& process = process_runtime_[process_index];
        const auto current_state = StringValue(data, "currentState");
        if (current_state.isEmpty()) {
            result.error = QStringLiteral("장치 상태에 currentState가 필요합니다.");
            return result;
        }
        std::optional<LineTracerPositionSnapshot> position_snapshot;
        if (type != mqtt::MessageType::kErrorOccurred &&
            process.status.key == QString::fromLatin1(kLineTracerProcessKey) &&
            !ParseLineTracerPositionSnapshot(data, position_snapshot, result.error)) {
            return result;
        }

        const auto device_order = ParseDeviceMessageOrder(message_id, source_id, type);
        DeviceMessageOrdering* device_ordering = nullptr;
        if (device_order.has_value() && device_order->stream != DeviceMessageOrder::Stream::kWill) {
            device_ordering = device_order->stream == DeviceMessageOrder::Stream::kApplication
                                  ? &process.application_messages
                                  : &process.transport_messages;
            const bool same_session = device_ordering->session_id == device_order->session_id;
            const bool stale_sequence =
                same_session && (device_order->sequence < device_ordering->last_sequence ||
                                 (device_order->sequence == device_ordering->last_sequence &&
                                  (device_order->stream == DeviceMessageOrder::Stream::kApplication ||
                                   device_order->phase <= device_ordering->last_phase)));
            if (device_ordering->retired_sessions.contains(device_order->session_id) || stale_sequence) {
                return result;
            }
        } else if (!device_order.has_value() && process.last_device_message_at.isValid() &&
                   timestamp < process.last_device_message_at) {
            return result;
        }

        if (type == mqtt::MessageType::kErrorOccurred) {
            const auto error_code = StringValue(data, "errorCode");
            if (error_code.isEmpty()) {
                result.error = QStringLiteral("오류 메시지에 errorCode가 필요합니다.");
                return result;
            }
            const auto work_id = StringValue(data, "jobId");
            if (!work_id.isEmpty() && !updateProcessWork(process, work_id)) {
                return result;
            }
            process.status.error_code = error_code;
            process.status.has_warning = IsSensorStaleErrorCode(error_code);
            process.status.has_error = !process.status.has_warning;
            if (process.status.has_warning) {
                ResetSensors(process.status);
            }
            if (!process.status.has_warning && SensorMeasurementForCurrentState(current_state).isEmpty()) {
                process.status.current_state = current_state;
            }
            process.status.work_completed = false;
            process.status.updated_at = timestamp;
        } else {
            const auto state_text = StringValue(data, "status");
            const auto connection_state = mqtt::ConnectionStateFromString(state_text.toStdString());
            if (connection_state == mqtt::ConnectionState::kUnknown) {
                result.error = QStringLiteral("알 수 없는 장치 연결 상태입니다: %1").arg(state_text);
                return result;
            }
            const auto work_id = StringValue(data, "jobId");
            if (!updateProcessWork(process, work_id)) {
                return result;
            }
            process.status.work_completed = false;
            if (process.status.key == QString::fromLatin1(kLineTracerProcessKey)) {
                const auto destination = destination_by_work_id_.constFind(work_id);
                if (destination != destination_by_work_id_.cend()) {
                    process.status.destination = destination.value();
                }
            }
            const auto error_code = StringValue(data, "errorCode");
            const bool sensor_stale = IsSensorStaleErrorCode(error_code);
            const auto sensor_measurement = SensorMeasurementForCurrentState(current_state);
            const auto sensor_id = SensorIdForCurrentState(current_state);
            const bool sensor_telemetry = !sensor_measurement.isEmpty();
            if (sensor_telemetry && sensor_id.has_value()) {
                UpdateSensor(process.status, *sensor_id, sensor_measurement, -1, timestamp);
            }
            if (sensor_stale) {
                ResetSensors(process.status);
            }
            process.status.connection_state =
                (sensor_stale || sensor_telemetry) && connection_state == mqtt::ConnectionState::kUartError
                    ? (process.status.connection_state == mqtt::ConnectionState::kUnknown
                           ? mqtt::ConnectionState::kOnline
                           : process.status.connection_state)
                    : connection_state;
            if ((!sensor_stale && !sensor_telemetry) || !process.status.updated_at.isValid()) {
                process.status.current_state = current_state;
            }
            process.status.error_code =
                error_code.isEmpty() && HasFaultedSensor(process.status) ? QStringLiteral("ERR-SENSOR") : error_code;
            process.status.has_warning = sensor_stale;
            process.status.has_error =
                !sensor_stale && (mqtt::IsConnectionFailure(connection_state) || !process.status.error_code.isEmpty() ||
                                  (!sensor_telemetry && StateMeaning(process.status, current_state) ==
                                                            contracts::DeviceStateMeaning::kError));
            if (mqtt::IsConnectionFailure(process.status.connection_state)) {
                process.status.destination.clear();
            }
            process.status.updated_at = timestamp;
        }
        if (position_snapshot.has_value()) {
            process.status.departure_position = position_snapshot->departure;
            process.status.target_position = position_snapshot->target;
            process.status.confirmed_position = position_snapshot->confirmed;
            process.status.movement_state = position_snapshot->movement_state;
        }
        if (device_ordering != nullptr) {
            if (!device_ordering->session_id.isEmpty() && device_ordering->session_id != device_order->session_id) {
                device_ordering->retired_sessions.enqueue(device_ordering->session_id);
                // ponytail: 512 sessions bounds malformed-device memory; persist epochs for longer replay protection.
                while (device_ordering->retired_sessions.size() > kMaximumRetiredDeviceSessions) {
                    device_ordering->retired_sessions.dequeue();
                }
            }
            device_ordering->session_id = device_order->session_id;
            device_ordering->last_sequence = device_order->sequence;
            device_ordering->last_phase = device_order->phase;
        }
        process.last_device_message_at = timestamp;
        process.last_received_at = effective_received_at;

        updateOverall(timestamp);
        publishProcessSnapshots();
        if (!device_order.has_value() || device_order->stream != DeviceMessageOrder::Stream::kWill) {
            rememberMessage(message_id);
        }
        result.applied = true;
        return result;
    }

    if (type == mqtt::MessageType::kCommandResponse) {
        if (!apply_command_to_overall) {
            rememberMessage(message_id);
            return result;
        }
        if (overall_.updated_at.isValid() && timestamp < overall_.updated_at) {
            return result;
        }
        updateOverallForCommand(data, timestamp);
        publishProcessSnapshots();
        rememberMessage(message_id);
        result.applied = true;
        return result;
    }

    if (type == mqtt::MessageType::kEmergencyStop) {
        for (auto& process : process_runtime_) {
            if (process.status.connection_state != mqtt::ConnectionState::kOffline &&
                process.status.connection_state != mqtt::ConnectionState::kUnknown) {
                process.status.current_state = QStringLiteral("EMERGENCY_STOP");
                process.status.updated_at = timestamp;
            }
        }
        command_override_ = OverallProcessState::EmergencyStop;
        command_override_stage_ = QStringLiteral("비상정지");
        command_override_detail_ = QStringLiteral("비상정지 명령 수신");
        overall_.state = OverallProcessState::EmergencyStop;
        overall_.stage = command_override_stage_;
        overall_.detail = command_override_detail_;
        overall_.updated_at = timestamp;
        publishProcessSnapshots();
        rememberMessage(message_id);
        result.applied = true;
        return result;
    }

    const auto work_id = WorkIdFor(type, data);
    if (type != mqtt::MessageType::kBoxDetected && !mqtt::IsValidTopicLevel(work_id.toStdString())) {
        result.error = QStringLiteral("공정 메시지에 유효한 workId가 필요합니다.");
        return result;
    }
    const auto destination = type == mqtt::MessageType::kDestinationSet ? StringValue(data, "destination") : QString{};
    if (type == mqtt::MessageType::kDestinationSet && !DestinationRouteIndex(destination).has_value()) {
        result.error = QStringLiteral("목적지는 1, 2, 3 중 하나여야 합니다.");
        return result;
    }
    if (type == mqtt::MessageType::kWorkCompleted) {
        const auto completed_result = StringValue(data, "result").toUpper();
        if (completed_result != QStringLiteral("SUCCESS") && completed_result != QStringLiteral("FAILED")) {
            result.error = QStringLiteral("작업 완료 메시지의 result가 올바르지 않습니다.");
            return result;
        }
    } else if (type == mqtt::MessageType::kWorkCreated &&
               (!last_completion_at_.isValid() || timestamp >= last_completion_at_)) {
        last_completion_at_ = {};
        last_completion_detail_.clear();
    }

    const auto process_index = processIndexForEvent(type);
    if (process_index >= 0) {
        auto& process = process_runtime_[process_index];
        if (process.last_event_at.isValid() && timestamp < process.last_event_at) {
            return result;
        }
        const bool work_updated = work_id.isEmpty() || updateProcessWork(process, work_id);
        if (!work_updated && type != mqtt::MessageType::kWorkCompleted) {
            return result;
        }
        if (work_updated) {
            process.status.current_state = StageFor(type);
            if (type == mqtt::MessageType::kDestinationSet) {
                process.status.destination = destination;
                if (!destination_by_work_id_.contains(work_id)) {
                    destination_work_order_.enqueue(work_id);
                }
                destination_by_work_id_.insert(work_id, destination);
                while (destination_work_order_.size() > kMaximumRememberedDestinations) {
                    destination_by_work_id_.remove(destination_work_order_.dequeue());
                }
            } else if (type == mqtt::MessageType::kWorkCompleted) {
                const auto cached_destination = destination_by_work_id_.constFind(work_id);
                if (cached_destination != destination_by_work_id_.cend()) {
                    process.status.destination = cached_destination.value();
                }
                process.status.work_completed = true;
            }
            process.status.updated_at = timestamp;
            if (type == mqtt::MessageType::kWorkCompleted &&
                StringValue(data, "result").toUpper() == QStringLiteral("FAILED")) {
                process.status.has_error = true;
                process.status.has_warning = false;
                process.status.error_code = QStringLiteral("WORK_FAILED");
            }
            process.last_event_at = timestamp;
        }
    }

    if (type == mqtt::MessageType::kWorkCompleted &&
        (!last_completion_at_.isValid() || timestamp >= last_completion_at_)) {
        last_completion_at_ = timestamp;
        last_completion_detail_ = StringValue(data, "message");
    }

    updateOverall(timestamp);
    publishProcessSnapshots();
    rememberMessage(message_id);
    result.applied = true;
    return result;
}

const QList<ProcessUnitStatus>& OperationsDashboardState::processes() const noexcept {
    return process_snapshots_;
}

const ProcessDashboardStatus& OperationsDashboardState::overall() const noexcept {
    return overall_;
}

bool OperationsDashboardState::markRecoveryCompleted(const QString& target_device_id, const QDateTime& timestamp) {
    const auto process_index = processIndexForDevice(target_device_id);
    if (process_index < 0 || !timestamp.isValid()) {
        return false;
    }
    auto& process = process_runtime_[process_index].status;
    process.current_state = QStringLiteral("STOPPED");
    process.error_code.clear();
    process.has_error = false;
    process.has_warning = false;
    process.updated_at = timestamp;
    if (command_override_ == OverallProcessState::EmergencyStop) {
        command_override_.reset();
        command_override_stage_.clear();
        command_override_detail_.clear();
    }
    updateOverall(timestamp);
    publishProcessSnapshots();
    return true;
}

void OperationsDashboardState::rememberMessage(const QString& message_id) {
    processed_message_ids_.insert(message_id);
    processed_message_order_.enqueue(message_id);
    while (processed_message_order_.size() > kMaximumRememberedMessages) {
        processed_message_ids_.remove(processed_message_order_.dequeue());
    }
}

void OperationsDashboardState::updateOverall(const QDateTime& timestamp) {
    int active_processes = 0;
    int error_processes = 0;
    int received_processes = 0;
    int operational_waiting_processes = 0;
    bool emergency_stop = false;
    bool recovery = false;
    bool stopped = false;
    QSet<QString> active_work_ids;
    QString first_error;

    for (const auto& runtime : process_runtime_) {
        const auto& process = runtime.status;
        if (!process.updated_at.isValid()) {
            continue;
        }
        ++received_processes;
        const auto meaning = StateMeaning(process, process.current_state);
        emergency_stop = emergency_stop || meaning == contracts::DeviceStateMeaning::kEmergencyStop;
        recovery = recovery || meaning == contracts::DeviceStateMeaning::kRecovery;
        stopped = stopped || meaning == contracts::DeviceStateMeaning::kStopped;
        if (IsOperationalWaitingState(process.current_state)) {
            ++operational_waiting_processes;
        }
        if (process.has_error || mqtt::IsConnectionFailure(process.connection_state)) {
            ++error_processes;
            if (first_error.isEmpty()) {
                first_error = process.error_code.isEmpty()
                                  ? process.display_name
                                  : QStringLiteral("%1 · %2").arg(process.display_name, process.error_code);
            }
        }
        if (IsBusy(process)) {
            ++active_processes;
            if (!process.work_id.isEmpty()) {
                active_work_ids.insert(process.work_id);
            }
        }
    }

    overall_.active_unit_count = active_processes;
    overall_.active_work_count = active_work_ids.size();
    overall_.detail.clear();
    if (command_override_ == OverallProcessState::EmergencyStop) {
        overall_.state = OverallProcessState::EmergencyStop;
        overall_.stage = command_override_stage_;
        overall_.detail = command_override_detail_;
    } else if (emergency_stop) {
        overall_.state = OverallProcessState::EmergencyStop;
        overall_.stage = QStringLiteral("비상정지 공정 확인 필요");
    } else if (error_processes > 0) {
        overall_.state = OverallProcessState::Error;
        overall_.stage = QStringLiteral("오류 %1 · 가동 %2").arg(error_processes).arg(active_processes);
        overall_.detail = first_error;
    } else if (command_override_ == OverallProcessState::Recovery) {
        overall_.state = OverallProcessState::Recovery;
        overall_.stage = command_override_stage_;
        overall_.detail = command_override_detail_;
    } else if (recovery) {
        overall_.state = OverallProcessState::Recovery;
        overall_.stage = QStringLiteral("장치 복구 중");
    } else if (command_override_ == OverallProcessState::Stopped) {
        overall_.state = OverallProcessState::Stopped;
        overall_.stage = command_override_stage_;
        overall_.detail = command_override_detail_;
    } else if (active_processes > 0) {
        overall_.state = OverallProcessState::Running;
        overall_.stage = QStringLiteral("가동 %1 · 대기 %2")
                             .arg(active_processes)
                             .arg(std::max(0, static_cast<int>(process_runtime_.size()) - active_processes));
    } else if (operational_waiting_processes > 0) {
        overall_.state = OverallProcessState::Running;
        overall_.stage = QStringLiteral("가동 준비 완료 · 상품 대기 %1").arg(operational_waiting_processes);
    } else if (stopped) {
        overall_.state = OverallProcessState::Stopped;
        overall_.stage = QStringLiteral("공정 정지");
    } else if (last_completion_at_.isValid()) {
        overall_.state = OverallProcessState::Completed;
        overall_.stage = QStringLiteral("최근 작업 완료");
        overall_.detail = last_completion_detail_;
    } else {
        overall_.state = OverallProcessState::Idle;
        overall_.stage =
            received_processes == 0 ? QStringLiteral("공정 상태 수신 대기") : QStringLiteral("전체 공정 대기");
    }
    if (!overall_.updated_at.isValid() || timestamp >= overall_.updated_at) {
        overall_.updated_at = timestamp;
    }
}

void OperationsDashboardState::updateOverallForCommand(const QJsonObject& data, const QDateTime& timestamp) {
    const auto command = StringValue(data, "command").toUpper();
    const auto command_result = StringValue(data, "result").toUpper();
    const auto stage = CommandStage(command);
    if (stage.isEmpty()) {
        return;
    }

    overall_.stage = stage;
    overall_.updated_at = timestamp;
    overall_.detail = StringValue(data, "message");
    if (command_result == QStringLiteral("FAILED") || command_result == QStringLiteral("REJECTED") ||
        command_result == QStringLiteral("TIMEOUT")) {
        overall_.state = OverallProcessState::Error;
        return;
    }
    if (command_result != QStringLiteral("SUCCESS")) {
        if (command == QStringLiteral("RECOVERY")) {
            command_override_ = OverallProcessState::Recovery;
            command_override_stage_ = stage;
            command_override_detail_ = overall_.detail;
            overall_.state = OverallProcessState::Recovery;
        }
        return;
    }

    if (command == QStringLiteral("START") || command == QStringLiteral("RESTART")) {
        command_override_.reset();
        command_override_stage_.clear();
        command_override_detail_.clear();
        overall_.state = OverallProcessState::Running;
    } else if (command == QStringLiteral("STOP")) {
        command_override_ = OverallProcessState::Stopped;
        command_override_stage_ = stage;
        command_override_detail_ = overall_.detail;
        overall_.state = OverallProcessState::Stopped;
    } else if (command == QStringLiteral("EMERGENCY_STOP")) {
        for (auto& process : process_runtime_) {
            if (process.status.connection_state != mqtt::ConnectionState::kOffline &&
                process.status.connection_state != mqtt::ConnectionState::kUnknown) {
                process.status.current_state = QStringLiteral("EMERGENCY_STOP");
                process.status.updated_at = timestamp;
            }
        }
        command_override_ = OverallProcessState::EmergencyStop;
        command_override_stage_ = stage;
        command_override_detail_ = overall_.detail;
        overall_.state = OverallProcessState::EmergencyStop;
    } else if (command == QStringLiteral("RECOVERY")) {
        for (auto& process : process_runtime_) {
            if (process.status.connection_state != mqtt::ConnectionState::kOffline &&
                process.status.connection_state != mqtt::ConnectionState::kUnknown) {
                process.status.current_state = QStringLiteral("STOPPED");
                process.status.error_code.clear();
                process.status.has_error = false;
                process.status.has_warning = false;
                process.status.updated_at = timestamp;
            }
        }
        command_override_ = OverallProcessState::Stopped;
        command_override_stage_ = QStringLiteral("복구 완료 · 시작 대기");
        command_override_detail_ = overall_.detail;
        overall_.state = OverallProcessState::Stopped;
        overall_.stage = command_override_stage_;
    }
}

int OperationsDashboardState::processIndexForDevice(const QString& device_id) const {
    const auto iterator = process_index_by_device_.constFind(device_id);
    return iterator == process_index_by_device_.cend() ? -1 : iterator.value();
}

int OperationsDashboardState::processIndexForEvent(mqtt::MessageType type) const {
    QString key;
    switch (type) {
        case mqtt::MessageType::kBoxDetected:
        case mqtt::MessageType::kWorkCreated:
            key = QString::fromLatin1(kInputProcessKey);
            break;
        case mqtt::MessageType::kPositionDetected:
        case mqtt::MessageType::kBarcodeDetected:
        case mqtt::MessageType::kProductImage:
        case mqtt::MessageType::kProductInfo:
            key = QString::fromLatin1(kVisionProcessKey);
            break;
        case mqtt::MessageType::kDestinationSet:
            key = QString::fromLatin1(kSortingProcessKey);
            break;
        case mqtt::MessageType::kWorkCompleted:
            key = QString::fromLatin1(kLineTracerProcessKey);
            break;
        default:
            return -1;
    }
    const auto iterator = process_index_by_key_.constFind(key);
    return iterator == process_index_by_key_.cend() ? -1 : iterator.value();
}

bool OperationsDashboardState::updateProcessWork(ProcessRuntime& process, const QString& work_id) {
    if (work_id.isEmpty()) {
        retireProcessWork(process, process.status.work_id);
        process.status.work_id.clear();
        process.status.destination.clear();
        process.status.work_completed = false;
        return true;
    }
    if (!mqtt::IsValidTopicLevel(work_id.toStdString()) || process.retired_work_ids.contains(work_id)) {
        return false;
    }
    if (!process.status.work_id.isEmpty() && process.status.work_id != work_id) {
        retireProcessWork(process, process.status.work_id);
    }
    if (process.status.work_id != work_id) {
        process.status.destination.clear();
        process.status.work_completed = false;
    }
    process.status.work_id = work_id;
    return true;
}

void OperationsDashboardState::retireProcessWork(ProcessRuntime& process, const QString& work_id) {
    if (work_id.isEmpty() || process.retired_work_ids.contains(work_id)) {
        return;
    }
    process.retired_work_ids.insert(work_id);
    process.retired_work_order.enqueue(work_id);
    while (process.retired_work_order.size() > kMaximumRetiredWorksPerProcess) {
        process.retired_work_ids.remove(process.retired_work_order.dequeue());
    }
}

void OperationsDashboardState::publishProcessSnapshots() {
    process_snapshots_.clear();
    process_snapshots_.reserve(process_runtime_.size());
    for (const auto& runtime : process_runtime_) {
        process_snapshots_.append(runtime.status);
    }
}

void OperationsDashboardState::resetForMqttTransition(const QString& current_state, const QString& detail,
                                                      const QDateTime& timestamp) {
    for (auto& process : process_runtime_) {
        process.status.connection_state = mqtt::ConnectionState::kUnknown;
        process.status.current_state = current_state;
        process.status.work_id.clear();
        process.status.destination.clear();
        process.status.departure_position.reset();
        process.status.target_position.reset();
        process.status.confirmed_position.reset();
        process.status.movement_state.clear();
        process.status.work_completed = false;
        process.status.error_code.clear();
        process.status.has_error = false;
        process.status.has_warning = false;
        process.status.updated_at = timestamp;
        ResetSensors(process.status);
        process.last_received_at = {};
        process.last_device_message_at = {};
        process.application_messages = {};
        process.transport_messages = {};
        process.last_event_at = {};
    }

    overall_.state = OverallProcessState::Idle;
    overall_.stage = QStringLiteral("공정 상태 수신 대기");
    overall_.detail = detail;
    overall_.updated_at = timestamp;
    overall_.active_unit_count = 0;
    overall_.active_work_count = 0;
    last_completion_at_ = {};
    last_completion_detail_.clear();
    command_override_.reset();
    command_override_stage_.clear();
    command_override_detail_.clear();
    destination_by_work_id_.clear();
    destination_work_order_.clear();
    publishProcessSnapshots();
}

}  // namespace logistics::control_center
