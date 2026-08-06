#pragma once

#include <QString>

class QApplication;
class QWidget;

namespace logistics::control_center {

void ApplyDialogTheme(QApplication& application);

[[nodiscard]] bool ShowConfirmationDialog(QWidget* parent, const QString& title, const QString& message,
                                          const QString& accept_text = QStringLiteral("확인"));

void ShowWarningDialog(QWidget* parent, const QString& title, const QString& message);

}  // namespace logistics::control_center
