#include "logistics/control_center/control_center_theme.hpp"

namespace logistics::control_center {

QString ControlCenterStyleSheet() {
    return QStringLiteral(R"qss(
QMainWindow, #centralSurface { background:#1f1f1f; color:#cccccc; }
QFrame#appHeader { background:#181818; border-bottom:1px solid #303030; }
QStatusBar { background:#181818; color:#cccccc; border-top:1px solid #303030; }
QToolTip { background:#252526; color:#f0f0f0; border:1px solid #454545; padding:5px; }
QTabWidget::pane { border:1px solid #303030; background:#181818; }
QTabBar::tab { background:#252526; color:#aaaaaa; border:1px solid #363636; padding:8px 16px; }
QTabBar::tab:selected { background:#181818; color:#f0f0f0; border-top:2px solid #4daafc; }
QComboBox, QAbstractSpinBox { background:#252526; color:#f0f0f0; border:1px solid #454545; padding:5px; }
QAbstractItemView { background:#252526; color:#f0f0f0; selection-background-color:#264f78; selection-color:#ffffff; }
QAbstractItemView::item:hover { background:#2a3f52; color:#ffffff; }
QTableView::item:selected, QTableWidget::item:selected { background:#264f78; color:#ffffff; }
QDialog { background:#181818; color:#cccccc; }
QDialog QLabel#detailTitle { color:#f0f0f0; font-size:18px; font-weight:700; }
QDialog QLabel#detailMetaLabel { color:#777777; font-size:10px; }
QDialog QLabel#detailMetaValue { color:#d4d4d4; font-size:11px; }
QDialog QPlainTextEdit { background:#202020; color:#e5e5e5; border:1px solid #303030; border-radius:8px; padding:10px; }
QPushButton { background:#2d2d30; color:#f0f0f0; border:1px solid #454545; border-radius:5px; padding:6px 12px; }
QPushButton:hover { border-color:#75beff; }
QPushButton:disabled { color:#6e6e6e; background:#252526; border-color:#333333; }
QScrollBar:vertical { width:7px; background:#1f1f1f; }
QScrollBar:horizontal { height:7px; background:#1f1f1f; }
QScrollBar::handle { background:#4a4a4a; border-radius:3px; min-height:24px; min-width:24px; }
QSplitter::handle { background:#242424; border:0; }
QSplitter::handle:hover { background:#264f78; }
)qss");
}

}  // namespace logistics::control_center
