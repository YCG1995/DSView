/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
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

#include <QAction>
#include <QButtonGroup>
#include <QFileDialog>
#include <QMessageBox>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>
#include <QDesktopServices>
#include <QKeyEvent>
#include <QEvent>
#include <QtGlobal>
#include <QApplication>
#include <QSettings>
#include <QDialog>
#include <QStandardPaths>
#include <QTimer>
#include <QPointer>
#include <QDebug>
#include <QScreen>
#include <QTimer>
#include <libusb-1.0/libusb.h>
#include <QGuiApplication>
#include <QTextStream>
#include <QJsonValue>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <functional>
#include "utility/formatting.h"

//include with qt5
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QDesktopWidget>
#endif

#include "mainwindow.h"
#if defined(Q_OS_ANDROID)
#include "android/android_ui.h"
#endif

#include "data/logicsnapshot.h"
#include "data/dsosnapshot.h"
#include "data/analogsnapshot.h"

#include "dialogs/about.h"
#include "dialogs/deviceoptions.h"
#include "dialogs/storeprogress.h"
#include "dialogs/waitingdialog.h"
#include "dialogs/regionoptions.h"

#include "toolbars/samplingbar.h"
#include "toolbars/trigbar.h"
#include "toolbars/filebar.h"
#include "toolbars/logobar.h"
#include "toolbars/titlebar.h"

#include "dock/triggerdock.h"
#include "dock/dsotriggerdock.h"
#include "dock/measuredock.h"
#include "dock/ppsresultdock.h"
#include "pps/ppsmanager.h"
#include "pps/ppstesthooks.h"
#include "pps/ppscalibration.h"
#include "pps/ppswavewindow.h"
#include "dialogs/ppsconfigdlg.h"
#include "dialogs/ppscalibdlg.h"

#include <cmath>
#include <QJsonArray>
#include "dock/searchdock.h"
#include "dock/protocoldock.h"
#include "dock/decoderconsoledock.h"

#include "view/view.h"
#include "view/trace.h"
#include "view/signal.h"
#include "view/dsosignal.h"
#include "view/logicsignal.h"
#include "view/analogsignal.h"

/* __STDC_FORMAT_MACROS is required for PRIu64 and friends (in C++). */
#include <inttypes.h>
#include <stdint.h>
#include <stdarg.h>
#include <glib.h>
#include <list>
#include "ui/msgbox.h"
#include "config/appconfig.h"
#include "appcontrol.h"
#include "dsvdef.h"
#include "appcontrol.h"
#include "utility/encoding.h"
#include "utility/path.h"
#include "log.h"
#include "sigsession.h"
#include "autosavemanager.h"
#include "mcp/mcpserver.h"
#include "deviceagent.h"
#include <algorithm>
#include <stdlib.h>
#include "ZipMaker.h"
#include "ui/langresource.h"
#include "mainframe.h"
#include "dsvdef.h"
#include <thread>
#include "ui/uimanager.h"

namespace pv
{

    namespace{
        QString tmp_file;
    }

    MainWindow::MainWindow(toolbars::TitleBar *title_bar, QWidget *parent)
        : QMainWindow(parent)
    {
        _msg = NULL;
        _frame = parent; 

        assert(title_bar);
        assert(_frame);

        _title_bar = title_bar;

        _session = AppControl::Instance()->GetSession();
        _session->set_callback(this);
        _device_agent = _session->get_device();
        _session->add_msg_listener(this);

        _is_auto_switch_device = false;
        _is_save_confirm_msg = false;

        _pattern_mode = "random";

        setup_ui();

        // Auto-save manager (repeat mode, background .dsl writes)
        _auto_save_mgr = new AutoSaveManager(_session, this);
        connect(_auto_save_mgr, &AutoSaveManager::status_changed,
                this, [this](const QString &text){
            if (text.isEmpty()){
                statusBar()->hide();
            }
            else{
                statusBar()->showMessage(text);
                statusBar()->show();
            }
        });
        statusBar()->setSizeGripEnabled(false);
        statusBar()->hide();

        // MCP control endpoint (loopback only). Port is configurable via
        // the DSVIEW_MCP_PORT environment variable.
        _mcp_server = new mcp::McpServer(this);
        _mcp_server->set_session(_session);
        _mcp_server->set_view(_view);  // _view is created in setup_ui() above
        _mcp_server->set_data_getter(this);  // MainWindow implements ISessionDataGetter
        _mcp_server->set_pps_control(this);  // ... and mcp::IPpsControl (1PPS mode)
        _mcp_server->set_console_text(this); // ... and mcp::IConsoleTextProvider
        quint16 mcp_port = 6347;
        // Not a test hook: the MCP listener port is a real deployment option,
        // so it stays read even in release builds.
        const QByteArray env_port = qgetenv("DSVIEW_MCP_PORT");
        if (!env_port.isEmpty())
            mcp_port = env_port.toUShort();
        if (!_mcp_server->start(mcp_port))
            dsv_warn("Failed to start MCP server on port %d", mcp_port);

        // 1PPS measurement mode: the manager and the result dock are created
        // on first use (see ensure_pps()), so nothing is added to the window
        // layout during startup.
        setContextMenuPolicy(Qt::NoContextMenu);

        _key_vaild = false;
        _last_key_press_time = high_resolution_clock::now();

        update_title_bar_text();

        // Automation hook for headless validation of the 1PPS measurement mode:
        // it is inert unless DSVIEW_PPS_AUTOSTART=1 is set in the environment.
        // DSVIEW_PPS_AUTOSTART_DELAY_MS tunes the start delay (default 2500 ms),
        // DSVIEW_PPS_AUTOSTOP_MS then stops the measurement after that delay.
        if (test_env("DSVIEW_PPS_AUTOSTART") == "1") {
            const int stop_after_ms = test_env("DSVIEW_PPS_AUTOSTOP_MS").toInt();
            int start_delay_ms = test_env("DSVIEW_PPS_AUTOSTART_DELAY_MS").toInt();
            if (start_delay_ms <= 0)
                start_delay_ms = 2500;
            QTimer::singleShot(start_delay_ms, this, [this]() {
                // ensure_pps() first: the manager only exists after this call.
                on_pps_result(true);

                // Optional head-less configuration override, only for automated
                // validation: DSVIEW_PPS_TESTCFG points at a JSON file with
                // {ref_channel, ref_edge(0/1), sample_rate, capture_ms,
                //  channels:[{ch, edge(0/1)}], disabled:[...]}.
                const QByteArray cfg_path = test_env("DSVIEW_PPS_TESTCFG");
                if (!cfg_path.isEmpty() && _pps_manager) {
                    QFile f(QString::fromUtf8(cfg_path));
                    if (f.open(QIODevice::ReadOnly)) {
                        const QJsonObject o =
                            QJsonDocument::fromJson(f.readAll()).object();
                        pps::PpsConfig cfg = _pps_manager->config();
                        if (o.contains("ref_channel"))
                            cfg.ref_channel = o.value("ref_channel").toInt();
                        if (o.contains("ref_edge"))
                            cfg.ref_edge = o.value("ref_edge").toInt();
                        if (o.contains("sample_rate"))
                            cfg.sample_rate = static_cast<uint64_t>(
                                o.value("sample_rate").toDouble());
                        if (o.contains("capture_ms"))
                            cfg.capture_time_ms = o.value("capture_ms").toDouble();
                        if (o.contains("range"))
                            cfg.mode_channels = o.value("range").toInt();
                        if (o.contains("channels")) {
                            cfg.channels.clear();
                            for (const QJsonValue &v : o.value("channels").toArray()) {
                                pps::PpsChannel ch;
                                ch.channel_index = v.toObject().value("ch").toInt();
                                ch.edge = v.toObject().value("edge").toInt();
                                ch.enabled = true;
                                cfg.channels.push_back(ch);
                            }
                        }
                        cfg.disabled_channels.clear();
                        for (const QJsonValue &v : o.value("disabled").toArray())
                            cfg.disabled_channels.push_back(v.toInt());
                        _pps_manager->set_config(cfg);
                        dsv_info("1PPS test config applied: ref CH%d edge %d, "
                            "rate %llu, capture %.1f ms, %u channel(s)",
                            cfg.ref_channel, cfg.ref_edge,
                            (unsigned long long)cfg.sample_rate,
                            cfg.capture_time_ms,
                            (unsigned)cfg.channels.size());
                    }
                }
                on_pps_start();
            });
            if (stop_after_ms > 0) {
                QTimer::singleShot(start_delay_ms + stop_after_ms, this, [this]() {
                    on_pps_stop();
                });
            }
            // Automation hook: open the configuration dialog once and close it
            // again, so the logged rate/channel list can be checked head-less
            // (DSVIEW_PPS_OPENCFG=1).
            if (test_env("DSVIEW_PPS_OPENCFG") == "1") {
                QTimer::singleShot(start_delay_ms + stop_after_ms + 1000,
                    this, [this]() {
                    QTimer::singleShot(400, this, []() {
                        // A screenshot run closes the dialog itself, after the grab.
                        if (!test_env("DSVIEW_PPS_CONFIG_SHOT").isEmpty())
                            return;
                        const QWidgetList tops =
                            QApplication::topLevelWidgets();
                        for (QWidget *w : tops) {
                            if (qobject_cast<QDialog *>(w))
                                w->close();
                        }
                    });
                    on_pps_config();
                });
            }
        }

        // Automation hook: open the waveform window on startup
        // (DSVIEW_PPS_WAVE=1), so its rendering can be captured head-less.
        if (test_env("DSVIEW_PPS_WAVE") == "1") {
            QTimer::singleShot(2000, this, [this]() { on_pps_wave(); });
        }

        // Automation hook: run the calibration dialog on startup
        // (DSVIEW_PPS_CALIB_AUTO=1), optionally grabbing it to a png.
        if (test_env("DSVIEW_PPS_CALIB_AUTO") == "1") {
            QTimer::singleShot(3000, this, [this]() { on_pps_calib(); });
        }

        // Automation hook for the result-window UI: DSVIEW_PPS_DOCK_SHOT=<png>
        // grabs the whole window with the dock shown, then clicks the toolbar's
        // PPS button (the real signal path) to hide and show it again, saving
        // <png>.hidden.png and <png>.shown.png so both states can be checked.
        const QString dock_shot = QString::fromUtf8(test_env("DSVIEW_PPS_DOCK_SHOT"));
        if (!dock_shot.isEmpty()) {
            int shot_ms = test_env("DSVIEW_PPS_DOCK_SHOT_MS").toInt();
            if (shot_ms <= 0)
                shot_ms = 4000;
            QTimer::singleShot(shot_ms, this, [this, dock_shot]() {
                on_pps_result(true);
                QTimer::singleShot(300, this, [this, dock_shot]() {
                    grab().save(dock_shot);

                    // Evidence for the head-less checks: the calibrate button must
                    // be disabled while a measurement is running.
                    if (QToolButton *cb = findChild<QToolButton*>("ppsCalibButton"))
                        dsv_info("1PPS dock: calib button enabled=%d (running=%d)",
                            cb->isEnabled() ? 1 : 0,
                            (_pps_manager && _pps_manager->is_running()) ? 1 : 0);

                    QToolButton *pps_btn = findChild<QToolButton*>("ppsToolButton");
                    if (!pps_btn)
                        return;

                    pps_btn->click(); // the toolbar toggle itself
                    QTimer::singleShot(200, this, [this, dock_shot]() {
                        grab().save(dock_shot + ".hidden.png");
                        QToolButton *pps_btn = findChild<QToolButton*>("ppsToolButton");
                        if (pps_btn)
                            pps_btn->click();
                        QTimer::singleShot(200, this, [this, dock_shot]() {
                            grab().save(dock_shot + ".shown.png");
                            // Optional: exercise the dock's configure icon, so
                            // the icon -> dialog route is checked too.
                            if (test_env("DSVIEW_PPS_DOCK_CLICK_CFG") == "1") {
                                QToolButton *cfg_btn =
                                    findChild<QToolButton*>("ppsConfigButton");
                                if (cfg_btn)
                                    cfg_btn->click();
                            }
                        });
                    });
                });
            });
        }
    }

    void MainWindow::setup_ui()
    {
        setObjectName(QString::fromUtf8("MainWindow"));
        setContentsMargins(0, 0, 0, 0);
        layout()->setSpacing(0);

        // Setup the central widget
        _central_widget = new QWidget(this);
        _vertical_layout = new QVBoxLayout(_central_widget);
        _vertical_layout->setSpacing(0);
        _vertical_layout->setContentsMargins(0, 0, 0, 0);
        setCentralWidget(_central_widget);

        // Setup the sampling bar
        _sampling_bar = new toolbars::SamplingBar(_session, this);        
        _sampling_bar->setObjectName("sampling_bar");
        _trig_bar = new toolbars::TrigBar(_session, this);
        _trig_bar->setObjectName("trig_bar");
        _file_bar = new toolbars::FileBar(_session, this);
        _file_bar->setObjectName("file_bar");
        _logo_bar = new toolbars::LogoBar(_session, this);
        _logo_bar->setObjectName("logo_bar");

        // trigger dock
        _trigger_dock = new QDockWidget(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TRIGGER_DOCK_TITLE), "Trigger Setting..."), this);
        _trigger_dock->setObjectName("trigger_dock");
        _trigger_dock->setFeatures(QDockWidget::DockWidgetMovable);
        _trigger_dock->setAllowedAreas(Qt::RightDockWidgetArea);
        _trigger_dock->setVisible(false);
        _trigger_widget = new dock::TriggerDock(_trigger_dock, _session);        
        _trigger_dock->setWidget(_trigger_widget);

        _dso_trigger_dock = new QDockWidget(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TRIGGER_DOCK_TITLE), "Trigger Setting..."), this);
        _dso_trigger_dock->setObjectName("dso_trigger_dock");
        _dso_trigger_dock->setFeatures(QDockWidget::DockWidgetMovable);
        _dso_trigger_dock->setAllowedAreas(Qt::RightDockWidgetArea);
        _dso_trigger_dock->setVisible(false);
        _dso_trigger_widget = new dock::DsoTriggerDock(_dso_trigger_dock, _session);
        _dso_trigger_dock->setWidget(_dso_trigger_widget);

        // Setup _view widget
        _view = new pv::view::View(_session, _sampling_bar, this);
        _vertical_layout->addWidget(_view);


        // 桌面端工具栏图标 38x38：40x40 在 100% 缩放屏幕上偏大，48x48 更大，
        // 用户指定用 38；图标框大小决定按钮高度，文字行高不变。
        setIconSize(QSize(38, 38));
#if defined(Q_OS_ANDROID)
        /* 手机屏宽只有 360~430dp，桌面版把采样/触发/文件/Logo 四条工具栏排在
         * 一行，右侧会被裁掉。这里换成两行：第一行 Logo + 采样，第二行触发 + 文件。
         * 第二行按钮多，改成纯图标 + 缩小图标，避免文字被省略成 "Op...ns"。 */
        addToolBar(_sampling_bar);
        addToolBar(_logo_bar);
        addToolBarBreak(Qt::TopToolBarArea);
        setIconSize(QSize(34, 34));
#endif
        addToolBar(_trig_bar);
        addToolBar(_file_bar);
#if defined(Q_OS_ANDROID)
        QToolBar *androidBars[2] = {_trig_bar, _file_bar};
        for (QToolBar *bar : androidBars) {
            for (QAction *a : bar->actions()) {
                QWidget *w = bar->widgetForAction(a);
                if (QToolButton *tb = qobject_cast<QToolButton *>(w))
                    tb->setToolButtonStyle(Qt::ToolButtonIconOnly);
            }
        }
#else
        addToolBar(_logo_bar);

        // 图标放到 38 后按钮默认宽度按图标算，文字（"模式(E)"、"选项(0)"）会被
        // 折成两行；这里按文字实际宽度给每个按钮一个最小宽度，保持单行。
        QToolBar *desktopBars[] = {_sampling_bar, _trig_bar, _file_bar, _logo_bar};
        for (QToolBar *bar : desktopBars) {
            // 注意：采样栏把按钮放在内部布局里再整体 addWidget，这些按钮不在
            // bar->actions() 中，必须用 findChildren 向下遍历才能全部取到。
            for (QToolButton *tb : bar->findChildren<QToolButton *>()) {
                if (tb == nullptr)
                    continue;
                // QToolBar::addWidget() 添加的按钮（模式/开始/立即/选项/设备类型）
                // 不会继承工具栏的 iconSize，仍用样式默认的 24x24，这里统一显式设置，
                // 否则图标放大后只有走 addAction 的按钮变大。
                tb->setIconSize(QSize(38, 38));

                // 兜底：样表里只有 QMainWindow 级别的 icon-size，采样栏这类
                // 通过 addWidget 挂按钮的工具栏不会自动继承，这里直接把工具栏
                // 自己的 iconSize 也写死，保证按钮取图标尺寸时有一致的来源。
                bar->setIconSize(QSize(38, 38));
                if (tb->text().isEmpty())
                    continue;
                const int text_w = tb->fontMetrics().boundingRect(tb->text()).width() + 14;
                tb->setMinimumWidth(qMax(tb->sizeHint().width(), text_w));
            }
        }
#endif

        // Setup the dockWidget
        _protocol_dock = new QDockWidget(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_PROTOCOL_DOCK_TITLE), "Decode Protocol"), this);
        _protocol_dock->setObjectName("protocol_dock");
        _protocol_dock->setFeatures(QDockWidget::DockWidgetMovable);
        _protocol_dock->setAllowedAreas(Qt::RightDockWidgetArea);
        _protocol_dock->setVisible(false);
        _protocol_widget = new dock::ProtocolDock(_protocol_dock, *_view, _session);
        _protocol_dock->setWidget(_protocol_widget);

        _session->set_decoder_pannel(_protocol_widget);

        // measure dock
        _measure_dock = new QDockWidget(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MEASURE_DOCK_TITLE), "Measurement"), this);
        _measure_dock->setObjectName("measure_dock");
        _measure_dock->setFeatures(QDockWidget::DockWidgetMovable);
        _measure_dock->setAllowedAreas(Qt::RightDockWidgetArea);
        _measure_dock->setVisible(false);
        _measure_widget = new dock::MeasureDock(_measure_dock, *_view, _session);
        _measure_dock->setWidget(_measure_widget);

        // search dock
        _search_dock = new QDockWidget(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEARCH_DOCK_TITLE), "Search..."), this);
        _search_dock->setObjectName("search_dock");
        _search_dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
        _search_dock->setTitleBarWidget(new QWidget(_search_dock));
        _search_dock->setAllowedAreas(Qt::BottomDockWidgetArea);
        _search_dock->setVisible(false);

        _search_widget = new dock::SearchDock(_search_dock, *_view, _session);
        _search_dock->setWidget(_search_widget);

        // decoder debug console dock
        _decoder_console_dock = new QDockWidget(tr("解码调试控制台"), this);
        _decoder_console_dock->setObjectName("decoder_console_dock");
        _decoder_console_dock->setFeatures(QDockWidget::DockWidgetMovable |
                                           QDockWidget::DockWidgetClosable);
        _decoder_console_dock->setAllowedAreas(Qt::BottomDockWidgetArea |
                                               Qt::RightDockWidgetArea);
        _decoder_console_dock->setVisible(false);
        _decoder_console_widget = new dock::DecoderConsoleDock(_decoder_console_dock, _session);
        _decoder_console_dock->setWidget(_decoder_console_widget);
        addDockWidget(Qt::BottomDockWidgetArea, _decoder_console_dock);

        // Arm the script watcher right away: the checkbox starts checked, but
        // the signal only fires when the user toggles it.
        _decoder_console_widget->apply_initial_state();

        connect(_protocol_widget, &dock::ProtocolDock::sig_show_console,
                this, [this]() {
            _decoder_console_dock->setVisible(true);
            _decoder_console_dock->raise();
        });

        // The per-decoder run button exists only for the debug console workflow,
        // so it follows the console dock's visibility.
        connect(_decoder_console_dock, &QDockWidget::visibilityChanged,
                this, [this](bool visible) {
            if (_protocol_widget)
                _protocol_widget->set_run_buttons_visible(visible);
        });

        // Test hook: DSVIEW_DECODE_CONSOLE=1 opens the console at startup.
        if (!test_env("DSVIEW_DECODE_CONSOLE").isEmpty())
            _decoder_console_dock->setVisible(true);

        if (_protocol_widget)
            _protocol_widget->set_run_buttons_visible(
                    _decoder_console_dock->isVisible());

        addDockWidget(Qt::RightDockWidgetArea, _protocol_dock);
        addDockWidget(Qt::RightDockWidgetArea, _trigger_dock);
        addDockWidget(Qt::RightDockWidgetArea, _dso_trigger_dock);
        addDockWidget(Qt::RightDockWidgetArea, _measure_dock);
        addDockWidget(Qt::BottomDockWidgetArea, _search_dock);

        // Test hook: DSVIEW_PROTOCOL_DOCK=1 opens the decoder dock at startup
        // so the per-decoder buttons can be screenshotted without clicking.
        if (!test_env("DSVIEW_PROTOCOL_DOCK").isEmpty())
            _protocol_dock->setVisible(true);

        // Test hook: DSVIEW_TEST_DECODER=<decoder id> adds one decoder row to
        // the dock at startup, mirroring what the "+" button does. Only used to
        // screenshot the per-row buttons, which otherwise need GUI clicks.
        // Deferred: the decoder package list is only filled once the loader has
        // scanned the decoders directory.
        const QByteArray test_decoder = test_env("DSVIEW_TEST_DECODER");
        if (!test_decoder.isEmpty() && _protocol_widget) {
            // The decoder package list is filled asynchronously, so retry until
            // the requested id resolves instead of assuming it is ready.
            QTimer *test_timer = new QTimer(this);
            int *test_tries = new int(0);
            test_timer->setInterval(1000);
            connect(test_timer, &QTimer::timeout, this,
                    [this, test_decoder, test_timer, test_tries]() {
                std::list<pv::data::decode::Decoder *> sub_decoders;
                const bool added = _protocol_widget->add_protocol_by_id(
                        QString::fromUtf8(test_decoder), true, sub_decoders);
                if (added || ++(*test_tries) >= 10) {
                    test_timer->stop();
                    test_timer->deleteLater();
                    delete test_tries;
                }
            });
            test_timer->start();
        }

        // event filter
        _view->installEventFilter(this);
        _sampling_bar->installEventFilter(this);
        _trig_bar->installEventFilter(this);
        _file_bar->installEventFilter(this);
        _logo_bar->installEventFilter(this);
        _dso_trigger_dock->installEventFilter(this);
        _trigger_dock->installEventFilter(this);
        _protocol_dock->installEventFilter(this);
        _measure_dock->installEventFilter(this);
        _search_dock->installEventFilter(this);

        // defaut language
        AppConfig &app = AppConfig::Instance();
        switchLanguage(app.frameOptions.language);
        switchTheme(app.frameOptions.style);

        _sampling_bar->set_view(_view);

        // event
        connect(&_event, SIGNAL(session_error()), this, SLOT(on_session_error()));
        connect(&_event, SIGNAL(signals_changed()), this, SLOT(on_signals_changed()));
        connect(&_event, SIGNAL(receive_trigger(quint64)), this, SLOT(on_receive_trigger(quint64)));
        connect(&_event, SIGNAL(frame_ended()), this, SLOT(on_frame_ended()), Qt::DirectConnection);
        connect(&_event, SIGNAL(frame_began()), this, SLOT(on_frame_began()), Qt::DirectConnection);
        connect(&_event, SIGNAL(decode_done()), this, SLOT(on_decode_done()));
        connect(&_event, SIGNAL(data_updated()), this, SLOT(on_data_updated()));
        connect(&_event, SIGNAL(cur_snap_samplerate_changed()), this, SLOT(on_cur_snap_samplerate_changed()));
        connect(&_event, SIGNAL(receive_data_len(quint64)), this, SLOT(on_receive_data_len(quint64)));
        connect(&_event, SIGNAL(trigger_message(int)), this, SLOT(on_trigger_message(int)));

        // view
        connect(_view, SIGNAL(cursor_update()), _measure_widget, SLOT(cursor_update()));
        connect(_view, SIGNAL(cursor_moving()), _measure_widget, SLOT(cursor_moving()));
        connect(_view, SIGNAL(cursor_moved()), _measure_widget, SLOT(reCalc()));
        connect(_view, SIGNAL(prgRate(int)), this, SIGNAL(prgRate(int)));
        connect(_view, SIGNAL(auto_trig(int)), _dso_trigger_widget, SLOT(auto_trig(int)));

        // trig_bar
        connect(_trig_bar, SIGNAL(sig_protocol(bool)), this, SLOT(on_protocol(bool)));
        connect(_trig_bar, SIGNAL(sig_trigger(bool)), this, SLOT(on_trigger(bool)));
        connect(_trig_bar, SIGNAL(sig_measure(bool)), this, SLOT(on_measure(bool)));
        connect(_trig_bar, SIGNAL(sig_search(bool)), this, SLOT(on_search(bool)));
        connect(_trig_bar, SIGNAL(sig_setTheme(QString)), this, SLOT(switchTheme(QString)));
        connect(_trig_bar, SIGNAL(sig_show_lissajous(bool)), _view, SLOT(show_lissajous(bool)));
        connect(_trig_bar, &toolbars::TrigBar::sig_pps_result,
                this, &MainWindow::on_pps_result);

        // file toolbar
        connect(_file_bar, SIGNAL(sig_load_file(QString)), this, SLOT(on_load_file(QString)));
        connect(_file_bar, SIGNAL(sig_save()), this, SLOT(on_save()));
        connect(_file_bar, SIGNAL(sig_export()), this, SLOT(on_export()));
        connect(_file_bar, SIGNAL(sig_screenShot()), this, SLOT(on_screenShot()), Qt::QueuedConnection);
        connect(_file_bar, SIGNAL(sig_load_session(QString)), this, SLOT(on_load_session(QString)));
        connect(_file_bar, SIGNAL(sig_store_session(QString)), this, SLOT(on_store_session(QString)));
        connect(_file_bar, &toolbars::FileBar::sig_reload_scripts, this,
                [this](const QString &text){
            statusBar()->showMessage(text, 8000);
            statusBar()->show();
        });

        // logobar
        connect(_logo_bar, SIGNAL(sig_open_doc()), this, SLOT(on_open_doc()));

        connect(_protocol_widget, SIGNAL(protocol_updated()), this, SLOT(on_signals_changed()));

        // SamplingBar
        connect(_sampling_bar, SIGNAL(sig_store_session_data()), this, SLOT(on_save()));

        //
        connect(_dso_trigger_widget, SIGNAL(set_trig_pos(int)), _view, SLOT(set_trig_pos(int)));

        _delay_prop_msg_timer.SetCallback(std::bind(&MainWindow::on_delay_prop_msg, this));
 
        _logo_bar->set_mainform_callback(this);

        // Try load from file.
        QString ldFileName(AppControl::Instance()->_open_file_name.c_str());
        if (ldFileName != "")
        {   
            std::string file_name = pv::path::ToUnicodePath(ldFileName);

            if (QFile::exists(ldFileName))
            {              
                dsv_info("Auto load file:%s", file_name.c_str());
                tmp_file = ldFileName;
            }
            else
            {
                dsv_err("file is not exists:%s", file_name.c_str());
                MsgBox::Show(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_OPEN_FILE_ERROR), "Open file error!"), ldFileName, NULL);
            }
        }

        on_load_device_first();
    }

    void MainWindow::on_load_device_first()
    {
        if (tmp_file != ""){
            on_load_file(tmp_file);
            tmp_file = "";
        }
        else{
            _session->set_default_device();
        }
    }

    void MainWindow::retranslateUi()
    {
        _trigger_dock->setWindowTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TRIGGER_DOCK_TITLE), "Trigger Setting..."));
        _dso_trigger_dock->setWindowTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TRIGGER_DOCK_TITLE), "Trigger Setting..."));
        _protocol_dock->setWindowTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_PROTOCOL_DOCK_TITLE), "Decode Protocol"));
        _measure_dock->setWindowTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MEASURE_DOCK_TITLE), "Measurement"));
        _search_dock->setWindowTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEARCH_DOCK_TITLE), "Search..."));
    }

    void MainWindow::on_load_file(QString file_name)
    {
        try
        {
            if (_device_agent->is_hardware()){
                save_config();
            }

            _session->set_file(file_name);
        }
        catch (QString e)
        {   
            QString strMsg(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_FAIL_TO_LOAD), "Failed to load "));
            strMsg += file_name;
            MsgBox::Show(strMsg);
            _session->set_default_device();
        }
    }

    void MainWindow::session_error()
    {
        _event.session_error();
    }

    void MainWindow::session_save()
    {
        save_config();
    }

    void MainWindow::on_session_error()
    {
        QString title;
        QString details;
        QString ch_status = "";

        switch (_session->get_error())
        {
        case SigSession::Hw_err:
            dsv_info("MainWindow::on_session_error(),Hw_err, stop capture");
            _session->stop_capture();
            title = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_HARDWARE_ERROR), "Hardware Operation Failed");
            details = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_HARDWARE_ERROR_DET), 
                      "Please replug device to refresh hardware configuration!");
            break;
        case SigSession::Malloc_err:
            dsv_info("MainWindow::on_session_error(),Malloc_err, stop capture");
            _session->stop_capture();
            title = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_MALLOC_ERROR), "Malloc Error");
            details = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_MALLOC_ERROR_DET), 
                      "Memory is not enough for this sample!\nPlease reduce the sample depth!");
            break;
        case SigSession::Pkt_data_err:
            title = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_PACKET_ERROR), "Packet Error");
            details = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_PACKET_ERROR_DET), 
            "the content of received packet are not expected!");
            _session->refresh(0);
            break;
        case SigSession::Data_overflow:
            dsv_info("MainWindow::on_session_error(),Data_overflow, stop capture");
            _session->stop_capture();
            title = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DATA_OVERFLOW), "Data Overflow");
            details = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DATA_OVERFLOW_DET), 
                      "USB bandwidth can not support current sample rate! \nPlease reduce the sample rate!");
            break;
        default:
            title = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_UNDEFINED_ERROR), "Undefined Error");
            details = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_UNDEFINED_ERROR_DET), "Not expected error!");
            break;
        }

        // While the 1PPS measurement loop runs, a modal dialog would block the
        // GUI thread and silently stall (or wedge) the measurement. Report the
        // error through the log instead and stop the measurement cleanly.
        if (_pps_manager && _pps_manager->is_running()) {
            dsv_err("1PPS measurement aborted by session error: %s - %s",
                    title.toUtf8().constData(), details.toUtf8().constData());
            _session->clear_error();
            on_pps_stop();
            return;
        }

        pv::dialogs::DSMessageBox msg(this, title);
        msg.mBox()->setText(details);
        msg.mBox()->setStandardButtons(QMessageBox::Ok);
        msg.mBox()->setIcon(QMessageBox::Warning);
        connect(_session->device_event_object(), SIGNAL(device_updated()), &msg, SLOT(accept()));
        _msg = &msg;
        msg.exec();
        _msg = NULL;

        _session->clear_error();
    }

    void MainWindow::save_config()
    { 
        if (_device_agent->have_instance() == false)
        {
            dsv_info("There is no need to save the configuration");
            return;
        }

        AppConfig &app = AppConfig::Instance();        

        if (_device_agent->is_hardware()){
            QString sessionFile = gen_config_file_path(true);
            save_config_to_file(sessionFile);
        }

        app.frameOptions.windowState = saveState();
        app.SaveFrame();
    }

    QString MainWindow::gen_config_file_path(bool isNewFormat)
    { 
        AppConfig &app = AppConfig::Instance();

        QString file = GetProfileDir();
        QDir dir(file);
        if (dir.exists() == false){
            dir.mkpath(file);
        } 

        QString driver_name = _device_agent->driver_name();
        QString mode_name = QString::number(_device_agent->get_work_mode()); 
        QString lang_name;
        QString base_path = dir.absolutePath() + "/" + driver_name + mode_name;

        if (!isNewFormat){
            lang_name = QString::number(app.frameOptions.language);           
        }

        return base_path + ".ses" + lang_name + ".dsc";
    }

    bool MainWindow::able_to_close()
    {
        if (_device_agent->is_hardware() && _session->have_hardware_data() == false){
            _sampling_bar->commit_settings();
        }
        // not used, refer to closeEvent of mainFrame
        save_config();
        
        if (confirm_to_store_data()){
            on_save();
            return false; 
        } 
        return true;
    }

    void MainWindow::on_protocol(bool visible)
    {
        show_side_dock(_protocol_dock, visible);

        if (!visible)
            _view->setFocus();
    }

    void MainWindow::on_trigger(bool visible)
    {
        if (_device_agent->get_work_mode() != DSO)
        {
            _trigger_widget->update_view();
            show_side_dock(_trigger_dock, visible);
            _dso_trigger_dock->setVisible(false);
        }
        else
        {
            _dso_trigger_widget->update_view();
            _trigger_dock->setVisible(false);
            show_side_dock(_dso_trigger_dock, visible);
        }

        if (!visible)
            _view->setFocus();
    }

    /* 手机上右侧面板按屏宽铺开：
     * 桌面版这些面板只有 300~400dp，放在手机上会把内容切掉一半，
     * 而且触摸拖动面板边框很难精准。面板展开时占满宽度，收起后波形区恢复。 */
    void MainWindow::show_side_dock(QDockWidget *dock, bool visible)
    {
        if (!dock)
            return;

        dock->setVisible(visible);
#if defined(Q_OS_ANDROID)
        /* 手机上中央波形区会跟右侧面板抢宽度：QDockWidget 最多只能拿到
         * ~276dp（受中央控件最小宽限制），面板里的 "Rising/Falling/Edge"
         * 这类长标签就会被右边缘切掉。手机上没有足够宽度并排显示，所以
         * 面板展开时把中央波形区收起，让面板独占屏宽；全部面板关闭后波形恢复。 */
        bool anyPanel = (_protocol_dock && _protocol_dock->isVisible())
            || (_trigger_dock && _trigger_dock->isVisible())
            || (_dso_trigger_dock && _dso_trigger_dock->isVisible())
            || (_measure_dock && _measure_dock->isVisible())
            || (_pps_dock && _pps_dock->isVisible());
        if (_central_widget)
            _central_widget->setVisible(!anyPanel);

        if (visible)
        {
            QPointer<QDockWidget> guard(dock);
            QTimer::singleShot(0, this, [this, guard]() {
                if (!guard)
                    return;
                resizeDocks({guard.data()}, {qMax(240, width() - 4)}, Qt::Horizontal);
            });
        }
#else
        Q_UNUSED(visible);
#endif
    }

    void MainWindow::on_measure(bool visible)
    {
        show_side_dock(_measure_dock, visible);

        if (!visible)
            _view->setFocus();
    }

    void MainWindow::ensure_pps()
    {
        if (_pps_manager)
            return;

        _pps_manager = new pps::PpsManager(_session, this);
        // Needed by the "save the anomalous frame" option: StoreSession asks the
        // application for the session blob, and MainWindow is that provider.
        _pps_manager->set_data_getter(this);
        connect(_pps_manager, &pps::PpsManager::status_message,
                this, &MainWindow::on_pps_status);

        // The calibration coefficient is a property of the analyzer, not of a
        // single session, so it survives restarts.
        {
            QSettings st(QApplication::organizationName(),
                QApplication::applicationName());
            const double f = st.value("pps/calib_factor", 1.0).toDouble();
            if (f > 0.0)
                _pps_manager->set_calib_factor(f);
        }

        _pps_dock = new QDockWidget(QString::fromUtf8("1PPS 测量"), this);
        _pps_dock->setObjectName("pps_dock");
        _pps_dock->setAllowedAreas(Qt::RightDockWidgetArea);
        _pps_dock->setFeatures(QDockWidget::DockWidgetMovable |
                QDockWidget::DockWidgetClosable);
        _pps_result = new dock::PpsResultDock(_pps_manager, _pps_dock);
        _pps_dock->setWidget(_pps_result);
        addDockWidget(Qt::RightDockWidgetArea, _pps_dock);
        _pps_dock->hide();
        connect(_pps_dock, &QDockWidget::visibilityChanged,
                _trig_bar, &toolbars::TrigBar::set_pps_result_checked);
        connect(_pps_result, &dock::PpsResultDock::config_requested,
                this, &MainWindow::on_pps_config);
        connect(_pps_result, &dock::PpsResultDock::start_requested,
                this, &MainWindow::on_pps_start);
        connect(_pps_result, &dock::PpsResultDock::stop_requested,
                this, &MainWindow::on_pps_stop);
        connect(_pps_result, &dock::PpsResultDock::calib_requested,
                this, &MainWindow::on_pps_calib);
        connect(_pps_result, &dock::PpsResultDock::wave_requested,
                this, &MainWindow::on_pps_wave);
    }

    void MainWindow::on_pps_config()
    {
        ensure_pps();
        dialogs::PpsConfigDlg dlg(_session, _pps_manager->config(), this);
        // Head-less check hook: DSVIEW_PPS_CONFIG_SHOT=<png> grabs the dialog
        // once it has been laid out, so the layout can be inspected without a
        // person clicking through the menus.
        const QString cfg_shot = QString::fromUtf8(test_env("DSVIEW_PPS_CONFIG_SHOT"));
        if (!cfg_shot.isEmpty()) {
            QTimer::singleShot(1500, &dlg, [&dlg, cfg_shot]() {
                dlg.grab().save(cfg_shot);
                dlg.accept();
            });
        }
        if (dlg.exec() == QDialog::Accepted) {
            // set_config() stores the whole setup in QSettings, so the dialog
            // opens with the same values after a restart.
            _pps_manager->set_config(dlg.result_config());
            save_channels_before_pps();
            apply_pps_channels_to_session();
        }
    }

    /*
     * Mirrors the PPS channel set into the main capture: the reference channel
     * and every measured channel are enabled, everything else is switched off.
     * Without this the main view still looks at whatever the last normal
     * capture used, which does not match what the PPS measurement samples.
     */
    void MainWindow::apply_pps_channels_to_session()
    {
        if (!_pps_manager || !_session || !_session->get_device())
            return;

        DeviceAgent *dev = _session->get_device();
        const std::vector<int> set = _pps_manager->measured_channel_set();
        for (GSList *l = dev->get_channels(); l; l = l->next) {
            sr_channel *c = static_cast<sr_channel *>(l->data);
            if (c->type != SR_CHANNEL_LOGIC)
                continue;
            const bool on = std::find(set.begin(), set.end(), c->index)
                != set.end();
            dev->enable_probe(c, on);
        }
        dsv_info("1PPS config: main capture channels mirrored (%d channel(s))",
            (int)set.size());
        if (_view)
            _view->signals_changed(NULL);
    }

    void MainWindow::save_channels_before_pps()
    {
        if (_pps_channels_saved || !_session || !_session->get_device())
            return;

        _pps_saved_channels.clear();
        for (GSList *l = _session->get_device()->get_channels(); l; l = l->next) {
            sr_channel *c = static_cast<sr_channel *>(l->data);
            if (c->type != SR_CHANNEL_LOGIC || c->index < 0)
                continue;
            if ((int)_pps_saved_channels.size() <= c->index)
                _pps_saved_channels.resize(c->index + 1, 0);
            _pps_saved_channels[c->index] = c->enabled ? 1 : 0;
        }
        _pps_channels_saved = true;
        dsv_info("1PPS: main capture channel state saved (%d channel(s))",
            (int)_pps_saved_channels.size());
    }

    void MainWindow::restore_channels_after_pps()
    {
        if (!_pps_channels_saved || !_session || !_session->get_device())
            return;

        DeviceAgent *dev = _session->get_device();
        int enabled = 0;
        for (GSList *l = dev->get_channels(); l; l = l->next) {
            sr_channel *c = static_cast<sr_channel *>(l->data);
            if (c->type != SR_CHANNEL_LOGIC || c->index < 0)
                continue;
            if ((size_t)c->index >= _pps_saved_channels.size())
                continue;
            const bool on = _pps_saved_channels[c->index] != 0;
            dev->enable_probe(c, on);
            if (on)
                enabled++;
        }
        _pps_channels_saved = false;
        dsv_info("1PPS: main capture channels restored (%d enabled)", enabled);
        if (_view)
            _view->signals_changed(NULL);
    }

    void MainWindow::on_pps_start()
    {
        ensure_pps();
        show_side_dock(_pps_dock, true);
        _trig_bar->set_pps_result_checked(true);

        if (_pps_manager->is_running()) {
            statusBar()->show();
            statusBar()->showMessage(QString::fromUtf8("1PPS 测量已在运行。"), 5000);
            return;
        }

        // Mirror the configured 1PPS channels onto the main capture view before
        // the device is taken over, so what the user measures in the PPS panel is
        // what the main waveform is set up for.
        save_channels_before_pps();
        apply_pps_channels_to_session();

        // Start directly: missing options are derived from the active device.
        if (!_pps_manager->start()) {
            // The mirror above already replaced the user's channel selection;
            // a failed start has no stop() to undo it, so hand the channels back
            // here (pps_start() does the same for the MCP entry point).
            restore_channels_after_pps();
            statusBar()->show();
            QMessageBox::warning(this, QString::fromUtf8("1PPS 测量"),
                    _pps_manager->start_error());
            _pps_result->refresh();
        }
        else {
            _pps_result->refresh(); // update the dock's start/stop buttons
        }
    }

    void MainWindow::on_pps_stop()
    {
        if (_pps_manager)
            _pps_manager->stop();
        restore_channels_after_pps();
        if (_pps_result)
            _pps_result->refresh();
    }

    void MainWindow::on_pps_result(bool visible)
    {
        ensure_pps();
        show_side_dock(_pps_dock, visible);
        _trig_bar->set_pps_result_checked(visible);
    }

    void MainWindow::on_pps_calib()
    {
        ensure_pps();

        // The calibration run owns the device for its whole duration, so it
        // cannot share the hardware with a running measurement. The button is
        // greyed out while measuring; this is the guarded entry point for the
        // other ways in (menu, MCP, automation).
        if (_pps_manager->is_running()) {
            statusBar()->show();
            statusBar()->showMessage(QString::fromUtf8(
                "1PPS 测量正在运行，请先停止测量再校准。"), 8000);
            if (_pps_result)
                _pps_result->refresh();
            return;
        }

        // Channel count of the active mode is the honest upper bound.
        pps::PpsDeviceCaps caps;
        pps::query_device_caps(_session, caps);
        const int max_ch = caps.channel_num > 0 ? caps.channel_num - 1 : 7;
        int def_ch = _pps_manager->config().ref_channel;
        const int ch_override = test_env("DSVIEW_PPS_CALIB_CH").toInt();
        if (ch_override > 0 || test_env("DSVIEW_PPS_CALIB_CH") == "0")
            def_ch = ch_override;
        if (def_ch < 0 || def_ch > max_ch)
            def_ch = 0;

        dialogs::PpsCalibDlg dlg(_session, def_ch, max_ch, this);
        // Head-less check hook: DSVIEW_PPS_CALIB_SHOT=<png> starts the run by
        // itself and grabs the dialog before closing it.
        const QString calib_shot = QString::fromUtf8(test_env("DSVIEW_PPS_CALIB_SHOT"));
        if (!calib_shot.isEmpty()) {
            QTimer::singleShot(600, &dlg, [&dlg]() {
                QMetaObject::invokeMethod(&dlg, "on_start");
            });
            const int shot_ms = test_env("DSVIEW_PPS_CALIB_SHOT_MS").toInt();
            QTimer::singleShot(shot_ms > 0 ? shot_ms : 45000, &dlg,
                [&dlg, calib_shot]() {
                dlg.grab().save(calib_shot);
                dlg.accept();
            });
        }
        // Calibration rewrites the probe enables as well; the user's own channel
        // selection has to survive it. The restore is deferred by one event loop
        // turn because the dialog stops its run in its destructor, which only
        // happens once this function returns - restoring earlier would hand the
        // channels back while the calibration is still sampling.
        save_channels_before_pps();
        dlg.exec();
        QTimer::singleShot(0, this, [this]() { restore_channels_after_pps(); });

        if (dlg.has_result()) {
            const double f = dlg.factor();
            _pps_manager->set_calib_factor(f);
            QSettings st(QApplication::organizationName(),
                QApplication::applicationName());
            st.setValue("pps/calib_factor", _pps_manager->calib_factor());
            dsv_info("1PPS calib: coefficient %.9f stored in the config", f);
            statusBar()->show();
            statusBar()->showMessage(QString::fromUtf8(
                "校准系数已更新为 %1，所有时间差将按该系数换算。")
                .arg(f, 0, 'f', 9), 8000);
        }
    }

    void MainWindow::on_pps_wave()
    {
        ensure_pps();
        // Independent top level window: parented for ownership only, so it can
        // be moved to another screen without dragging the main window.
        if (_pps_wave.isNull())
            _pps_wave = new pps::PpsWaveWindow(_pps_manager, this);
        _pps_wave->show();
        _pps_wave->raise();
        _pps_wave->activateWindow();
    }

    void MainWindow::on_pps_status(const QString &text)
    {
        statusBar()->showMessage(text, 8000);
    }

    // ---- 1PPS over MCP ----------------------------------------------------
    // The three helpers below translate between pps::PpsConfig and the JSON
    // shape used by the MCP tools (get_pps_config / set_pps_config).
    static QJsonObject pps_config_json(const pps::PpsConfig &cfg)
    {
        QJsonArray channels;
        for (size_t i = 0; i < cfg.channels.size(); ++i) {
            QJsonObject o;
            o["ch"] = cfg.channels[i].channel_index;
            o["edge"] = cfg.channels[i].edge;
            o["save"] = cfg.channels[i].save_on_loss;
            channels.append(o);
        }

        QJsonArray disabled;
        for (size_t i = 0; i < cfg.disabled_channels.size(); ++i)
            disabled.append(cfg.disabled_channels[i]);

        QJsonObject o;
        o["ref_channel"] = cfg.ref_channel;
        o["ref_edge"] = cfg.ref_edge;
        o["trigger_pos_percent"] = cfg.trigger_pos_percent;
        o["capture_ms"] = cfg.capture_time_ms;
        o["sample_rate"] = static_cast<double>(cfg.sample_rate);
        o["save_dir"] = cfg.save_dir;
        o["range"] = cfg.mode_channels;
        o["auto_save_loss"] = cfg.dsl_save_loss;   // kept for older callers
        o["dsl_save_loss"] = cfg.dsl_save_loss;
        o["dsl_save_abnormal"] = cfg.dsl_save_abnormal;
        o["dsl_save_always"] = cfg.dsl_save_always;
        o["calib_factor"] = cfg.calib_factor;
        o["channels"] = channels;
        o["disabled"] = disabled;
        return o;
    }

    static void pps_config_apply(const QJsonObject &o, pps::PpsConfig &cfg)
    {
        if (o.contains("ref_channel"))
            cfg.ref_channel = o.value("ref_channel").toInt();
        if (o.contains("ref_edge"))
            cfg.ref_edge = o.value("ref_edge").toInt();
        if (o.contains("trigger_pos_percent"))
            cfg.trigger_pos_percent = o.value("trigger_pos_percent").toInt();
        if (o.contains("capture_ms"))
            cfg.capture_time_ms = o.value("capture_ms").toDouble();
        if (o.contains("sample_rate"))
            cfg.sample_rate = static_cast<uint64_t>(
                o.value("sample_rate").toDouble());
        if (o.contains("save_dir"))
            cfg.save_dir = o.value("save_dir").toString();
        if (o.contains("range"))
            cfg.mode_channels = o.value("range").toInt();
        if (o.contains("dsl_save_loss"))
            cfg.dsl_save_loss = o.value("dsl_save_loss").toBool();
        else if (o.contains("auto_save_loss"))
            cfg.dsl_save_loss = o.value("auto_save_loss").toBool();
        if (o.contains("dsl_save_abnormal"))
            cfg.dsl_save_abnormal = o.value("dsl_save_abnormal").toBool();
        if (o.contains("dsl_save_always"))
            cfg.dsl_save_always = o.value("dsl_save_always").toBool();
        if (o.contains("calib_factor")) {
            const double f = o.value("calib_factor").toDouble(1.0);
            if (f > 0.0)
                cfg.calib_factor = f;
        }
        if (o.contains("channels")) {
            cfg.channels.clear();
            for (const QJsonValue &v : o.value("channels").toArray()) {
                const QJsonObject c = v.toObject();
                pps::PpsChannel ch;
                ch.channel_index = c.value("ch").toInt();
                ch.edge = c.value("edge").toInt(cfg.ref_edge);
                ch.enabled = true;
                ch.save_on_loss = c.value("save").toBool(cfg.dsl_save_loss);
                cfg.channels.push_back(ch);
            }
        }
        if (o.contains("disabled")) {
            cfg.disabled_channels.clear();
            for (const QJsonValue &v : o.value("disabled").toArray())
                cfg.disabled_channels.push_back(v.toInt());
        }
    }

    QJsonObject MainWindow::pps_describe()
    {
        ensure_pps();

        QJsonObject out;
        out["config"] = pps_config_json(_pps_manager->config());

        pps::PpsDeviceCaps caps;
        if (pps::query_device_caps(_session, caps)) {
            QJsonArray rates;
            for (size_t i = 0; i < caps.rates.size(); ++i)
                rates.append(static_cast<double>(caps.rates[i]));

            QJsonArray modes;
            for (size_t i = 0; i < caps.modes.size(); ++i) {
                QJsonObject m;
                m["id"] = caps.modes[i].id;
                m["channels"] = caps.modes[i].channels;
                m["max_rate"] = static_cast<double>(caps.modes[i].max_rate);
                modes.append(m);
            }

            const pps::PpsConfig &cfg = _pps_manager->config();
            QJsonObject c;
            c["channel_num"] = caps.channel_num;
            c["max_rate"] = static_cast<double>(caps.max_rate);
            c["hw_depth"] = static_cast<double>(caps.hw_depth);
            c["rates"] = rates;
            c["modes"] = modes;
            c["channels_for_rate"] =
                caps.channels_for_rate(cfg.sample_rate);
            c["max_capture_ms"] = caps.max_capture_ms(
                cfg.sample_rate, caps.channels_for_rate(cfg.sample_rate));
            out["device"] = c;
        }

        out["running"] = _pps_manager->is_running();
        return out;
    }

    bool MainWindow::pps_configure(const QJsonObject &cfg, QString &error)
    {
        ensure_pps();

        if (_pps_manager->is_running()) {
            error = QString::fromUtf8("1PPS 正在运行，请先调用 stop_pps 再修改配置。");
            return false;
        }

        pps::PpsConfig next = _pps_manager->config();
        pps_config_apply(cfg, next);
        _pps_manager->set_config(next);

        // Normalise against the device (samplerate wins over channel range)
        // and surface the driver error if nothing usable is left.
        if (!_pps_manager->prepare_config()) {
            error = _pps_manager->start_error();
            if (error.isEmpty())
                error = QString::fromUtf8("1PPS 配置无法在当前设备上成立。");
            return false;
        }
        return true;
    }

    bool MainWindow::pps_start(QString &error)
    {
        ensure_pps();
        show_side_dock(_pps_dock, true);
        _trig_bar->set_pps_result_checked(true);

        if (_pps_manager->is_running()) {
            error = QString::fromUtf8("1PPS 已在运行。");
            return false;
        }

        // Unlike on_pps_start() this never opens a modal box: an MCP client
        // gets the failure text instead of a dialog it cannot dismiss.
        save_channels_before_pps();
        if (!_pps_manager->start()) {
            restore_channels_after_pps();
            error = _pps_manager->start_error();
            if (error.isEmpty())
                error = QString::fromUtf8("1PPS 启动失败。");
            _pps_result->refresh();
            return false;
        }

        _pps_result->refresh();
        return true;
    }

    void MainWindow::pps_stop()
    {
        if (_pps_manager)
            _pps_manager->stop();
        restore_channels_after_pps();
        if (_pps_result)
            _pps_result->refresh();
    }

    // 1PPS deltas are reported with one decimal place, after the calibration
    // factor has been applied.
    static double pps_round(double v)
    {
        return std::round(v * 10.0) / 10.0;
    }

    QJsonObject MainWindow::pps_status()
    {
        ensure_pps();

        QJsonObject out;
        out["running"] = _pps_manager->is_running();
        out["frame_count"] = _pps_manager->frame_count();
        out["csv_path"] = _pps_manager->csv_path();
        out["last_capture_ms"] = _pps_manager->last_capture_ms();
        out["last_report_ms"] = _pps_manager->last_report_ms();
        out["last_total_ms"] = _pps_manager->last_total_ms();

        QJsonArray results;
        const std::vector<pps::PpsResult> &last = _pps_manager->last_results();
        for (size_t i = 0; i < last.size(); ++i) {
            QJsonObject r;
            r["ch"] = last[i].channel_index;
            r["valid"] = last[i].valid;
            r["delta_ns"] = pps_round(last[i].delta_ns);
            results.append(r);
        }
        out["results"] = results;

        QJsonArray stats;
        const std::vector<pps::PpsStats> s = _pps_manager->statistics();
        for (size_t i = 0; i < s.size(); ++i) {
            QJsonObject o;
            o["ch"] = s[i].channel_index;
            o["count"] = s[i].count;
            o["mean_ns"] = pps_round(s[i].mean);
            o["min_ns"] = pps_round(s[i].minimum);
            o["max_ns"] = pps_round(s[i].maximum);
            o["stddev_ns"] = pps_round(s[i].stddev);
            stats.append(o);
        }
        out["statistics"] = stats;
        out["loss_frames"] = _pps_manager->loss_frame_count();
        out["last_dsl_path"] = _pps_manager->last_dsl_path();

        return out;
    }

    QString MainWindow::console_text() const
    {
        return _decoder_console_widget
                ? _decoder_console_widget->text() : QString();
    }

    void MainWindow::on_search(bool visible)
    {
        _search_dock->setVisible(visible);
        _view->show_search_cursor(visible);

        if (!visible)
            _view->setFocus();
    }

    void MainWindow::on_screenShot()
    {
        AppConfig &app = AppConfig::Instance();
        QString dateTimeString = Formatting::DateTimeToString(QDateTime::currentDateTime(), TimeStrigFormatType::TIME_STR_FORMAT_SHORT2);
        QString default_name = app.userHistory.screenShotPath + "/" + APP_NAME + "-" + dateTimeString;

        int x = parentWidget()->pos().x();
        int y = parentWidget()->pos().y();
        int w = parentWidget()->frameGeometry().width();
        int h = parentWidget()->frameGeometry().height();

        (void)h;
        (void)w;
        (void)x;
        (void)y;

#ifdef _WIN32 
    #if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QPixmap pixmap = QGuiApplication::primaryScreen()->grabWindow(QApplication::desktop->winId(), x, y, w, h);
    #else
        QPixmap pixmap = QPixmap::grabWidget(parentWidget());
    #endif
#elif __APPLE__ 
        x += MainFrame::Margin;
        y += MainFrame::Margin;
        w -= MainFrame::Margin * 2;
        h -= MainFrame::Margin * 2;

    #if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QPixmap pixmap = QGuiApplication::primaryScreen()->grabWindow(winId(), x, y, w, h);
    #else
        QDesktopWidget *desktop = QApplication::desktop();
        int curMonitor = desktop->screenNumber(this);
        QPixmap pixmap = QGuiApplication::screens().at(curMonitor)->grabWindow(winId(), x, y, w, h);
    #endif       
#else       
        QPixmap pixmap = QGuiApplication::primaryScreen()->grabWindow(winId());
#endif

        QString format = "png";
        QString fileName = QFileDialog::getSaveFileName(
            this,
            L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SAVE_AS), "Save As"),
            default_name,
            "png file(*.png);;jpeg file(*.jpeg)",
            &format);

        if (!fileName.isEmpty())
        {
            QStringList list = format.split('.').last().split(')');
            QString suffix = list.first();

            QFileInfo f(fileName);
            if (f.suffix().compare(suffix))
            {
                //tr
                fileName += "." + suffix;
            }

            pixmap.save(fileName, suffix.toLatin1());

            fileName = path::GetDirectoryName(fileName);

            if (app.userHistory.screenShotPath != fileName)
            {
                app.userHistory.screenShotPath = fileName;
                app.SaveHistory();
            }
        }
    }

    // save file
    void MainWindow::on_save()
    {
        using pv::dialogs::StoreProgress;

        if (_device_agent->have_instance() == false)
        {
            dsv_info("Have no device, can't to save data.");
            return;
        }

        if (_session->is_working()){
            dsv_info("Save data: stop the current device."); 
            _session->stop_capture();
        }

        _session->set_saving(true);

        StoreProgress *dlg = new StoreProgress(_session, this);
        dlg->SetView(_view);
        dlg->save_run(this);
    }

    void MainWindow::on_export()
    {
        using pv::dialogs::StoreProgress;

        if (_session->is_working()){
            dsv_info("Export data: stop the current device."); 
            _session->stop_capture();
        }

        StoreProgress *dlg = new StoreProgress(_session, this);
        dlg->SetView(_view);
        dlg->export_run();
    }

    bool MainWindow::on_load_session(QString name)
    {
        return load_config_from_file(name);
    }

    bool MainWindow::load_config_from_file(QString file)
    {
        if (file == ""){
            dsv_err("File name is empty.");
            assert(false);
        }

        _protocol_widget->del_all_protocol();

        std::string file_name = pv::path::ToUnicodePath(file);
        dsv_info("Load device profile: \"%s\"", file_name.c_str());
        
        QFile sf(file);

        if (!sf.exists()){ 
            dsv_warn("Warning: device profile is not exists: \"%s\"", file_name.c_str());
            return false;
        }

        if (!sf.open(QIODevice::ReadOnly))
        {
            dsv_warn("Warning: Couldn't open device profile to load!");
            return false;
        }

        QString data = QString::fromUtf8(sf.readAll());
        QJsonDocument doc = QJsonDocument::fromJson(data.toUtf8());
        sf.close();

        bool bDecoder = false;
        int ret = load_config_from_json(doc, bDecoder);

        if (ret && _device_agent->get_work_mode() == DSO)
        {
            _dso_trigger_widget->update_view();
        }

        if (_device_agent->is_hardware()){
            _title_ext_string = file;
            update_title_bar_text();
        }

        return ret;
    }

    bool MainWindow::gen_config_json(QJsonObject &sessionVar)
    {
        AppConfig &app = AppConfig::Instance();

        GVariant *gvar_opts;
        GVariant *gvar;
        gsize num_opts;

        QString title = QApplication::applicationName() + " v" + QApplication::applicationVersion();

        QJsonArray channelVar;
        sessionVar["Version"] = QJsonValue::fromVariant(SESSION_FORMAT_VERSION);
        sessionVar["Device"] = QJsonValue::fromVariant(_device_agent->driver_name());
        sessionVar["DeviceMode"] = QJsonValue::fromVariant(_device_agent->get_work_mode());
        sessionVar["Language"] = QJsonValue::fromVariant(app.frameOptions.language);
        sessionVar["Title"] = QJsonValue::fromVariant(title);

        if (_device_agent->is_hardware() && _device_agent->get_work_mode() == LOGIC)
        {
            sessionVar["CollectMode"] = _session->get_collect_mode();
        }

        gvar_opts = _device_agent->get_config_list(NULL, SR_CONF_DEVICE_SESSIONS);
        if (gvar_opts == NULL)
        {
            dsv_warn("Device config list is empty. id:SR_CONF_DEVICE_SESSIONS");
            /* Driver supports no device instance sessions. */
            return false;
        }

        const int *const options = (const int32_t *)g_variant_get_fixed_array(
                                        gvar_opts, &num_opts, sizeof(int32_t));

        for (unsigned int i = 0; i < num_opts; i++)
        {
            const struct sr_config_info *const info = _device_agent->get_config_info(options[i]);
            gvar = _device_agent->get_config(info->key);
            if (gvar != NULL)
            {
                if (info->datatype == SR_T_BOOL)
                    sessionVar[info->name] = QJsonValue::fromVariant(g_variant_get_boolean(gvar));
                else if (info->datatype == SR_T_UINT64)
                    sessionVar[info->name] = QJsonValue::fromVariant(QString::number(g_variant_get_uint64(gvar)));
                else if (info->datatype == SR_T_UINT8)
                    sessionVar[info->name] = QJsonValue::fromVariant(g_variant_get_byte(gvar));
                 else if (info->datatype == SR_T_INT16)
                    sessionVar[info->name] = QJsonValue::fromVariant(g_variant_get_int16(gvar));
                else if (info->datatype == SR_T_FLOAT) //save as string format
                    sessionVar[info->name] = QJsonValue::fromVariant(QString::number(g_variant_get_double(gvar)));
                else if (info->datatype == SR_T_CHAR)
                    sessionVar[info->name] = QJsonValue::fromVariant(g_variant_get_string(gvar, NULL));
                else if (info->datatype == SR_T_LIST)
                    sessionVar[info->name] =  QJsonValue::fromVariant(g_variant_get_int16(gvar));
                else{
                    dsv_err("Unkown config info type:%d", info->datatype);
                    assert(false);
                }
                g_variant_unref(gvar);                
            }
        }

        for (auto s : _session->get_signals())
        {
            QJsonObject s_obj;
            s_obj["index"] = s->get_index();
            s_obj["view_index"] = s->get_view_index();
            s_obj["type"] = s->get_type();
            s_obj["enabled"] = s->enabled();
            s_obj["name"] = s->get_name();

            if (s->get_colour().isValid())
                s_obj["colour"] = QJsonValue::fromVariant(s->get_colour());
            else
                s_obj["colour"] = QJsonValue::fromVariant("default");

            view::LogicSignal *logicSig = NULL;
            if ((logicSig = dynamic_cast<view::LogicSignal *>(s)))
            {
                s_obj["strigger"] = logicSig->get_trig();
            }
            
            if (s->signal_type() == SR_CHANNEL_DSO)
            {
                view::DsoSignal *dsoSig = (view::DsoSignal*)s;
                s_obj["vdiv"] = QJsonValue::fromVariant(static_cast<qulonglong>(dsoSig->get_vDialValue()));
                s_obj["vfactor"] = QJsonValue::fromVariant(static_cast<qulonglong>(dsoSig->get_factor()));
                s_obj["coupling"] = dsoSig->get_acCoupling();
                s_obj["trigValue"] = dsoSig->get_trig_vrate();
                s_obj["zeroPos"] = dsoSig->get_zero_ratio();
            }
 
            if (s->signal_type() == SR_CHANNEL_ANALOG)
            {
                view::AnalogSignal *analogSig = (view::AnalogSignal*)s;
                s_obj["vdiv"] = QJsonValue::fromVariant(static_cast<qulonglong>(analogSig->get_vdiv()));
                s_obj["vfactor"] = QJsonValue::fromVariant(static_cast<qulonglong>(analogSig->get_factor()));
                s_obj["coupling"] = analogSig->get_acCoupling();
                s_obj["zeroPos"] = analogSig->get_zero_ratio();
                s_obj["mapUnit"] = analogSig->get_mapUnit();
                s_obj["mapMin"] = analogSig->get_mapMin();
                s_obj["mapMax"] = analogSig->get_mapMax();
                s_obj["mapDefault"] = analogSig->get_mapDefault();
            }
            channelVar.append(s_obj);
        }
        sessionVar["channel"] = channelVar;

        if (_device_agent->get_work_mode() == LOGIC)
        {
            sessionVar["trigger"] = _trigger_widget->get_session();
        }

        StoreSession ss(_session);
        QJsonArray decodeJson;
        ss.gen_decoders_json(decodeJson);
        sessionVar["decoder"] = decodeJson;

        if (_device_agent->get_work_mode() == DSO)
        {
            sessionVar["measure"] = _view->get_viewstatus()->get_session();
        }

        if (gvar_opts != NULL)
            g_variant_unref(gvar_opts);

        return true;
    }

    bool MainWindow::load_config_from_json(QJsonDocument &doc, bool &haveDecoder)
    {
        haveDecoder = false;

        QJsonObject sessionObj = doc.object();

        int mode = _device_agent->get_work_mode();

        // check config file version
        if (!sessionObj.contains("Version"))
        {
            dsv_dbg("Profile version is not exists!");
            return false;
        }

        int format_ver = sessionObj["Version"].toInt();

        if (format_ver < 2)
        {
            dsv_err("Profile version is error!");
            return false;
        }

        if (sessionObj.contains("CollectMode") && _device_agent->is_hardware()){
            int collect_mode = sessionObj["CollectMode"].toInt();
            _session->set_collect_mode((DEVICE_COLLECT_MODE)collect_mode);
        }

        int conf_dev_mode = sessionObj["DeviceMode"].toInt();

        if (_device_agent->is_hardware())
        {
            QString driverName = _device_agent->driver_name();
            QString sessionDevice = sessionObj["Device"].toString();            
            // check device and mode
            if (driverName != sessionDevice || mode != conf_dev_mode)
            {
                MsgBox::Show(NULL, L_S(STR_PAGE_MSG, S_ID(IDS_MSG_PROFILE_NOT_COMPATIBLE), "Profile is not compatible with current device or mode!"), this);
                return false;
            }
        }

        // load device settings
        GVariant *gvar_opts = _device_agent->get_config_list(NULL, SR_CONF_DEVICE_SESSIONS);
        gsize num_opts;

        if (gvar_opts != NULL)
        {
            const int *const options = (const int32_t *)g_variant_get_fixed_array(
                gvar_opts, &num_opts, sizeof(int32_t));

            for (unsigned int i = 0; i < num_opts; i++)
            {
                const struct sr_config_info *info = _device_agent->get_config_info(options[i]);

                if (!sessionObj.contains(info->name))
                    continue;

                GVariant *gvar = NULL;
                int id = 0;

                if (info->datatype == SR_T_BOOL){
                    gvar = g_variant_new_boolean(sessionObj[info->name].toInt());
                }
                else if (info->datatype == SR_T_UINT64){
                    //from string text.
                    gvar = g_variant_new_uint64(sessionObj[info->name].toString().toULongLong());         
                }
                else if (info->datatype == SR_T_UINT8){
                    if (sessionObj[info->name].toString() != "")
                        gvar = g_variant_new_byte(sessionObj[info->name].toString().toUInt());
                    else
                        gvar = g_variant_new_byte(sessionObj[info->name].toInt());                       
                }
                else if (info->datatype == SR_T_INT16){
                    gvar = g_variant_new_int16(sessionObj[info->name].toInt());
                }
                else if (info->datatype == SR_T_FLOAT){
                    if (sessionObj[info->name].toString() != "")
                        gvar = g_variant_new_double(sessionObj[info->name].toString().toDouble());
                    else
                        gvar = g_variant_new_double(sessionObj[info->name].toDouble()); 
                }
                else if (info->datatype == SR_T_CHAR){
                    gvar = g_variant_new_string(sessionObj[info->name].toString().toLocal8Bit().data());
                }
                else if (info->datatype == SR_T_LIST)
                { 
                    id = 0;

                    if (format_ver > 2){
                        // Is new version format.
                        id = sessionObj[info->name].toInt();
                    }
                    else{
                        const char *fd_key = sessionObj[info->name].toString().toLocal8Bit().data();
                        id = ds_dsl_option_value_to_code(conf_dev_mode, info->key, fd_key);
                        if (id == -1){
                            dsv_err("Convert failed, key:\"%s\", value:\"%s\""
                                ,info->name, fd_key);
                            id = 0; //set default value.
                        }
                        else{
                            dsv_info("Convert success, key:\"%s\", value:\"%s\", get code:%d"
                                ,info->name, fd_key, id);
                        }
                    }            
                    gvar = g_variant_new_int16(id);
                }

                if (gvar == NULL)
                {
                    dsv_warn("Warning: Profile failed to parse key:'%s'", info->name);
                    continue;
                }

                bool bFlag = _device_agent->set_config(info->key, gvar);
                if (!bFlag){
                    dsv_err("Set device config option failed, id:%d, code:%d", info->key, id);
                }   
            }
        }

        // load channel settings
        if (mode == DSO)
        {
            for (const GSList *l = _device_agent->get_channels(); l; l = l->next)
            {
                sr_channel *const probe = (sr_channel *)l->data;
                assert(probe);

                for (const QJsonValue &value : sessionObj["channel"].toArray())
                {
                    QJsonObject obj = value.toObject();
                    if (QString(probe->name) == obj["name"].toString() &&
                        probe->type == obj["type"].toDouble())
                    {
                        probe->vdiv = obj["vdiv"].toDouble();
                        probe->coupling = obj["coupling"].toDouble();
                        probe->vfactor = obj["vfactor"].toDouble();
                        probe->trig_value = obj["trigValue"].toDouble();
                        probe->map_unit = g_strdup(obj["mapUnit"].toString().toStdString().c_str());
                        probe->map_min = obj["mapMin"].toDouble();
                        probe->map_max = obj["mapMax"].toDouble();
                        probe->enabled = obj["enabled"].toBool();
                        break;
                    }
                }
            }
        }
        else
        {
            for (const GSList *l = _device_agent->get_channels(); l; l = l->next)
            {
                sr_channel *const probe = (sr_channel *)l->data;
                assert(probe);
                bool isEnabled = false;

                for (const QJsonValue &value : sessionObj["channel"].toArray())
                {
                    QJsonObject obj = value.toObject();

                    if ((probe->index == obj["index"].toInt()) &&
                        (probe->type == obj["type"].toInt()))
                    {
                        isEnabled = true;
                        QString chan_name = obj["name"].toString().trimmed();
                        if (chan_name == ""){
                            chan_name = QString::number(probe->index);
                        }
                        
                        probe->enabled = obj["enabled"].toBool();
                        probe->name = g_strdup(chan_name.toStdString().c_str());
                        probe->vdiv = obj["vdiv"].toDouble();
                        probe->coupling = obj["coupling"].toDouble();
                        probe->vfactor = obj["vfactor"].toDouble();
                        probe->trig_value = obj["trigValue"].toDouble();
                        probe->map_unit = g_strdup(obj["mapUnit"].toString().toStdString().c_str());
                        probe->map_min = obj["mapMin"].toDouble();
                        probe->map_max = obj["mapMax"].toDouble();

                        if (obj.contains("mapDefault"))
                        {
                            probe->map_default = obj["mapDefault"].toBool();
                        }

                        break;
                    }
                }
                if (!isEnabled)
                    probe->enabled = false;
            }
        }

        _session->reload();

        // load signal setting
        if (mode == DSO)
        {
            for (auto s : _session->get_signals())
            {
                for (const QJsonValue &value : sessionObj["channel"].toArray())
                {
                    QJsonObject obj = value.toObject();

                    if (s->get_name() ==  obj["name"].toString() &&
                        s->get_type() ==  obj["type"].toDouble())
                    {
                        s->set_colour(QColor(obj["colour"].toString()));
                       
                        if (s->signal_type() == SR_CHANNEL_DSO)
                        {   
                            view::DsoSignal *dsoSig = (view::DsoSignal*)s;
                            dsoSig->load_settings();
                            dsoSig->set_zero_ratio(obj["zeroPos"].toDouble());
                            dsoSig->set_trig_ratio(obj["trigValue"].toDouble());
                            dsoSig->commit_settings();
                        }
                        break;
                    }
                }
            }
        }
        else
        {
            for (auto s : _session->get_signals())
            {
                for (const QJsonValue &value : sessionObj["channel"].toArray())
                {
                    QJsonObject obj = value.toObject();
                    if ((s->get_index() == obj["index"].toInt()) &&
                        (s->get_type() == obj["type"].toInt()))
                    {
                        QString chan_name = obj["name"].toString().trimmed();
                        if (chan_name == ""){
                            chan_name = QString::number(s->get_index());
                        }

                        s->set_colour(QColor(obj["colour"].toString()));
                        s->set_name(chan_name);

                        view::LogicSignal *logicSig = NULL;
                        if ((logicSig = dynamic_cast<view::LogicSignal *>(s)))
                        {
                            logicSig->set_trig(obj["strigger"].toDouble());
                        }
 
                        if (s->signal_type() == SR_CHANNEL_DSO)
                        {
                            view::DsoSignal *dsoSig = (view::DsoSignal*)s;
                            dsoSig->load_settings();
                            dsoSig->set_zero_ratio(obj["zeroPos"].toDouble());
                            dsoSig->set_trig_ratio(obj["trigValue"].toDouble());
                            dsoSig->commit_settings();
                        }
 
                        if (s->signal_type() == SR_CHANNEL_ANALOG)
                        {   
                            view::AnalogSignal *analogSig = (view::AnalogSignal*)s;
                            analogSig->set_zero_ratio(obj["zeroPos"].toDouble());
                            analogSig->commit_settings();
                        }
                        
                        break;
                    }
                }
            }
        }

        // update UI settings
        _sampling_bar->update_sample_rate_list();
        _trigger_widget->device_updated();
        _view->header_updated();

        // load trigger settings
        if (sessionObj.contains("trigger"))
        {
            _trigger_widget->set_session(sessionObj["trigger"].toObject());
        }

        // load decoders
        if (sessionObj.contains("decoder"))
        {
            QJsonArray deArray = sessionObj["decoder"].toArray();
            if (deArray.empty() == false)
            {
                haveDecoder = true;
                StoreSession ss(_session);
                ss.load_decoders(_protocol_widget, deArray);
                _view->update_all_trace_postion();
            }
        }

        // load measure
        if (sessionObj.contains("measure"))
        {
            auto *bottom_bar = _view->get_viewstatus();
            bottom_bar->load_session(sessionObj["measure"].toArray(), format_ver);
        }

        if (gvar_opts != NULL)
            g_variant_unref(gvar_opts);

        load_channel_view_indexs(doc);

        return true;
    }

    void MainWindow::load_channel_view_indexs(QJsonDocument &doc)
    {
        QJsonObject sessionObj = doc.object();

        int mode = _device_agent->get_work_mode();
        if (mode != LOGIC)
            return;

        std::vector<int> view_indexs;

        for (const QJsonValue &value : sessionObj["channel"].toArray()){
            QJsonObject obj = value.toObject();

            if (obj.contains("view_index")){  
                view_indexs.push_back(obj["view_index"].toInt());
            }
        }

         if (view_indexs.size()){
            int i = 0;

            for (auto s : _session->get_signals()){
                s->set_view_index(view_indexs[i]);
                i++;
            }

            _view->update_all_trace_postion();
        }
    }
    
    bool MainWindow::on_store_session(QString name)
    {
        return save_config_to_file(name);
    }

    bool MainWindow::save_config_to_file(QString name)
    {
        if (name == ""){
            dsv_err("Session file name is empty.");
            assert(false);
        }

        std::string file_name = pv::path::ToUnicodePath(name);
        dsv_info("Store session to file: \"%s\"", file_name.c_str());

        QFile sf(name);
        if (!sf.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            dsv_warn("Warning: Couldn't open profile to write!");
            return false;
        }

        QTextStream outStream(&sf);
        encoding::set_utf8(outStream);

        QJsonObject sessionVar;
        if (!gen_config_json(sessionVar)){
            return false;
        }

        QJsonDocument sessionDoc(sessionVar);
        outStream << QString::fromUtf8(sessionDoc.toJson());
        sf.close();
        return true;
    }

    bool MainWindow::genSessionData(std::string &str)
    {
        QJsonObject sessionVar;
        if (!gen_config_json(sessionVar))
        {
            return false;
        }

        QJsonDocument sessionDoc(sessionVar);
        QString data = QString::fromUtf8(sessionDoc.toJson());
        str.append(data.toLocal8Bit().data());
        return true;
    }

    void MainWindow::restore_dock()
    { 
        // default dockwidget size
        AppConfig &app = AppConfig::Instance();
        QByteArray st = app.frameOptions.windowState;
#if defined(Q_OS_ANDROID)
        /* 桌面版保存的 dock 布局（含面板宽度、浮动位置）在手机上会把面板
         * 挤成窄条或推到屏幕外，手机上直接用默认布局。 */
        st.clear();
#endif
        if (!st.isEmpty())
        {
            try
            {
                restoreState(st);
            }
            catch (...)
            {
                MsgBox::Show(NULL, L_S(STR_PAGE_MSG, S_ID(IDS_MSG_RESTORE_WINDOW_ERROR), "restore window status error!"));
            }
        }

        // The restored layout may show docks that were closed when the state
        // was saved; keep the PPS menu entry in sync with the real visibility.
        if (_pps_dock && _trig_bar)
            _trig_bar->set_pps_result_checked(_pps_dock->isVisible());

        // Resotre the dock pannel.
        if (_device_agent->have_instance())
            _trig_bar->reload();
    }

    bool MainWindow::eventFilter(QObject *object, QEvent *event)
    {
        (void)object;
    
        if (event->type() == QEvent::KeyPress)
        {
            const auto &sigs = _session->get_signals();
            QKeyEvent *ke = (QKeyEvent *)event;
            
            int modifier = ke->modifiers(); 
 
            if(modifier & Qt::ControlModifier || 
               modifier & Qt::ShiftModifier || 
               modifier & Qt::AltModifier)
            {
                return true;
            }

            high_resolution_clock::time_point key_press_time = high_resolution_clock::now();
            milliseconds timeInterval = std::chrono::duration_cast<milliseconds>(key_press_time - _last_key_press_time);
            int64_t time_keep =  timeInterval.count();
            if (time_keep < 200){
                return true;
            }
            _last_key_press_time = key_press_time;           
            
            switch (ke->key())
            {
            case Qt::Key_S:
                _sampling_bar->run_or_stop();
                break;

            case Qt::Key_I:
                _sampling_bar->run_or_stop_instant();
                break;

            case Qt::Key_T:
                _trig_bar->trigger_clicked();
                break;

            case Qt::Key_D:
                _trig_bar->protocol_clicked();
                break;

            case Qt::Key_M:
                _trig_bar->measure_clicked();
                break;

            case Qt::Key_R:
                _trig_bar->search_clicked();
                break;

            case Qt::Key_O:
                _sampling_bar->config_device();
                break;

            case Qt::Key_PageUp:
#ifdef _WIN32
            case 33:
#endif
                _view->set_scale_offset(_view->scale(),
                                        _view->offset() - _view->get_view_width());
                break;
            case Qt::Key_PageDown:
#ifdef _WIN32
            case 34:
#endif
                _view->set_scale_offset(_view->scale(),
                                        _view->offset() + _view->get_view_width());

                break;

            case Qt::Key_Left:
#ifdef _WIN32
            case 37:
#endif
                _view->zoom(1);
                break;

            case Qt::Key_Right:
#ifdef _WIN32
            case 39:
#endif
                _view->zoom(-1);
                break;

            case Qt::Key_0:
                for (auto s : sigs)
                {
                    if (s->signal_type() == SR_CHANNEL_DSO)
                    {
                        view::DsoSignal *dsoSig = (view::DsoSignal*)s;
                        if (dsoSig->get_index() == 0)
                            dsoSig->set_vDialActive(!dsoSig->get_vDialActive());
                        else
                            dsoSig->set_vDialActive(false);
                    }
                }
                _view->setFocus();
                update();
                break;

            case Qt::Key_1:
                for (auto s : sigs)
                {
                    if (s->signal_type() == SR_CHANNEL_DSO)
                    {
                        view::DsoSignal *dsoSig = (view::DsoSignal*)s;
                        if (dsoSig->get_index() == 1)
                            dsoSig->set_vDialActive(!dsoSig->get_vDialActive());
                        else
                            dsoSig->set_vDialActive(false);
                    }
                }
                _view->setFocus();
                update();
                break;

            case Qt::Key_Up: 
#ifdef _WIN32
            case 38:
#endif
                for (auto s : sigs)
                {
                    if (s->signal_type() == SR_CHANNEL_DSO){
                        view::DsoSignal *dsoSig = (view::DsoSignal*)s;
                        if (dsoSig->get_vDialActive())
                        {
                            dsoSig->go_vDialNext(true);
                            update();
                            break;
                        }
                    }
                }
                break;

            case Qt::Key_Down:
#ifdef _WIN32
            case 40:
#endif
                for (auto s : sigs)
                {
                    if (s->signal_type() == SR_CHANNEL_DSO){
                        view::DsoSignal *dsoSig = (view::DsoSignal*)s;
                        if (dsoSig->get_vDialActive())
                        {
                            dsoSig->go_vDialPre(true);
                            update();
                            break;
                        }
                    }
                }
                break;
           
            default:
                QWidget::keyPressEvent((QKeyEvent *)event);
            }
            return true;
        }
        return false;
    }

    void MainWindow::switchLanguage(int language)
    {
        if (language == 0)
            return;
        
        AppConfig &app = AppConfig::Instance();

        if (app.frameOptions.language != language && language > 0)
        {
            app.frameOptions.language = language;
            app.SaveFrame();
            LangResource::Instance()->Load(language);     
        }        

        if (language == LAN_CN)
        {
            _qtTrans.load(":/qt_" + QString::number(language));
            qApp->installTranslator(&_qtTrans);
            _myTrans.load(":/my_" + QString::number(language));
            qApp->installTranslator(&_myTrans);
        }
        else if (language == LAN_EN)
        {
            qApp->removeTranslator(&_qtTrans);
            qApp->removeTranslator(&_myTrans);
        }

        retranslateUi();

        UiManager::Instance()->Update(UI_UPDATE_ACTION_LANG);
        _session->update_lang_text();
    }

    void MainWindow::switchTheme(QString style)
    {
        AppConfig &app = AppConfig::Instance();

        if (app.frameOptions.style != style)
        {
            app.frameOptions.style = style;
            app.SaveFrame();
        }

        QString qssRes = ":/" + style + ".qss";
        QFile qss(qssRes);
        qss.open(QFile::ReadOnly | QFile::Text);
        QString qssText = qss.readAll();
        qss.close();
#if defined(Q_OS_ANDROID)
        /* 手机触屏：桌面版 QSS 的按钮/下拉框只有 22~30px 高，点不中。
         * 这里追加一层覆盖，把可点区域抬到 40~44dp、字号放大。 */
        qssText += android_qss_overlay();
#endif
        qApp->setStyleSheet(qssText);

        UiManager::Instance()->Update(UI_UPDATE_ACTION_THEME);
        UiManager::Instance()->Update(UI_UPDATE_ACTION_FONT);

        data_updated();
    }

    void MainWindow::data_updated()
    {
        _event.data_updated(); // safe call
    }

    void MainWindow::on_data_updated()
    {
        _measure_widget->reCalc();
        _view->data_updated();
    }

    void MainWindow::on_open_doc()
    {
        openDoc();
    }

    void MainWindow::openDoc()
    {
        QDir dir(GetAppDataDir());
        AppConfig &app = AppConfig::Instance();
        int lan = app.frameOptions.language;
        QDesktopServices::openUrl(
            QUrl("file:///" + dir.absolutePath() + "/ug" + QString::number(lan) + ".pdf"));
    }

    void MainWindow::update_capture()
    {
        _view->update_hori_res();
    }

    void MainWindow::cur_snap_samplerate_changed()
    {
        _event.cur_snap_samplerate_changed(); // safe call
    }

    void MainWindow::on_cur_snap_samplerate_changed()
    {
        _measure_widget->reCalc();
    }

    /*------------------on event end-------*/

    void MainWindow::signals_changed()
    {
        _event.signals_changed(); // safe call
    }

    void MainWindow::on_signals_changed()
    {
        _view->signals_changed(NULL);
    }

    void MainWindow::receive_trigger(quint64 trigger_pos)
    {
        _event.receive_trigger(trigger_pos); // save call
    }

    void MainWindow::on_receive_trigger(quint64 trigger_pos)
    {
        _view->receive_trigger(trigger_pos);
    }

    void MainWindow::frame_ended()
    {
        _event.frame_ended(); // save call
    }

    void MainWindow::on_frame_ended()
    {
        _view->receive_end();
    }

    void MainWindow::frame_began()
    {
        _event.frame_began(); // save call
    }

    void MainWindow::on_frame_began()
    {
        _view->frame_began();
    }

    void MainWindow::show_region(uint64_t start, uint64_t end, bool keep)
    {
        _view->show_region(start, end, keep);
    }

    void MainWindow::show_wait_trigger()
    {
        _view->show_wait_trigger();
    }

    void MainWindow::repeat_hold(int percent)
    {
        (void)percent;
        _view->repeat_show();
    }

    void MainWindow::decode_done()
    {
        _event.decode_done(); // safe call
    }

    void MainWindow::on_decode_done()
    {
        _protocol_widget->update_model();
    }

    void MainWindow::receive_data_len(quint64 len)
    {
        _event.receive_data_len(len); // safe call
    }

    void MainWindow::on_receive_data_len(quint64 len)
    {
        _view->set_receive_len(len);
    }

    void MainWindow::receive_header()
    {
    }

    void MainWindow::check_usb_device_speed()
    {
        // USB device speed check
        if (_device_agent->is_hardware())
        {
            int usb_speed = LIBUSB_SPEED_HIGH;
            _device_agent->get_config_int32(SR_CONF_USB_SPEED, usb_speed);

            bool usb30_support = false;

            if (_device_agent->get_config_bool(SR_CONF_USB30_SUPPORT, usb30_support))
            {
                dsv_info("The device's USB module version: %d.0", usb30_support ? 3 : 2);

                int cable_ver = 1;
                if (usb_speed == LIBUSB_SPEED_HIGH)
                    cable_ver = 2;
                else if (usb_speed == LIBUSB_SPEED_SUPER)
                    cable_ver = 3;

                dsv_info("The cable's USB port version: %d.0", cable_ver);

                if (usb30_support && usb_speed == LIBUSB_SPEED_HIGH){
                    QString str_err(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CHECK_USB_SPEED_ERROR),
                        "Plug the device into a USB 2.0 port will seriously affect its performance.\nPlease replug it into a USB 3.0 port."));
                    delay_prop_msg(str_err);
                }
            }
        }
    }

    void MainWindow::trigger_message(int msg)
    {
        _event.trigger_message(msg);
    }

    void MainWindow::on_trigger_message(int msg)
    {
        _session->broadcast_msg(msg);
    }

    void MainWindow::reset_all_view()
    {
        _sampling_bar->reload();
        _view->status_clear();
        _view->reload();
        _view->set_device();
        _trigger_widget->update_view();
        _trigger_widget->device_updated();
        _trig_bar->reload(); 
        _dso_trigger_widget->update_view();
        _measure_widget->reload();

        if (_device_agent->get_work_mode() == ANALOG)
            _view->get_viewstatus()->setVisible(false);
        else
            _view->get_viewstatus()->setVisible(true);
    }

    bool MainWindow::confirm_to_store_data()
    {   
        bool ret = false;
        _is_save_confirm_msg = true;       

        if (_session->have_hardware_data() && _session->is_first_store_confirm())
        {   
            // Only popup one time.
            ret =  MsgBox::Confirm(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_SAVE_CAPDATE), "Save captured data?"));

            if (!ret && _is_auto_switch_device)
            {
                dsv_info("The data save confirm end, auto switch to the new device.");
                _is_auto_switch_device = false;

                if (_session->is_working())
                    _session->stop_capture();

                _session->set_default_device();
            }
        }

        _is_save_confirm_msg = false;
        return ret;
    }

    void MainWindow::check_config_file_version()
    {
        auto device_agent = _session->get_device();
        if (device_agent->is_file() && device_agent->is_new_device())
        {
            if (device_agent->get_work_mode() == LOGIC)
            {   
                int version = -1; 
                if (device_agent->get_config_int16(SR_CONF_FILE_VERSION, version))
                {
                    if (version == 1)
                    {
                        QString strMsg(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CHECK_SESSION_FILE_VERSION_ERROR), 
                        "Current loading file has an old format. \nThis will lead to a slow loading speed. \nPlease resave it after loaded."));
                        MsgBox::Show(strMsg);
                    }
                }
            }
        }
    }

    void MainWindow::load_device_config()
    {   
        _title_ext_string = "";        
        int mode = _device_agent->get_work_mode();
        QString file;

        if (_device_agent->is_hardware())
        { 
            QString ses_name = gen_config_file_path(true);

            bool bExist = false;

            QFile sf(ses_name);
            if (!sf.exists()){
                dsv_info("Try to load the low version profile.");
                ses_name =  gen_config_file_path(false);
            }
            else{
                bExist = true;
            }

            if (!bExist)
            {
                QFile sf2(ses_name);
                if (!sf2.exists()){
                    dsv_info("Try to load the default profile.");
                    ses_name = _file_bar->genDefaultSessionFile();
                }
            } 

            file =  ses_name;
        }
        else if (_device_agent->is_demo())
        {
            QDir dir(GetFirmwareDir());
            if (dir.exists())
            {
                QString ses_name = dir.absolutePath() + "/" 
                            + _device_agent->driver_name() + QString::number(mode) + ".dsc";

                QFile sf(ses_name);
                if (sf.exists()){
                    file = ses_name;
                }
            }
        }

        if (file != ""){
            bool ret = load_config_from_file(file);
            if (ret && _device_agent->is_hardware()){
                _title_ext_string = file;
            }
        }
    }

    QJsonDocument MainWindow::get_config_json_from_data_file(QString file, bool &bSucesss)
    {
        QJsonDocument sessionDoc;
        QJsonParseError error;
        bSucesss = false;

        if (file == ""){
            dsv_err("File name is empty.");
            assert(false);
        }

        auto f_name = pv::path::ConvertPath(file);
        ZipReader rd(f_name.c_str());
        auto *data = rd.GetInnterFileData("session");

        if (data != NULL)
        {
            QByteArray raw_bytes = QByteArray::fromRawData(data->data(), data->size());
            QString jsonStr(raw_bytes.data());
            QByteArray qbs = jsonStr.toUtf8();
            sessionDoc = QJsonDocument::fromJson(qbs, &error);

            if (error.error != QJsonParseError::NoError)
            {
                QString estr = error.errorString();
                dsv_err("File::get_session(), parse json error:\"%s\"!", estr.toUtf8().data());
            }
            else{
                bSucesss = true;
            }

            rd.ReleaseInnerFileData(data);
        }

        return sessionDoc;
    }

    QJsonArray MainWindow::get_decoder_json_from_data_file(QString file, bool &bSucesss)
    {
        QJsonArray dec_array;
        QJsonParseError error;

        bSucesss = false;

        if (file == ""){
            dsv_err("File name is empty.");
            assert(false);
        }

        /* read "decoders" */
        auto f_name = path::ConvertPath(file);
        ZipReader rd(f_name.c_str());
        auto *data = rd.GetInnterFileData("decoders");

        if (data != NULL)
        {
            QByteArray raw_bytes = QByteArray::fromRawData(data->data(), data->size());
            QString jsonStr(raw_bytes.data());
            QByteArray qbs = jsonStr.toUtf8();
            QJsonDocument sessionDoc = QJsonDocument::fromJson(qbs, &error);

            if (error.error != QJsonParseError::NoError)
            {
                QString estr = error.errorString();
                dsv_err("MainWindow::get_decoder_json_from_file(), parse json error:\"%s\"!", estr.toUtf8().data());
            }
            else{
                bSucesss = true;
            }

            dec_array = sessionDoc.array();
            rd.ReleaseInnerFileData(data);
        }

        return dec_array;
    }

    void MainWindow::update_toolbar_view_status()
    {
        _sampling_bar->update_view_status();
        _file_bar->update_view_status();
        _trig_bar->update_view_status();
    }

    void MainWindow::OnMessage(int msg)
    {
        switch (msg)
        {
            case DSV_MSG_DEVICE_LIST_UPDATED:
            {
                _sampling_bar->update_device_list();
                break;
            }
            case DSV_MSG_START_COLLECT_WORK_PREV:
            {
                // While a 1PPS measurement drives the acquisition, the PPS
                // manager owns the trigger configuration. Letting the trigger
                // dock re-apply its own settings here would reset trigger_en to
                // 0 and the position to the dock slider value, so the capture
                // would never trigger and every frame came back flat.
                // A calibration run owns the trigger the same way: without this
                // check each calibration frame was captured free-running, the
                // reference edge landed at an arbitrary phase in the window, and
                // a 1.2 s window then only held two edges ~20% of the time.
                if (_device_agent->get_work_mode() == LOGIC
                        && !(_pps_manager && _pps_manager->is_running())
                        && !pps::PpsCalibration::any_active())
                    _trigger_widget->try_commit_trigger();
                else if (_device_agent->get_work_mode() == DSO)
                    _dso_trigger_widget->check_setting();

                _view->capture_init();
                _view->on_state_changed(false);
                break;
            }
            case DSV_MSG_START_COLLECT_WORK:
            {
                update_toolbar_view_status();
                _view->on_state_changed(false);
                _protocol_widget->update_view_status();
                break;
            }        
            case DSV_MSG_COLLECT_END:
            {
                prgRate(0);
                _view->repeat_unshow();
                _view->on_state_changed(true);                 
                break;
            }
            case DSV_MSG_END_COLLECT_WORK:
            {
                update_toolbar_view_status();
                _protocol_widget->update_view_status();   
                break;
            }
            case DSV_MSG_CURRENT_DEVICE_CHANGE_PREV:
            {
                if (_msg != NULL){
                    _msg->close();
                    _msg = NULL;
                }
                _view->hide_calibration();

                // The protocol rows belong to the device that is going away and
                // are torn down here. Keep their settings first: a data file that
                // carries no decoder section (autosave frames, raw exports) can
                // then re-create the same rows instead of opening with nothing to
                // decode.
                _last_decoder_json = QJsonArray();
                {
                    StoreSession ss(_session);
                    ss.gen_decoders_json(_last_decoder_json);
                }

                _protocol_widget->del_all_protocol();
                _view->reload();
                break;
            }
            case DSV_MSG_CURRENT_DEVICE_CHANGED:
            {
                reset_all_view();
                load_device_config();
                update_title_bar_text();
                _sampling_bar->update_device_list();
                
                _logo_bar->dsl_connected(_session->get_device()->is_hardware());
                update_toolbar_view_status();
                _session->device_event_object()->device_updated();

                if (_device_agent->is_hardware())
                {
                    _session->on_load_config_end();
                }                
                
                if (_device_agent->get_work_mode() == LOGIC && _device_agent->is_file() == false)
                    _view->auto_set_max_scale();

                if (_device_agent->is_file())
                {
                    check_config_file_version();

                    bool bDoneDecoder = false;
                    bool bLoadSuccess = false;
                    QJsonDocument doc = get_config_json_from_data_file(_device_agent->path(), bLoadSuccess);

                    if (bLoadSuccess){
                        load_config_from_json(doc, bDoneDecoder);
                    }

                    if (!bDoneDecoder && _device_agent->get_work_mode() == LOGIC)
                    {                    
                        QJsonArray deArray = get_decoder_json_from_data_file(_device_agent->path(), bLoadSuccess);

                        if (bLoadSuccess && !deArray.isEmpty()){
                            StoreSession ss(_session);
                            ss.load_decoders(_protocol_widget, deArray);
                            bDoneDecoder = !_session->get_decode_signals().empty();
                        }

                        // Files written by the autosave manager and raw exports
                        // hold samples only - no decoder section. Opening such a
                        // file used to leave the protocol rows empty, which reads
                        // as "file opened but nothing decoded". Fall back to the
                        // decoders that were on screen before the switch; the
                        // capture started below then decodes the file with them.
                        if (!bDoneDecoder && !_last_decoder_json.isEmpty()){
                            dsv_info("File has no decoder section, restoring %d decoder(s) from the session.",
                                    _last_decoder_json.size());
                            StoreSession ss(_session);
                            ss.load_decoders(_protocol_widget, _last_decoder_json);
                        }
                    }

                    _view->update_all_trace_postion();                
                    QTimer::singleShot(100, this, [this](){
                        _session->start_capture(true);
                    });
                }
                else if (_device_agent->is_demo())
                {
                    if(_device_agent->get_work_mode() == LOGIC)
                    {
                        _pattern_mode = _device_agent->get_demo_operation_mode();
                        _protocol_widget->del_all_protocol();
                        _view->auto_set_max_scale();

                        if(_pattern_mode != "random"){
                        load_demo_decoder_config(_pattern_mode);
                        }
                    }
                }
        
                calc_min_height();

                if (_device_agent->is_hardware() && _device_agent->is_new_device()){
                    check_usb_device_speed();
                }

                break;
            }
            case DSV_MSG_DEVICE_OPTIONS_UPDATED:
            {
                _trigger_widget->device_updated();
                _measure_widget->reload();
                _view->check_calibration();                      
                break;
            }
            case DSV_MSG_DEVICE_DURATION_UPDATED:
            {
                _trigger_widget->device_updated();
                _view->timebase_changed();
                break;
            }
            case DSV_MSG_DEVICE_MODE_CHANGED:
            {
                _view->mode_changed(); 
                reset_all_view();
                load_device_config();
                update_title_bar_text();
                _view->hide_calibration();

                update_toolbar_view_status();
                _sampling_bar->update_sample_rate_list();

                if (_device_agent->is_hardware())
                    _session->on_load_config_end();
                
                if (_device_agent->get_work_mode() == LOGIC)
                    _view->auto_set_max_scale();

                if(_device_agent->is_demo())
                {
                    _pattern_mode = _device_agent->get_demo_operation_mode();
                    _protocol_widget->del_all_protocol();

                    if(_device_agent->get_work_mode() == LOGIC)
                    {
                        if(_pattern_mode != "random"){
                            _device_agent->update();
                            load_demo_decoder_config(_pattern_mode);
                        }
                    }
                }

                calc_min_height();           
                break;
            }
            case DSV_MSG_NEW_USB_DEVICE:
            {
                if (_msg != NULL){
                    _msg->close();
                    _msg = NULL;
                }

                _sampling_bar->update_device_list();

                //If the current device is working, do not remind to switch new device.
                if (_session->get_device()->is_hardware() && _session->is_working()){
                    return;
                }

                // If a saving task is running, not need to remind to switch device, 
                // when the task end, the new device will be selected.
                if (_session->get_device()->is_demo() == false && !_is_save_confirm_msg)
                {
                    QString msgText = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_TO_SWITCH_DEVICE), "To switch the new device?");
                    
                    if (MsgBox::Confirm(msgText, "", &_msg, NULL) == false){ 
                        _msg = NULL;
                        return;
                    }
                    _msg = NULL;
                }

                // The store confirm is not processed.
                if (_is_save_confirm_msg){
                    dsv_info("New device attached:Waitting for the confirm box be closed.");
                    _is_auto_switch_device = true; 
                    return;
                }

                if (_session->is_saving()){
                    dsv_info("New device attached:Waitting for store the data. and will switch to new device.");
                    _is_auto_switch_device = true;
                    return;
                }

                int mode = _device_agent->get_work_mode();

                if (mode != DSO && confirm_to_store_data())
                {
                    _is_auto_switch_device = true;

                    if (_session->is_working())
                        _session->stop_capture();

                    on_save();
                }
                else
                {   
                    if (_session->is_working())
                        _session->stop_capture();
                    
                    _session->set_default_device();
                }

                break;
            }
            case DSV_MSG_CURRENT_DEVICE_DETACHED:
            {
                if (_msg != NULL){
                    _msg->close();
                    _msg = NULL;
                }

                // Save current config, and switch to the last device.
                _session->device_event_object()->device_updated();
                save_config();
                _view->hide_calibration();

                if (_session->is_saving()){
                    dsv_info("Device detached:Waitting for store the data. and will switch to new device.");
                    _is_auto_switch_device = true;
                    return;
                }

                if (confirm_to_store_data()){
                    _is_auto_switch_device = true;
                    on_save();
                }
                else{
                    _session->set_default_device();
                }
                break;
            }
            case DSV_MSG_SAVE_COMPLETE:
            {
                _session->clear_store_confirm_flag();

                if (_is_auto_switch_device)
                {
                    _is_auto_switch_device = false;
                    _session->set_default_device();
                }
                else
                {
                    ds_device_handle devh = _sampling_bar->get_next_device_handle();
                    if (devh != NULL_HANDLE)
                    {
                        dsv_info("Auto switch to the selected device.");
                        _session->set_device(devh);
                    }
                }
                break;
            }
            case DSV_MSG_CLEAR_DECODE_DATA:
            {
                if (_device_agent->get_work_mode() == LOGIC)
                    _protocol_widget->reset_view();
                break;
            }            
            case DSV_MSG_STORE_CONF_PREV:
            {
                if (_device_agent->is_hardware() && _session->have_hardware_data() == false){
                    _sampling_bar->commit_settings();
                }
                break;
            }
            case DSV_MSG_BEGIN_DEVICE_OPTIONS:
            case DSV_MSG_COLLECT_MODE_CHANGED:
            {
                if(_device_agent->is_demo()){
                    _pattern_mode = _device_agent->get_demo_operation_mode();
                }
                if (msg == DSV_MSG_COLLECT_MODE_CHANGED){
                    _trigger_widget->device_updated();
                    _view->update();
                }           
                break;
            }
            case DSV_MSG_END_DEVICE_OPTIONS:
            case DSV_MSG_DEMO_OPERATION_MODE_CHNAGED:
            {
                if(_device_agent->is_demo() &&_device_agent->get_work_mode() == LOGIC){                
                    QString pattern_mode = _device_agent->get_demo_operation_mode();       
                    
                    if(pattern_mode != _pattern_mode)
                    {
                        _pattern_mode = pattern_mode; 

                        _device_agent->update();
                        _session->clear_view_data();
                        _session->init_signals();
                        update_toolbar_view_status();
                        _sampling_bar->update_sample_rate_list();
                        _protocol_widget->del_all_protocol();
                            
                        if(_pattern_mode != "random"){
                            _session->set_collect_mode(COLLECT_SINGLE);
                            load_demo_decoder_config(_pattern_mode);

                            if (msg == DSV_MSG_END_DEVICE_OPTIONS)
                                _session->start_capture(false); // Auto load data.
                        }
                    }                
                }
                calc_min_height();            
                break;
            }
            case DSV_MSG_APP_OPTIONS_CHANGED:
            {
                update_title_bar_text();
                break;
            }
            case DSV_MSG_FONT_OPTIONS_CHANGED:
            {
                UiManager::Instance()->Update(UI_UPDATE_ACTION_FONT);          
                break;
            }
            case DSV_MSG_DATA_POOL_CHANGED:
            {
                _view->check_measure();
                break;
            }            
        }
    }

    void MainWindow::calc_min_height()
    {
        if (_frame != NULL)
        {
            if (_device_agent->get_work_mode() == LOGIC)
            {
                int ch_num = _session->get_ch_num(-1);
                int win_height = Base_Height + Per_Chan_Height * ch_num;

                if (win_height < Min_Height)
                    _frame->setMinimumHeight(win_height);
                else
                    _frame->setMinimumHeight(Min_Height);
            }
            else{
                _frame->setMinimumHeight(Min_Height);
            }
        }  
    }

    void MainWindow::delay_prop_msg(QString strMsg)
    {
        _strMsg = strMsg;
        if (_strMsg != ""){
            _delay_prop_msg_timer.Start(500);
        }
    }

    void MainWindow::auto_save_frame()
    {
        // MainWindow itself implements ISessionDataGetter.
        _auto_save_mgr->start_frame(this);
    }

    void MainWindow::on_delay_prop_msg()
    {
        _delay_prop_msg_timer.Stop();

        if (_strMsg != ""){
            MsgBox::Show("", _strMsg, this, &_msg);
            _msg = NULL;
        }            
    }

    void MainWindow::update_title_bar_text()
    {
          // Set the title
        QString title = QApplication::applicationName() + " v" + QApplication::applicationVersion();
        AppConfig &app = AppConfig::Instance();

        if (_title_ext_string != "" && app.appOptions.displayProfileInBar){
            title += " [" + _title_ext_string + "]";
        }

        if (_lst_title_string != title){
            _lst_title_string = title;

            setWindowTitle(QApplication::translate("MainWindow", title.toLocal8Bit().data(), 0));
            _title_bar->setTitle(this->windowTitle());
        }        
    }

    void MainWindow::load_demo_decoder_config(QString optname)
    { 
        QString file = GetAppDataDir() + "/demo/logic/" + optname + ".demo";
        bool bLoadSurccess = false;

        QJsonArray deArray = get_decoder_json_from_data_file(file, bLoadSurccess);

        if (bLoadSurccess){
            StoreSession ss(_session);
            ss.load_decoders(_protocol_widget, deArray);
        }

        QJsonDocument doc = get_config_json_from_data_file(file, bLoadSurccess);
        if (bLoadSurccess){
            load_channel_view_indexs(doc);
        }
        
        _view->update_all_trace_postion();
    }

    QWidget* MainWindow::GetBodyView()
    {
        return _view;
    }

    void MainWindow::OnWindowsPowerEvent(bool bEnterSleep)
    {
        _session->ProcessPowerEvent(bEnterSleep);
    }
  
} // namespace pv
