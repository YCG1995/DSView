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
#include <QLabel>
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

private:
    QToolButton *make_button(QToolButton *btn, const QString &icon,
            const QString &tip);
    void apply_icons();

    pps::PpsManager *_manager;
    QWidget *_host;
    QLabel *_frame_label;
    QLabel *_timing_label;
    QLabel *_csv_label;
    QString _csv_shown;       // last text put into _csv_label
    QToolButton *_config_btn;
    QToolButton *_start_btn;
    QToolButton *_stop_btn;
    QToolButton *_calib_btn;
    QToolButton *_wave_btn;
    QTableWidget *_table;
    QString _icon_path;
};

} // namespace dock
} // namespace pv

#endif // DSVIEW_PV_DOCK_PPSRESULTDOCK_H
