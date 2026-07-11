#include <QApplication>

#include "logistics/control_center/main_window.hpp"

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    logistics::control_center::MainWindow window;
    window.show();
    return application.exec();
}
