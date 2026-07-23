#include <QApplication>

#include "logistics/control_center/main_window.hpp"
#include "logistics/control_center/ui_dialog.hpp"

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    logistics::control_center::ApplyDialogTheme(application);
    logistics::control_center::MainWindow window;
    window.show();
    return application.exec();
}
