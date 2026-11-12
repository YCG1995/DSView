/*
 * 1PPS measurement state machine implementation.
 */
#include "ppsmanager.h"
#include "ppsdecoder.h"
#include "ppswriter.h"
#include "../sigsession.h"
#include "../storesession.h"
#include "../deviceagent.h"
#include "../data/logicsnapshot.h"
#include "../data/snapshot.h"
#include "../log.h"

#include <libsigrok.h>
#include <glib.h>

#include <cmath>
#include <algorithm>
#include <thread>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QMetaObject>
#include <QStringList>
#include <QTimer>
#include <QApplication>
#include <QSettings>
#include <QVariant>

using std::vector;

namespace pv {
namespace pps {

PpsConfig::PpsConfig()
	: ref_channel(0)
	, ref_edge(PPS_EDGE_RISING)
	, trigger_pos_percent(50)
	, capture_time_ms(10.0)
	, sample_rate(0)
	, auto_save_loss(false)
	, mode_channels(0)
	, calib_factor(1.0)
{
	save_dir = QCoreApplication::applicationDirPath() + "/autosave";
}

PpsDeviceCaps::PpsDeviceCaps()
	: channel_num(0)
	, max_rate(0)
	, hw_depth(0)
{
}

int PpsDeviceCaps::channels_for_rate(uint64_t rate) const
{
	// Prefer the mode with the most channels that still reaches this rate.
	int best = 0;
	for (const PpsModeInfo &m : modes) {
		if (m.max_rate >= rate && m.channels > best)
			best = m.channels;
	}
	if (best > 0)
		return best;
	// No mode reaches the rate: fall back to what the device reports now.
	return channel_num;
}

uint64_t PpsDeviceCaps::rate_for_channels(int channels) const
{
	uint64_t best = 0;
	for (const PpsModeInfo &m : modes) {
		if (m.channels >= channels && m.max_rate > best)
			best = m.max_rate;
	}
	return best != 0 ? best : max_rate;
}

double PpsDeviceCaps::max_capture_ms(uint64_t rate, int channels) const
{
	if (rate == 0)
		return 500.0;
	uint64_t depth = hw_depth;
	if (depth == 0 && !modes.empty())
		depth = 0; // unknown, leave the caller's limit
	const double per_ch = static_cast<double>(depth);
	if (per_ch <= 0.0)
		return 500.0;
	return per_ch / static_cast<double>(rate) * 1000.0;
}

const PpsModeInfo *PpsDeviceCaps::mode_for_rate(uint64_t rate) const
{
	const PpsModeInfo *best = nullptr;
	for (const PpsModeInfo &m : modes) {
		if (m.max_rate >= rate) {
			if (best == nullptr || m.channels > best->channels)
				best = &m;
		}
	}
	if (best == nullptr && !modes.empty())
		best = &modes.front();
	return best;
}

static void collect_rates(DeviceAgent *dev, PpsDeviceCaps &caps)
{
	caps.rates.clear();
	GVariant *dict = dev->get_config_list(nullptr, SR_CONF_SAMPLERATE);
	if (dict == nullptr)
		return;
	GVariant *list = g_variant_lookup_value(dict, "samplerates",
			G_VARIANT_TYPE("at"));
	if (list != nullptr) {
		gsize n = 0;
		const uint64_t *el = static_cast<const uint64_t *>(
			g_variant_get_fixed_array(list, &n, sizeof(uint64_t)));
		for (gsize i = 0; i < n; ++i)
			caps.rates.push_back(el[i]);
		g_variant_unref(list);
	}
	g_variant_unref(dict);
	std::sort(caps.rates.begin(), caps.rates.end());
	caps.rates.erase(std::unique(caps.rates.begin(), caps.rates.end()),
		caps.rates.end());
}

/*
 * Buffer channel modes of the driver. Probing a mode is the only portable way
 * to learn its channel count (the config list only carries a title), so the
 * active mode is saved and restored around the probe. Devices without a
 * channel-mode list (e.g. the demo driver) simply report a single mode built
 * from their current state.
 */
static void collect_rates_of_mode(DeviceAgent *dev, vector<uint64_t> &out)
{
	GVariant *dict = dev->get_config_list(nullptr, SR_CONF_SAMPLERATE);
	if (dict == nullptr)
		return;
	GVariant *list = g_variant_lookup_value(dict, "samplerates",
			G_VARIANT_TYPE("at"));
	if (list != nullptr) {
		gsize n = 0;
		const uint64_t *el = static_cast<const uint64_t *>(
			g_variant_get_fixed_array(list, &n, sizeof(uint64_t)));
		for (gsize i = 0; i < n; ++i)
			out.push_back(el[i]);
		g_variant_unref(list);
	}
	g_variant_unref(dict);
}

static void collect_modes(DeviceAgent *dev, PpsDeviceCaps &caps)
{
	caps.modes.clear();

	GVariant *gvar = dev->get_config_list(nullptr, SR_CONF_CHANNEL_MODE);
	if (gvar != nullptr) {
		sr_list_item *plist =
			reinterpret_cast<sr_list_item *>(g_variant_get_uint64(gvar));
		g_variant_unref(gvar);

		int saved_mode = 0;
		const bool have_saved =
			dev->get_config_int16(SR_CONF_CHANNEL_MODE, saved_mode);
		// Probing modes makes the driver clamp the current rate to the probed
		// mode, so remember it and put it back afterwards.
		uint64_t saved_rate = 0;
		const bool have_rate =
			dev->get_config_uint64(SR_CONF_SAMPLERATE, saved_rate);

		vector<PpsModeInfo> found;
		for (sr_list_item *it = plist; it != nullptr && it->id >= 0; ++it) {
			if (!dev->set_config_int16(SR_CONF_CHANNEL_MODE, it->id))
				continue;
			int ch = 0;
			uint64_t max_rate = 0;
			if (!dev->get_config_int16(SR_CONF_VLD_CH_NUM, ch) || ch <= 0)
				continue;
			if (!dev->get_config_uint64(SR_CONF_MAX_DSO_SAMPLERATE,
					max_rate) || max_rate == 0)
				max_rate = caps.rates.empty() ? 0 : caps.rates.back();
			PpsModeInfo info;
			info.id = it->id;
			info.channels = ch;
			info.max_rate = max_rate;
			found.push_back(info);

			// The driver publishes the samplerate list of the mode it is
			// currently in, so 1GHz only shows up while the 8-channel mode is
			// active. Union the lists over every mode: the rate list is what the
			// device can reach in some mode, not just in the one it happens to
			// sit in right now.
			collect_rates_of_mode(dev, caps.rates);
		}

		if (have_saved)
			dev->set_config_int16(SR_CONF_CHANNEL_MODE, saved_mode);
		if (have_rate && saved_rate != 0)
			dev->set_config_uint64(SR_CONF_SAMPLERATE, saved_rate);

		std::sort(caps.rates.begin(), caps.rates.end());
		caps.rates.erase(std::unique(caps.rates.begin(), caps.rates.end()),
			caps.rates.end());

		// Keep only buffer modes: stream modes cap at the low stream rates and
		// cannot reach what the PPS measurement needs.
		for (const PpsModeInfo &info : found) {
			bool duplicate = false;
			for (const PpsModeInfo &k : caps.modes) {
				if (k.channels == info.channels &&
					k.max_rate == info.max_rate)
					duplicate = true;
			}
			if (!duplicate)
				caps.modes.push_back(info);
		}
		// Largest channel count first, then highest rate.
		std::sort(caps.modes.begin(), caps.modes.end(),
			[](const PpsModeInfo &a, const PpsModeInfo &b) {
				if (a.channels != b.channels)
					return a.channels > b.channels;
				return a.max_rate > b.max_rate;
			});
	}

	if (caps.modes.empty() && caps.channel_num > 0) {
		PpsModeInfo info;
		info.id = -1;
		info.channels = caps.channel_num;
		info.max_rate = caps.max_rate;
		caps.modes.push_back(info);
	}

	if (!caps.modes.empty())
		caps.max_rate = caps.modes.front().max_rate;
}

bool query_device_caps(SigSession *session, PpsDeviceCaps &caps)
{
	caps = PpsDeviceCaps();
	if (session == nullptr)
		return false;
	DeviceAgent *dev = session->get_device();
	if (dev == nullptr)
		return false;

	int ch = 0;
	if (dev->get_config_int16(SR_CONF_VLD_CH_NUM, ch) && ch > 0)
		caps.channel_num = ch;
	if (caps.channel_num <= 0)
		caps.channel_num = static_cast<int>(
			g_slist_length(dev->get_channels()));

	uint64_t depth = 0;
	if (dev->get_config_uint64(SR_CONF_HW_DEPTH, depth))
		caps.hw_depth = depth;

	uint64_t max_rate = 0;
	if (dev->get_config_uint64(SR_CONF_MAX_DSO_SAMPLERATE, max_rate))
		caps.max_rate = max_rate;

	collect_rates(dev, caps);
	collect_modes(dev, caps);

	if (caps.max_rate == 0 && !caps.rates.empty())
		caps.max_rate = caps.rates.back();

	return caps.channel_num > 0;
}

PpsManager::PpsManager(SigSession *session, QObject *parent)
	: QObject(parent)
	, _session(session)
	, _running(false)
	, _stop_requested(false)
	, _frame_count(0)
	, _active_ch_num(0)
	, _active_rate(0)
	, _last_capture_ms(0.0)
	, _last_report_ms(0.0)
	, _last_total_ms(0.0)
{
	if (_session)
		_session->add_msg_listener(this);

	_watchdog = new QTimer(this);
	_watchdog->setSingleShot(true);
	connect(_watchdog, &QTimer::timeout, this, &PpsManager::on_watchdog_timeout);

	// Every option the dialog edits is a property of the measurement, not of a
	// single session, so it is read back here on every start.
	load_config();
}

void PpsManager::set_config(const PpsConfig &cfg)
{
	_config = cfg;
	reset_statistics();
	save_config();
}

void PpsManager::set_calib_factor(double f)
{
	if (f > 0.0) {
		_config.calib_factor = f;
		save_config();
	}
}

void PpsManager::load_config()
{
	QSettings st(QApplication::organizationName(),
		QApplication::applicationName());
	PpsConfig cfg;
	cfg.ref_channel = st.value("pps/ref_channel", cfg.ref_channel).toInt();
	cfg.ref_edge = st.value("pps/ref_edge", cfg.ref_edge).toInt();
	cfg.trigger_pos_percent = st.value("pps/trigger_pos_percent",
		cfg.trigger_pos_percent).toInt();
	cfg.capture_time_ms = st.value("pps/capture_time_ms",
		cfg.capture_time_ms).toDouble();
	cfg.sample_rate = (uint64_t)st.value("pps/sample_rate",
		(qulonglong)cfg.sample_rate).toULongLong();
	cfg.save_dir = st.value("pps/save_dir", cfg.save_dir).toString();
	cfg.auto_save_loss = st.value("pps/auto_save_loss",
		cfg.auto_save_loss).toBool();
	cfg.mode_channels = st.value("pps/mode_channels",
		cfg.mode_channels).toInt();
	cfg.calib_factor = st.value("pps/calib_factor",
		cfg.calib_factor).toDouble();
	if (cfg.calib_factor <= 0.0)
		cfg.calib_factor = 1.0;

	// Channels: "index:edge:save_on_loss" per measured channel, so the dialog
	// opens with the same tick state and edge choice as it was left.
	cfg.channels.clear();
	const QStringList parts =
		st.value("pps/channels").toStringList();
	for (int i = 0; i < parts.size(); ++i) {
		const QStringList f = parts[i].split(':');
		if (f.size() < 2)
			continue;
		PpsChannel ch;
		ch.channel_index = f[0].toInt();
		ch.edge = f[1].toInt();
		ch.enabled = true;
		ch.save_on_loss = (f.size() > 2) ? (f[2].toInt() != 0) : false;
		cfg.channels.push_back(ch);
	}
	cfg.disabled_channels.clear();
	const QStringList off =
		st.value("pps/disabled_channels").toStringList();
	for (int i = 0; i < off.size(); ++i)
		cfg.disabled_channels.push_back(off[i].toInt());

	// A stored set that no longer parses is worse than no stored set: fall back
	// to the defaults and let prepare_config() fill the channels from the device.
	if (cfg.channels.empty() && !parts.isEmpty())
		return;

	_config = cfg;
	// The statistics arrays are sized from the channel list, so a restored
	// config has to size them too - otherwise the result dock reads past the
	// end of an empty array the moment it is built, before any capture ran.
	reset_statistics();
}

void PpsManager::save_config() const
{
	QSettings st(QApplication::organizationName(),
		QApplication::applicationName());
	st.setValue("pps/ref_channel", _config.ref_channel);
	st.setValue("pps/ref_edge", _config.ref_edge);
	st.setValue("pps/trigger_pos_percent", _config.trigger_pos_percent);
	st.setValue("pps/capture_time_ms", _config.capture_time_ms);
	st.setValue("pps/sample_rate", (qulonglong)_config.sample_rate);
	st.setValue("pps/save_dir", _config.save_dir);
	st.setValue("pps/auto_save_loss", _config.auto_save_loss);
	st.setValue("pps/mode_channels", _config.mode_channels);
	st.setValue("pps/calib_factor", _config.calib_factor);
	QStringList parts;
	for (size_t i = 0; i < _config.channels.size(); ++i) {
		const PpsChannel &ch = _config.channels[i];
		parts.append(QString("%1:%2:%3").arg(ch.channel_index)
			.arg(ch.edge).arg(ch.save_on_loss ? 1 : 0));
	}
	st.setValue("pps/channels", parts);
	QStringList off;
	for (size_t i = 0; i < _config.disabled_channels.size(); ++i)
		off.append(QString::number(_config.disabled_channels[i]));
	st.setValue("pps/disabled_channels", off);
}

std::vector<int> PpsManager::measured_channel_set() const
{
	std::vector<int> out;
	out.push_back(_config.ref_channel);
	for (size_t i = 0; i < _config.channels.size(); ++i) {
		if (_config.channels[i].enabled)
			out.push_back(_config.channels[i].channel_index);
	}
	return out;
}

PpsManager::~PpsManager()
{
	if (_watchdog) {
		_watchdog->stop();
		_watchdog = nullptr;
	}
	if (_session)
		_session->remove_msg_listener(this);
}

void PpsManager::arm_watchdog()
{
	if (_watchdog == nullptr)
		return;
	// Generous budget: capture window plus USB readout and decode overhead.
	const double budget = _config.capture_time_ms * 4.0 + 2500.0;
	_watchdog->start(static_cast<int>(budget));
}

void PpsManager::on_watchdog_timeout()
{
	if (!_running)
		return;

	// No frame arrived for the whole budget: the trigger never fired, which
	// normally means the reference channel lost its 1PPS signal. The measurement
	// is meant to stay armed until the user stops it, so keep waiting instead of
	// ending the run - the next reference edge will revive it by itself.
	if (_watchdog) {
		// Back off a little: a second attempt costs nothing, but a dead reference
		// should not flood the log either.
		const int budget = static_cast<int>(_config.capture_time_ms * 4.0 + 2500.0);
		_watchdog->start(std::max(budget, 3000));
	}
	dsv_info("1PPS wait: no frame within %d ms, reference CH%d has no edge; "
		"measurement stays armed (frame_count=%d)",
		static_cast<int>(_config.capture_time_ms * 4.0 + 2500.0),
		_config.ref_channel, _frame_count);
	emit status_message(QString::fromUtf8(
		"1PPS 测量：暂无数据（参考通道 CH%1 没有边沿），继续等待触发。")
		.arg(_config.ref_channel));
}

void PpsManager::reset_statistics()
{
	_frame_count = 0;
	_loss_frames = 0;
	_loss_seq = 0;
	_loss_skipped = 0;
	_last_loss_save_ms = -1;
	_last_dsl_path.clear();
	const size_t n = _config.channels.size();
	_count.assign(n, 0);
	_mean.assign(n, 0.0);
	_m2.assign(n, 0.0);
	_min.assign(n, 0.0);
	_max.assign(n, 0.0);
}

bool PpsManager::save_loss_frame(const std::vector<PpsResult> &results)
{
	QStringList lost;
	for (size_t i = 0; i < results.size() && i < _config.channels.size(); ++i) {
		if (!results[i].valid && _config.channels[i].save_on_loss)
			lost << QString("CH%1").arg(results[i].channel_index);
	}
	if (lost.isEmpty())
		return false;

	if (_data_getter == nullptr) {
		dsv_err("1PPS loss: cannot save %s, no session data getter",
			lost.join(",").toUtf8().constData());
		return false;
	}

	const QString dir = _config.save_dir;
	if (dir.isEmpty() || !QDir().mkpath(dir)) {
		dsv_err("1PPS loss: cannot use directory %s",
			dir.toLocal8Bit().constData());
		return false;
	}

	const QString stamp = QDateTime::currentDateTime()
		.toString("yyyyMMdd-HHmmss-zzz");
	QString path = QDir(dir).filePath(QString("LOSS-%1-%2.dsl")
		.arg(stamp).arg(_loss_seq++, 3, 10, QChar('0')));

	// The frame that just finished is the one to keep, and the PPS loop has not
	// re-armed the trigger yet, so the buffer is still intact. Write it out
	// synchronously (a nested event loop keeps the queued store signals flowing,
	// the same way the MCP save tool does it).
	StoreSession store(_session);
	store.set_file_name(path);
	store._sessionDataGetter = _data_getter;
	if (!store.save_start()) {
		dsv_err("1PPS loss: save failed for %s: %s",
			path.toLocal8Bit().constData(), store.error().toLocal8Bit().constData());
		return false;
	}

	QEventLoop loop;
	QString err;
	std::thread waiter([&]() {
		store.wait();
		err = store.error();
		QMetaObject::invokeMethod(&loop, "quit", Qt::QueuedConnection);
	});
	loop.exec();
	if (waiter.joinable())
		waiter.join();

	if (!err.isEmpty()) {
		dsv_err("1PPS loss: store error for %s: %s",
			path.toLocal8Bit().constData(), err.toLocal8Bit().constData());
		return false;
	}

	_last_dsl_path = path;
	dsv_info("1PPS loss: %s had no edge, frame %d saved to %s",
		lost.join(",").toUtf8().constData(), _frame_count,
		path.toLocal8Bit().constData());
	emit status_message(QString::fromUtf8("1PPS 数据丢失（%1），已保存 %2")
		.arg(lost.join(",")).arg(QFileInfo(path).fileName()));
	return true;
}

bool PpsManager::prepare_config()
{
	if (query_device_caps(_session, _caps) == false || _caps.channel_num <= 0)
		return false;

	if (_config.ref_channel < 0 || _config.ref_channel >= _caps.channel_num)
		_config.ref_channel = 0;

	// Channel count we actually need: the highest measured index plus the
	// reference.  With no user configuration we only need the eight channels of
	// the fast 1GHz mode, which is the normal 1PPS wiring (CH0..CH7).
	int needed = 0;
	if (_config.channels.empty()) {
		needed = _caps.channel_num < 8 ? _caps.channel_num : 8;
	} else {
		needed = _config.ref_channel + 1;
		for (size_t i = 0; i < _config.channels.size(); ++i) {
			PpsChannel &ch = _config.channels[i];
			if (ch.enabled && ch.channel_index + 1 > needed)
				needed = ch.channel_index + 1;
		}
	}

	// The samplerate and the channel range are two views of the same device
	// mode. When the pinned range cannot reach the chosen rate the rate wins and
	// the range shrinks (1GHz means CH0-CH7 on the U2Pro16) - the same rule the
	// configuration dialog applies, so either setting alone gives this result.
	if (_config.mode_channels > 0 && _config.sample_rate > 0) {
		uint64_t range_max = 0;
		for (const PpsModeInfo &m : _caps.modes) {
			if (m.channels == _config.mode_channels && m.max_rate > range_max)
				range_max = m.max_rate;
		}
		if (range_max != 0 && _config.sample_rate > range_max) {
			const int reach = _caps.channels_for_rate(_config.sample_rate);
			if (reach > 0 && reach < _config.mode_channels) {
				dsv_info("1PPS config: rate %" G_GUINT64_FORMAT " Hz does not "
					"fit CH0-CH%d, range narrowed to CH0-CH%d",
					(uint64_t)_config.sample_rate,
					_config.mode_channels - 1, reach - 1);
				_config.mode_channels = reach;
			}
		}
	}

	// Pick the samplerate: honour the rate the user chose when the device really
	// supports it, otherwise take the highest rate that still offers `needed`
	// channels (1GHz/8ch on the U2Pro16 instead of 500MHz/16ch).
	// A user-chosen channel range (8 or 16) also caps the rate: CH0-CH7 reaches
	// 1GHz, the full 16-channel range stops at 500MHz.
	uint64_t mode_limit = _caps.max_rate;
	if (_config.mode_channels > 0) {
		mode_limit = 0;
		for (const PpsModeInfo &m : _caps.modes) {
			if (m.channels == _config.mode_channels &&
					m.max_rate > mode_limit)
				mode_limit = m.max_rate;
		}
		if (mode_limit == 0)
			mode_limit = _caps.max_rate;
		if (needed > _config.mode_channels)
			needed = _config.mode_channels;
	}

	uint64_t wanted = _config.sample_rate;
	const bool supported = !_caps.rates.empty() &&
		std::find(_caps.rates.begin(), _caps.rates.end(), wanted)
			!= _caps.rates.end() &&
		(mode_limit == 0 || wanted <= mode_limit);
	if (!supported) {
		uint64_t chosen = _caps.rate_for_channels(needed);
		if (mode_limit != 0 && chosen > mode_limit)
			chosen = mode_limit;
		if (chosen == 0 && !_caps.rates.empty())
			chosen = _caps.rates.back();
		wanted = chosen;
	}
	if (wanted == 0 && !_caps.rates.empty())
		wanted = _caps.rates.back();
	_config.sample_rate = wanted;

	if (_config.capture_time_ms <= 0.0)
		_config.capture_time_ms = 10.0;

	// A faster mode exposes fewer channels than the device total, so drop
	// measured channels that the chosen rate cannot sample.
	int rate_channels = _caps.channels_for_rate(_config.sample_rate);
	if (_config.mode_channels > 0 && rate_channels > _config.mode_channels)
		rate_channels = _config.mode_channels;
	if (rate_channels > 0 && _config.ref_channel >= rate_channels)
		return false;
	if (rate_channels > 0 && !_config.channels.empty()) {
		std::vector<PpsChannel> kept;
		for (size_t i = 0; i < _config.channels.size(); ++i) {
			if (_config.channels[i].channel_index < rate_channels)
				kept.push_back(_config.channels[i]);
		}
		_config.channels.swap(kept);
	}

	// Default measured channels: every usable channel except the reference.
	if (_config.channels.empty()) {
		for (int i = 0; i < rate_channels; ++i) {
			if (i == _config.ref_channel)
				continue;
			// Skip channels the user switched off in the dialog.
			bool off = false;
			for (size_t k = 0; k < _config.disabled_channels.size(); ++k) {
				if (_config.disabled_channels[k] == i)
					off = true;
			}
			if (off)
				continue;
			PpsChannel ch;
			ch.channel_index = i;
			ch.edge = PPS_EDGE_RISING;
			ch.enabled = true;
			_config.channels.push_back(ch);
		}
	}

	if (_config.save_dir.isEmpty())
		_config.save_dir =
			QCoreApplication::applicationDirPath() + "/autosave";

	return !_config.channels.empty();
}

bool PpsManager::apply_hardware_config()
{
	DeviceAgent *dev = _session->get_device();
	if (!dev)
		return false;

	// Single capture, buffer (not stream) operation.
	// The driver switches buffer/stream through SR_CONF_OPERATION_MODE;
	// SR_CONF_STREAM is read-only / not accepted by config_set.
	_session->set_collect_mode(COLLECT_SINGLE);
	dev->set_config_int16(SR_CONF_OPERATION_MODE, LO_OP_BUFFER);

	// Select the channel mode. The user may pin the range to CH0-CH7 (8
	// channels, up to 1GHz) or CH0-CH15 (16 channels, up to 500MHz); without a
	// pinned range the widest mode that still reaches the rate wins.
	const PpsModeInfo *mode = nullptr;
	if (!_caps.modes.empty()) {
		if (_config.mode_channels > 0) {
			// Exact channel count, cheapest mode that still reaches the rate.
			for (const PpsModeInfo &m : _caps.modes) {
				if (m.channels != _config.mode_channels)
					continue;
				if (m.max_rate < _config.sample_rate)
					continue;
				if (mode == nullptr || m.max_rate < mode->max_rate)
					mode = &m;
			}
			if (mode == nullptr) {
				for (const PpsModeInfo &m : _caps.modes) {
					if (m.channels == _config.mode_channels &&
							(mode == nullptr || m.max_rate > mode->max_rate))
						mode = &m;
				}
			}
		}
		if (mode == nullptr)
			mode = _caps.mode_for_rate(_config.sample_rate);
	}
	if (mode != nullptr && mode->id >= 0)
		dev->set_config_int16(SR_CONF_CHANNEL_MODE, mode->id);

	int ch_num = 0;
	if (dev->get_config_int16(SR_CONF_VLD_CH_NUM, ch_num) == false || ch_num <= 0)
		ch_num = (mode != nullptr) ? mode->channels : _caps.channel_num;
	if (ch_num <= 0)
		return false;

	uint64_t max_rate = 0;
	if (!dev->get_config_uint64(SR_CONF_MAX_DSO_SAMPLERATE, max_rate) ||
		max_rate == 0)
		max_rate = (mode != nullptr) ? mode->max_rate : _caps.max_rate;

	uint64_t rate = _config.sample_rate;
	if (max_rate != 0 && rate > max_rate)
		rate = max_rate;
	_active_ch_num = ch_num;
	_active_rate = rate;

	// Enable exactly the channels of this mode, so the driver's channel count
	// and the FPGA mode always agree (a mismatch breaks the capture entirely).
	GSList *chs = dev->get_channels();
	for (GSList *l = chs; l != nullptr; l = l->next) {
		sr_channel *c = static_cast<sr_channel *>(l->data);
		if (c->type != SR_CHANNEL_LOGIC)
			continue;
		dev->enable_probe(c->index, c->index < ch_num);
	}

	dev->set_config_uint64(SR_CONF_SAMPLERATE, rate);

	uint64_t sample_count = static_cast<uint64_t>(
		rate * _config.capture_time_ms / 1000.0);
	if (_caps.hw_depth != 0 && sample_count > _caps.hw_depth)
		sample_count = _caps.hw_depth;
	if (sample_count == 0)
		return false;
	dev->set_config_uint64(SR_CONF_LIMIT_SAMPLES, sample_count);

	// Build a simple single-edge trigger on the reference channel.
	// In SIMPLE_TRIGGER mode the FPGA reads the probe row (index
	// TriggerStages), which is populated by ds_trigger_probe_set.
	ds_trigger_reset();
	const char edge = (_config.ref_edge == PPS_EDGE_RISING) ? 'R' : 'F';
	for (int i = 0; i < ch_num; ++i) {
		if (i == _config.ref_channel)
			ds_trigger_probe_set(i, edge, 'X');
		else
			ds_trigger_probe_set(i, 'X', 'X');
	}
	ds_trigger_set_pos(static_cast<uint16_t>(_config.trigger_pos_percent));
	ds_trigger_set_mode(SIMPLE_TRIGGER);
	ds_trigger_set_en(1);
	return true;
}

bool PpsManager::launch_capture()
{
	return _session->start_capture(false);
}

bool PpsManager::start()
{
	if (_running)
		return true;

	_start_error.clear();
	auto fail = [this](const QString &msg) {
		_start_error = msg;
		emit status_message(msg);
		return false;
	};

	if (_session == nullptr || _session->get_device() == nullptr)
		return fail(QString::fromUtf8("未检测到可用设备，无法开始 1PPS 测量。"));
	if (_session->get_device()->get_work_mode() != LOGIC)
		return fail(QString::fromUtf8("当前设备不在逻辑分析仪模式，无法开始 1PPS 测量。"));

	// Build a usable default configuration when the user starts without
	// opening the configuration dialog first.
	if (!prepare_config())
		return fail(QString::fromUtf8("无法获取设备的逻辑通道信息，无法开始 1PPS 测量。"));
	if (_config.channels.empty())
		return fail(QString::fromUtf8("没有可用的测试通道，请在配置中至少启用一个测试通道。"));
	if (_config.sample_rate == 0)
		return fail(QString::fromUtf8("采样率无效，无法开始 1PPS 测量。"));

	const uint64_t sample_count = static_cast<uint64_t>(
		_config.sample_rate * _config.capture_time_ms / 1000.0);
	if (sample_count == 0)
		return fail(QString::fromUtf8("采集时间与采样率组合得到的样本数为 0，无法开始。"));
	if (_config.save_dir.isEmpty())
		return fail(QString::fromUtf8("CSV 保存目录为空，无法开始 1PPS 测量。"));

	reset_statistics();
	_stop_requested = false;
	if (!apply_hardware_config())
		return fail(QString::fromUtf8("配置设备失败，无法开始 1PPS 测量。"));
	if (!QDir().mkpath(_config.save_dir))
		return fail(QString::fromUtf8("无法创建 CSV 保存目录：") + _config.save_dir);
	_csv_path = QDir(_config.save_dir).filePath(
		PpsWriter::build_file_name(QDateTime::currentDateTime()));

	_running = true;
	if (!launch_capture()) {
		_running = false;
		return fail(QString::fromUtf8("启动采集失败，无法开始 1PPS 测量。"));
	}
	_frame_timer.start(); // frame time = capture window + USB upload + decode
	arm_watchdog();
	dsv_info("1PPS config: rate %" G_GUINT64_FORMAT " Hz, channels %d, buffer mode, "
		"capture %.1f ms (%" G_GUINT64_FORMAT " samples), trigger CH%d %s at %d%%, "
		"measured %u channel(s)",
		_active_rate, _active_ch_num, _config.capture_time_ms, sample_count,
		_config.ref_channel,
		_config.ref_edge == PPS_EDGE_RISING ? "rising" : "falling",
		_config.trigger_pos_percent,
		(unsigned)_config.channels.size());
	char *rate_text = sr_samplerate_string(_active_rate);
	const QString rate_label = QString::fromLatin1(rate_text ? rate_text : "");
	if (rate_text)
		g_free(rate_text);
	emit status_message(QString::fromUtf8("1PPS 测量已开始（%1 个通道，%2，%3 ms）。")
		.arg(_active_ch_num)
		.arg(rate_label)
		.arg(_config.capture_time_ms));
	return true;
}

void PpsManager::stop()
{
	if (!_running)
		return;

	// Stop has to be immediate. Previously this only raised a flag that was
	// examined when the next frame arrived, so a click made while the driver was
	// waiting for the next 1PPS edge looked dead for up to a second - and
	// stayed dead forever if the edge never came. Abort the pending capture
	// here, then mark the run over so any late frame is ignored.
	_stop_requested = true;
	_running = false;
	if (_watchdog)
		_watchdog->stop();
	ds_trigger_set_en(0);
	if (_session)
		_session->stop_capture();
	dsv_info("1PPS stop: %d frame(s) measured, capture %.1f ms",
		_frame_count, _config.capture_time_ms);
	emit status_message(QString::fromUtf8("1PPS 测量已停止。"));
	emit frame_updated(); // refresh the dock's start/stop buttons
}

void PpsManager::OnMessage(int msg)
{
	if (msg == DSV_MSG_END_COLLECT_WORK && _running) {
		// Split the frame into its two real parts: the capture window the
		// hardware spends filling the buffer, and the readout the driver needs
		// to move the samples over USB. The driver times the frame from the
		// trigger packet, so its value already contains the capture window.
		_last_capture_ms = _config.capture_time_ms;
		const double transfer_ms = ds_last_frame_transfer_ms();
		if (transfer_ms > 0.0) {
			_last_total_ms = transfer_ms;
			_last_report_ms = transfer_ms - _last_capture_ms;
		} else if (_frame_timer.isValid()) {
			// Fallback: no driver measurement available for this frame.
			_last_total_ms = _frame_timer.elapsed();
			_last_report_ms = _last_total_ms - _last_capture_ms;
		}
		if (_last_report_ms < 0.0)
			_last_report_ms = 0.0;
		process_frame();
	}
}

void PpsManager::process_frame()
{
	data::Snapshot *base = _session->get_snapshot(SR_CHANNEL_LOGIC);
	data::LogicSnapshot *snap = dynamic_cast<data::LogicSnapshot*>(base);
	if (snap == nullptr || snap->empty() || snap->get_sample_count() == 0) {
		emit status_message("PPS: empty frame (no data), skipping decode.");
	}
	else {
		const uint64_t sample_count = snap->get_sample_count();
		const uint64_t sample_rate = _session->cur_samplerate();
		const uint64_t ref_pos = _session->get_trigger_pos();

		PpsDecoder decoder;
		_last = decoder.decode(snap, sample_count, sample_rate,
				ref_pos, _config.channels);

		// Compensate the analyzer's own crystal error: every delta is measured
		// in the analyzer's (slightly off) time base, so multiply by the
		// calibration factor before it reaches CSV, statistics or the view.
		if (_config.calib_factor != 1.0 && _config.calib_factor > 0.0) {
			for (size_t i = 0; i < _last.size(); ++i) {
				if (_last[i].valid)
					_last[i].delta_ns *= _config.calib_factor;
			}
		}

		// One log line per frame is ~220 MB per week at 4 fps, so only every
		// 60th frame goes to the log (DSVIEW_PPS_VERBOSE=1 restores every frame).
		const bool verbose = qEnvironmentVariableIsSet("DSVIEW_PPS_VERBOSE")
				|| ((_frame_count + 1) % 60 == 0);
		if (verbose) {
			dsv_info("PPS frame: samples=%" G_GUINT64_FORMAT " rate=%" G_GUINT64_FORMAT
				" ref=%" G_GUINT64_FORMAT " valid=%d/%u nanosec=%g "
				"(capture %.1f ms + upload/decode %.1f ms = %.1f ms)",
				sample_count, sample_rate, ref_pos,
				(int)std::count_if(_last.begin(), _last.end(),
					[](const PpsResult &r) { return r.valid; }),
				(unsigned)_last.size(),
				(double)(1.0e9 / (double)sample_rate),
				_last_capture_ms, _last_report_ms, _last_total_ms);
		}

		// All channels empty is usually a configuration mistake, not a hardware
		// fault: the wanted edges are simply not inside the window around the
		// reference edge. Say so once per frame instead of leaving a blank table.
		if (verbose && std::count_if(_last.begin(), _last.end(),
				[](const PpsResult &r) { return r.valid; }) == 0) {
			dsv_info("PPS: no measured edge inside this window (%.1f ms, "
				"trigger CH%d %s at %d%%) - check edge direction or "
				"capture time", _config.capture_time_ms, _config.ref_channel,
				_config.ref_edge == PPS_EDGE_FALLING ? "falling" : "rising",
				_config.trigger_pos_percent);
		}

		// Accumulate statistics for valid values.
		for (size_t i = 0; i < _last.size(); ++i) {
			if (!_last[i].valid)
				continue;
			const double v = _last[i].delta_ns;
			if (_count[i] == 0) {
				_min[i] = _max[i] = v;
				_mean[i] = v;
				_m2[i] = 0.0;
			} else {
				if (v < _min[i]) _min[i] = v;
				if (v > _max[i]) _max[i] = v;
				// Welford: update the running mean first, then the running
				// squared deviation. Accurate for millions of samples.
				const double delta = v - _mean[i];
				_mean[i] += delta / (double)(_count[i] + 1);
				_m2[i] += delta * (v - _mean[i]);
			}
			_count[i]++;
		}
		_frame_count++;

		// Log the frame to CSV (writer opens/creates the file as needed).
		PpsWriter writer;
		if (!writer.write_frame(_csv_path, _config, _last,
				QDateTime::currentDateTime()))
			emit status_message("PPS: CSV write failed.");

		// Anomalous frame: one of the watched channels had no edge. Keep the raw
		// capture as well, so the very frame can be inspected in DSView later.
		if (_config.auto_save_loss &&
				std::count_if(_last.begin(), _last.end(),
					[](const PpsResult &r) { return !r.valid; }) > 0) {
			// Throttle: at most one saved frame per minute. A channel that is
			// permanently edge-less would otherwise fill the disk with 45 KB
			// DSL files (4 per second, ~15 GB per week) and keep the capture
			// thread inside a nested event loop.
			const qint64 now = QDateTime::currentMSecsSinceEpoch();
			const qint64 kMinGapMs = 60000;
			if (_last_loss_save_ms >= 0 && now - _last_loss_save_ms < kMinGapMs) {
				if (_loss_skipped++ % 100 == 0)
					dsv_info("1PPS loss: %d loss frame(s) not saved "
						"(rate limited to 1 per 60 s)", _loss_skipped);
			} else if (save_loss_frame(_last)) {
				_last_loss_save_ms = now;
				_loss_frames++;
			}
		}

		emit frame_updated();
	}

	if (_stop_requested) {
		_running = false;
		ds_trigger_set_en(0);
		if (_watchdog)
			_watchdog->stop();
		emit status_message("PPS measurement stopped.");
		return;
	}

	// Immediate re-trigger with the same configuration.
	if (!launch_capture()) {
		_running = false;
		if (_watchdog)
			_watchdog->stop();
		emit status_message("PPS: re-trigger failed, stopping.");
	} else {
		_frame_timer.start();
		arm_watchdog();
	}
}

vector<PpsStats> PpsManager::statistics() const
{
	vector<PpsStats> out;
	for (size_t i = 0; i < _config.channels.size(); ++i) {
		PpsStats st;
		st.channel_index = _config.channels[i].channel_index;
		// Defensive: the accumulators are sized from the same channel list, but
		// a config edited between two captures could still leave them short.
		const size_t have = std::min(_count.size(), i + 1);
		st.count = (have > i) ? _count[i] : 0;
		st.mean = st.minimum = st.maximum = st.stddev = 0.0;
		if (st.count > 0 && have > i) {
			st.mean = _mean[i];
			st.minimum = _min[i];
			st.maximum = _max[i];
			const double var = _m2[i] / _count[i];
			st.stddev = std::sqrt(var > 0.0 ? var : 0.0);
		}
		out.push_back(st);
	}
	return out;
}

} // namespace pps
} // namespace pv
