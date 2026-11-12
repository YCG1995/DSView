/*
 * 1PPS measurement state machine.
 * Drives buffer-mode single captures, synchronous decode, CSV logging and
 * immediate re-triggering without the generic Python decode path.
 */
#ifndef DSVIEW_PV_PPS_PPSMANAGER_H
#define DSVIEW_PV_PPS_PPSMANAGER_H

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

struct PpsConfig
{
    int ref_channel;            // reference logic channel
    int ref_edge;               // PPS_EDGE
    int trigger_pos_percent;    // 0..100, default 50
    double capture_time_ms;     // time window, default 10
    uint64_t sample_rate;       // selected buffer-mode rate
    QString save_dir;           // directory for CSV files
    // Master switch for "save the raw frame when a measured channel has no
    // edge". Each channel can opt in separately (PpsChannel::save_on_loss).
    bool auto_save_loss;
    std::vector<PpsChannel> channels; // measured channels (ref excluded)
    // Channels the user explicitly switched off in the dialog. They are kept
    // here so reopening the dialog shows the same tick state again.
    std::vector<int> disabled_channels;
    // Channel range the user picked: 8 (CH0-CH7, up to 1GHz) or 16 (CH0-CH15,
    // up to 500MHz). 0 lets the driver decide from the requested rate.
    int mode_channels;
    // Multiplier applied to every measured time difference, to compensate the
    // logic analyzer's own crystal error. 1.0 = uncompensated.
    double calib_factor;

    PpsConfig();
};

/*
 * Channel mode of a logic analyzer that runs in buffer (non-stream) mode.
 * Each mode has a fixed channel count and a maximum samplerate, so which
 * channels can be measured depends on both the model and the chosen rate.
 */
struct PpsModeInfo
{
    int id;                 // SR_CONF_CHANNEL_MODE value
    int channels;           // usable channel count in this mode
    uint64_t max_rate;      // highest samplerate supported by this mode
};

/*
 * Device capabilities used to fill the PPS configuration.
 * Populated from the driver so every supported logic analyzer works,
 * instead of assuming a fixed 16-channel 1GHz device.
 */
struct PpsDeviceCaps
{
    int channel_num;                    // channels of the active buffer mode
    uint64_t max_rate;                  // samplerate limit of the active mode
    uint64_t hw_depth;                  // per-channel sample depth
    std::vector<uint64_t> rates;        // supported samplerates, ascending
    std::vector<PpsModeInfo> modes;     // buffer channel modes, largest first

    PpsDeviceCaps();

    // Channel count usable at the given samplerate.
    int channels_for_rate(uint64_t rate) const;
    // Highest samplerate that still offers the given channel count.
    uint64_t rate_for_channels(int channels) const;
    // Longest capture time (ms) that fits the hardware depth at this rate.
    double max_capture_ms(uint64_t rate, int channels) const;
    // The mode that will actually be used for this configuration.
    const PpsModeInfo *mode_for_rate(uint64_t rate) const;
};

// Reads the capabilities above from the currently active device.
bool query_device_caps(SigSession *session, PpsDeviceCaps &caps);

// Per-channel running statistics over valid frames.
struct PpsStats
{
    int channel_index;
    int count;
    double mean;
    double minimum;
    double maximum;
    double stddev;
};

class PpsManager : public QObject, public IMessageListener
{
    Q_OBJECT

public:
    explicit PpsManager(SigSession *session, QObject *parent = nullptr);
    ~PpsManager();

    bool is_running() const { return _running; }
    // Reason of the last failed start(), for the UI to surface it.
    const QString &start_error() const { return _start_error; }
    const PpsConfig &config() const { return _config; }
    // Stores the configuration and persists it, so the settings survive a
    // restart without the dialog being opened first.
    void set_config(const PpsConfig &cfg);
    // Reads the persisted configuration (called once from the constructor).
    void load_config();
    void save_config() const;
    // Calibration result from the calibration dialog: written straight into the
    // live config so the next frame is already compensated.
    double calib_factor() const { return _config.calib_factor; }
    void set_calib_factor(double f);
    // The channel set this configuration measures: reference channel plus every
    // enabled measured channel. Used to mirror the setup into the main view.
    std::vector<int> measured_channel_set() const;
    // StoreSession needs a session-data getter to write a readable .dsl file;
    // MainWindow provides it (it is the application's ISessionDataGetter).
    void set_data_getter(ISessionDataGetter *getter) { _data_getter = getter; }
    // Frames whose measured data went missing, and the files saved for them.
    int loss_frame_count() const { return _loss_frames; }
    const QString &last_dsl_path() const { return _last_dsl_path; }

    // Fill the unset parts of the config (channels/rate) from the device,
    // so "Start" works even when the user never opened the dialog.
    bool prepare_config();
    // Apply the current config and start the capture loop.
    bool start();
    // Stop after the current frame completes.
    void stop();

    // IMessageListener
    void OnMessage(int msg) override;

    // Latest frame values and accumulated statistics.
    const std::vector<PpsResult> &last_results() const { return _last; }
    std::vector<PpsStats> statistics() const;
    int frame_count() const { return _frame_count; }
    // Per-frame timing: nominal capture window, measured upload+decode, total.
    double last_capture_ms() const { return _last_capture_ms; }
    double last_report_ms() const { return _last_report_ms; }
    double last_total_ms() const { return _last_total_ms; }
    // Path of the CSV file the current run writes to.
    const QString &csv_path() const { return _csv_path; }

signals:
    void frame_updated();
    void status_message(const QString &text);

private:
    bool apply_hardware_config();
    void process_frame();
    bool launch_capture();
    void reset_statistics();
    // No-data watchdog: if the very first frame never arrives (typically because
    // the reference channel has no 1PPS signal, so the FPGA trigger never fires)
    // the capture is aborted with a message instead of waiting forever.
    void arm_watchdog();
    void on_watchdog_timeout();
    // Save the current frame as a .dsl file because a selected channel lost its
    // edge; returns true when a file was written.
    bool save_loss_frame(const std::vector<PpsResult> &results);

    SigSession *_session;
    QTimer *_watchdog = nullptr;
    PpsConfig _config;
    PpsDeviceCaps _caps;
    bool _running;
    bool _stop_requested;
    QString _start_error;
    int _frame_count;
    int _active_ch_num;
    uint64_t _active_rate;
    QString _csv_path;
    std::vector<PpsResult> _last;

    // Auto-save of anomalous frames (a selected channel had no edge).
    ISessionDataGetter *_data_getter = nullptr;
    int _loss_frames = 0;
    int _loss_seq = 0;
    QString _last_dsl_path;
    // One DSL per minute at most: a channel that keeps losing its edge would
    // otherwise write a 45 KB file four times a second for a week.
    qint64 _last_loss_save_ms = -1;
    int _loss_skipped = 0;

    // Timing of the current/last frame: capture window plus the time the
    // driver took to move the samples over USB and decode them.
    QElapsedTimer _frame_timer;
    double _last_capture_ms;
    double _last_report_ms;
    double _last_total_ms;

    // Welford accumulators per measured channel (index aligned to
    // _config.channels). Running mean/m2 instead of sum/sum2 keeps the
    // variance accurate over a week of samples (millions of them).
    std::vector<int> _count;
    std::vector<double> _mean;
    std::vector<double> _m2;
    std::vector<double> _min;
    std::vector<double> _max;
};

} // namespace pps
} // namespace pv

#endif // DSVIEW_PV_PPS_PPSMANAGER_H
