/*
 * Calibration run for the 1PPS measurement mode.
 *
 * The logic analyzer times everything with its own crystal, which is typically
 * a few ppm off. Feeding it a known-accurate interval (a 1PPS pulse or a 10ms
 * test signal) and measuring how long that interval looks in the analyzer's
 * own time base gives the error directly:
 *
 *     factor = standard_interval / measured_interval
 *
 * The run is deliberately independent of the 1PPS measurement loop: it uses
 * the smallest channel count the device allows, 100 MHz and a 1.2 s window
 * triggered at 0% on the calibration channel, so at least two edges of the
 * input are always inside the frame.
 */
#ifndef DSVIEW_PV_PPS_PPSCALIBRATION_H
#define DSVIEW_PV_PPS_PPSCALIBRATION_H

#include <stdint.h>
#include <vector>
#include <QObject>
#include <QString>
#include <QElapsedTimer>

#include "../interface/icallbacks.h"
#include "ppsdecoder.h"

class QTimer;

namespace pv {

class SigSession;

namespace pps {

struct PpsCalibConfig
{
    int channel;            // channel the reference signal is wired to
    int captures;           // number of captures to average, default 10
    double window_ms;       // capture window, must be >1000 and <2000
    uint64_t sample_rate;   // fixed at 100MHz

    PpsCalibConfig();
};

// Outcome of one calibration run.
struct PpsCalibResult
{
    double factor;          // standard_interval / measured_interval
    double measured_ms;     // averaged measured interval, in ms
    double standard_ms;     // 1000.0 for 1PPS, 10.0 for a 10ms input
    int captures;           // captures actually averaged
    int edges;              // edges seen in the last capture
    QString input_name;     // "1PPS" or "10ms"

    PpsCalibResult();
};

class PpsCalibration : public QObject, public IMessageListener
{
    Q_OBJECT

public:
    explicit PpsCalibration(SigSession *session, QObject *parent = nullptr);
    ~PpsCalibration();

    bool is_running() const { return _running; }
    const QString &error() const { return _error; }

    // True while any calibration run owns the hardware trigger. MainWindow
    // must not let the trigger dock re-apply its own settings then: doing so
    // resets trigger_en and the trigger position, and the capture degrades to
    // a free-running frame that starts at an arbitrary phase.
    static bool any_active();

    bool start(const PpsCalibConfig &cfg);
    void stop();

    // IMessageListener
    void OnMessage(int msg) override;

signals:
    // progress: 0-based capture index, total captures, and the measured
    // interval of that capture in milliseconds (0 when it was unusable).
    void progress(int done, int total, double measured_ms, int edges);
    void finished(const pps::PpsCalibResult &result);
    void failed(const QString &message);
    void status_message(const QString &message);

private:
    bool apply_hardware_config();
    void finish_current_capture();
    void advance_after_capture(bool usable, size_t edges,
            double period_samples);

    SigSession *_session;
    PpsCalibConfig _cfg;
    bool _running;
    bool _stop_requested;
    QString _error;

    int _done;
    int _retry;
    int _active_ch_num;
    uint64_t _active_rate;
    uint64_t _sample_count;
    double _sum_samples;    // summed measured period, in samples
    int _usable;
    int _last_edges;
    double _last_measured_ms;

    QElapsedTimer _capture_timer;
    QTimer *_watchdog;
    bool _owns_trigger;     // registered in s_active_runs
};

} // namespace pps
} // namespace pv

#endif // DSVIEW_PV_PPS_PPSCALIBRATION_H
