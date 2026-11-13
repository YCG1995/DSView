#ifndef ANDROID_ASSETS_H
#define ANDROID_ASSETS_H

#include <QString>

/* 把 APK assets 里的 Python 标准库与解码器脚本复制到应用私有目录，
 * 返回可用作 PYTHONHOME 的目录（<appData>/python）。 */
QString android_extract_assets(const QString &appDataDir);

/* APK 原生库目录（扩展模块与 libpython 所在），用作 PYTHONPATH。 */
QString android_native_lib_dir();

#endif // ANDROID_ASSETS_H
