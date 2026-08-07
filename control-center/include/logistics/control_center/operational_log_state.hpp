#pragma once

#include <QDateTime>
#include <QList>
#include <QQueue>
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
    static constexpr qsizetype kPageSize = 100;
    static constexpr qsizetype kDefaultMaximumEntries = 500;

    explicit OperationalLogState(qsizetype maximum_entries = kDefaultMaximumEntries);

    [[nodiscard]] OperationalLogUpdateResult applyEnvelope(const QString& topic, const QJsonObject& envelope);
    void appendLocal(OperationalLogSeverity severity, const QString& device_id, const QString& category,
                     const QString& code, const QString& message, const QDateTime& occurred_at = {});
    [[nodiscard]] QList<OperationalLogEntry> appendOlderEntries(QList<OperationalLogEntry> entries);
    [[nodiscard]] bool acknowledge(const QString& id);
    [[nodiscard]] int acknowledgeAllAlerts();
    [[nodiscard]] QList<OperationalLogEntry> filteredEntries(const OperationalLogFilter& filter) const;
    [[nodiscard]] const QList<OperationalLogEntry>& entries() const noexcept;
    [[nodiscard]] int unacknowledgedCount() const noexcept;
    [[nodiscard]] int activeAlertCount() const noexcept;
    [[nodiscard]] qsizetype maximumEntries() const noexcept;
    [[nodiscard]] qsizetype processedMessageIdCount() const noexcept;
    void setMaximumEntries(qsizetype maximum_entries);

private:
    void append(OperationalLogEntry entry);
    void rememberProcessedMessageId(const QString& id);
    void trimProcessedMessageIds();

    QList<OperationalLogEntry> entries_;
    QSet<QString> processed_message_ids_;
    QQueue<QString> processed_message_id_order_;
    qsizetype maximum_entries_{ kDefaultMaximumEntries };
    quint64 local_sequence_{ 0 };
};

[[nodiscard]] QString OperationalSeverityLabel(OperationalLogSeverity severity);

}  // namespace logistics::control_center
