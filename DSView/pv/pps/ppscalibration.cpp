/*
 * Calibration run implementation.
 */
#include "ppscalibration.h"
#include "ppsmanager.h"
#include "../sigsession.h"
#include "../deviceagent.h"
#include "../data/logicsnapshot.h"
#include "../data/snapshot.h"
#include "../log.h"

#include <libsigrok.h>
#include <glib.h>

#include <cmath>
#include <algorithm>
#include <QTimer>
#include <QMetaObject>

using std::vector;

namespace pv {
namespace pps {

// Below this the input is treated as the 10ms test signal, above it as 1PPS.
// A factor of twenty sits comfortably between the two standards and cannot be
// reached by a healthy 1PPS or by any plausible 10ms source.
static const double kOnePpsThresholdMs = 200.0;

// A 1.2 s window only holds two 1PPS edges when the first edge lands in the
// first 200 ms of the frame, so a missed capture is simply repeated: only a
// frame with at least two edges counts towards the requested number.
static const int kMaxAttemptsPerCapture = 40;

// Number of calibration runs that currently own the hardware trigger.
static int s_active_runs = 0;

PpsCalibConfig::PpsCalibConfig()
	: channel(0)
	, captures(10)
	, window_ms(1200.0)
	, sample_rate(100000000ULL)
{
}

PpsCalibResult::PpsCalibResult()
	: factor(1.0)
	, measured_ms(0.0)
	, standard_ms(0.0)
	, captures(0)
	, edges(0)
{
}

PpsCalibration::PpsCalibration(SigSession *session, QObject *parent)
	: QObject(parent)
	, _session(session)
	, _running(false)
	, _stop_requested(false)
	, _done(0)
	, _retry(0)
	, _active_ch_num(0)
	, _active_rate(0)
	, _sample_count(0)
	, _sum_samples(0.0)
	, _usable(0)
	, _last_edges(0)
	, _last_measured_ms(0.0)
	, _owns_trigger(false)
{
	if (_session)
		_session->add_msg_listener(this);

	_watchdog = new QTimer(this);
	_watchdog->setSingleShot(true);
	connect(_watchdog, &QTimer::timeout, this, [this]() {
		if (!_running)
			return;
		_error = QString::fromUtf8(
			"校准采集超时：CH%1 在 %2 ms 内没有完成一帧采集，"
			"请检查校准通道是否接到了信号。")
			.arg(_cfg.channel).arg((int)(_cfg.window_ms * 3 + 4000));
		stop();
		emit failed(_error);
	});
}

PpsCalibration::~PpsCalibration()
{
	if (_owns_trigger) {
		s_active_runs--;
		_owns_trigger = false;
	}
	if (_watchdog) {
		_watchdog->stop();
		_watchdog = nullptr;
	}
	if (_session)
		_session->remove_msg_listener(this);
}

bool PpsCalibration::any_active()
{
	return s_active_runs > 0;
}

bool PpsCalibration::start(const PpsCalibConfig &cfg)
{
	if (_running)
		return true;

	_error.clear();
	if (_session == nullptr || _session->get_device() == nullptr) {
		_error = QString::fromUtf8("未检测到可用设备，无法校准。");
		emit status_message(_error);
		return false;
	}
	if (_session->get_device()->get_work_mode() != LOGIC) {
		_error = QString::fromUtf8("当前设备不在逻辑分析仪模式，无法校准。");
		emit status_message(_error);
		return false;
	}

	_cfg = cfg;
	if (_cfg.window_ms < 1000.0 || _cfg.window_ms > 2000.0)
		_cfg.window_ms = 1200.0;
	if (_cfg.captures < 1)
		_cfg.captures = 1;

	_done = 0;
	_retry = 0;
	_sum_samples = 0.0;
	_usable = 0;
	_last_edges = 0;
	_last_measured_ms = 0.0;
	_stop_requested = false;

	if (!apply_hardware_config()) {
		emit status_message(_error);
		return false;
	}

	// From here on this run owns the trigger configuration: MainWindow must
	// not let the trigger dock re-apply its settings before each capture, or
	// trigger_en is cleared and the frame starts at an arbitrary phase.
	if (!_owns_trigger) {
		s_active_runs++;
		_owns_trigger = true;
	}

	_running = true;
	_capture_timer.start();
	if (!_session->start_capture(false)) {
		_running = false;
		if (_owns_trigger) {
			s_active_runs--;
			_owns_trigger = false;
		}
		_error = QString::fromUtf8("启动校准采集失败。");
		emit status_message(_error);
		return false;
	}
	_watchdog->start(static_cast<int>(_cfg.window_ms * 3 + 4000));
	emit status_message(QString::fromUtf8(
		"校准开始：CH%1，%2，%3 ms/次，共 %4 次。")
		.arg(_cfg.channel)
		.arg(_cfg.sample_rate / 1000000ULL)
		.arg(_cfg.window_ms, 0, 'f', 0)
		.arg(_cfg.captures));
	return true;
}

bool PpsCalibration::apply_hardware_config()
{
	DeviceAgent *dev = _session->get_device();
	if (!dev)
		return false;

	// The point of the calibration run is to see the incoming interval with as
	// few channels as possible, so the device spends its whole sample memory on
	// one channel and a single window can be longer than a second.
	PpsDeviceCaps caps;
	if (!query_device_caps(_session, caps)) {
		_error = QString::fromUtf8("无法读取设备的逻辑通道信息，无法校准。");
		return false;
	}

	// Fixed 100MHz: fast enough to time a 10ms or 1PPS interval to a few ppm,
	// slow enough that a >1s window fits the buffer.
	uint64_t rate = _cfg.sample_rate;
	// Verification hook: force a rate, so the stream fallback can be exercised
	// on a device whose buffer is large enough at the default rate.
	const QByteArray rate_env = qgetenv("DSVIEW_PPS_CALIB_RATE");
	if (!rate_env.isEmpty() && rate_env.toULongLong() > 0)
		rate = rate_env.toULongLong();
	if (!caps.rates.empty()) {
		uint64_t best = 0;
		for (uint64_t r : caps.rates) {
			if (r <= rate)
				best = std::max(best, r);
		}
		if (best == 0)
			best = caps.rates.front();
		rate = best;
	}

	_session->set_collect_mode(COLLECT_SINGLE);
	dev->set_config_int16(SR_CONF_OPERATION_MODE, LO_OP_BUFFER);

	// Fewest channels that still reaches the rate.
	const PpsModeInfo *mode = nullptr;
	for (const PpsModeInfo &m : caps.modes) {
		if (m.max_rate < rate)
			continue;
		if (mode == nullptr || m.channels < mode->channels)
			mode = &m;
	}
	if (mode == nullptr && caps.channel_num > 0) {
		// No mode list: keep the active one.
	}
	if (mode != nullptr && mode->id >= 0)
		dev->set_config_int16(SR_CONF_CHANNEL_MODE, mode->id);

	int ch_num = 0;
	if (!dev->get_config_int16(SR_CONF_VLD_CH_NUM, ch_num) || ch_num <= 0)
		ch_num = (mode != nullptr) ? mode->channels : caps.channel_num;
	if (ch_num <= 0) {
		_error = QString::fromUtf8("设备没有可用的逻辑通道。");
		return false;
	}
	if (_cfg.channel < 0 || _cfg.channel >= ch_num) {
		_error = QString::fromUtf8("校准通道 CH%1 超出当前模式可用的 %2 个通道。")
			.arg(_cfg.channel).arg(ch_num);
		return false;
	}

	uint64_t max_rate = 0;
	if (!dev->get_config_uint64(SR_CONF_MAX_DSO_SAMPLERATE, max_rate) ||
			max_rate == 0)
		max_rate = (mode != nullptr) ? mode->max_rate : caps.max_rate;
	if (max_rate != 0 && rate > max_rate)
		rate = max_rate;
	dev->set_config_uint64(SR_CONF_SAMPLERATE, rate);

	uint64_t sample_count = static_cast<uint64_t>(
		rate * _cfg.window_ms / 1000.0);
	uint64_t depth = caps.hw_depth;
	bool stream = false;

	if (depth != 0 && sample_count > depth) {
		/*
		 * The window does not fit the sample memory. A 100M-point buffer at a
		 * high rate is far short of a 1200 ms window, which is what the 1PPS
		 * calibration needs to see two edges. Stream mode sends the samples over
		 * USB as they arrive instead of stopping at the depth, so it is the only
		 * way to reach a >1s window on such a device. The switch is automatic:
		 * the buffer attempt is the fallback when the device cannot stream.
		 */
		dsv_info("1PPS calib: window needs %" G_GUINT64_FORMAT " samples but the "
			"buffer holds %" G_GUINT64_FORMAT ", switching to stream mode",
			sample_count, depth);
		if (!dev->set_config_int16(SR_CONF_OPERATION_MODE, LO_OP_STREAM)) {
			dsv_info("1PPS calib: stream mode unavailable, clamping the window to "
				"the buffer depth");
			sample_count = depth;
		}
		else {
			stream = true;
			// The driver changed the channel mode with the operation mode, so
			// the usable channel count and the rate ceiling are new values.
			int stream_ch = 0;
			if (dev->get_config_int16(SR_CONF_VLD_CH_NUM, stream_ch) &&
					stream_ch > 0)
				ch_num = stream_ch;
			if (_cfg.channel < 0 || _cfg.channel >= ch_num) {
				_error = QString::fromUtf8(
					"校准通道 CH%1 超出 Stream 模式可用的 %2 个通道。")
					.arg(_cfg.channel).arg(ch_num);
				return false;
			}
			// Stream caps the rate well below the buffer modes; take the highest
			// published rate that still fits, so the edge timing stays as fine
			// as the stream path allows.
			uint64_t stream_max = 0;
			if (!dev->get_config_uint64(SR_CONF_MAX_DSO_SAMPLERATE, stream_max))
				stream_max = 0;
			uint64_t want = rate;
			if (stream_max != 0 && want > stream_max)
				want = stream_max;
			uint64_t best = 0;
			for (uint64_t r : caps.rates) {
				if (r <= want)
					best = std::max(best, r);
			}
			if (best != 0)
				want = best;
			dev->set_config_uint64(SR_CONF_SAMPLERATE, want);
			uint64_t actual = 0;
			if (dev->get_config_uint64(SR_CONF_SAMPLERATE, actual) &&
					actual != 0)
				rate = actual;
			else
				rate = want;
			/*
			 * Stream mode does not honour the trigger position the way the buffer
			 * does: the window starts wherever the USB stream happens to begin,
			 * so a 1200 ms window can hold a single 1PPS edge (measured phase was
			 * ~999.93 ms) and never a second one. Take two windows worth of
			 * samples instead - that always covers at least two periods of a 1 Hz
			 * reference, and is merely "more edges" for a faster one. Stream has
			 * no depth limit, so this only costs capture time.
			 */
			_cfg.window_ms = _cfg.window_ms * 2.0;
			sample_count = static_cast<uint64_t>(
				rate * _cfg.window_ms / 1000.0);
			dsv_info("1PPS calib: stream mode, %d channel(s), %" G_GUINT64_FORMAT
				" Hz, window %" G_GUINT64_FORMAT " ms (%" G_GUINT64_FORMAT
				" samples)",
				ch_num, rate, static_cast<uint64_t>(_cfg.window_ms), sample_count);
		}
	}

	_active_ch_num = ch_num;
	_active_rate = rate;

	// Only the calibration channel is sampled. Done after the stream switch,
	// because changing the operation mode resets the probe set.
	GSList *chs = dev->get_channels();
	for (GSList *l = chs; l != nullptr; l = l->next) {
		sr_channel *c = static_cast<sr_channel *>(l->data);
		if (c->type != SR_CHANNEL_LOGIC)
			continue;
		dev->enable_probe(c->index, c->index == _cfg.channel);
	}

	if (sample_count == 0) {
		_error = QString::fromUtf8("校准窗口与采样率组合得到的样本数为 0。");
		return false;
	}
	_sample_count = sample_count;
	dev->set_config_uint64(SR_CONF_LIMIT_SAMPLES, sample_count);

	// Trigger on the calibration channel at 0%, so the first edge of the frame
	// is the reference and the following edges are the measured ones.
	// DSVIEW_PPS_CALIB_POS is a diagnostic override: a working trigger must pin
	// the edge at the given percentage of the window in every frame, so a
	// non-zero value is the quickest way to tell "trigger latched" from
	// "capture ran free". Clamped, because ds_trigger_set_pos() asserts
	// position <= 100.
	int trig_pos = 0;
	const QString pos_env = QString::fromUtf8(qgetenv("DSVIEW_PPS_CALIB_POS"));
	if (!pos_env.isEmpty())
		trig_pos = qBound(0, pos_env.toInt(), 100);
	ds_trigger_reset();
	for (int i = 0; i < ch_num; ++i) {
		if (i == _cfg.channel)
			ds_trigger_probe_set(i, 'R', 'X');
		else
			ds_trigger_probe_set(i, 'X', 'X');
	}
	ds_trigger_set_pos(static_cast<uint16_t>(trig_pos));
	ds_trigger_set_mode(SIMPLE_TRIGGER);
	ds_trigger_set_en(1);

	dsv_info("1PPS calib: CH%d, rate %" G_GUINT64_FORMAT " Hz, %d channel mode, "
		"%s, window %.0f ms (%" G_GUINT64_FORMAT " samples), trigger 0%%, %d capture(s)",
		_cfg.channel, rate, _active_ch_num, stream ? "stream" : "buffer",
		_cfg.window_ms, sample_count, _cfg.captures);
	return true;
}

void PpsCalibration::stop()
{
	if (!_running)
		return;
	_stop_requested = true;
	_running = false;
	if (_owns_trigger) {
		s_active_runs--;
		_owns_trigger = false;
	}
	if (_watchdog)
		_watchdog->stop();
	ds_trigger_set_en(0);
	if (_session)
		_session->stop_capture();
	dsv_info("1PPS calib: stopped after %d capture(s)", _done);
}

void PpsCalibration::OnMessage(int msg)
{
	if (msg == DSV_MSG_END_COLLECT_WORK && _running)
		finish_current_capture();
}

void PpsCalibration::finish_current_capture()
{
	data::Snapshot *base = _session->get_snapshot(SR_CHANNEL_LOGIC);
	data::LogicSnapshot *snap = dynamic_cast<data::LogicSnapshot *>(base);
	if (snap == nullptr || snap->empty() || snap->get_sample_count() == 0) {
		emit progress(_done + 1, _cfg.captures, 0.0, 0);
		advance_after_capture(false, 0, 0.0);
		return;
	}

	const uint64_t sample_count = snap->get_sample_count();
	const uint64_t sample_rate = _session->cur_samplerate() != 0
			? _session->cur_samplerate() : _active_rate;

	vector<uint64_t> edges;
	PpsDecoder::find_all_edges(snap, _cfg.channel, PPS_EDGE_RISING,
			sample_count, edges);
	_last_edges = static_cast<int>(edges.size());

	// Detail line for the dialog: how long the frame really was and where the
	// edges sat, which is what tells a "one edge" frame from a lost trigger.
	emit status_message(QString::fromUtf8(
		"  帧 %1：%2 个样本（%3 ms），%4 个沿%5")
		.arg(_done + 1)
		.arg(sample_count)
		.arg(sample_rate ? (double)sample_count / sample_rate * 1000.0 : 0.0, 0, 'f', 1)
		.arg(edges.size())
		.arg(edges.empty() ? QString()
			: QString::fromUtf8("（首 %1，末 %2）")
				.arg(edges.front()).arg(edges.back())));

	if (edges.size() < 2 || sample_rate == 0) {
		emit progress(_done + 1, _cfg.captures, 0.0, _last_edges);
		advance_after_capture(false, edges.size(), 0.0);
		return;
	}

	// Average period over every edge in the window: for a 10ms input that is
	// ~120 periods, which pushes the quantization error far below one sample.
	// A slow stream path can pick up a spurious edge (narrow spike), so each
	// interval is filtered against the median interval before averaging instead
	// of trusting (last - first) / (n - 1).
	vector<double> intervals;
	for (size_t i = 1; i < edges.size(); ++i)
		intervals.push_back(static_cast<double>(edges[i] - edges[i - 1]));
	double period_samples = static_cast<double>(edges.back() - edges.front())
			/ static_cast<double>(edges.size() - 1);
	if (intervals.size() >= 3) {
		vector<double> sorted = intervals;
		std::sort(sorted.begin(), sorted.end());
		const double median = sorted[sorted.size() / 2];
		vector<double> keep;
		for (double d : intervals) {
			if (median > 0.0 && d > median * 0.8 && d < median * 1.25)
				keep.push_back(d);
		}
		if (keep.size() >= 2) {
			double sum = 0.0;
			for (double d : keep)
				sum += d;
			period_samples = sum / static_cast<double>(keep.size());
		}
	}
	const double measured_ms = period_samples
			/ static_cast<double>(sample_rate) * 1000.0;
	_last_measured_ms = measured_ms;
	emit progress(_done + 1, _cfg.captures, measured_ms, _last_edges);
	advance_after_capture(true, edges.size(), period_samples);
}

void PpsCalibration::advance_after_capture(bool usable, size_t edges,
		double period_samples)
{
	Q_UNUSED(edges);

	// A frame where the input edge spacing left only one edge in the window is
	// retried rather than counted, so "10 captures" really averages 10 usable
	// intervals.
	// Only a frame with at least two edges counts as one of the requested
	// captures; a frame that missed the second edge is simply repeated.
	if (!usable && !_stop_requested && _retry + 1 < kMaxAttemptsPerCapture) {
		_retry++;
		emit status_message(QString::fromUtf8(
			"  本次没有采到两个沿，重测（第 %1 次尝试，已得 %2/%3 次）。")
			.arg(_retry + 1).arg(_usable).arg(_cfg.captures));
		_capture_timer.restart();
		if (_session->start_capture(false)) {
			_watchdog->start(static_cast<int>(_cfg.window_ms * 3 + 4000));
			return;
		}
		_error = QString::fromUtf8("重新触发校准采集失败。");
	}

	if (usable && period_samples > 0.0) {
		_retry = 0;
		_done++;
		_sum_samples += period_samples;
		_usable++;
	} else if (!usable) {
		_retry = 0;
		_done++;
	}

	if (_done < _cfg.captures && !_stop_requested) {
		_capture_timer.restart();
		if (_session->start_capture(false)) {
			_watchdog->start(static_cast<int>(_cfg.window_ms * 3 + 4000));
			return;
		}
		_error = QString::fromUtf8("重新触发校准采集失败。");
	}

	_running = false;
	if (_owns_trigger) {
		s_active_runs--;
		_owns_trigger = false;
	}
	if (_watchdog)
		_watchdog->stop();
	ds_trigger_set_en(0);

	if (_usable == 0) {
		_error = QString::fromUtf8(
			"校准失败：CH%1 在 %.0f ms 窗口内没有采到两个上升沿，"
			"请确认信号接到该通道上（10ms 或 1PPS 都可以）。")
			.arg(_cfg.channel).arg(_cfg.window_ms);
		emit failed(_error);
		return;
	}

	PpsCalibResult res;
	res.measured_ms = _sum_samples / static_cast<double>(_usable)
			/ static_cast<double>(_active_rate) * 1000.0;
	res.captures = _usable;
	res.edges = _last_edges;
	if (res.measured_ms >= kOnePpsThresholdMs) {
		res.standard_ms = 1000.0;   // 1PPS
		res.input_name = QString::fromUtf8("1PPS");
	} else {
		res.standard_ms = 10.0;     // 10ms test signal
		res.input_name = QString::fromUtf8("10ms");
	}
	res.factor = res.standard_ms / res.measured_ms;

	dsv_info("1PPS calib: %s input, measured %.6f ms (standard %.1f ms) over "
		"%d capture(s), factor %.9f",
		res.input_name.toUtf8().constData(), res.measured_ms,
		res.standard_ms, res.captures, res.factor);
	emit finished(res);
}

} // namespace pps
} // namespace pv
