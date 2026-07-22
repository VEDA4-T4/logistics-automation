#include "logistics/control_center/operational_log_panel.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
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

    OperationalLogPanel panel;
    panel.resize(390, 500);
    panel.setState(state);
    panel.setAcknowledgeHandler([&state, &panel](const QString& id) {
        assert(state.acknowledge(id));
        panel.setState(state);
    });
    panel.show();
    application.processEvents();

    auto* table = panel.findChild<QTableWidget*>(QStringLiteral("operationalLogTable"));
    auto* severity = panel.findChild<QComboBox*>(QStringLiteral("logSeverityFilter"));
    auto* query = panel.findChild<QLineEdit*>(QStringLiteral("logQueryFilter"));
    auto* unacknowledged = panel.findChild<QCheckBox*>(QStringLiteral("logUnacknowledgedOnly"));
    auto* acknowledge = panel.findChild<QPushButton*>(QStringLiteral("acknowledgeLogButton"));
    assert(table != nullptr && severity != nullptr && query != nullptr && unacknowledged != nullptr &&
           acknowledge != nullptr);
    assert(table->rowCount() == 3);

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
    table->selectRow(0);
    application.processEvents();
    acknowledge->click();
    unacknowledged->setChecked(true);
    application.processEvents();
    assert(table->rowCount() == 2);
    return 0;
}
