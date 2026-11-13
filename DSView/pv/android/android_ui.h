#ifndef ANDROID_UI_H
#define ANDROID_UI_H

#include <QString>
#include <QWidget>

/* Android（手机/触屏）界面适配：
 * - 触屏可点性：控件最小高度、内边距、字号统一放大
 * - 尺寸约束：对话框不得超出屏幕
 * 本文件只在 Q_OS_ANDROID 下有意义，其它平台为空操作。 */

/* 返回当前设备上的逻辑像素缩放系数（用于把 dp 换算成 Qt 逻辑像素）。 */
qreal android_ui_density();

/* 把 dp 换算成 Qt 逻辑像素。 */
int android_dp(int dp);

/* 追加到主题后面的 QSS 覆盖层：放大字号、放大可点区域、加宽滚动条。 */
QString android_qss_overlay();

/* 让 widget（一般是对话框）不超过可用屏幕区域。 */
void android_fit_to_screen(QWidget *w);

#endif // ANDROID_UI_H
