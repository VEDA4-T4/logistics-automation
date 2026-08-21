#include "logistics/control_center/operational_log_panel.hpp"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTableView>
#include <cassert>
#include <utility>

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);

    using logistics::control_center::OperationalLogPanel;
    using logistics::control_center::OperationalLogSeverity;
    using logistics::control_center::OperationalLogState;

    OperationalLogState state;
    state.appendLocal(OperationalLogSeverity::Info, QStringLiteral("central-server"), QStringLiteral("통신"),
                      QStringLiteral("CONNECTED"), QStringLiteral("연결됨"));
    state.appendLocal(OperationalLogSeverity::Warning, QStringLiteral("PI-VISION-01"), QStringLiteral("인식 경고"),
                      QStringLiteral("BARCODE_FAILED"), QStringLiteral("바코드 인식 실패"));
    state.appendLocal(OperationalLogSeverity::Error, QStringLiteral("PI-SORTING-01"), QStringLiteral("장치 오류"),
                      QStringLiteral("SENSOR_TIMEOUT"), QStringLiteral("센서 응답 없음"));
    state.appendLocal(OperationalLogSeverity::Critical, QStringLiteral("central-server"), QStringLiteral("통신 장애"),
                      QStringLiteral("MQTT_AUTH_ERROR"),
                      QStringLiteral("인증 정보가 올바르지 않아 중앙 서버 연결을 완료하지 못했습니다."));

    OperationalLogPanel panel;
    panel.resize(390, 500);
    panel.setEntryPageProvider(
        [&state](qsizetype offset, qsizetype limit) { return state.entries().mid(offset, limit); });
    panel.setEntryCountProvider([&state]() { return state.entries().size(); });
    panel.reloadEntries(state.activeAlertCount());
    panel.setAcknowledgeHandler([&state, &panel](const QString& id) {
        assert(state.acknowledge(id));
        panel.setEntryAcknowledged(id, state.activeAlertCount());
    });
    panel.setAcknowledgeAllHandler([&state, &panel]() {
        assert(state.acknowledgeAllAlerts() > 0);
        panel.setAllAlertsAcknowledged(state.activeAlertCount());
    });
    panel.show();
    application.processEvents();

    auto* table = panel.findChild<QTableView*>(QStringLiteral("operationalLogTable"));
    auto* severity = panel.findChild<QComboBox*>(QStringLiteral("logSeverityFilter"));
    auto* query = panel.findChild<QLineEdit*>(QStringLiteral("logQueryFilter"));
    auto* unacknowledged = panel.findChild<QCheckBox*>(QStringLiteral("logUnacknowledgedOnly"));
    auto* acknowledge_all = panel.findChild<QPushButton*>(QStringLiteral("acknowledgeAllLogsButton"));
    auto* empty_state = panel.findChild<QLabel*>(QStringLiteral("operationalLogEmptyState"));
    auto* result_count = panel.findChild<QLabel*>(QStringLiteral("operationalLogResultCount"));
    assert(table != nullptr && severity != nullptr && query != nullptr && unacknowledged != nullptr &&
           acknowledge_all != nullptr && empty_state != nullptr && result_count != nullptr);
    assert(panel.findChild<QPushButton*>(QStringLiteral("acknowledgeLogButton")) == nullptr);
    assert(panel.findChild<QPushButton*>(QStringLiteral("showAllUnacknowledgedLogsButton")) == nullptr);
    assert(table->model()->rowCount() == 4);
    assert(table->model()->columnCount() == 4);
    assert(table->verticalHeader()->defaultSectionSize() == 34);
    assert(table->verticalScrollMode() == QAbstractItemView::ScrollPerPixel);
    assert(table->rowHeight(0) == 34);
    assert(table->horizontalHeader()->sectionResizeMode(3) == QHeaderView::Stretch);
    assert(severity->view()->styleSheet().isEmpty());
    panel.resize(640, 180);
    application.processEvents();
    auto* table_surface = panel.findChild<QWidget*>(QStringLiteral("operationalLogTableSurface"));
    assert(table_surface != nullptr && table_surface->height() >= 60);
    assert(table->height() >= table->horizontalHeader()->height() + table->rowHeight(0));

    severity->setCurrentIndex(severity->findData(static_cast<int>(OperationalLogSeverity::Error)));
    application.processEvents();
    assert(table->model()->rowCount() == 1);
    assert(table->model()->index(0, 2).data().toString() == QStringLiteral("PI-SORTING-01"));

    severity->setCurrentIndex(0);
    query->setText(QStringLiteral("VISION"));
    application.processEvents();
    assert(table->model()->rowCount() == 1);
    assert(table->model()->index(0, 3).data().toString().contains(QStringLiteral("바코드 인식 실패")));

    query->clear();
    query->setText(QStringLiteral("NO-SUCH-OPERATIONAL-LOG"));
    application.processEvents();
    assert(table->model()->rowCount() == 0);
    assert(empty_state->isVisible());
    assert(table->isVisible());
    assert(empty_state->parentWidget() == table->viewport());
    const QRect empty_state_rect(empty_state->mapTo(table, QPoint{}), empty_state->size());
    assert(!empty_state_rect.intersects(table->horizontalHeader()->geometry()));
    assert(table->rect().contains(empty_state_rect));
    assert(severity->isEnabled() && query->isEnabled() && unacknowledged->isEnabled());
    query->clear();
    application.processEvents();
    assert(table->model()->rowCount() == 4);
    assert(!empty_state->isVisible());

    table->doubleClicked(table->model()->index(0, 3));
    application.processEvents();
    assert(state.activeAlertCount() == 1);
    auto* detail_dialog = panel.findChild<QDialog*>(QStringLiteral("operationalLogDetailDialog"));
    assert(detail_dialog != nullptr && detail_dialog->isVisible());
    assert(detail_dialog->styleSheet().isEmpty());
    auto* detail_message = detail_dialog->findChild<QPlainTextEdit*>(QStringLiteral("operationalLogDetailMessage"));
    assert(detail_message != nullptr && detail_message->toPlainText().contains(QStringLiteral("인증 정보")));
    detail_dialog->close();
    application.processEvents();

    table->clicked(table->model()->index(1, 3));
    application.processEvents();
    assert(state.activeAlertCount() == 0);
    assert(!acknowledge_all->isEnabled());
    assert(table->model()->index(1, 3).data(Qt::ForegroundRole).value<QColor>() == QColor(QStringLiteral("#777777")));

    unacknowledged->setChecked(true);
    application.processEvents();
    assert(table->model()->rowCount() == 2);
    table->clicked(table->model()->index(0, 3));
    application.processEvents();
    assert(table->model()->rowCount() == 1);

    unacknowledged->setChecked(false);
    for (qsizetype index = 0; index < OperationalLogState::kPageSize; ++index) {
        state.appendLocal(OperationalLogSeverity::Info, QStringLiteral("PI-LOAD-01"), QStringLiteral("부하 테스트"),
                          QStringLiteral("LOAD"), QStringLiteral("로그 %1").arg(index));
    }
    panel.reloadEntries(state.activeAlertCount());
    application.processEvents();
    assert(table->model()->rowCount() == OperationalLogState::kPageSize);
    assert(result_count->text().contains(QStringLiteral("100건 표시")));
    assert(result_count->text().contains(QStringLiteral("전체 104건")));
    assert(panel.canLoadOlderEntries());
    panel.requestOlderEntries();
    application.processEvents();
    assert(table->model()->rowCount() == OperationalLogState::kPageSize + 4);
    assert(!panel.canLoadOlderEntries());
    table->doubleClicked(table->model()->index(0, 3));
    application.processEvents();
    detail_dialog = panel.findChild<QDialog*>(QStringLiteral("operationalLogDetailDialog"));
    assert(detail_dialog != nullptr && detail_dialog->isVisible());
    detail_dialog->close();
    application.processEvents();

    table->verticalScrollBar()->setValue(100);
    const auto previous_scroll_value = table->verticalScrollBar()->value();
    QList<logistics::control_center::OperationalLogEntry> batch;
    for (int index = 0; index < 3; ++index) {
        state.appendLocal(OperationalLogSeverity::Info, QStringLiteral("PI-BATCH-01"), QStringLiteral("배치 테스트"),
                          QStringLiteral("BATCH"), QStringLiteral("배치 로그 %1").arg(index));
        batch.prepend(state.entries().front());
    }
    panel.prependEntries(batch, state.activeAlertCount());
    application.processEvents();
    assert(table->model()->rowCount() == OperationalLogState::kPageSize + 7);
    assert(table->model()->index(0, 3).data().toString().contains(QStringLiteral("배치 로그 2")));
    assert(table->verticalScrollBar()->value() == previous_scroll_value + 3 * 34);
    assert(result_count->text().contains(QStringLiteral("새 로그 3건")));

    table->scrollToTop();
    application.processEvents();
    assert(!result_count->text().contains(QStringLiteral("새 로그")));

    QList<logistics::control_center::OperationalLogEntry> overflow_batch;
    for (int index = 0; index < 600; ++index) {
        state.appendLocal(OperationalLogSeverity::Info, QStringLiteral("PI-BATCH-02"), QStringLiteral("한도 테스트"),
                          QStringLiteral("BATCH_LIMIT"), QStringLiteral("한도 로그 %1").arg(index));
        overflow_batch.prepend(state.entries().front());
    }
    panel.prependEntries(overflow_batch, state.activeAlertCount());
    application.processEvents();
    assert(table->model()->rowCount() == OperationalLogState::kPageSize * 2);
    assert(table->model()->index(0, 3).data().toString().contains(QStringLiteral("한도 로그 599")));
    const auto capped_row_count = table->model()->rowCount();
    panel.prependEntries({ overflow_batch.front(), overflow_batch.front() }, state.activeAlertCount());
    application.processEvents();
    assert(table->model()->rowCount() == capped_row_count);

    state = OperationalLogState{};
    state.appendLocal(OperationalLogSeverity::Info, QStringLiteral("PI-LIVE-01"), QStringLiteral("실시간"),
                      QStringLiteral("LIVE"), QStringLiteral("현재 로그"));
    panel.reloadEntries(state.activeAlertCount());
    int older_page_request_count = 0;
    panel.setOlderEntriesRequestHandler([&older_page_request_count]() { ++older_page_request_count; });
    panel.appendOlderEntries({}, true, state.activeAlertCount());
    assert(panel.canLoadOlderEntries());
    panel.requestOlderEntries();
    assert(older_page_request_count == 1);
    assert(!panel.canLoadOlderEntries());
    const auto older_timestamp = QDateTime::fromString(QStringLiteral("2026-07-01T00:00:00.000Z"), Qt::ISODateWithMs);
    QList<logistics::control_center::OperationalLogEntry> older_entries{
        { .id = QStringLiteral("HISTORY-1"),
          .occurred_at = older_timestamp,
          .severity = OperationalLogSeverity::Info,
          .device_id = QStringLiteral("PI-HISTORY-01"),
          .category = QStringLiteral("서버 이력"),
          .code = QStringLiteral("DEVICE_STATUS"),
          .message = QStringLiteral("과거 로그 1"),
          .topic = QStringLiteral("server-history"),
          .acknowledged = false },
        { .id = QStringLiteral("HISTORY-2"),
          .occurred_at = older_timestamp.addMSecs(-1),
          .severity = OperationalLogSeverity::Warning,
          .device_id = QStringLiteral("PI-HISTORY-02"),
          .category = QStringLiteral("서버 이력"),
          .code = QStringLiteral("REJECTED"),
          .message = QStringLiteral("과거 로그 2"),
          .topic = QStringLiteral("server-history"),
          .acknowledged = false },
    };
    panel.appendOlderEntries(std::move(older_entries), false, state.activeAlertCount());
    application.processEvents();
    assert(table->model()->rowCount() == 3);
    assert(table->model()->index(2, 3).data().toString().contains(QStringLiteral("과거 로그 2")));
    assert(!panel.canLoadOlderEntries());

    state = OperationalLogState{};
    panel.reloadEntries(state.activeAlertCount());
    application.processEvents();
    assert(empty_state != nullptr && empty_state->isVisible());
    assert(table->isVisible());
    assert(empty_state->text() == QStringLiteral("표시할 운영로그 없음"));
    assert(severity->isEnabled() && query->isEnabled() && unacknowledged->isEnabled());

    for (qsizetype index = 0; index < OperationalLogState::kDefaultMaximumEntries + 10; ++index) {
        const auto log_severity = static_cast<OperationalLogSeverity>(index % 4);
        state.appendLocal(log_severity, QStringLiteral("PI-LOAD-01"), QStringLiteral("부하 로그"),
                          QStringLiteral("OVERFLOW_ENTRY"), QStringLiteral("미확인 로그 %1").arg(index));
    }
    panel.reloadEntries(state.activeAlertCount());
    application.processEvents();
    assert(state.unacknowledgedCount() == OperationalLogState::kDefaultMaximumEntries);
    assert(state.activeAlertCount() == OperationalLogState::kDefaultMaximumEntries / 2);
    for (int page = 1; page < OperationalLogState::kDefaultMaximumEntries / OperationalLogState::kPageSize; ++page) {
        assert(panel.canLoadOlderEntries());
        panel.requestOlderEntries();
    }
    assert(table->model()->rowCount() == OperationalLogState::kDefaultMaximumEntries);
    assert(!panel.canLoadOlderEntries());

    QList<logistics::control_center::OperationalLogEntry> first_sliding_history_page;
    QList<logistics::control_center::OperationalLogEntry> second_sliding_history_page;
    for (qsizetype index = 0; index < OperationalLogState::kDefaultMaximumEntries; ++index) {
        first_sliding_history_page.append({
            .id = QStringLiteral("SLIDING-HISTORY-1-%1").arg(index),
            .occurred_at = older_timestamp.addSecs(-index),
            .severity = OperationalLogSeverity::Info,
            .device_id = QStringLiteral("PI-HISTORY-SLIDING"),
            .category = QStringLiteral("server history"),
            .code = QStringLiteral("SLIDING_PAGE_1"),
            .message = QStringLiteral("sliding history page 1 entry %1").arg(index),
            .topic = QStringLiteral("server-history"),
            .acknowledged = false,
        });
        second_sliding_history_page.append({
            .id = QStringLiteral("SLIDING-HISTORY-2-%1").arg(index),
            .occurred_at = older_timestamp.addSecs(-OperationalLogState::kDefaultMaximumEntries - index),
            .severity = OperationalLogSeverity::Info,
            .device_id = QStringLiteral("PI-HISTORY-SLIDING"),
            .category = QStringLiteral("server history"),
            .code = QStringLiteral("SLIDING_PAGE_2"),
            .message = QStringLiteral("sliding history page 2 entry %1").arg(index),
            .topic = QStringLiteral("server-history"),
            .acknowledged = false,
        });
    }
    assert(panel.appendOlderEntries(first_sliding_history_page, true, state.activeAlertCount()) ==
           OperationalLogState::kDefaultMaximumEntries);
    assert(table->model()->rowCount() == OperationalLogState::kDefaultMaximumEntries);
    assert(table->model()->index(0, 3).data().toString().contains(QStringLiteral("sliding history page 1 entry 0")));
    assert(table->model()
               ->index(OperationalLogState::kDefaultMaximumEntries - 1, 3)
               .data()
               .toString()
               .contains(QStringLiteral("sliding history page 1 entry 499")));
    assert(panel.canLoadOlderEntries());
    panel.requestOlderEntries();
    assert(older_page_request_count == 2);
    assert(!panel.canLoadOlderEntries());
    assert(panel.appendOlderEntries(second_sliding_history_page, false, state.activeAlertCount()) ==
           OperationalLogState::kDefaultMaximumEntries);
    application.processEvents();
    assert(table->model()->rowCount() == OperationalLogState::kDefaultMaximumEntries);
    assert(table->model()->index(0, 3).data().toString().contains(QStringLiteral("sliding history page 2 entry 0")));
    assert(table->model()
               ->index(OperationalLogState::kDefaultMaximumEntries - 1, 3)
               .data()
               .toString()
               .contains(QStringLiteral("sliding history page 2 entry 499")));
    assert(!panel.canLoadOlderEntries());

    panel.setAcknowledgeHandler([&panel](const QString& id) { panel.setEntryAcknowledged(id, 0); });
    table->doubleClicked(table->model()->index(0, 3));
    application.processEvents();
    assert(state.unacknowledgedCount() == OperationalLogState::kDefaultMaximumEntries);
    assert(state.activeAlertCount() == OperationalLogState::kDefaultMaximumEntries / 2);
    detail_dialog = panel.findChild<QDialog*>(QStringLiteral("operationalLogDetailDialog"));
    assert(detail_dialog != nullptr && detail_dialog->isVisible());
    detail_message = detail_dialog->findChild<QPlainTextEdit*>(QStringLiteral("operationalLogDetailMessage"));
    assert(detail_message != nullptr &&
           detail_message->toPlainText().contains(QStringLiteral("sliding history page 2 entry 0")));
    detail_dialog->close();
    application.processEvents();

    panel.setMaximumEntries(OperationalLogState::kDefaultMaximumEntries);
    panel.reloadEntries(state.activeAlertCount());
    int model_reset_count = 0;
    int rows_inserted_count = 0;
    int rows_removed_count = 0;
    QObject::connect(table->model(), &QAbstractItemModel::modelReset, [&model_reset_count]() { ++model_reset_count; });
    QObject::connect(table->model(), &QAbstractItemModel::rowsInserted,
                     [&rows_inserted_count]() { ++rows_inserted_count; });
    QObject::connect(table->model(), &QAbstractItemModel::rowsRemoved,
                     [&rows_removed_count]() { ++rows_removed_count; });
    QList<logistics::control_center::OperationalLogEntry> incremental_batch;
    constexpr int kLoadTestEntryCount = 5000;
    constexpr int kLoadTestBatchSize = 200;
    for (int index = 0; index < kLoadTestEntryCount; ++index) {
        state.appendLocal(OperationalLogSeverity::Info, QStringLiteral("PI-LOAD-02"), QStringLiteral("증분 부하"),
                          QStringLiteral("INCREMENTAL"), QStringLiteral("증분 로그 %1").arg(index));
        incremental_batch.prepend(state.entries().front());
        if (incremental_batch.size() == kLoadTestBatchSize) {
            panel.prependEntries(incremental_batch, state.activeAlertCount());
            incremental_batch.clear();
        }
    }
    application.processEvents();
    assert(model_reset_count == 0);
    assert(rows_inserted_count == kLoadTestEntryCount / kLoadTestBatchSize);
    assert(rows_removed_count == kLoadTestEntryCount / kLoadTestBatchSize);
    assert(table->model()->rowCount() == OperationalLogState::kPageSize);
    assert(table->model()->index(0, 3).data().toString().contains(QStringLiteral("증분 로그 4999")));
    assert(panel.findChild<QDialog*>(QStringLiteral("unacknowledgedOperationalLogDialog")) == nullptr);

    constexpr qsizetype kPagedLiveLogCount = 1200;
    constexpr qsizetype kPagedLiveBatchSize = 200;
    state = OperationalLogState{ 5000 };
    panel.setMaximumEntries(5000);
    panel.setEntryPageProvider(
        [&state](qsizetype offset, qsizetype limit) { return state.entries().mid(offset, limit); });
    panel.reloadEntries(state.activeAlertCount());
    for (qsizetype index = 0; index < kPagedLiveLogCount; ++index) {
        state.appendLocal(OperationalLogSeverity::Info, QStringLiteral("PI-PAGED-LIVE-01"),
                          QStringLiteral("실시간 페이지"), QStringLiteral("PAGED_LIVE"),
                          QStringLiteral("연속 로그 %1").arg(index));
        incremental_batch.prepend(state.entries().front());
        if (incremental_batch.size() == kPagedLiveBatchSize) {
            panel.prependEntries(incremental_batch, state.activeAlertCount());
            incremental_batch.clear();
        }
    }
    application.processEvents();
    assert(table->model()->rowCount() == OperationalLogState::kPageSize);
    for (qsizetype page = 1; page < kPagedLiveLogCount / OperationalLogState::kPageSize; ++page) {
        assert(panel.canLoadOlderEntries());
        panel.requestOlderEntries();
    }
    assert(table->model()->rowCount() == kPagedLiveLogCount);
    assert(result_count->text().contains(QStringLiteral("1200건 표시")));
    assert(!result_count->text().contains(QStringLiteral("전체 1200건")));
    assert(!panel.canLoadOlderEntries());
    for (qsizetype row = 0; row < kPagedLiveLogCount; ++row) {
        const auto expected_index = kPagedLiveLogCount - row - 1;
        assert(table->model()
                   ->index(static_cast<int>(row), 3)
                   .data()
                   .toString()
                   .contains(QStringLiteral("연속 로그 %1").arg(expected_index)));
    }
    return 0;
}
