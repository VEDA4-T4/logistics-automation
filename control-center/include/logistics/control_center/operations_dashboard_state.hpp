#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QQueue>
#include <QSet>
#include <QString>
#include <optional>

#include "logistics/contracts/mqtt_message.hpp"

namespace logistics::control_center {

inline constexpr auto kInputProcessKey = "input";
inline constexpr auto kVisionProcessKey = "vision";
inline constexpr auto kGripperProcessKey = "gripper";
inline constexpr auto kSortingProcessKey = "sorting";
inline constexpr auto kLineTracerProcessKey = "linetracer";

[[nodiscard]] inline bool IsSensorStaleErrorCode(const QString& error_code) {
    auto normalized = error_code.trimmed().toUpper();
    normalized.replace(QLatin1Char('_'), QLatin1Char('-'));
    return normalized == QStringLiteral("ERR-HEALTH-SENSOR-STALE");
}

enum class OverallProcessState {
    Idle,
    Running,
    Completed,
    Stopped,
    Error,
    EmergencyStop,
    Recovery,
};

struct ProcessDefinition {
    QString key;
    QString display_name;
    QString device_id;
};

struct SensorUnitStatus {
    int sensor_id{ 0 };
    QString display_name;
    QString measurement_status{ QStringLiteral("UNKNOWN") };
    int distance_cm{ -1 };
    QDateTime updated_at;
};

struct ProcessUnitStatus {
    QString key;
    QString display_name;
    QString device_id;
    logistics::contracts::mqtt::ConnectionState connection_state{
        logistics::contracts::mqtt::ConnectionState::kUnknown
    };
    QString current_state{ QStringLiteral("상태 수신 대기") };
    QString work_id;
    QString error_code;
    QDateTime updated_at;
    bool has_error{ false };
    bool has_warning{ false };
    QList<SensorUnitStatus> sensors;
};

struct ProcessDashboardStatus {
    OverallProcessState state{ OverallProcessState::Idle };
    QString stage{ QStringLiteral("공정 상태 수신 대기") };
    QString detail;
    QDateTime updated_at;
    int active_unit_count{ 0 };
    int active_work_count{ 0 };
};

struct DashboardUpdateResult {
    bool handled{ false };
    bool applied{ false };
    QString error;
};

[[nodiscard]] QList<ProcessDefinition> DefaultProcessDefinitions();

class OperationsDashboardState final {
public:
    OperationsDashboardState();

    void configureProcesses(const QList<ProcessDefinition>& definitions);
    void markMqttConnectedAwaitingStatus(const QDateTime& timestamp);
    void markMqttDisconnected(const QDateTime& timestamp);
    [[nodiscard]] bool expireStaleProcesses(const QDateTime& timestamp);
    [[nodiscard]] DashboardUpdateResult applyEnvelope(const QJsonObject& envelope, const QDateTime& received_at = {},
                                                      bool apply_command_to_overall = true);
    [[nodiscard]] const QList<ProcessUnitStatus>& processes() const noexcept;
    [[nodiscard]] const ProcessDashboardStatus& overall() const noexcept;

private:
    struct ProcessRuntime {
        ProcessUnitStatus status;
        QDateTime last_received_at;
        QDateTime last_event_at;
        QSet<QString> retired_work_ids;
        QQueue<QString> retired_work_order;
    };

    void rememberMessage(const QString& message_id);
    void updateOverall(const QDateTime& timestamp);
    void updateOverallForCommand(const QJsonObject& data, const QDateTime& timestamp);
    [[nodiscard]] int processIndexForDevice(const QString& device_id) const;
    [[nodiscard]] int processIndexForEvent(logistics::contracts::mqtt::MessageType type) const;
    [[nodiscard]] bool updateProcessWork(ProcessRuntime& process, const QString& work_id);
    void retireProcessWork(ProcessRuntime& process, const QString& work_id);
    void publishProcessSnapshots();
    void resetForMqttTransition(const QString& current_state, const QString& detail, const QDateTime& timestamp);

    QList<ProcessRuntime> process_runtime_;
    QList<ProcessUnitStatus> process_snapshots_;
    QHash<QString, int> process_index_by_device_;
    QHash<QString, int> process_index_by_key_;
    ProcessDashboardStatus overall_;
    QSet<QString> processed_message_ids_;
    QQueue<QString> processed_message_order_;
    QDateTime last_completion_at_;
    QString last_completion_detail_;
    std::optional<OverallProcessState> command_override_;
    QString command_override_stage_;
    QString command_override_detail_;
};

}  // namespace logistics::control_center
