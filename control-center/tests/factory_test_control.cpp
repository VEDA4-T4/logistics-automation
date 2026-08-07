#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <cassert>
#include <functional>

#include "logistics/control_center/factory_top_view.hpp"

namespace {

using logistics::control_center::FactoryTopViewWidget;
using logistics::control_center::LineTracerPositionStatus;
using logistics::control_center::OverallProcessState;
using logistics::control_center::ProcessUnitStatus;

class FactoryTestControl final : public QWidget {
public:
    FactoryTestControl() {
        setObjectName(QStringLiteral("factoryTestControlWindow"));
        setWindowTitle(QStringLiteral("공정 탑뷰 테스트 제어"));
        resize(1100, 620);

        auto* layout = new QHBoxLayout(this);
        top_view_ = new FactoryTopViewWidget(this);
        top_view_->setObjectName(QStringLiteral("factoryTestTopView"));
        layout->addWidget(top_view_, 1);

        auto* controls = new QWidget(this);
        controls->setMaximumWidth(300);
        auto* control_layout = new QVBoxLayout(controls);
        control_layout->addWidget(new QLabel(QStringLiteral("테스트 빌드 전용 · MQTT 전송 없음"), controls));
        work_id_ = new QLineEdit(QStringLiteral("TEST-WORK-001"), controls);
        work_id_->setPlaceholderText(QStringLiteral("작업 ID"));
        control_layout->addWidget(work_id_);

        addButton(control_layout, QStringLiteral("투입 상자 이동"), QStringLiteral("testInputButton"),
                  [this]() { activate(QStringLiteral("input"), QStringLiteral("BUSY")); });
        addButton(control_layout, QStringLiteral("비전 인식"), QStringLiteral("testVisionButton"),
                  [this]() { activate(QStringLiteral("vision"), QStringLiteral("VISION_PROCESSING")); });
        addButton(control_layout, QStringLiteral("그리퍼 픽업"), QStringLiteral("testGripperPickButton"),
                  [this]() { activate(QStringLiteral("gripper"), QStringLiteral("PICKING")); });
        addButton(control_layout, QStringLiteral("그리퍼 이송"), QStringLiteral("testGripperTransferButton"),
                  [this]() { activate(QStringLiteral("gripper"), QStringLiteral("TRANSFERRING")); });
        addButton(control_layout, QStringLiteral("그리퍼 배치"), QStringLiteral("testGripperPlaceButton"),
                  [this]() { activate(QStringLiteral("gripper"), QStringLiteral("PLACED")); });

        addRouteRow(control_layout, QStringLiteral("분류 위치"), [this](int route) {
            activate(QStringLiteral("sorting"), QStringLiteral("SORTING"));
            auto& sorting = process(QStringLiteral("sorting"));
            sorting.sensors[route - 1].measurement_status = QStringLiteral("DETECTED");
            render();
        });
        addRouteRow(control_layout, QStringLiteral("라인 출발"), [this](int route) { departure_ = route; });
        addRouteRow(control_layout, QStringLiteral("라인 도착"), [this](int route) { destination_ = route; });
        addButton(control_layout, QStringLiteral("라인 이동 시작"), QStringLiteral("testLineStartButton"), [this]() {
            activate(QStringLiteral("linetracer"), QStringLiteral("FOLLOWING_LINE"));
            auto& line = process(QStringLiteral("linetracer"));
            line.departure_position = position(QStringLiteral("DEPARTURE"), departure_);
            line.target_position = position(QStringLiteral("DESTINATION"), destination_);
            line.confirmed_position = line.departure_position;
            line.movement_state = QStringLiteral("MOVING");
            render();
        });
        addButton(control_layout, QStringLiteral("도면 한 단계 이동"), QStringLiteral("testAdvanceButton"),
                  [this]() { top_view_->advanceAnimationsForTest(); });
        addButton(control_layout, QStringLiteral("비상정지"), QStringLiteral("testEmergencyButton"), [this]() {
            overall_state_ = OverallProcessState::EmergencyStop;
            render();
        });
        addButton(control_layout, QStringLiteral("복구 후 대기"), QStringLiteral("testRecoveryButton"), [this]() {
            resetProcesses();
            overall_state_ = OverallProcessState::Stopped;
            render();
        });
        addButton(control_layout, QStringLiteral("초기화"), QStringLiteral("testResetButton"), [this]() {
            resetProcesses();
            overall_state_ = OverallProcessState::Idle;
            render();
        });
        control_layout->addStretch();
        layout->addWidget(controls);

        processes_ = {
            makeProcess(QStringLiteral("input"), QStringLiteral("투입 컨베이어"), QStringLiteral("PI-INPUT-01")),
            makeProcess(QStringLiteral("vision"), QStringLiteral("비전 처리"), QStringLiteral("PI-VISION-01")),
            makeProcess(QStringLiteral("gripper"), QStringLiteral("그리퍼 이송"), QStringLiteral("PI-GRIPPER-01")),
            makeProcess(QStringLiteral("sorting"), QStringLiteral("분류 컨베이어"), QStringLiteral("PI-SORTING-01")),
            makeProcess(QStringLiteral("linetracer"), QStringLiteral("라인트레이서"), QStringLiteral("PI-LT-01")),
        };
        process(QStringLiteral("input"))
            .sensors.append({ .sensor_id = 1,
                              .display_name = QStringLiteral("S1"),
                              .measurement_status = QStringLiteral("UNKNOWN"),
                              .distance_cm = -1,
                              .updated_at = {} });
        for (int route = 1; route <= 3; ++route) {
            process(QStringLiteral("sorting"))
                .sensors.append({ .sensor_id = route,
                                  .display_name = QStringLiteral("US%1").arg(route + 1),
                                  .measurement_status = QStringLiteral("UNKNOWN"),
                                  .distance_cm = -1,
                                  .updated_at = {} });
        }
        resetProcesses();
        render();
    }

private:
    static ProcessUnitStatus makeProcess(const QString& key, const QString& name, const QString& device_id) {
        ProcessUnitStatus value;
        value.key = key;
        value.display_name = name;
        value.device_id = device_id;
        value.connection_state = logistics::contracts::mqtt::ConnectionState::kOnline;
        return value;
    }

    static LineTracerPositionStatus position(const QString& area, int route) {
        return { .area = area, .location = QString(QChar(QLatin1Char('A').unicode() + route - 1)) };
    }

    void addButton(QVBoxLayout* layout, const QString& text, const QString& object_name,
                   const std::function<void()>& action) {
        auto* button = new QPushButton(text, this);
        button->setObjectName(object_name);
        connect(button, &QPushButton::clicked, this, action);
        layout->addWidget(button);
    }

    void addRouteRow(QVBoxLayout* layout, const QString& title, const std::function<void(int)>& action) {
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(title, this));
        for (int route = 1; route <= 3; ++route) {
            auto* button = new QPushButton(QString(QChar(QLatin1Char('A').unicode() + route - 1)), this);
            connect(button, &QPushButton::clicked, this, [action, route]() { action(route); });
            row->addWidget(button);
        }
        layout->addLayout(row);
    }

    ProcessUnitStatus& process(const QString& key) {
        for (auto& value : processes_) {
            if (value.key == key) {
                return value;
            }
        }
        qFatal("Unknown test process");
    }

    void resetProcesses() {
        for (auto& value : processes_) {
            value.current_state = QStringLiteral("IDLE");
            value.work_id.clear();
            value.work_completed = false;
            value.departure_position.reset();
            value.target_position.reset();
            value.confirmed_position.reset();
            value.movement_state.clear();
            for (auto& sensor : value.sensors) {
                sensor.measurement_status = QStringLiteral("CLEAR");
            }
        }
    }

    void activate(const QString& key, const QString& state) {
        resetProcesses();
        auto& value = process(key);
        value.current_state = state;
        value.work_id =
            work_id_->text().trimmed().isEmpty() ? QStringLiteral("TEST-WORK-001") : work_id_->text().trimmed();
        overall_state_ = OverallProcessState::Running;
        render();
    }

    void render() {
        top_view_->setProcesses(processes_, overall_state_);
    }

    FactoryTopViewWidget* top_view_{ nullptr };
    QLineEdit* work_id_{ nullptr };
    QList<ProcessUnitStatus> processes_;
    OverallProcessState overall_state_{ OverallProcessState::Idle };
    int departure_{ 1 };
    int destination_{ 1 };
};

}  // namespace

int main(int argc, char* argv[]) {
    const bool self_test = argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--self-test");
    if (self_test) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication application(argc, argv);
    FactoryTestControl control;
    control.show();
    QApplication::processEvents();
    if (!self_test) {
        return application.exec();
    }

    auto* view = control.findChild<FactoryTopViewWidget*>(QStringLiteral("factoryTestTopView"));
    auto click = [&control](const QString& name) {
        auto* button = control.findChild<QPushButton*>(name);
        assert(button != nullptr);
        button->click();
    };
    assert(view != nullptr);
    click(QStringLiteral("testInputButton"));
    assert(view->nodeColor(QStringLiteral("input")) == QColor(QStringLiteral("#75beff")));
    click(QStringLiteral("testAdvanceButton"));
    click(QStringLiteral("testLineStartButton"));
    assert(view->nodeColor(QStringLiteral("linetracer")) == QColor(QStringLiteral("#75beff")));
    click(QStringLiteral("testEmergencyButton"));
    click(QStringLiteral("testAdvanceButton"));
    assert(view->nodeColor(QStringLiteral("input")) == QColor(QStringLiteral("#f14c4c")));
    assert(view->nodeOpacity(QStringLiteral("input")) == 1.0);
    click(QStringLiteral("testRecoveryButton"));
    assert(view->nodeColor(QStringLiteral("input")) == QColor(QStringLiteral("#ffffff")));
    return 0;
}
