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


#ifndef DSVIEW_PV_MAINWINDOW_H
#define DSVIEW_PV_MAINWINDOW_H

#include <list>
#include <vector>
#include <QMainWindow>
#include <QTranslator> 
#include "dialogs/dsmessagebox.h"
#include "interface/icallbacks.h"
#include "eventobject.h" 
#include "mcp/mcpserver.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <chrono>
#include <QTimer>
#include <QPointer>
#include "dstimer.h"

class QAction;
class QMenuBar;
class QMenu;
class QVBoxLayout;
class QStatusBar;
class QToolBar;
class QWidget;
class QDockWidget;
class AppControl;
class DeviceAgent;

using std::chrono::high_resolution_clock;
using std::chrono::milliseconds;

namespace pv {

class SigSession;
class AutoSaveManager;

namespace mcp {
class McpServer;
class IPpsControl;
class IConsoleTextProvider;
}

namespace pps {
class PpsManager;
class PpsWaveWindow;
}

namespace toolbars {
class SamplingBar;
class TrigBar;
class FileBar;
class LogoBar;
class TitleBar;
}

namespace dock{
class ProtocolDock;
class TriggerDock;
class DsoTriggerDock;
class MeasureDock;
class SearchDock;
class DecoderConsoleDock;
class PpsResultDock;
}

namespace view {
class View;
}
 
//The mainwindow,referenced by MainFrame
//TODO: create graph view,toolbar,and show device list
class MainWindow : 
    public QMainWindow,
    public ISessionCallback,
    public IMainForm,
    public ISessionDataGetter,
    public IMessageListener,
    public mcp::IPpsControl,
    public mcp::IConsoleTextProvider
{
	Q_OBJECT

public:
    static const int Min_Width  = 350;
    static const int Min_Height = 300;
    static const int Base_Height = 150;
    static const int Per_Chan_Height = 35;
     
public:
    explicit MainWindow(toolbars::TitleBar *title_bar, QWidget *parent = 0);

    void openDoc();

public slots: 
    void switchTheme(QString style);
    void restore_dock();

private slots:
	void on_load_file(QString file_name);
    void on_open_doc();  
    void on_protocol(bool visible);
    void on_trigger(bool visible);
    void on_measure(bool visible);
    void on_search(bool visible);
    void on_screenShot();
    void on_save();

    void on_export();
    bool on_load_session(QString name);  
    bool on_store_session(QString name); 
    void on_data_updated();
 
    void on_session_error();
    void on_signals_changed();
    void on_receive_trigger(quint64 trigger_pos);
    void on_frame_ended();
    void on_frame_began();
    void on_decode_done();
    void on_receive_data_len(quint64 len);
    void on_cur_snap_samplerate_changed();
    void on_trigger_message(int msg);
    void on_delay_prop_msg();
    void on_load_device_first();

    void ensure_pps();
    void on_pps_config();
    void on_pps_start();
    void on_pps_stop();
    void on_pps_result(bool visible);
    void on_pps_wave();
    void on_pps_calib();
    void on_pps_status(const QString &text);

signals:
    void prgRate(int progress);

public:
    //IMainForm
    void switchLanguage(int language) override;
    bool able_to_close();    
    QWidget* GetBodyView();

    void OnWindowsPowerEvent(bool bEnterSleep);

public:
    //mcp::IPpsControl — 1PPS measurement driven from the MCP endpoint.
    QJsonObject pps_describe() override;
    bool pps_configure(const QJsonObject &cfg, QString &error) override;
    bool pps_start(QString &error) override;
    void pps_stop() override;
    QJsonObject pps_status() override;

    //mcp::IConsoleTextProvider — what the decoder console window currently shows.
    QString console_text() const override;

private: 
	void setup_ui();
    void retranslateUi(); 
    // 显示/隐藏右侧面板；Android 下展开时按屏宽铺满（桌面按原尺寸）
    void show_side_dock(QDockWidget *dock, bool visible);
    // Enables exactly the PPS channel set on the main capture, so the main view
    // matches what the 1PPS measurement samples.
    void apply_pps_channels_to_session();
    // 1PPS 借用设备时会按自己的通道集重写使能位；测完/校准完必须还原成用户
    // 进入 1PPS 之前的状态，否则界面显示的通道数与驱动实际不一致。
    void save_channels_before_pps();
    void restore_channels_after_pps();
    std::vector<char> _pps_saved_channels;
    bool _pps_channels_saved = false;
    bool eventFilter(QObject *object, QEvent *event);
    void check_usb_device_speed();
    void reset_all_view();
    bool confirm_to_store_data();
    void update_toolbar_view_status();
    void calc_min_height();    
    void update_title_bar_text();

    //json operation
private:
    QString gen_config_file_path(bool isNewFormat);
    bool load_config_from_file(QString file);
    bool load_config_from_json(QJsonDocument &doc, bool &haveDecoder);
    void load_device_config();
    bool gen_config_json(QJsonObject &sessionVar);
    void save_config();
    bool save_config_to_file(QString file);
    void load_channel_view_indexs(QJsonDocument &doc); 
    QJsonDocument get_config_json_from_data_file(QString file, bool &bSucesss);
    QJsonArray get_decoder_json_from_data_file(QString file, bool &bSucesss);
    void check_config_file_version(); 
    void load_demo_decoder_config(QString optname);

  
private:
    //ISessionCallback 
    void session_error() override;
    void session_save() override;
    void data_updated() override;
    void update_capture() override;
    void cur_snap_samplerate_changed() override;      
    void signals_changed() override;
    void receive_trigger(quint64 trigger_pos) override;
    void frame_ended() override;
    void frame_began() override;
    void show_region(uint64_t start, uint64_t end, bool keep) override;
    void show_wait_trigger() override;
    void repeat_hold(int percent) override;
    void decode_done() override;
    void receive_data_len(quint64 len) override;
    void receive_header() override;    
    void trigger_message(int msg) override;
    void delay_prop_msg(QString strMsg) override;
    void auto_save_frame() override;

    //ISessionDataGetter
    bool genSessionData(std::string &str) override;

    //IMessageListener
    void OnMessage(int msg) override;

private: 
	pv::view::View          *_view;
    dialogs::DSMessageBox   *_msg;

	QMenuBar                *_menu_bar;
	QMenu                   *_menu_file;
	QAction                 *_action_open;
	QAction                 *_action_connect;
	QAction                 *_action_quit;

	QMenu                   *_menu_view;
	QAction                 *_action_view_zoom_in;
	QAction                 *_action_view_zoom_out;
	QAction                 *_action_view_show_cursors;

	QMenu                   *_menu_help;
	QAction                 *_action_about;

	QWidget                 *_central_widget;
	QVBoxLayout             *_vertical_layout;

	toolbars::SamplingBar   *_sampling_bar;
    toolbars::TrigBar       *_trig_bar;
    toolbars::FileBar       *_file_bar;
    toolbars::LogoBar       *_logo_bar; //help button, on top right
    toolbars::TitleBar      *_title_bar;


    QDockWidget             *_protocol_dock;
    dock::ProtocolDock      *_protocol_widget;
    QDockWidget             *_trigger_dock;
    QDockWidget             *_dso_trigger_dock;
    dock::TriggerDock       *_trigger_widget;
    dock::DsoTriggerDock    *_dso_trigger_widget;
    QDockWidget             *_measure_dock;
    dock::MeasureDock       *_measure_widget;
    QDockWidget             *_search_dock;
    dock::SearchDock        *_search_widget;
    QDockWidget                 *_decoder_console_dock;
    dock::DecoderConsoleDock    *_decoder_console_widget;

    QTranslator     _qtTrans;
    QTranslator     _myTrans;
    EventObject     _event; 
    SigSession      *_session;
    DeviceAgent     *_device_agent;
    AutoSaveManager *_auto_save_mgr;
    mcp::McpServer *_mcp_server;
    pps::PpsManager *_pps_manager = nullptr;
    QDockWidget *_pps_dock = nullptr;
    dock::PpsResultDock *_pps_result = nullptr;
    QPointer<pps::PpsWaveWindow> _pps_wave;
    bool            _is_auto_switch_device;
    high_resolution_clock::time_point _last_key_press_time;
    bool            _is_save_confirm_msg;
    QString         _pattern_mode;
    // Decoders that were active before the last device switch. A data file that
    // carries no decoder section (autosave frames, raw exports) falls back to
    // these, so opening a saved capture still decodes instead of showing nothing.
    QJsonArray      _last_decoder_json;
    QWidget         *_frame;
    DsTimer         _delay_prop_msg_timer;
    QString         _strMsg;
    QString         _lst_title_string;
    QString         _title_ext_string;

    int         _key_value;
    bool        _key_vaild;
};

} // namespace pv

#endif // DSVIEW_PV_MAINWINDOW_H
