/*
 * Android 资源落地：
 * APK 里的 assets/python/lib/python3.13 与 assets/decoders 在首次启动时
 * 复制到应用私有目录，供 CPython 作为 PYTHONHOME / 解码器搜索路径使用。
 *
 * 只在 Q_OS_ANDROID 下编译（见 CMakeLists 的 Android 分支）。
 */
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QDebug>
#include <QCoreApplication>
#include <QList>

#include "android_assets.h"

static bool copyDirRecursive(const QString &srcRoot, const QString &dstRoot, int depth = 0)
{
    QDir src(srcRoot);
    if (!src.exists())
        return false;
    if (!QDir().mkpath(dstRoot))
        return false;

    const QFileInfoList entries = src.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                                   QDir::DirsFirst);
    for (const QFileInfo &fi : entries) {
        const QString dst = dstRoot + "/" + fi.fileName();
        if (fi.isDir()) {
            if (depth < 12)
                copyDirRecursive(fi.absoluteFilePath(), dst, depth + 1);
        } else {
            QFile in(fi.absoluteFilePath());
            if (!in.open(QIODevice::ReadOnly))
                continue;
            if (QFile::exists(dst))
                QFile::remove(dst);
            QFile out(dst);
            if (!out.open(QIODevice::WriteOnly))
                continue;
            out.write(in.readAll());
            out.close();
        }
    }
    return true;
}

/* 复制一次，用 .stamp 记录已完成，避免每次启动都写几十 MB 文件 */
static void syncAssetDir(const QString &assetPath, const QString &targetDir, const QString &stamp)
{
    QDir().mkpath(targetDir);
    if (QFile::exists(stamp))
        return;
    qInfo("android_assets: extracting %s -> %s", qPrintable(assetPath), qPrintable(targetDir));
    if (copyDirRecursive(assetPath, targetDir)) {
        QFile f(stamp);
        if (f.open(QIODevice::WriteOnly))
            f.write("1");
    } else {
        qWarning("android_assets: source %s not found", qPrintable(assetPath));
    }
}

/* APK 里的原生库目录（/data/app/<pkg>/lib/<abi>）：
 * 扩展模块 *.so 和 libpython3.13.so 都在这里，import 时要把该目录
 * 放进 sys.path（通过 PYTHONPATH）才能找到。applicationDirPath 在 Qt for
 * Android 上就指向这个目录，拿不到时再从 /proc/self/maps 里解析。 */
QString android_native_lib_dir()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    if (appDir.contains("/lib/"))
        return appDir;

    QFile maps("/proc/self/maps");
    if (maps.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> lines = maps.readAll().split('\n');
        for (const QByteArray &line : lines) {
            if (line.contains("libDSView_")) {
                const QByteArray path = line.mid(line.indexOf('/'));
                const QString full = QString::fromUtf8(path).trimmed();
                const int slash = full.lastIndexOf('/');
                if (slash > 0)
                    return full.left(slash);
            }
        }
    }
    return appDir;
}

QString android_extract_assets(const QString &appDataDir){
    const QString pythonHome = appDataDir + "/python";
    const QString decodersDir = appDataDir + "/decoders";

    /* assets:/ 是 Qt for Android 访问 APK assets 的前缀 */
    syncAssetDir("assets:/python/lib/python3.13",
                 pythonHome + "/lib/python3.13",
                 pythonHome + "/.stdlib.stamp");
    syncAssetDir("assets:/decoders", decodersDir, decodersDir + "/.stamp");

    qInfo("android_assets: python home = %s", qPrintable(pythonHome));
    return pythonHome;
}

