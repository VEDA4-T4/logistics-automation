#include "logistics/control_center/ui_dialog.hpp"

#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <cassert>

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    logistics::control_center::ApplyDialogTheme(application);

    assert(application.styleSheet().contains(QStringLiteral("QMessageBox")));

    QTimer::singleShot(0, []() {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        assert(dialog != nullptr);
        assert(dialog->objectName() == QStringLiteral("confirmationDialog"));
        assert(dialog->styleSheet().contains(QStringLiteral("#181818")));
        auto* accept = dialog->findChild<QPushButton*>(QStringLiteral("primaryDialogButton"));
        auto* cancel = dialog->findChild<QPushButton*>(QStringLiteral("cancelDialogButton"));
        assert(accept != nullptr && accept->text() == QStringLiteral("공정 시작"));
        assert(cancel != nullptr && cancel->text() == QStringLiteral("취소"));
        accept->click();
    });
    assert(logistics::control_center::ShowConfirmationDialog(nullptr, QStringLiteral("공정 제어 확인"),
                                                             QStringLiteral("공정을 시작하시겠습니까?"),
                                                             QStringLiteral("공정 시작")));

    QTimer::singleShot(0, []() {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        assert(dialog != nullptr);
        assert(dialog->objectName() == QStringLiteral("warningDialog"));
        auto* accept = dialog->findChild<QPushButton*>(QStringLiteral("primaryDialogButton"));
        assert(accept != nullptr && accept->text() == QStringLiteral("확인"));
        accept->click();
    });
    logistics::control_center::ShowWarningDialog(nullptr, QStringLiteral("설정 확인"),
                                                 QStringLiteral("설정 파일을 확인하세요."));
    return 0;
}
