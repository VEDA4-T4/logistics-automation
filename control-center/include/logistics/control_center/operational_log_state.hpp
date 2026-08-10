#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

class QJsonObject;

namespace logistics::control_center {

enum class OperationalLogSeverity {
    Info,
    Warning,
    Error,
    Critical,
};

struct OperationalLogEntry {
    QString id;
    QDateTime occurred_at;
    OperationalLogSeverity severity{ OperationalLogSeverity::Info };
    QString device_id;
    QString category;
    QString code;
    QString message;
    QString topic;
    bool acknowledged{ false };
};

struct OperationalLogFilter {
    bool filter_by_severity{ false };
    OperationalLogSeverity severity{ OperationalLogSeverity::Info };
    QString query;
    bool unacknowledged_only{ false };
};

struct OperationalLogUpdateResult {
    bool handled{ false };
    bool applied{ false };
    QString error;
};

class OperationalLogState final {
public:
    static constexpr qsizetype kMaximumEntries = 500;

    [[nodiscard]] OperationalLogUpdateResult applyEnvelope(const QString& topic, const QJsonObject& envelope);
    void appendLocal(OperationalLogSeverity severity, const QString& device_id, const QString& category,
                     const QString& code, const QString& message, const QDateTime& occurred_at = {});
    [[nodiscard]] bool acknowledge(const QString& id);
    [[nodiscard]] int acknowledgeAllAlerts();
    [[nodiscard]] QList<OperationalLogEntry> filteredEntries(const OperationalLogFilter& filter) const;
    [[nodiscard]] const QList<OperationalLogEntry>& entries() const noexcept;
    [[nodiscard]] int unacknowledgedCount() const noexcept;
    [[nodiscard]] int activeAlertCount() const noexcept;

private:
    void append(OperationalLogEntry entry);

    QList<OperationalLogEntry> entries_;
    QSet<QString> processed_message_ids_;
    QHash<QString, QSet<QString>> active_device_alerts_;
    quint64 local_sequence_{ 0 };
};

[[nodiscard]] QString OperationalSeverityLabel(OperationalLogSeverity severity);

}  // namespace logistics::control_center
