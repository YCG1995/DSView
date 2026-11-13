#include "android_ui.h"

#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QFont>
#include <QtGlobal>

#if defined(Q_OS_ANDROID)
#include <QJniObject>
#endif

qreal android_ui_density()
{
#if defined(Q_OS_ANDROID)
    /* Qt for Android 里 1 逻辑像素 ≈ 1 dp，设备的 dpr 由系统处理；
     * 这里取 dpr 只是为了给图标/间距做个保守的换算基线。 */
    QScreen *s = QGuiApplication::primaryScreen();
    if (s)
        return qBound<qreal>(1.0, s->devicePixelRatio(), 4.0);
    return 2.0;
#else
    return 1.0;
#endif
}

int android_dp(int dp)
{
#if defined(Q_OS_ANDROID)
    /* Android 上 Qt 的坐标单位已经是 dp，不再乘 dpr，避免放大两遍。 */
    return dp;
#else
    return dp;
#endif
}

QString android_qss_overlay()
{
#if defined(Q_OS_ANDROID)
    /* 触屏版覆盖层：整套桌面版 QSS 里的 5px 内边距 + 22px 高度按钮
     * 在手机上点不中，这里统一抬到 44dp 级别的可点区域，并放大字号。 */
    QString qss;
    qss += "QPushButton { padding: 10px 16px; min-height: 40px; border-radius: 8px; }";
    qss += "QToolButton { min-width: 44px; min-height: 44px; padding: 6px; }";
    qss += "QComboBox { min-height: 40px; padding: 6px 12px; }";
    qss += "QComboBox::drop-down { width: 32px; }";
    qss += "QLineEdit { min-height: 40px; padding: 6px 10px; }";
    qss += "QSpinBox, QDoubleSpinBox { min-height: 40px; min-width: 96px; }";
    qss += "QCheckBox { min-height: 40px; spacing: 10px; }";
    qss += "QCheckBox::indicator { width: 26px; height: 26px; }";
    qss += "QRadioButton { min-height: 40px; spacing: 10px; }";
    qss += "QRadioButton::indicator { width: 26px; height: 26px; }";
    qss += "QGroupBox { margin-top: 18px; padding-top: 14px; }";
    qss += "QTabBar::tab { min-height: 44px; min-width: 88px; padding: 8px 16px; }";
    qss += "QHeaderView::section { min-height: 36px; padding: 6px; }";
    qss += "QTableView, QTreeView { gridline-color: #2A2A2A; }";
    qss += "QTableView::item, QTreeView::item { padding: 6px; min-height: 36px; }";
    qss += "QScrollBar:vertical { width: 18px; }";
    qss += "QScrollBar:horizontal { height: 18px; }";
    qss += "QScrollBar::handle:vertical { min-height: 48px; }";
    qss += "QScrollBar::handle:horizontal { min-width: 48px; }";
    qss += "QMenu::item { padding: 10px 24px; min-height: 36px; }";
    qss += "QSlider::groove:horizontal { height: 8px; }";
    qss += "QSlider::handle:horizontal { width: 34px; height: 34px; margin: -14px 0; border-radius: 17px; }";
    qss += "QMessageBox QLabel { font-size: 15px; }";
    return qss;
#else
    return QString();
#endif
}

void android_fit_to_screen(QWidget *w)
{
    if (!w)
        return;

#if defined(Q_OS_ANDROID)
    QScreen *screen = w->screen() ? w->screen() : QGuiApplication::primaryScreen();
    if (!screen)
        return;

    QRect avail = screen->availableGeometry();
    /* 给系统栏/键盘留出余量 */
    int maxW = (int)(avail.width() * 0.96);
    int maxH = (int)(avail.height() * 0.90);

    if (w->minimumWidth() > maxW)
        w->setMinimumWidth(0);
    if (w->minimumHeight() > maxH)
        w->setMinimumHeight(0);

    w->setMaximumWidth(maxW);
    w->setMaximumHeight(maxH);

    if (w->width() > maxW || w->height() > maxH)
        w->resize(qMin(w->width(), maxW), qMin(w->height(), maxH));
#else
    Q_UNUSED(w);
#endif
}
