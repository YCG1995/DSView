/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 * 
 * Copyright (C) 2021 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "appcontrol.h"

#include <libsigrok.h>
#include <libsigrokdecode.h>
#if defined(Q_OS_ANDROID)
#include <glib.h>
#endif
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QCoreApplication>
#include <QWidget>
#include <string>
#include <assert.h>
#include "sigsession.h"
#include "dsvdef.h"
#include "config/appconfig.h"
#include "log.h"
#include "utility/path.h"
#include "utility/encoding.h"
#if defined(Q_OS_ANDROID)
#include "android/android_assets.h"
#endif

AppControl::AppControl()
{
    _topWindow = NULL; 
    _session = new pv::SigSession();
}

AppControl::AppControl(AppControl &o)
{
    (void)o;
}
 
AppControl::~AppControl()
{ 
   // DESTROY_OBJECT(_session);
}

AppControl* AppControl::Instance()
{
    static AppControl *ins = NULL;
    if (ins == NULL){
        ins = new AppControl();
    }
    return ins;
}

void AppControl::Destroy(){
     
} 

bool AppControl::Init()
{  
    pv::encoding::init();

    QString qs;
    std::string cs;

    qs = GetAppDataDir();
    cs = pv::path::ToUnicodePath(qs);
    dsv_info("GetAppDataDir:\"%s\"", cs.c_str());
    cs = pv::path::ConvertPath(qs);
    ds_set_user_data_dir(cs.c_str());

    qs = GetFirmwareDir();
    cs = pv::path::ToUnicodePath(qs);
    dsv_info("GetFirmwareDir:\"%s\"", cs.c_str());

    qs = GetUserDataDir();
    cs = pv::path::ToUnicodePath(qs);
    dsv_info("GetUserDataDir:\"%s\"", cs.c_str());

    qs = GetDecodeScriptDir();
    cs = pv::path::ToUnicodePath(qs);
    dsv_info("GetDecodeScriptDir:\"%s\"", cs.c_str());
    //---------------end print directorys.

    _session->init();

    srd_log_set_context(dsv_log_context());

#if defined(_WIN32) && defined(DEBUG_INFO)
    //able run debug with qtcreator
    QString pythonHome = "c:/python";
    QDir pydir;
    if (pydir.exists(pythonHome)){
        const wchar_t *pyhome = reinterpret_cast<const wchar_t*>(pythonHome.utf16());
        srd_set_python_home(pyhome);
    }
  
#endif

#if defined(_WIN32) && !defined(DEBUG_INFO)
    // Portable Python: the bundled Python DLL cannot locate its standard
    // library on its own, so point it at the copy shipped next to the
    // executable ("./lib/python3.*/").
    {
        QString appDir = QCoreApplication::applicationDirPath();
        QDir libDir(appDir + "/lib");
        if (libDir.exists()) {
            QStringList entries = libDir.entryList(QStringList() << "python3*",
                                                   QDir::Dirs | QDir::NoDotAndDotDot);
            if (!entries.isEmpty()) {
                QDir pyDir(libDir.absolutePath() + "/" + entries.first());
                if (QFile::exists(pyDir.absolutePath() + "/os.py")) {
                    const wchar_t *pyhome = reinterpret_cast<const wchar_t*>(appDir.utf16());
                    srd_set_python_home(pyhome);
                }
            }
        }
    }
#endif

#if defined(Q_OS_ANDROID) && !defined(DSVIEW_ANDROID_NO_PYTHON)
    // Android: CPython 的标准库和解码器脚本都打在 APK assets 里，
    // 首次启动复制到私有目录（/data/data/<pkg>/files{,/python}）。
    {
        QString pyHome = android_extract_assets(GetAppDataDir());
        dsv_info("android python home:\"%s\"", pyHome.toUtf8().constData());
        /* 扩展模块（_struct/binascii 等）在 APK 的 lib/<abi> 里，
         * 通过 PYTHONPATH 让 import 能找到它们。 */
        QString libDir = android_native_lib_dir();
        dsv_info("android native lib dir:\"%s\"", libDir.toUtf8().constData());
        qputenv("PYTHONPATH", libDir.toUtf8());
        qputenv("PYTHONHOME", pyHome.toUtf8());
        /* 注意：Android 的 wchar_t 是 4 字节，不能用 QString::utf16()（那是
         * 2 字节 UTF-16，Windows 才适用）。必须 toStdWString() 才能拿到
         * 宽字符编码正确的 home，否则 Python 读到乱码路径后会初始化失败，
         * 并直接 exit(1)（错误只写 stderr，logcat 里什么都看不到）。 */
        std::wstring pyHomeW = pyHome.toStdWString();
        srd_set_python_home(pyHomeW.c_str());
    }
#endif

    //the python script path of decoder
    char path[256] = {0};
    QString dir = GetDecodeScriptDir();   
    strcpy(path, dir.toUtf8().data());

    // Initialise libsigrokdecode
    int srd_ret = srd_init(path);
    if (srd_ret != SRD_OK)
    { 
        dsv_err("ERROR: libsigrokdecode init failed.");
        qWarning("libsigrokdecode init failed (ret=%d, decoders=%s)", srd_ret, path);
        qWarning("pythonhome=%s pythonpath=%s", qgetenv("PYTHONHOME").constData(),
                 qgetenv("PYTHONPATH").constData());
        return false;
    }

    // Load the protocol decoders
    if (srd_decoder_load_all() != SRD_OK)
    {
        dsv_err("ERROR: load the protocol decoders failed.");
        qWarning("load the protocol decoders failed (%s)", path);
        return false;
    }
#if defined(Q_OS_ANDROID)
    // 诊断用：确认 Python 解码器真的被扫描并加载（DSVIEW_ANDROID_PYTHON）
    qWarning("decoders loaded: %d (dir=%s)",
             g_slist_length((GSList *)srd_decoder_list()), path);
#endif
 
    return true;
}

bool AppControl::Start()
{  
    _session->Open(); 
    return true;
}

 void AppControl::Stop()
 {
    _session->Close();  
 }

void AppControl::UnInit()
{  
    // Destroy libsigrokdecode
    srd_exit();

    _session->uninit();
}

bool AppControl::TopWindowIsMaximized()
{
    if (_topWindow != NULL){
        return _topWindow->isMaximized();
    }
    return false;
}