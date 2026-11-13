/*
 * Result dock for the 1PPS measurement mode.
 * Shows the latest frame value and running statistics per measured channel.
 *
 * All the PPS commands live here as icon buttons (configure / start / stop /
 * calibrate / waveform); the toolbar's PPS button only shows or hides this
 * window.
 */
#ifndef DSVIEW_PV_DOCK_PPSRESULTDOCK_H
#define DSVIEW_PV_DOCK_PPSRESULTDOCK_H

#include <QScrollArea>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QToolButton>
#include <QString>
#include <QMap>
#include <vector>

#include "../pps/ppsmanager.h"

namespace pv {

namespace dock {

class PpsResultDock : public QScrollArea
{
    Q_OBJECT

public:
    explicit PpsResultDock(pps::PpsManager *manager, QWidget *parent = nullptr);

public slots:
    void refresh();

signals:
    // Raised by the dock's own buttons; the main window owns the actual
    // behaviour so the toolbar toggle and the dock always agree.
    void config_requested();
    void start_requested();
    void stop_requested();
    void calib_requested();
    void wave_requested();

private slots:
    // Shows the folder of the current result CSV in Explorer, with the file
    // itself selected; the label above only carries the file name.
    void on_open_file();

private:
    QToolButton *make_button(QToolButton *btn, const QString &icon,
            const QString &tip);
    void apply_icons();
    // Frames / 耗时 / 测试时长 / file name line. Also runs on its own clock so
    // 测试时长 keeps counting between two frames.
    void update_info_line();

    pps::PpsManager *_manager;
    QWidget *_host;
    // One compact line: frames, round-trip time and the CSV file name.
    QLabel *_info_label;
    QString _info_shown;      // last text put into _info_label
    QPushButton *_open_btn;
    QCheckBox *_autosave_chk;   // "自动保存波形" (every trigger saves its raw frame)
    QToolButton *_config_btn;
    QToolButton *_start_btn;
    QToolButton *_stop_btn;
    QToolButton *_calib_btn;
    QToolButton *_wave_btn;
    QTableWidget *_table;
    QString _icon_path;
    QTimer *_run_tick;      // 1 s, keeps 测试时长 live while a run is going
};

} // namespace dock
} // namespace pv

#endif // DSVIEW_PV_DOCK_PPSRESULTDOCK_H
