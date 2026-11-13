/*
 * Calibration dialog for the 1PPS measurement mode.
 * Runs a short, single-channel, 100MHz capture series on a channel that carries
 * a known-accurate 10ms or 1PPS signal, derives the analyzer's crystal error and
 * hands the factor back to the caller, which stores it as the calibration
 * coefficient.
 */
#ifndef DSVIEW_PV_DIALOGS_PPSCALIBDLG_H
#define DSVIEW_PV_DIALOGS_PPSCALIBDLG_H

#include <QDialog>
#include <QSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QTextEdit>

#include "../pps/ppscalibration.h"

namespace pv {

class SigSession;

namespace dialogs {

class PpsCalibDlg : public QDialog
{
    Q_OBJECT

public:
    PpsCalibDlg(SigSession *session, int default_channel, int max_channel,
                QWidget *parent = nullptr);
    ~PpsCalibDlg();

    // Factor computed by the last finished run (1.0 when none ran).
    double factor() const { return _factor; }
    bool has_result() const { return _has_result; }

private slots:
    void on_start();
    void on_stop();
    void on_progress(int done, int total, double measured_ms, int edges);
    void on_finished(const pps::PpsCalibResult &result);
    void on_failed(const QString &message);
    void on_status(const QString &message);

private:
    void append_log(const QString &line);
    void set_running(bool running);

    SigSession *_session;
    pps::PpsCalibration *_calib;

    QSpinBox *_channel_spin;
    QSpinBox *_count_spin;
    QLabel *_window_label;
    QLabel *_state_label;
    QPushButton *_start_btn;
    QPushButton *_stop_btn;
    QTextEdit *_log;

    double _factor;
    bool _has_result;
};

} // namespace dialogs
} // namespace pv

#endif // DSVIEW_PV_DIALOGS_PPSCALIBDLG_H
