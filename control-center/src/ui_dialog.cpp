#include "logistics/control_center/ui_dialog.hpp"

#include <QApplication>
#include <QMessageBox>
#include <QPushButton>

namespace logistics::control_center {
namespace {

QString DialogStyleSheet() {
    return QStringLiteral(
        "QMessageBox{background:#181818;border:1px solid #333333;}"
        "QMessageBox QLabel{color:#d4d4d4;font-size:12px;}"
        "QMessageBox QLabel#qt_msgbox_label{min-width:360px;padding:8px 4px;}"
        "QMessageBox QLabel#qt_msgboxex_icon_label{min-width:44px;}"
        "QMessageBox QPushButton{min-width:78px;min-height:30px;background:#2d2d30;color:#f0f0f0;"
        "border:1px solid #454545;border-radius:6px;font-size:11px;font-weight:600;padding:2px 12px;}"
        "QMessageBox QPushButton:hover{background:#3a3a3d;border-color:#5a5a5d;}"
        "QMessageBox QPushButton:pressed{background:#252526;}"
        "QMessageBox QPushButton#primaryDialogButton{background:#0e639c;border-color:#1177bb;color:#ffffff;}"
        "QMessageBox QPushButton#primaryDialogButton:hover{background:#1177bb;border-color:#2089c9;}");
}

void ConfigureMessageBox(QMessageBox& dialog) {
    dialog.setTextFormat(Qt::PlainText);
    dialog.setTextInteractionFlags(Qt::TextSelectableByMouse);
    dialog.setStyleSheet(DialogStyleSheet());
}

}  // namespace

void ApplyDialogTheme(QApplication& application) {
    application.setStyleSheet(DialogStyleSheet());
}

bool ShowConfirmationDialog(QWidget* parent, const QString& title, const QString& message, const QString& accept_text) {
    QMessageBox dialog(parent);
    dialog.setObjectName(QStringLiteral("confirmationDialog"));
    dialog.setIcon(QMessageBox::Question);
    dialog.setWindowTitle(title);
    dialog.setText(message);

    auto* accept_button = dialog.addButton(accept_text, QMessageBox::AcceptRole);
    accept_button->setObjectName(QStringLiteral("primaryDialogButton"));
    auto* cancel_button = dialog.addButton(QStringLiteral("취소"), QMessageBox::RejectRole);
    cancel_button->setObjectName(QStringLiteral("cancelDialogButton"));
    dialog.setDefaultButton(accept_button);
    dialog.setEscapeButton(cancel_button);
    ConfigureMessageBox(dialog);

    dialog.exec();
    return dialog.clickedButton() == accept_button;
}

void ShowWarningDialog(QWidget* parent, const QString& title, const QString& message) {
    QMessageBox dialog(parent);
    dialog.setObjectName(QStringLiteral("warningDialog"));
    dialog.setIcon(QMessageBox::Warning);
    dialog.setWindowTitle(title);
    dialog.setText(message);

    auto* accept_button = dialog.addButton(QStringLiteral("확인"), QMessageBox::AcceptRole);
    accept_button->setObjectName(QStringLiteral("primaryDialogButton"));
    dialog.setDefaultButton(accept_button);
    dialog.setEscapeButton(accept_button);
    ConfigureMessageBox(dialog);
    dialog.exec();
}

}  // namespace logistics::control_center
