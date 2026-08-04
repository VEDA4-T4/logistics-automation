#include "logistics/control_center/operational_log_panel.hpp"

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
#include <QTableWidget>
#include <cassert>

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
    panel.setState(state);
    panel.setAcknowledgeHandler([&state, &panel](const QString& id) {
        assert(state.acknowledge(id));
        panel.setEntryAcknowledged(id);
    });
    panel.setAcknowledgeAllHandler([&state, &panel]() {
        assert(state.acknowledgeAllAlerts() > 0);
        panel.setState(state);
    });
    panel.show();
    application.processEvents();

    auto* table = panel.findChild<QTableWidget*>(QStringLiteral("operationalLogTable"));
    auto* severity = panel.findChild<QComboBox*>(QStringLiteral("logSeverityFilter"));
    auto* query = panel.findChild<QLineEdit*>(QStringLiteral("logQueryFilter"));
    auto* unacknowledged = panel.findChild<QCheckBox*>(QStringLiteral("logUnacknowledgedOnly"));
    auto* acknowledge_all = panel.findChild<QPushButton*>(QStringLiteral("acknowledgeAllLogsButton"));
    auto* empty_state = panel.findChild<QLabel*>(QStringLiteral("operationalLogEmptyState"));
    assert(table != nullptr && severity != nullptr && query != nullptr && unacknowledged != nullptr &&
           acknowledge_all != nullptr && empty_state != nullptr);
    assert(panel.findChild<QPushButton*>(QStringLiteral("acknowledgeLogButton")) == nullptr);
    assert(table->rowCount() == 4);
    assert(table->columnCount() == 4);
    assert(table->verticalHeader()->defaultSectionSize() == 34);
    assert(table->rowHeight(0) == 34);
    assert(table->horizontalHeader()->sectionResizeMode(3) == QHeaderView::Stretch);
    assert(severity->view()->styleSheet().isEmpty());

    severity->setCurrentIndex(severity->findData(static_cast<int>(OperationalLogSeverity::Error)));
    application.processEvents();
    assert(table->rowCount() == 1);
    assert(table->item(0, 2)->text() == QStringLiteral("PI-SORTING-01"));

    severity->setCurrentIndex(0);
    query->setText(QStringLiteral("VISION"));
    application.processEvents();
    assert(table->rowCount() == 1);
    assert(table->item(0, 3)->text().contains(QStringLiteral("바코드 인식 실패")));

    query->clear();
    query->setText(QStringLiteral("NO-SUCH-OPERATIONAL-LOG"));
    application.processEvents();
    assert(table->rowCount() == 0);
    assert(empty_state->isVisible());
    assert(severity->isEnabled() && query->isEnabled() && unacknowledged->isEnabled());
    query->clear();
    application.processEvents();
    assert(table->rowCount() == 4);
    assert(!empty_state->isVisible());

    table->cellDoubleClicked(0, 3);
    application.processEvents();
    assert(state.activeAlertCount() == 1);
    auto* detail_dialog = panel.findChild<QDialog*>(QStringLiteral("operationalLogDetailDialog"));
    assert(detail_dialog != nullptr && detail_dialog->isVisible());
    assert(detail_dialog->styleSheet().isEmpty());
    auto* detail_message = detail_dialog->findChild<QPlainTextEdit*>(QStringLiteral("operationalLogDetailMessage"));
    assert(detail_message != nullptr && detail_message->toPlainText().contains(QStringLiteral("인증 정보")));
    detail_dialog->close();
    application.processEvents();

    table->cellClicked(1, 3);
    application.processEvents();
    assert(state.activeAlertCount() == 0);
    assert(!acknowledge_all->isEnabled());
    assert(table->item(1, 3)->foreground().color() == QColor(QStringLiteral("#777777")));

    unacknowledged->setChecked(true);
    application.processEvents();
    assert(table->rowCount() == 2);
    table->cellClicked(0, 3);
    application.processEvents();
    assert(table->rowCount() == 1);

    unacknowledged->setChecked(false);
    for (qsizetype index = 0; index < OperationalLogState::kMaximumEntries; ++index) {
        state.appendLocal(OperationalLogSeverity::Info, QStringLiteral("PI-LOAD-01"), QStringLiteral("부하 테스트"),
                          QStringLiteral("LOAD"), QStringLiteral("로그 %1").arg(index));
    }
    panel.setState(state);
    application.processEvents();
    assert(table->rowCount() == 200);
    table->cellDoubleClicked(0, 3);
    application.processEvents();
    detail_dialog = panel.findChild<QDialog*>(QStringLiteral("operationalLogDetailDialog"));
    assert(detail_dialog != nullptr && detail_dialog->isVisible());

    panel.setState(OperationalLogState{});
    application.processEvents();
    assert(empty_state != nullptr && empty_state->isVisible());
    assert(empty_state->text() == QStringLiteral("표시할 운영 로그가 없습니다"));
    assert(severity->isEnabled() && query->isEnabled() && unacknowledged->isEnabled());
    return 0;
}
