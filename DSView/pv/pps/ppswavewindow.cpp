/*
 * 1PPS waveform window — implementation.
 */
#include "ppswavewindow.h"
#include "ppsmanager.h"
#include "../log.h"

#include <QAction>
#include <QCheckBox>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <climits>
#include <cmath>

// Qt 6 renamed QMouseEvent::pos()/localPos() to position(); this build uses
// Qt 5, so the two are bridged here.
static inline QPointF mouse_pos(const QMouseEvent *e)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	return e->position();
#else
	return e->localPos();
#endif
}

static inline QPointF mouse_pos(const QWheelEvent *e)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	return e->position();
#else
	return e->posF();
#endif
}

namespace pv {

namespace pps {

static const char *kColors[] = {
	"#2f7ed8", "#e06b1a", "#2a9d3f", "#d13a3a", "#8a5fc4",
	"#8c6a3f", "#d76aa8", "#6c7a89", "#b8a52c", "#1aa7b8",
	"#4a5fa8", "#6b8f4a", "#a8732a", "#9a3b3b", "#7a4a8a",
	"#367fa0"
};
static const int kColorCount = sizeof(kColors) / sizeof(kColors[0]);

static QColor channelColor(int i)
{
	return QColor(kColors[((i % kColorCount) + kColorCount) % kColorCount]);
}

// Values are nanoseconds; switch unit when the magnitude gets large so the
// axis labels stay readable instead of showing 2304240.
static QString fmtNs(double v)
{
	const double a = std::fabs(v);
	if (a >= 1e6)
		return QString::number(v / 1e6, 'f', 3) + QString::fromUtf8(" ms");
	if (a >= 1e3)
		return QString::number(v / 1e3, 'f', 3) + QString::fromUtf8(" \u03bcs");
	if (a >= 1.0)
		return QString::number(v, 'f', 1) + " ns";
	return QString::number(v, 'f', 3) + " ns";
}

// Tick label for an axis spanning `span` units of v. Fixed 3 decimals is not
// enough when the whole band covers e.g. 1.2799..1.2801 ms: every tick then
// prints as the same "1.280 ms", which looks exactly like a broken axis.
static QString fmtTick(double v, double span)
{
	const double a = std::fabs(v);
	const double u = (a >= 1e6) ? 1e6 : (a >= 1e3 ? 1e3 : 1.0);
	const QString suffix = (u == 1e6) ? QString::fromUtf8(" ms")
		: (u == 1e3 ? QString::fromUtf8(" \u03bcs") : QString(" ns"));
	const double s = std::fabs(span) / u;
	int dec = 3;
	if (s > 0.0 && s < 1.0) {
		dec = (int)std::ceil(-std::log10(s / 4.0));
		if (dec < 1)
			dec = 1;
		// Cap at 4: the label has to fit the 60-odd pixels of the left gutter,
		// and a number longer than the gutter reads as a truncated number.
		if (dec > 4)
			dec = 4;
	}
	return QString::number(v / u, 'f', dec) + suffix;
}

static void niceTicks(double lo, double hi, int target, QVector<double> &out)
{
	out.clear();
	if (!(hi > lo) || target < 2)
		return;
	const double span = hi - lo;
	const double raw = span / target;
	const double mag = std::pow(10.0, std::floor(std::log10(raw)));
	double step = mag;
	if (raw / mag > 5.0)
		step = mag * 10.0;
	else if (raw / mag > 2.0)
		step = mag * 5.0;
	else if (raw / mag > 1.0)
		step = mag * 2.0;
	const double first = std::ceil(lo / step) * step;
	for (double v = first; v <= hi + step * 1e-9; v += step)
		out.append(v);
	// A band narrower than one step would otherwise end up with a single tick,
	// or none at all - which is what "the Y axis shows no scale" looked like.
	if (out.size() < 2) {
		out.clear();
		out.append(lo);
		out.append(hi);
	}
}

PpsWaveCanvas::PpsWaveCanvas(QWidget *parent)
	: QWidget(parent)
{
	setMinimumSize(320, 180);
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
	setAutoFillBackground(false);
}

void PpsWaveCanvas::clear()
{
	_names.clear();
	_times.clear();
	_values.clear();
	_valid.clear();
	_visible.clear();
	_lod.clear();
	_lod_ready.clear();
	_time_is_index = false;
	_auto_x = true;
	_hover = -1;
	_base_ms = 0.0;
	_cache_dirty = true;
	update();
}

int PpsWaveCanvas::visibleCount() const
{
	int n = 0;
	for (int ch = 0; ch < _names.size(); ++ch) {
		if (ch >= _visible.size() || _visible[ch])
			n++;
	}
	return n;
}

void PpsWaveCanvas::setChannelVisible(int ch, bool on)
{
	if (ch < 0 || ch >= _names.size())
		return;
	if (_visible.size() != _names.size())
		_visible.fill(true, _names.size());
	_visible[ch] = on;
	_cache_dirty = true;
	update();
}

bool PpsWaveCanvas::channelVisible(int ch) const
{
	if (ch < 0 || ch >= _names.size())
		return false;
	return ch >= _visible.size() ? true : _visible[ch];
}

void PpsWaveCanvas::setViewMode(ViewMode mode)
{
	_mode = mode;
	_cache_dirty = true;
	update();
}

void PpsWaveCanvas::setShowPoints(bool on)
{
	_show_points = on;
	_cache_dirty = true;
	update();
}

void PpsWaveCanvas::setTimeAxisIsIndex(bool on)
{
	_time_is_index = on;
	_cache_dirty = true;
	update();
}

void PpsWaveCanvas::setTimeBase(double epoch_ms)
{
	_base_ms = (epoch_ms > 0.0) ? epoch_ms : 0.0;
	_cache_dirty = true;
	update();
}

void PpsWaveCanvas::setClipSigma(double k)
{
	// 0 disables the outlier cut entirely; 1..10 is the usable range.
	const double v = std::max(0.0, std::min(10.0, k));
	if (std::fabs(v - _clip_sigma) < 1e-9)
		return;
	_clip_sigma = v;
	_cache_dirty = true;
	update();
}

void PpsWaveCanvas::beginBulk()
{
	_bulk = true;
}

void PpsWaveCanvas::endBulk()
{
	if (!_bulk)
		return;
	_bulk = false;
	_cache_dirty = true;
	update();
}

void PpsWaveCanvas::selectionRange(double &t0, double &t1) const
{
	t0 = std::min(_sel_t0, _sel_t1);
	t1 = std::max(_sel_t0, _sel_t1);
}

void PpsWaveCanvas::setSelection(double t0, double t1)
{
	if (t1 < t0)
		std::swap(t0, t1);
	if (t1 <= t0)
		return;
	_sel_t0 = t0;
	_sel_t1 = t1;
	_sel_active = true;
	update();
	emit selection_changed();
}

void PpsWaveCanvas::clearSelection()
{
	if (!_sel_active && !_sel_dragging)
		return;
	_sel_active = false;
	_sel_dragging = false;
	update();
	emit selection_changed();
}

void PpsWaveCanvas::zoomToSelection()
{
	if (!_sel_active)
		return;
	double t0, t1;
	selectionRange(t0, t1);
	if (t1 <= t0)
		return;
	_auto_x = false;
	_t0 = t0;
	_t1 = t1;
	_cache_dirty = true;
	update();
}

double PpsWaveCanvas::benchFullRepaint(int n)
{
	if (n <= 0 || _times.isEmpty())
		return 0.0;
	QElapsedTimer timer;
	_dbg_collect_ms = 0.0;
	_dbg_axis_ms = 0.0;
	_dbg_curves_ms = 0.0;
	_dbg_points = 0;
	_dbg_path_ms = 0.0;
	_dbg_draw_ms = 0.0;
	timer.start();
	for (int i = 0; i < n; ++i) {
		_cache_dirty = true;
		grab();
	}
	return (double)timer.elapsed() / n;
}

void PpsWaveCanvas::setAutoX(bool on)
{
	_auto_x = on;
	if (on)
		recomputeTimeRange();
	_cache_dirty = true;
	update();
}

void PpsWaveCanvas::beginSeries(const QVector<QString> &names)
{
	_names = names;
	_times.clear();
	_values.clear();
	_valid.clear();
	_values.resize(_names.size());
	_valid.resize(_names.size());
	// A new run starts with every channel visible again.
	_visible.fill(true, _names.size());
	_auto_x = true;
	_hover = -1;
	_lod.clear();
	_lod_ready.clear();
	_cache_dirty = true;
}

void PpsWaveCanvas::appendFrame(double t, const QVector<double> &values,
		const QVector<char> &valid)
{
	if (_names.isEmpty())
		return;
	if (_values.size() != _names.size()) {
		_values.resize(_names.size());
		_valid.resize(_names.size());
		invalidateLod();
	}
	const int old_count = _times.size();
	_times.append(t);
	for (int ch = 0; ch < _names.size(); ++ch) {
		const double v = (ch < values.size()) ? values[ch] : 0.0;
		const char ok = (ch < valid.size()) ? valid[ch] : 0;
		_values[ch].append(v);
		_valid[ch].append(ok);
	}
	// Same ceiling as the window buffer: a month of frames must not turn into
	// a QVector that shifts millions of doubles on every new frame.
	const int kMaxFrames = 500000;
	if (_times.size() > kMaxFrames) {
		const int drop = std::max(kMaxFrames / 10, (int)_times.size() - kMaxFrames);
		_times.remove(0, drop);
		for (int ch = 0; ch < _values.size(); ++ch)
			_values[ch].remove(0, drop);
		for (int ch = 0; ch < _valid.size(); ++ch)
			_valid[ch].remove(0, drop);
		// A trim shifts every block boundary: the pyramid has to start over.
		invalidateLod();
	} else {
		for (int ch = 0; ch < _names.size(); ++ch)
			updateLodTail(ch, old_count, _times.size());
	}
	if (_auto_x)
		recomputeTimeRange();
	_cache_dirty = true;
	if (!_bulk)
		update();
}

void PpsWaveCanvas::setData(const QVector<QString> &names,
		const QVector<double> &times,
		const QVector<QVector<double> > &values,
		const QVector<QVector<char> > &valid,
		bool time_axis_is_index)
{
	_names = names;
	_times = times;
	_values = values;
	_valid = valid;
	_time_is_index = time_axis_is_index;
	_values.resize(_names.size());
	_valid.resize(_names.size());
	// Keep the user's visible/hidden choices when the channel set is unchanged.
	if (_visible.size() != _names.size())
		_visible.fill(true, _names.size());
	_auto_x = true;
	_hover = -1;
	invalidateLod();
	recomputeTimeRange();
	_cache_dirty = true;
	update();
}

double PpsWaveCanvas::timeSpan() const
{
	if (_times.size() < 2)
		return 0.0;
	return _times.last() - _times.first();
}

QRectF PpsWaveCanvas::plotRect() const
{
	// Every mode has its own Y scale and prints tick labels in the left gutter,
	// so the gutter has to hold "2.400 ms"-style text in all of them.
	int left = 76;
	if (_mode == SharedOverlay)
		left = 92;   // room for "2.400 ms"-style labels
	else if (_mode == Grid)
		left = 84;
	const int top = 10;
	const int right = 16;
	const int bottom = 56;   // two staggered label rows + the axis caption
	QRectF r(left, top, width() - left - right, height() - top - bottom);
	if (r.width() < 10 || r.height() < 10)
		return QRectF();
	return r;
}

void PpsWaveCanvas::recomputeTimeRange()
{
	if (_times.isEmpty()) {
		_t0 = 0.0;
		_t1 = 1.0;
		return;
	}
	double lo = _times.first();
	double hi = _times.last();
	const double span = hi - lo;
	if (_time_is_index) {
		if (span < 1.0)
			hi = lo + 1.0;
		lo -= 0.5;
		hi += 0.5;
	} else if (span < 1e-6) {
		lo -= 0.0005;
		hi += 0.0005;
	} else {
		// The curve starts at the very left edge of the plot: no padding before
		// the first frame, only a little room after the last one.
		hi += span * 0.02;
	}
	_t0 = lo;
	_t1 = hi;
}

double PpsWaveCanvas::mapTime(double t, const QRectF &r) const
{
	const double span = _t1 - _t0;
	if (span <= 0.0)
		return r.left();
	return r.left() + (t - _t0) / span * r.width();
}

double PpsWaveCanvas::timeAtX(double x, const QRectF &r) const
{
	if (r.width() <= 0.0)
		return _t0;
	return _t0 + (x - r.left()) / r.width() * (_t1 - _t0);
}

int PpsWaveCanvas::indexNear(double x, const QRectF &r) const
{
	if (_times.isEmpty() || !r.contains(QPointF(x, r.center().y())))
		return -1;
	const double span = _t1 - _t0;
	if (span <= 0.0)
		return 0;
	// Times are ascending: convert the pixel to a time and round it.
	const double t = timeAtX(x, r);
	int lo = 0, hi = _times.size() - 1;
	while (lo < hi) {
		const int mid = (lo + hi) / 2;
		if (_times[mid] < t)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo > 0 && std::fabs(_times[lo - 1] - t) < std::fabs(_times[lo] - t))
		return lo - 1;
	return lo;
}

// ---------------------------------------------------------------------------
// Level of detail: block min/max, so a 500k-frame run paints like a 2k one.
// ---------------------------------------------------------------------------

static const int kLodBlocks[] = {16, 256, 4096, 65536, 1048576};
static const int kLodLevelCount = (int)(sizeof(kLodBlocks) / sizeof(kLodBlocks[0]));

// Adds the scope's wall time into a counter, for the head-less benchmark.
struct AccTimer {
	QElapsedTimer t;
	double *acc;
	explicit AccTimer(double *a) : acc(a) { t.start(); }
	~AccTimer() { *acc += (double)t.nsecsElapsed() / 1e6; }
};

void PpsWaveCanvas::invalidateLod()
{
	_lod.clear();
	_lod_ready.clear();
}

void PpsWaveCanvas::invalidateLod(int ch)
{
	if (ch < 0)
		return;
	if (ch < _lod_ready.size())
		_lod_ready[ch] = 0;
	if (ch < _lod.size())
		_lod[ch].clear();
}

void PpsWaveCanvas::ensureLod(int ch) const
{
	if (ch < 0 || ch >= _values.size())
		return;
	if (_lod_ready.size() < _values.size()) {
		const int old = _lod_ready.size();
		_lod_ready.resize(_values.size());
		for (int i = old; i < _lod_ready.size(); ++i)
			_lod_ready[i] = 0;
	}
	if (_lod.size() < _values.size())
		_lod.resize(_values.size());
	if (_lod_ready[ch] && _lod[ch].size() == kLodLevelCount)
		return;

	QVector<LodLevel> &lvl = _lod[ch];
	lvl.clear();
	lvl.resize(kLodLevelCount);
	const QVector<double> &v = _values[ch];
	const QVector<char> &ok = _valid[ch];
	const int n = v.size();

	for (int li = 0; li < kLodLevelCount; ++li) {
		LodLevel &L = lvl[li];
		const int block = kLodBlocks[li];
		const int nb = (n + block - 1) / block;
		L.mn.resize(nb);
		L.mx.resize(nb);
		L.gap.resize(nb);

		if (li == 0) {
			for (int b = 0; b < nb; ++b) {
				const int s = b * block;
				const int e = std::min(n, s + block);
				double mn = 0.0, mx = 0.0;
				bool any = false, gap = false;
				for (int i = s; i < e; ++i) {
					if (!ok[i]) {
						gap = true;
						continue;
					}
					if (!any) {
						mn = mx = v[i];
						any = true;
					} else {
						mn = std::min(mn, v[i]);
						mx = std::max(mx, v[i]);
					}
				}
				L.mn[b] = mn;
				L.mx[b] = mx;
				L.gap[b] = any ? (gap ? 1 : 0) : 2;
			}
		} else {
			const LodLevel &P = lvl[li - 1];
			for (int b = 0; b < nb; ++b) {
				const int s = b * 16;
				const int e = std::min((int)P.mn.size(), s + 16);
				double mn = 0.0, mx = 0.0;
				bool any = false, gap = false;
				for (int k = s; k < e; ++k) {
					if (P.gap[k] == 2) {
						gap = true;
						continue;
					}
					if (!any) {
						mn = P.mn[k];
						mx = P.mx[k];
						any = true;
					} else {
						mn = std::min(mn, P.mn[k]);
						mx = std::max(mx, P.mx[k]);
					}
					if (P.gap[k] == 1)
						gap = true;
				}
				L.mn[b] = mn;
				L.mx[b] = mx;
				L.gap[b] = any ? (gap ? 1 : 0) : 2;
			}
		}
	}
	_lod_ready[ch] = 1;
}

void PpsWaveCanvas::updateLodTail(int ch, int old_count, int new_count)
{
	Q_UNUSED(old_count);
	if (ch < 0 || ch >= _lod_ready.size() || !_lod_ready[ch])
		return;   // not built yet: the lazy build covers all frames later
	if (ch >= _lod.size() || _lod[ch].size() != kLodLevelCount)
		return;
	QVector<LodLevel> &lvl = _lod[ch];
	const int n = _values[ch].size();
	for (int li = 0; li < kLodLevelCount; ++li) {
		LodLevel &L = lvl[li];
		const int block = kLodBlocks[li];
		const int nb = (std::max(1, new_count) + block - 1) / block;
		if (L.mn.size() < nb) {
			L.mn.resize(nb);
			L.mx.resize(nb);
			L.gap.resize(nb);
		}
		// Only the last two blocks can have changed.
		for (int b = std::max(0, nb - 2); b < nb; ++b) {
			const int s = b * block;
			const int e = std::min(n, s + block);
			double mn = 0.0, mx = 0.0;
			bool any = false, gap = false;
			if (li == 0) {
				for (int i = s; i < e; ++i) {
					if (!_valid[ch][i]) {
						gap = true;
						continue;
					}
					const double x = _values[ch][i];
					if (!any) {
						mn = mx = x;
						any = true;
					} else {
						mn = std::min(mn, x);
						mx = std::max(mx, x);
					}
				}
			} else {
				const LodLevel &P = lvl[li - 1];
				for (int k = b * 16; k < std::min((int)P.mn.size(), b * 16 + 16); ++k) {
					if (P.gap[k] == 2) {
						gap = true;
						continue;
					}
					if (!any) {
						mn = P.mn[k];
						mx = P.mx[k];
						any = true;
					} else {
						mn = std::min(mn, P.mn[k]);
						mx = std::max(mx, P.mx[k]);
					}
					if (P.gap[k] == 1)
						gap = true;
				}
			}
			L.mn[b] = mn;
			L.mx[b] = mx;
			L.gap[b] = any ? (gap ? 1 : 0) : 2;
		}
	}
}

void PpsWaveCanvas::visibleFrameRange(int &begin, int &end) const
{
	begin = 0;
	end = _times.size() - 1;
	if (_times.isEmpty())
		return;
	// Times are ascending: binary search both edges of the visible window.
	begin = (int)(std::lower_bound(_times.begin(), _times.end(), _t0)
			- _times.begin());
	if (begin >= _times.size())
		begin = _times.size() - 1;
	if (begin > 0)
		--begin;   // keep the point just outside, so the line reaches the edge
	end = (int)(std::upper_bound(_times.begin(), _times.end(), _t1)
			- _times.begin()) - 1;
	if (end < begin)
		end = begin;
	if (end < _times.size() - 1)
		++end;
}

// One min/max pair per painted pixel column, built from the pyramid when the
// window holds far more frames than columns. Gaps (invalid frames) are marked
// so the curve breaks there instead of jumping across missing data.
void PpsWaveCanvas::collectColumns(int ch, int begin, int end, const QRectF &r,
		QVector<double> &xs, QVector<double> &los, QVector<double> &his,
		QVector<char> &gap_before) const
{
	AccTimer guard(&_dbg_collect_ms);
	xs.clear();
	los.clear();
	his.clear();
	gap_before.clear();
	if (ch < 0 || ch >= _values.size() || _times.isEmpty() || r.width() < 1.0)
		return;
	begin = std::max(0, begin);
	end = std::min((int)_times.size() - 1, end);
	if (begin > end)
		return;

	const QVector<double> &v = _values[ch];
	const QVector<char> &ok = _valid[ch];
	const int width = std::max(1, (int)r.width());
	const int range = end - begin + 1;

	int cur_col = INT_MIN;
	double cur_x = 0.0, cur_lo = 0.0, cur_hi = 0.0;
	bool pending_gap = false;

	auto flush = [&]() {
		if (cur_col != INT_MIN) {
			xs.append(cur_x);
			los.append(cur_lo);
			his.append(cur_hi);
			gap_before.append(pending_gap ? 1 : 0);
			pending_gap = false;
		}
		cur_col = INT_MIN;
	};
	auto feed = [&](double x, double lo, double hi) {
		const int c = (int)std::floor(x);
		if (c != cur_col) {
			flush();
			cur_col = c;
			cur_x = x;
			cur_lo = lo;
			cur_hi = hi;
		} else {
			cur_lo = std::min(cur_lo, lo);
			cur_hi = std::max(cur_hi, hi);
			cur_x = x;
		}
	};
	auto feedRaw = [&](int s, int e) {
		for (int i = s; i <= e && i < v.size(); ++i) {
			if (!ok[i]) {
				// A gap narrower than a couple of pixels cannot be seen, and
				// splitting the path at every dropped frame multiplied the
				// segment count (and with it the paint time) by ten. Only a
				// gap wide enough to show up breaks the line.
				const double xg = mapTime(_times[i], r);
				if (cur_col != INT_MIN && std::fabs(xg - cur_x) > 2.0)
					flush();
				pending_gap = true;
				continue;
			}
			feed(mapTime(_times[i], r), v[i], v[i]);
		}
	};

	// A window that is not much wider than the canvas is cheaper to read raw
	// (and exact); beyond that the pyramid keeps it at ~2 samples per column.
	if (range <= 16 * width) {
		feedRaw(begin, end);
		flush();
		return;
	}

	ensureLod(ch);
	if (ch >= _lod.size() || _lod[ch].size() != kLodLevelCount) {
		feedRaw(begin, end);
		flush();
		return;
	}

	int li = -1;
	const double want = (double)range / (2.0 * width);
	for (int l = 0; l < kLodLevelCount; ++l) {
		if (kLodBlocks[l] <= want)
			li = l;
	}
	if (li < 0) {
		feedRaw(begin, end);
		flush();
		return;
	}

	const LodLevel &L = _lod[ch][li];
	const int block = kLodBlocks[li];
	const int b0 = begin / block;
	const int b1 = end / block;
	for (int b = b0; b <= b1; ++b) {
		if (b >= L.gap.size())
			break;
		const int bs = b * block;
		const int be = std::min((int)v.size() - 1, bs + block - 1);
		const int s = std::max(begin, bs);
		const int e = std::min(end, be);
		// A full block without a gap collapses into one vertical segment.
		if (L.gap[b] == 0 && s == bs && e == be) {
			const double t = _times[std::min(e, bs + block / 2)];
			feed(mapTime(t, r), L.mn[b], L.mx[b]);
		} else {
			feedRaw(s, e);
		}
	}
	flush();
}

// Robust Y range: samples farther than k * sigma (sigma = 1.4826 * MAD) from
// the median are ignored when fitting the range, so one runaway frame cannot
// flatten the whole curve into a line. With no such sample the range is the
// plain min/max - "no outliers, no trimming". This is one global rule, not a
// per-channel percentage.
static void robustRange(const QVector<double> &los, const QVector<double> &his,
		double k, double &lo, double &hi, int &dropped)
{
	dropped = 0;
	const int n = los.size();
	if (n <= 0) {
		lo = -1.0;
		hi = 1.0;
		return;
	}

	// Column min/max pairs: fit the range over every value that is drawn.
	QVector<double> all;
	all.reserve(n * 2);
	for (int i = 0; i < n; ++i) {
		all.append(los[i]);
		all.append(his[i]);
	}

	auto fullRange = [&]() {
		lo = los[0];
		hi = his[0];
		for (int i = 1; i < n; ++i) {
			lo = std::min(lo, los[i]);
			hi = std::max(hi, his[i]);
		}
	};

	if (k <= 0.0 || all.size() < 8) {
		fullRange();
		return;
	}

	// Median and MAD over the sampled values (median of absolute deviations).
	const int m = all.size();
	QVector<double> tmp = all;
	std::nth_element(tmp.begin(), tmp.begin() + m / 2, tmp.end());
	const double median = tmp[m / 2];
	for (int i = 0; i < m; ++i)
		tmp[i] = std::fabs(all[i] - median);
	std::nth_element(tmp.begin(), tmp.begin() + m / 2, tmp.end());
	const double mad = tmp[m / 2];
	const double sigma = 1.4826 * mad;
	if (sigma <= 0.0) {
		// More than half of the samples are identical, so MAD says nothing
		// about the spread: keep every value instead of guessing.
		fullRange();
		return;
	}

	const double lim = k * sigma;
	double keep_lo = 0.0, keep_hi = 0.0;
	bool have = false;
	for (int i = 0; i < n; ++i) {
		if (los[i] < median - lim || his[i] > median + lim) {
			++dropped;
			continue;
		}
		if (!have) {
			keep_lo = los[i];
			keep_hi = his[i];
			have = true;
		} else {
			keep_lo = std::min(keep_lo, los[i]);
			keep_hi = std::max(keep_hi, his[i]);
		}
	}
	if (!have || keep_hi <= keep_lo) {
		// Everything landed outside the band: fall back to the real extrema
		// rather than showing an empty axis.
		dropped = 0;
		fullRange();
		return;
	}
	lo = keep_lo;
	hi = keep_hi;
}


// Leave a little air above and below the curve, so the extremes are not glued
// to the band edges.
static void padRange(double &lo, double &hi)
{
	double span = hi - lo;
	if (span <= 0.0)
		span = (std::fabs(hi) > 1e-6) ? std::fabs(hi) * 0.05 : 1.0;
	lo -= span * 0.12;
	hi += span * 0.12;
}

void PpsWaveCanvas::drawColumns(QPainter &p, const QVector<double> &xs,
		const QVector<double> &los, const QVector<double> &his,
		const QVector<char> &gap_before, double lo, double hi,
		double top, double bottom) const
{
	AccTimer path_guard(&_dbg_path_ms);
	AccTimer draw_guard(&_dbg_draw_ms);
	_dbg_points += xs.size();
	const double scale = (hi > lo) ? (bottom - top) / (hi - lo) : 0.0;
	auto yOf = [&](double v) {
		double y = bottom - (v - lo) * scale;
		return std::max(top, std::min(bottom, y));
	};
	// Qt paints a stroked QPainterPath by intersecting it with the clip path,
	// which on this toolchain costs hundreds of milliseconds for a path of a
	// few thousand segments, while the same segments issued as plain drawLine
	// calls take about a millisecond. So the polyline is emitted segment by
	// segment here.
	double prev_x = 0.0;
	double prev_y = 0.0;
	bool have_prev = false;
	for (int i = 0; i < xs.size(); ++i) {
		const double x = xs[i];
		const double yh = yOf(his[i]);
		const double yl = yOf(los[i]);
		if (i > 0 && !gap_before[i] && his[i] == his[i - 1] && los[i] == los[i - 1]
				&& yh == yl) {
			// Flat run: the segment would be a single pixel again and again.
			prev_x = x;
			prev_y = yh;
			have_prev = true;
			continue;
		}
		if (i == 0 || gap_before[i] || !have_prev) {
			p.drawLine(QPointF(x, yh), QPointF(x, yl));
		} else {
			p.drawLine(QPointF(prev_x, prev_y), QPointF(x, yh));
			p.drawLine(QPointF(x, yh), QPointF(x, yl));
		}
		prev_x = x;
		prev_y = yl;
		have_prev = true;
	}
}

void PpsWaveCanvas::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setRenderHint(QPainter::TextAntialiasing, true);

	QRectF r = plotRect();
	if (r.isNull()) {
		p.fillRect(rect(), palette().color(QPalette::Base));
		p.setPen(palette().color(QPalette::Text));
		p.drawText(rect(), Qt::AlignCenter,
				QString::fromUtf8("窗口太小，请放大后再看波形"));
		return;
	}

	// The plot (panel, axis, curves) is rendered into a pixmap once per data /
	// view / size change. Hovering then only blits that pixmap and draws the
	// readout, so moving the mouse over a long run costs nothing.
	const int dpr = std::max(1, (int)devicePixelRatioF());
	const QSize want = QSize(width() * dpr, height() * dpr);
	const QColor base_now = palette().color(QPalette::Base);
	if (_cache_dirty || _cache.isNull() || _cache.size() != want
			|| _cache_base != base_now) {
		_cache = QPixmap(want);
		_cache.setDevicePixelRatio(dpr);
		_cache_base = base_now;
		QPainter cp(&_cache);
		cp.setRenderHint(QPainter::Antialiasing, true);
		cp.setRenderHint(QPainter::TextAntialiasing, true);
		renderPlot(cp, r);
		cp.end();
		_cache_dirty = false;
	}

	p.drawPixmap(0, 0, _cache);
	paintSelection(p, r);
	paintHover(p, r);
}

// Shaded block over the picked time range plus a small label. Drawn outside
// the cached pixmap, because the selection changes without the data changing.
void PpsWaveCanvas::paintSelection(QPainter &p, const QRectF &r)
{
	if (!_sel_active && !_sel_dragging)
		return;
	double x0, x1;
	if (_sel_dragging) {
		x0 = std::min(_sel_px0, _sel_px1);
		x1 = std::max(_sel_px0, _sel_px1);
	} else {
		x0 = mapTime(std::min(_sel_t0, _sel_t1), r);
		x1 = mapTime(std::max(_sel_t0, _sel_t1), r);
	}
	if (x1 - x0 < 1.0)
		return;
	x0 = std::max(x0, r.left());
	x1 = std::min(x1, r.right());

	QColor fill = palette().color(QPalette::Highlight);
	fill.setAlpha(55);
	p.fillRect(QRectF(x0, r.top(), x1 - x0, r.height()), fill);
	p.setPen(QPen(fill.darker(140), 1.0));
	p.drawLine(QPointF(x0, r.top()), QPointF(x0, r.bottom()));
	p.drawLine(QPointF(x1, r.top()), QPointF(x1, r.bottom()));

	// Range and frame count, anchored at the top inside the block.
	double t0, t1;
	if (_sel_dragging) {
		t0 = std::min(timeAtX(_sel_px0, r), timeAtX(_sel_px1, r));
		t1 = std::max(timeAtX(_sel_px0, r), timeAtX(_sel_px1, r));
	} else {
		selectionRange(t0, t1);
	}
	QString label = QString::fromUtf8("选区 %1 s").arg(t1 - t0, 0, 'f', 3);
	if (_base_ms > 0.0 && !_time_is_index) {
		const QDateTime a = QDateTime::fromMSecsSinceEpoch(
			(qint64)std::llround(_base_ms + t0 * 1000.0));
		const QDateTime b = QDateTime::fromMSecsSinceEpoch(
			(qint64)std::llround(_base_ms + t1 * 1000.0));
		label = QString::fromUtf8("%1 – %2  (%3 s)")
			.arg(a.toString("HH:mm:ss.zzz"))
			.arg(b.toString("HH:mm:ss.zzz"))
			.arg(t1 - t0, 0, 'f', 3);
	}
	QFont f = p.font();
	f.setPointSizeF(8.0);
	p.setFont(f);
	const QFontMetrics fm(f);
	const double w = fm.horizontalAdvance(label) + 10.0;
	QRectF box(std::min(x0, r.right() - w), r.top() + 3, w, 16);
	box.moveLeft(std::max(r.left() + 1.0,
		std::min(box.left(), r.right() - w)));
	QColor bg = palette().color(QPalette::Base);
	bg.setAlpha(220);
	p.setPen(QPen(fill.darker(140), 1.0));
	p.setBrush(bg);
	p.drawRect(box);
	p.setBrush(Qt::NoBrush);
	p.setPen(palette().color(QPalette::Text));
	p.drawText(box, Qt::AlignCenter, label);
}

void PpsWaveCanvas::renderPlot(QPainter &p, const QRectF &r)
{
	p.fillRect(rect(), palette().color(QPalette::Base));

	const QColor ink = palette().color(QPalette::Text);
	QColor axis = ink;
	axis.setAlpha(130);

	// Panel behind the curves.
	QColor panel = ink;
	panel.setAlpha(8);
	p.fillRect(r, panel);
	p.setPen(QPen(axis, 1.0));
	p.drawRect(r);

	if (_names.isEmpty() || _times.isEmpty()) {
		p.setPen(QPen(axis, 1.0));
		p.drawText(r, Qt::AlignCenter,
				QString::fromUtf8("暂无数据\n启动 1PPS 测量后自动跟随，或用“打开 CSV…”载入历史记录"));
		return;
	}
	if (_auto_x)
		recomputeTimeRange();

	paintTimeAxis(p, r);

	// Curves clip themselves to their own band; the surrounding box is not
	// clipped, because the Y tick labels and the channel tags live in the left
	// gutter - clipping to the plot rect silently swallowed them, which is why
	// the Y axis sometimes showed no scale at all.
	switch (_mode) {
	case SharedOverlay:
		paintShared(p, r);
		break;
	case Grid:
		paintGrid(p, r);
		break;
	case StackedOverlay:
	default:
		paintStacked(p, r);
		break;
	}
}

void PpsWaveCanvas::paintTimeAxis(QPainter &p, const QRectF &r)
{
	AccTimer axis_guard(&_dbg_axis_ms);
	const QColor ink = palette().color(QPalette::Text);
	QColor grid = ink;
	grid.setAlpha(45);
	QColor axis = ink;
	axis.setAlpha(140);

	QVector<double> ticks;
	const double span = _t1 - _t0;
	// Three label styles: frame index, wall clock, seconds since the first frame.
	const bool wall = (!_time_is_index && _base_ms > 0.0);
	QString wall_fmt = "HH:mm:ss";
	int dec = 2;
	if (_time_is_index) {
		dec = 0;
	} else if (!wall) {
		if (span >= 100)
			dec = 1;
		else if (span >= 10)
			dec = 2;
		else if (span >= 1)
			dec = 3;
		else if (span >= 0.1)
			dec = 4;
		else if (span >= 0.01)
			dec = 5;
		else
			dec = 6;
	}

	QFont f = p.font();
	f.setPointSizeF(8.0);
	p.setFont(f);

	// Wall clock labels need more digits as the axis is zoomed in, otherwise
	// every tick prints the same second. Pick the format from the span first,
	// then ask for as many ticks as those labels actually fit.
	if (wall) {
		if (span >= 60)
			wall_fmt = "HH:mm:ss";
		else if (span >= 3)
			wall_fmt = "HH:mm:ss.z";
		else if (span >= 0.3)
			wall_fmt = "HH:mm:ss.zz";
		else
			wall_fmt = "HH:mm:ss.zzz";
	}
	QString sample;
	if (_time_is_index)
		sample = "888888";
	else if (wall)
		sample = QDateTime::currentDateTime().toString(wall_fmt);
	else
		sample = QString::number(-1.0, 'f', dec);

	const double lw = std::max(52.0, (double)QFontMetrics(f).horizontalAdvance(sample) + 10.0);
	// Ask for fewer ticks than the labels could physically be packed: 26 px of
	// air per label is what keeps an 8 pt "10:23:15.120" from reading as one
	// continuous string.
	niceTicks(_t0, _t1, std::max(2, (int)(r.width() / (lw + 26.0))), ticks);

	const double step = ticks.size() > 1 ? (ticks[1] - ticks[0]) : span;
	if (wall && ticks.size() > 1) {
		// The format chosen above assumed a coarse span; refine it with the
		// real tick step so a zoomed-in axis does not repeat its labels.
		if (step >= 60)
			wall_fmt = "HH:mm:ss";
		else if (step >= 1)
			wall_fmt = "HH:mm:ss";
		else if (step >= 0.1)
			wall_fmt = "HH:mm:ss.z";
		else if (step >= 0.01)
			wall_fmt = "HH:mm:ss.zz";
		else
			wall_fmt = "HH:mm:ss.zzz";
	}

	p.setPen(QPen(grid, 1.0, Qt::DashLine));
	for (int i = 0; i < ticks.size(); ++i) {
		const double x = mapTime(ticks[i], r);
		if (x < r.left() - 1 || x > r.right() + 1)
			continue;
		p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
	}

	p.setPen(QPen(axis));
	// Two label rows: a dense axis puts every other label on a second line
	// instead of dropping every other tick or letting two labels collide. Each
	// row keeps its own gap, measured from the real text width of its
	// neighbours rather than from one sample string.
	const QFontMetrics fm(f);
	const double row_y[2] = {r.bottom() + 5, r.bottom() + 21};
	double row_right[2] = {-1e9, -1e9};
	for (int i = 0; i < ticks.size(); ++i) {
		const double x = mapTime(ticks[i], r);
		if (x < r.left() - 1 || x > r.right() + 1)
			continue;
		p.drawLine(QPointF(x, r.bottom()), QPointF(x, r.bottom() + 4));
		QString label;
		if (_time_is_index) {
			label = QString::number((qint64)ticks[i]);
		} else if (wall) {
			const QDateTime dt = QDateTime::fromMSecsSinceEpoch(
					(qint64)std::llround(_base_ms + ticks[i] * 1000.0));
			label = dt.toString(wall_fmt);
		} else {
			label = QString::number(ticks[i], 'f', dec);
		}
		const double w = (double)fm.horizontalAdvance(label) + 2.0;
		// Drop a label that cannot fit whole at either edge instead of clipping
		// it or sliding it into its neighbour.
		if (x - w / 2.0 < 1.0 || x + w / 2.0 > width() - 1.0)
			continue;
		// First row, then the second one, otherwise drop it: 10 px of air on
		// either side is the minimum that still reads as separate numbers.
		int row = -1;
		for (int k = 0; k < 2; ++k) {
			if (x - w / 2.0 - row_right[k] >= 10.0) {
				row = k;
				break;
			}
		}
		if (row < 0)
			continue;
		row_right[row] = x + w / 2.0;
		p.drawText(QRectF(x - w / 2.0, row_y[row], w, 15),
				Qt::AlignHCenter | Qt::AlignTop, label);
	}

	// Axis caption, anchored bottom right so it never collides with the
	// tick labels of a dense axis.
	QString caption;
	if (_time_is_index) {
		caption = QString::fromUtf8("帧序号");
	} else if (wall) {
		const QDateTime dt = QDateTime::fromMSecsSinceEpoch(
				(qint64)std::llround(_base_ms + _t0 * 1000.0));
		caption = QString::fromUtf8("时间 %1").arg(dt.toString("yyyy-MM-dd"));
	} else {
		caption = QString::fromUtf8("时间 (s)");
	}
	p.drawText(QRectF(r.right() - 160, r.bottom() + 37, 160, 15),
			Qt::AlignRight | Qt::AlignTop, caption);
}

void PpsWaveCanvas::paintStacked(QPainter &p, const QRectF &r)
{
	AccTimer curves_guard(&_dbg_curves_ms);
	const int n = _names.size();
	// Hidden channels do not take a band: the remaining curves use the full
	// height instead of leaving a gap.
	const int shown = std::max(1, visibleCount());
	const double band_h = r.height() / shown;

	QColor sep = palette().color(QPalette::Text);
	sep.setAlpha(60);

	QFont f = p.font();
	f.setPointSizeF(8.0);
	p.setFont(f);

	int begin = 0, end = 0;
	visibleFrameRange(begin, end);

	int band_index = 0;
	for (int ch = 0; ch < n; ++ch) {
		if (!channelVisible(ch))
			continue;
		const QRectF band(r.left(), r.top() + band_index * band_h,
				r.width(), band_h);
		if (band_index > 0) {
			p.setPen(QPen(sep, 1.0, Qt::DotLine));
			p.drawLine(QPointF(band.left(), band.top()),
					QPointF(band.right(), band.top()));
		}

		QVector<double> xs, los, his;
		QVector<char> gaps;
		collectColumns(ch, begin, end, r, xs, los, his, gaps);
		double lo, hi;
		int dropped = 0;
		robustRange(los, his, _clip_sigma, lo, hi, dropped);
		padRange(lo, hi);
		const double inner_top = band.top() + std::min(band_h * 0.18, 14.0);
		const double inner_bottom = band.bottom() - std::min(band_h * 0.12, 8.0);

		// Y ticks for this band. Two to four labels fit; they live in the left
		// gutter and the faint dashed lines make the band readable as a scale
		// instead of a strip with a range tag.
		QVector<double> yticks;
		niceTicks(lo, hi, std::max(2, (int)(band_h / 26)), yticks);
		{
			QColor tl = palette().color(QPalette::Text);
			tl.setAlpha(150);
			p.setPen(QPen(tl, 1.0, Qt::DashLine));
			for (int i = 0; i < yticks.size(); ++i) {
				const double y = inner_bottom - (yticks[i] - lo) / (hi - lo)
					* (inner_bottom - inner_top);
				p.drawLine(QPointF(band.left(), y), QPointF(band.right(), y));
			}
			p.setPen(QPen(tl));
			for (int i = 0; i < yticks.size(); ++i) {
				const double y = inner_bottom - (yticks[i] - lo) / (hi - lo)
					* (inner_bottom - inner_top);
				p.drawText(QRectF(2, y - 8, band.left() - 6, 16),
						Qt::AlignRight | Qt::AlignVCenter, fmtTick(yticks[i], hi - lo));
			}
		}

		const QColor col = channelColor(ch);
		p.setPen(QPen(col, 1.4));
		const QVector<double> &v = _values[ch];
		const QVector<char> &ok = _valid[ch];

		// Clip to the band: a clamped point plus a 1.4 px pen must never paint
		// over the neighbouring band or the left gutter.
		p.save();
		p.setClipRect(band);
		drawColumns(p, xs, los, his, gaps, lo, hi,
				inner_top, inner_bottom);

		if (_show_points) {
			if (end - begin < 2000) {
				const double inner_h = std::max(1.0, inner_bottom - inner_top);
				p.setBrush(col);
				p.setPen(Qt::NoPen);
				for (int i = begin; i <= end && i < v.size(); ++i) {
					if (!ok[i])
						continue;
					const double x = mapTime(_times[i], r);
					double y = inner_bottom - (v[i] - lo) / (hi - lo) * inner_h;
					y = std::max(inner_top, std::min(inner_bottom, y));
					p.drawEllipse(QPointF(x, y), 1.6, 1.6);
				}
				p.setBrush(Qt::NoBrush);
			}
		}
		p.restore();

		// Channel tag on the curve band; the outlier count only when some were
		// actually left out, so a clean run shows nothing extra.
		if (band_h >= 16) {
			p.setPen(col);
			p.drawText(QRectF(band.left() + 4, band.top() + 1, 60, 14),
					Qt::AlignLeft | Qt::AlignVCenter, _names[ch]);
			if (dropped > 0) {
				QColor dim = palette().color(QPalette::Text);
				dim.setAlpha(150);
				p.setPen(dim);
				p.drawText(QRectF(band.right() - 160, band.top() + 1, 156, 14),
						Qt::AlignRight | Qt::AlignVCenter,
						QString::fromUtf8("已剔除 %1 个异常值").arg(dropped));
			}
		}
		++band_index;
	}
}

void PpsWaveCanvas::paintShared(QPainter &p, const QRectF &r)
{
	QColor ink = palette().color(QPalette::Text);
	QColor grid = ink;
	grid.setAlpha(45);

	int begin = 0, end = 0;
	visibleFrameRange(begin, end);

	// Collect every visible channel once: the shared axis needs the union of
	// the per-channel (clipped) ranges, and the curves are drawn from the same
	// column lists afterwards.
	const int n = _names.size();
	QVector<QVector<double> > col_x(n), col_lo(n), col_hi(n);
	QVector<QVector<char> > col_gap(n);
	bool any = false;
	double lo = 0.0, hi = 1.0;
	for (int ch = 0; ch < n; ++ch) {
		if (!channelVisible(ch))
			continue;
		collectColumns(ch, begin, end, r, col_x[ch], col_lo[ch],
				col_hi[ch], col_gap[ch]);
		double a, b;
		int dropped = 0;
		robustRange(col_lo[ch], col_hi[ch], _clip_sigma, a, b, dropped);
		if (!any) {
			lo = a;
			hi = b;
			any = true;
		} else {
			lo = std::min(lo, a);
			hi = std::max(hi, b);
		}
	}
	if (!any) {
		lo = -1.0;
		hi = 1.0;
	}
	padRange(lo, hi);

	QFont f = p.font();
	f.setPointSizeF(8.0);
	p.setFont(f);

	// Y axis labels.
	QVector<double> yticks;
	niceTicks(lo, hi, std::max(2, (int)(r.height() / 45)), yticks);
	p.setPen(QPen(grid, 1.0, Qt::DashLine));
	for (int i = 0; i < yticks.size(); ++i) {
		const double y = r.bottom() - (yticks[i] - lo) / (hi - lo) * r.height();
		p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
	}
	for (int i = 0; i < yticks.size(); ++i) {
		const double y = r.bottom() - (yticks[i] - lo) / (hi - lo) * r.height();
		p.setPen(QPen(ink));
		p.drawLine(QPointF(r.left() - 4, y), QPointF(r.left(), y));
		p.drawText(QRectF(2, y - 8, r.left() - 8, 16),
				Qt::AlignRight | Qt::AlignVCenter, fmtTick(yticks[i], hi - lo));
	}

	// Curves.
	for (int ch = 0; ch < n; ++ch) {
		if (!channelVisible(ch))
			continue;
		const QColor col = channelColor(ch);
		p.setPen(QPen(col, 1.3));
		p.save();
		p.setClipRect(r);
		drawColumns(p, col_x[ch], col_lo[ch], col_hi[ch],
				col_gap[ch], lo, hi, r.top(), r.bottom());
		p.restore();
	}

	// Legend inside the plot so it stays visible when the plot is short.
	QFont lf = p.font();
	lf.setPointSizeF(7.5);
	p.setFont(lf);
	const int item_w = 54;
	const int item_h = 14;
	const int shown_ch = std::max(1, visibleCount());
	const int per_row = std::max(1, (int)((r.width() - 20) / item_w));
	const int rows = (shown_ch + per_row - 1) / per_row;
	QRectF box(r.left() + 6, r.top() + 6,
			std::min((double)per_row, (double)shown_ch) * item_w + 8,
			rows * item_h + 8);
	QColor box_bg = palette().color(QPalette::Base);
	box_bg.setAlpha(215);
	p.setPen(QPen(grid));
	p.setBrush(box_bg);
	p.drawRect(box);
	p.setBrush(Qt::NoBrush);
	int item = 0;
	for (int ch = 0; ch < n; ++ch) {
		if (!channelVisible(ch))
			continue;
		const int row = item / per_row;
		const int col = item % per_row;
		++item;
		const QPointF o(box.left() + 4 + col * item_w, box.top() + 5 + row * item_h);
		p.setPen(QPen(channelColor(ch), 2.0));
		p.drawLine(o, o + QPointF(10, 0));
		p.setPen(palette().color(QPalette::Text));
		p.drawText(QRectF(o.x() + 13, o.y() - 7, item_w - 13, 14),
				Qt::AlignLeft | Qt::AlignVCenter, _names[ch]);
	}
}

void PpsWaveCanvas::paintGrid(QPainter &p, const QRectF &r)
{
	const int n = _names.size();
	const int shown = std::max(1, visibleCount());
	const double row_h = r.height() / shown;

	QFont f = p.font();
	f.setPointSizeF(7.5);
	p.setFont(f);

	QColor ink = palette().color(QPalette::Text);
	QColor sep = ink;
	sep.setAlpha(70);

	int begin = 0, end = 0;
	visibleFrameRange(begin, end);

	int row_index = 0;
	for (int ch = 0; ch < n; ++ch) {
		if (!channelVisible(ch))
			continue;
		const QRectF row(r.left(), r.top() + row_index * row_h,
				r.width(), row_h);

		QVector<double> xs, los, his;
		QVector<char> gaps;
		collectColumns(ch, begin, end, r, xs, los, his, gaps);
		double lo, hi;
		int dropped = 0;
		robustRange(los, his, _clip_sigma, lo, hi, dropped);
		padRange(lo, hi);

		if (row_index > 0) {
			p.setPen(QPen(sep, 1.0, Qt::DotLine));
			p.drawLine(QPointF(row.left(), row.top()),
					QPointF(row.right(), row.top()));
		}

		const double pad = 6.0;
		const double inner_top = row.top() + pad;
		const double inner_bottom = row.bottom() - pad;
		const double inner_h = std::max(1.0, inner_bottom - inner_top);

		// Mid line for reference.
		QColor mid = ink;
		mid.setAlpha(30);
		p.setPen(QPen(mid, 1.0, Qt::DashLine));
		p.drawLine(QPointF(row.left(), (inner_top + inner_bottom) / 2),
				QPointF(row.right(), (inner_top + inner_bottom) / 2));

		// Y ticks: 2..4 labels in the gutter plus their dashed lines, so the
		// scale of each row is readable even when the row is short (the old
		// min/max-only labels disappeared below 34 px).
		QVector<double> yticks;
		niceTicks(lo, hi, std::max(2, (int)(row_h / 24)), yticks);
		{
			QColor tl = ink;
			tl.setAlpha(120);
			p.setPen(QPen(tl, 1.0, Qt::DashLine));
			for (int i = 0; i < yticks.size(); ++i) {
				const double y = inner_bottom - (yticks[i] - lo) / (hi - lo)
					* inner_h;
				p.drawLine(QPointF(row.left(), y), QPointF(row.right(), y));
			}
			p.setPen(QPen(tl));
			for (int i = 0; i < yticks.size(); ++i) {
				const double y = inner_bottom - (yticks[i] - lo) / (hi - lo)
					* inner_h;
				p.drawText(QRectF(2, y - 7, row.left() - 6, 14),
						Qt::AlignRight | Qt::AlignVCenter, fmtTick(yticks[i], hi - lo));
			}
		}

		const QColor col = channelColor(ch);
		p.setPen(QPen(col, 1.3));
		const QVector<double> &v = _values[ch];
		const QVector<char> &ok = _valid[ch];
		p.save();
		p.setClipRect(row);
		drawColumns(p, xs, los, his, gaps, lo, hi,
				inner_top, inner_bottom);

		if (_show_points && end - begin < 2000) {
			p.setBrush(col);
			p.setPen(Qt::NoPen);
			for (int i = begin; i <= end && i < v.size(); ++i) {
				if (!ok[i])
					continue;
				const double x = mapTime(_times[i], r);
				double y = inner_bottom - (v[i] - lo) / (hi - lo) * inner_h;
				y = std::max(inner_top, std::min(inner_bottom, y));
				p.drawEllipse(QPointF(x, y), 1.5, 1.5);
			}
			p.setBrush(Qt::NoBrush);
		}
		p.restore();

		// Row frame: a faint separator keeps the extrema of two neighbouring
		// channels from reading as one block of numbers.
		if (row_index > 0) {
			QColor sep2 = ink;
			sep2.setAlpha(70);
			p.setPen(QPen(sep2, 1, Qt::SolidLine));
			p.drawLine(QPointF(2, row.top()), QPointF(r.right(), row.top()));
		}

		// Left gutter: channel name at the top of the row, the tick labels sit
		// below it (the extrema are implied by the outermost ticks now).
		p.setPen(col);
		p.drawText(QRectF(2, row.top() + 1, r.left() - 10, 14),
				Qt::AlignRight | Qt::AlignTop, _names[ch]);
		if (dropped > 0) {
			QColor dim = ink;
			dim.setAlpha(150);
			p.setPen(dim);
			p.drawText(QRectF(2, row.bottom() - 15, r.left() - 10, 14),
					Qt::AlignRight | Qt::AlignBottom,
					QString::fromUtf8("剔 %1").arg(dropped));
		}
		++row_index;
	}
}

void PpsWaveCanvas::paintHover(QPainter &p, const QRectF &r)
{
	if (_hover < 0 || _hover >= _times.size())
		return;

	QColor ink = palette().color(QPalette::Text);
	QColor bar = ink;
	bar.setAlpha(110);
	const double x = mapTime(_times[_hover], r);

	p.setPen(QPen(bar, 1.0, Qt::DashLine));
	p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));

	// Readout box on the right: frame time plus one line per visible channel.
	// Channels that do not fit are simply not listed - no extra hint line.
	const int line_h = 15;
	const int max_lines = std::max(1, (int)((r.height() - 20) / line_h) - 1);
	QVector<int> lines;
	for (int ch = 0; ch < _names.size(); ++ch) {
		if (!channelVisible(ch))
			continue;
		if (lines.size() >= max_lines)
			break;
		lines.append(ch);
	}
	const int shown = lines.size();
	QRectF box(0, r.top() + 6, 168, line_h * (shown + 1) + 10);
	box.moveLeft(r.right() - box.width() - 8);
	QColor bg = palette().color(QPalette::Base);
	bg.setAlpha(230);
	p.setPen(QPen(bar));
	p.setBrush(bg);
	p.drawRect(box);
	p.setBrush(Qt::NoBrush);

	QFont f = p.font();
	f.setPointSizeF(8.0);
	p.setFont(f);

	QString head;
	if (_time_is_index) {
		head = QString::fromUtf8("帧 #%1").arg((qint64)_times[_hover]);
	} else if (_base_ms > 0.0) {
		const QDateTime dt = QDateTime::fromMSecsSinceEpoch((qint64)std::llround(
				_base_ms + _times[_hover] * 1000.0));
		head = dt.toString("yyyy-MM-dd HH:mm:ss.z");
	} else {
		head = QString::fromUtf8("t = %1 s").arg(_times[_hover], 0, 'f', 6);
	}
	p.setPen(ink);
	p.drawText(QRectF(box.left() + 6, box.top() + 4, box.width() - 12, line_h),
			Qt::AlignLeft | Qt::AlignVCenter, head);

	for (int i = 0; i < shown; ++i) {
		const int ch = lines[i];
		const QVector<char> &ok = _valid[ch];
		const bool has = (_hover < ok.size() && ok[_hover]);
		const double y0 = box.top() + 4 + (i + 1) * line_h;

		p.setPen(QPen(channelColor(ch), 2.0));
		p.drawLine(QPointF(box.left() + 6, y0 + line_h / 2.0),
				QPointF(box.left() + 15, y0 + line_h / 2.0));

		p.setPen(ink);
		p.drawText(QRectF(box.left() + 20, y0, 44, line_h),
				Qt::AlignLeft | Qt::AlignVCenter, _names[ch]);
		p.drawText(QRectF(box.left() + 60, y0, box.width() - 66, line_h),
				Qt::AlignRight | Qt::AlignVCenter,
				has ? fmtNs(_values[ch][_hover])
				    : QString::fromUtf8("无效"));
	}
}

void PpsWaveCanvas::mouseMoveEvent(QMouseEvent *event)
{
	QRectF r = plotRect();
	if (_sel_dragging) {
		_sel_px1 = std::max(r.left(), std::min(r.right(), mouse_pos(event).x()));
		update();
		return;
	}
	if (_panning) {
		const double span = _pan_t1 - _pan_t0;
		if (span > 0.0 && r.width() > 0.0) {
			const double dt = (mouse_pos(event).x() - _pan_x0) / r.width() * span;
			_t0 = _pan_t0 - dt;
			_t1 = _pan_t1 - dt;
			// Keep the window inside the data, so panning cannot scroll the
			// curve out of sight and leave an empty canvas.
			if (!_times.isEmpty()) {
				if (_t0 < _times.first()) {
					_t0 = _times.first();
					_t1 = _t0 + span;
				}
				if (_t1 > _times.last()) {
					_t1 = _times.last();
					_t0 = _t1 - span;
				}
			}
			_cache_dirty = true;
			update();
		}
		return;
	}
	if (r.isNull()) {
		return;
	}
	if (_auto_x)
		recomputeTimeRange();
	const int idx = indexNear(mouse_pos(event).x(), r);
	if (idx != _hover) {
		_hover = idx;
		update();
	}
}

void PpsWaveCanvas::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton)
		return;
	QRectF r = plotRect();
	if (r.isNull() || !r.contains(mouse_pos(event).toPoint()))
		return;
	if (_auto_x)
		recomputeTimeRange();
	// Shift + left drag marks a time range instead of panning: the range can
	// then be zoomed into or written out as a CSV.
	if (event->modifiers() & Qt::ShiftModifier) {
		_sel_dragging = true;
		_sel_px0 = std::max(r.left(), std::min(r.right(),
			mouse_pos(event).x()));
		_sel_px1 = _sel_px0;
		setCursor(Qt::CrossCursor);
		return;
	}
	_sel_active = false;
	_panning = true;
	_pan_x0 = mouse_pos(event).x();
	_pan_t0 = _t0;
	_pan_t1 = _t1;
	setCursor(Qt::ClosedHandCursor);
}

void PpsWaveCanvas::mouseReleaseEvent(QMouseEvent *)
{
	if (_sel_dragging) {
		_sel_dragging = false;
		unsetCursor();
		QRectF r = plotRect();
		if (!r.isNull() && std::fabs(_sel_px1 - _sel_px0) >= 4.0) {
			_sel_t0 = timeAtX(std::min(_sel_px0, _sel_px1), r);
			_sel_t1 = timeAtX(std::max(_sel_px0, _sel_px1), r);
			_sel_active = true;
		} else {
			_sel_active = false;
		}
		update();
		emit selection_changed();
		return;
	}
	if (_panning) {
		_panning = false;
		_auto_x = false;
		unsetCursor();
	}
}

void PpsWaveCanvas::mouseDoubleClickEvent(QMouseEvent *)
{
	_auto_x = true;
	_sel_active = false;
	recomputeTimeRange();
	_cache_dirty = true;
	update();
	emit selection_changed();
}

void PpsWaveCanvas::leaveEvent(QEvent *)
{
	if (_hover != -1) {
		_hover = -1;
		update();
	}
}

void PpsWaveCanvas::wheelEvent(QWheelEvent *event)
{
	if (_times.size() < 2)
		return;
	QRectF r = plotRect();
	if (r.isNull() || !r.contains(mouse_pos(event).toPoint()))
		return;

	if (_auto_x) {
		recomputeTimeRange();
		_auto_x = false;
	}

	const double full = std::max(_times.last() - _times.first(), 1e-9);
	// Never zoom below roughly two frame intervals: a window narrower than that
	// holds at most one sample, which paints as a lone dot and leaves most of
	// the canvas looking empty.
	const int nframes = _times.size();
	const double frame_dt = (nframes > 1) ? full / (nframes - 1) : full;
	const double min_span = std::max(full * 0.001, frame_dt * 2.0);
	const double span = _t1 - _t0;
	const bool zoom_in = event->angleDelta().y() > 0;
	double newSpan = zoom_in ? span / 1.4 : span * 1.4;
	newSpan = std::max(min_span, std::min(full, newSpan));

	const double ta = timeAtX(mouse_pos(event).x(), r);
	const double frac = (span > 0.0) ? (ta - _t0) / span : 0.5;
	_t0 = ta - frac * newSpan;
	_t1 = _t0 + newSpan;

	// Never show more than the data itself: a margin wider than the visible
	// window would leave the whole canvas blank at high zoom.
	if (_t0 < _times.first()) {
		_t0 = _times.first();
		_t1 = _t0 + newSpan;
	}
	if (_t1 > _times.last()) {
		_t1 = _times.last();
		_t0 = _t1 - newSpan;
	}
	_cache_dirty = true;
	update();
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

PpsWaveWindow::PpsWaveWindow(pps::PpsManager *manager, QWidget *parent)
	: QWidget(parent, Qt::Window)
	, _manager(manager)
{
	setWindowTitle(QString::fromUtf8("1PPS 波形 — 每帧延时变化"));
	setAttribute(Qt::WA_DeleteOnClose, false);
	resize(1100, 660);
	buildUi();

	_shot_path = QString::fromUtf8(qgetenv("DSVIEW_PPS_WAVE_SHOT"));
	const QByteArray shot_delay = qgetenv("DSVIEW_PPS_WAVE_SHOT_DELAY");
	if (!shot_delay.isEmpty())
		_shot_delay_ms = shot_delay.toInt();
	// Optional start-up view mode (0 stacked / 1 shared axis / 2 grid).
	const QByteArray mode_env = qgetenv("DSVIEW_PPS_WAVE_MODE");
	if (!mode_env.isEmpty()) {
		const int m = mode_env.toInt();
		if (m >= 0 && m <= 2)
			_mode_box->setCurrentIndex(m);
	}
	// Optional Y-axis outlier clipping percentage, for head-less verification.
	const QByteArray clip_env = qgetenv("DSVIEW_PPS_WAVE_CLIP");
	if (!clip_env.isEmpty())
		_clip_spin->setValue(clip_env.toDouble());

	// Optional CSV to show on open, for verifying the file reader head-lessly.
	const QString csv = QString::fromUtf8(qgetenv("DSVIEW_PPS_WAVE_CSV"));
	if (!csv.isEmpty()) {
		QString error;
		if (openCsv(csv, error)) {
			QTimer::singleShot(400, this, [this]() { maybeSaveShot(); });
			QTimer::singleShot(_shot_delay_ms, this, [this]() { maybeSaveShot(); });
			// Head-less benchmark: report the average full repaint cost of the
			// loaded file, so a big CSV can be compared before/after a change.
			const QByteArray bench_env = qgetenv("DSVIEW_PPS_WAVE_BENCH");
			if (!bench_env.isEmpty()) {
				const int n = std::max(3, bench_env.toInt());
				QTimer::singleShot(700, this, [this, n]() {
					const double ms = _canvas->benchFullRepaint(n);
					dsv_info("PPS wave bench: %d frames, %d channels, "
						"%.2f ms per full repaint",
						_canvas->frameCount(), _canvas->channelCount(), ms);
					// Also next to the screenshot: the log file is not always
					// where one expects it on this build.
					if (!_shot_path.isEmpty()) {
						QFile f(_shot_path + ".bench.txt");
						if (f.open(QIODevice::WriteOnly | QIODevice::Text))
							f.write(QString::fromUtf8(
								"frames=%1 channels=%2 ms_per_repaint=%3 ms_collect=%4 ms_axis=%5 ms_curves=%6 points=%7\n")
								.arg(_canvas->frameCount())
								.arg(_canvas->channelCount())
								.arg(QString::number(ms, 'f', 2))
								.arg(QString::number(_canvas->debugCollectMs() / n, 'f', 2))
								.arg(QString::number(_canvas->debugAxisMs() / n, 'f', 2))
								.arg(QString::number(_canvas->debugCurvesMs() / n, 'f', 2))
								.arg(_canvas->debugPoints())
								.toUtf8());
					}
				});
			}
		} else {
			dsv_warn("PPS wave: CSV load failed: %s",
				error.toUtf8().constData());
			_info->setText(error);
		}
	}

	if (_manager) {
		connect(_manager, &PpsManager::frame_updated,
				this, &PpsWaveWindow::on_frame_updated);
		connect(_manager, &PpsManager::status_message,
				this, [this](const QString &text) {
			Q_UNUSED(text);
		});
	}

	// Automation hook: mark a time range as if it had been dragged with Shift,
	// and write that range out, so the shaded block and the CSV export can be
	// checked without a mouse.
	const QString sel_test = QString::fromUtf8(qgetenv("DSVIEW_PPS_WAVE_SEL_TEST"));
	if (!sel_test.isEmpty()) {
		QTimer::singleShot(6000, this, [this, sel_test]() {
			QStringList parts = sel_test.split(',');
			if (parts.size() < 2 || _canvas->frameCount() < 10)
				return;
			double t0 = parts[0].toDouble();
			double t1 = parts[1].toDouble();
			_canvas->setSelection(t0, t1);
			const QString out = _shot_path.isEmpty()
				? QString() : _shot_path + ".sel.png";
			if (!out.isEmpty())
				_canvas->grab().save(out);
			if (_shot_path.isEmpty())
				return;
			int rows = 0;
			QString error;
			const QString csv = _shot_path + ".sel.csv";
			if (writeRangeCsv(csv, t0, t1, rows, error))
				dsv_info("PPS wave: selection test %s -> %d rows", csv.toUtf8().constData(), rows);
			else
				dsv_err("PPS wave: selection test failed: %s", error.toUtf8().constData());
		});
	}

	// Automation hook: put the hover readout on a frame and keep a copy, so the
	// readout box can be inspected without moving a mouse.
	const QString hover_test = QString::fromUtf8(
		qgetenv("DSVIEW_PPS_WAVE_HOVER_TEST"));
	if (!hover_test.isEmpty()) {
		QTimer::singleShot(hover_test.toInt() > 0 ? hover_test.toInt() : 6000,
				this, [this]() {
			const QPointF pos(_canvas->width() * 0.55, _canvas->height() / 2.0);
			QMouseEvent me(QEvent::MouseMove, pos,
					_canvas->mapToGlobal(pos.toPoint()), Qt::NoButton,
					Qt::NoButton, Qt::NoModifier);
			QApplication::sendEvent(_canvas, &me);
			const QString out = _shot_path.isEmpty()
				? QString() : _shot_path + ".hover.png";
			if (!out.isEmpty() && _canvas->frameCount() > 0)
				_canvas->grab().save(out);
			dsv_info("PPS wave: hover test on frame %d/%d",
				_canvas->hoverFrame() + 1, _canvas->frameCount());
		});
	}

	// Automation hook: freeze and unfreeze the live view once, to prove that
	// frames keep being buffered while the view is not following.
	if (qgetenv("DSVIEW_PPS_WAVE_FOLLOW_TEST") == "1") {
		QTimer::singleShot(5000, this, [this]() {
			_follow_box->setChecked(false);
		});
		QTimer::singleShot(13000, this, [this]() {
			_follow_box->setChecked(true);
		});
	}

	// Automation hook: crank the wheel to maximum zoom-in, then keep a copy of
	// that frame so the clamped view can be inspected without a mouse.
	const QString zoom_test = QString::fromUtf8(qgetenv("DSVIEW_PPS_WAVE_ZOOM_TEST"));
	if (!zoom_test.isEmpty()) {
		// Optional: drop 实时跟随 first, so the zoom survives the next frame the
		// way a person zooming by hand would experience it.
		const QString unfollow = QString::fromUtf8(
			qgetenv("DSVIEW_PPS_WAVE_UNFOLLOW_AT"));
		if (!unfollow.isEmpty()) {
			QTimer::singleShot(unfollow.toInt(), this, [this]() {
				_follow_box->setChecked(false);
			});
		}
		QTimer::singleShot(zoom_test.toInt() > 0 ? zoom_test.toInt() : 8000,
				this, [this, zoom_test]() {
			const double fx = qgetenv("DSVIEW_PPS_WAVE_ZOOM_X").isEmpty()
				? 0.5 : qgetenv("DSVIEW_PPS_WAVE_ZOOM_X").toDouble();
			const QPointF pos(_canvas->width() * fx,
					_canvas->height() / 2.0);
			for (int i = 0; i < 60; ++i) {
				QWheelEvent we(pos, _canvas->mapToGlobal(pos.toPoint()),
						QPoint(), QPoint(0, 120), Qt::NoButton,
						Qt::NoModifier, Qt::NoScrollPhase, false);
				QApplication::sendEvent(_canvas, &we);
			}
			const QString out = _shot_path.isEmpty()
				? QString() : _shot_path + ".zoom.png";
			if (!out.isEmpty() && _canvas->frameCount() > 0)
				_canvas->grab().save(out);
			dsv_info("PPS wave: zoom test view=%.0f ms (data span %.0f ms), %d frames",
				_canvas->viewSpan() * 1000.0, _canvas->timeSpan() * 1000.0,
				_canvas->frameCount());
		});
	}
	updateInfo();
}

void PpsWaveWindow::buildUi()
{
	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 6, 8, 6);
	layout->setSpacing(4);

	QHBoxLayout *bar = new QHBoxLayout();
	bar->setSpacing(8);

	QLabel *mode_lbl = new QLabel(QString::fromUtf8("视图"), this);
	bar->addWidget(mode_lbl);

	_mode_box = new QComboBox(this);
	_mode_box->addItem(QString::fromUtf8("叠加（各通道自适应）"));
	_mode_box->addItem(QString::fromUtf8("叠加（统一纵轴）"));
	_mode_box->addItem(QString::fromUtf8("分栏（每通道一格）"));
	bar->addWidget(_mode_box);

	_follow_box = new QCheckBox(QString::fromUtf8("实时跟随"), this);
	_follow_box->setChecked(true);
	_follow_box->setEnabled(_manager != nullptr);
	bar->addWidget(_follow_box);

	_points_box = new QCheckBox(QString::fromUtf8("显示点"), this);
	bar->addWidget(_points_box);

	// Outlier trimming for the automatic Y range: one global value for every
	// channel. A sample farther than k * sigma from the median is left out of
	// the range fit, so a runaway frame does not stretch the band into a line.
	// No sample is dropped when none is that far out.
	bar->addWidget(new QLabel(QString::fromUtf8("Y 轴剔除"), this));
	_clip_spin = new QDoubleSpinBox(this);
	_clip_spin->setRange(0.0, 10.0);
	_clip_spin->setDecimals(1);
	_clip_spin->setSingleStep(0.5);
	_clip_spin->setValue(3.0);
	_clip_spin->setSuffix(QString::fromUtf8(" ×MAD"));
	_clip_spin->setMaximumWidth(96);
	_clip_spin->setToolTip(QString::fromUtf8(
			"自适应 Y 轴时忽略偏离中位数超过 k×1.4826×MAD 的异常值（全局统一口径）。\n"
			"没有异常值时不剔除；0 表示始终使用完整极值。"));
	bar->addWidget(_clip_spin);

	_open_btn = new QPushButton(QString::fromUtf8("打开 CSV…"), this);
	bar->addWidget(_open_btn);
	_clear_btn = new QPushButton(QString::fromUtf8("清空"), this);
	bar->addWidget(_clear_btn);
	_export_btn = new QPushButton(QString::fromUtf8("导出图片…"), this);
	bar->addWidget(_export_btn);

	// Region selection lives in one menu: pick the range with Shift + drag, then
	// zoom into it or write just those frames out.
	_sel_btn = new QToolButton(this);
	_sel_btn->setText(QString::fromUtf8("选区"));
	_sel_btn->setPopupMode(QToolButton::InstantPopup);
	_sel_btn->setToolTip(QString::fromUtf8(
			"按住 Shift 在图上拖动可框选时间段"));
	_sel_menu = new QMenu(_sel_btn);
	_sel_btn->setMenu(_sel_menu);
	_sel_view_act = _sel_menu->addAction(QString::fromUtf8("查看选区"));
	_sel_save_act = _sel_menu->addAction(QString::fromUtf8("保存选区 CSV…"));
	_sel_visible_act = _sel_menu->addAction(QString::fromUtf8("保存当前可见范围 CSV…"));
	_sel_menu->addSeparator();
	_sel_clear_act = _sel_menu->addAction(QString::fromUtf8("取消选区"));
	bar->addWidget(_sel_btn);

	// Per-channel visibility: a menu of tick boxes, rebuilt whenever the channel
	// set changes, so a busy plot can be thinned down to the curves of interest.
	_ch_btn = new QToolButton(this);
	_ch_btn->setText(QString::fromUtf8("通道"));
	_ch_btn->setPopupMode(QToolButton::InstantPopup);
	_ch_menu = new QMenu(_ch_btn);
	_ch_btn->setMenu(_ch_menu);
	bar->addWidget(_ch_btn);

	bar->addStretch(1);
	_info = new QLabel(this);
	bar->addWidget(_info);

	layout->addLayout(bar);

	_canvas = new PpsWaveCanvas(this);
	layout->addWidget(_canvas, 1);

	connect(_mode_box, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
			this, [this](int idx) {
		_canvas->setViewMode((PpsWaveCanvas::ViewMode)idx);
	});
	connect(_points_box, &QCheckBox::toggled, this, [this](bool on) {
		_canvas->setShowPoints(on);
	});
	connect(_clip_spin,
			static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
			this, [this](double k) {
		_canvas->setClipSigma(k);
	});
	connect(_sel_view_act, &QAction::triggered, this, &PpsWaveWindow::onViewSelection);
	connect(_sel_save_act, &QAction::triggered, this, &PpsWaveWindow::onSaveSelection);
	connect(_sel_visible_act, &QAction::triggered, this, &PpsWaveWindow::onSaveVisible);
	connect(_sel_clear_act, &QAction::triggered, this, [this]() {
		_canvas->clearSelection();
	});
	connect(_canvas, &PpsWaveCanvas::selection_changed,
			this, &PpsWaveWindow::updateSelectionButtons);
	updateSelectionButtons();

	_flush_timer = new QTimer(this);
	_flush_timer->setSingleShot(true);
	connect(_flush_timer, &QTimer::timeout, this, [this]() {
		// Hand the buffered frames to the canvas in one batch: following every
		// arriving frame repainted the whole plot each time, which is what kept
		// the CPU busy on a long run.
		if (!_follow_live || _canvas == nullptr)
			return;
		_canvas->beginBulk();
		for (int i = _flushed; i < _pend_t_ms.size(); ++i) {
			QVector<double> vals(_pend_values.size());
			QVector<char> okv(_pend_valid.size());
			for (int ch = 0; ch < _pend_values.size(); ++ch) {
				vals[ch] = (i < _pend_values[ch].size()) ? _pend_values[ch][i] : 0.0;
				okv[ch] = (i < _pend_valid[ch].size()) ? _pend_valid[ch][i] : 0;
			}
			_canvas->appendFrame((_pend_t_ms[i] - _origin_ms) / 1000.0, vals, okv);
		}
		_flushed = _pend_t_ms.size();
		_canvas->endBulk();
		updateInfo();
		// Give the first frames a chance to fill the plot before capturing. The
		// delay is configurable so a test can wait for the whole run.
		if (_canvas->frameCount() >= 5 && !_shot_done && !_shot_scheduled) {
			_shot_scheduled = true;
			QTimer::singleShot(_shot_delay_ms, this, [this]() { maybeSaveShot(); });
		}
	});
	connect(_follow_box, &QCheckBox::toggled, this, [this](bool on) {
		_follow_live = on;
		if (!on)
			return;
		// Everything that arrived while the view was frozen is still in the
		// buffer: repaint it from scratch instead of starting from now.
		replayPending();
		_canvas->setAutoX(true);
		_source = QString::fromUtf8("实时跟随");
		if (!_live_csv.isEmpty())
			_source = QString::fromUtf8("实时跟随  %1")
				.arg(QFileInfo(_live_csv).fileName());
		updateInfo();
	});
	connect(_open_btn, &QPushButton::clicked, this, &PpsWaveWindow::onBrowse);
	connect(_clear_btn, &QPushButton::clicked, this, &PpsWaveWindow::onClear);
	connect(_export_btn, &QPushButton::clicked, this, &PpsWaveWindow::onExport);

	_source = QString::fromUtf8("等待测量");
}

void PpsWaveWindow::on_frame_updated()
{
	if (!_manager)
		return;

	if (_manager->csv_path() != _live_csv) {
		// A new run started: everything before belongs to another measurement.
		_live_csv = _manager->csv_path();
		_origin_ms = -1.0;
		_canvas->clear();
		_pend_names.clear();
		_pend_t_ms.clear();
		_pend_values.clear();
		_pend_valid.clear();
		_source = (QFileInfo(_live_csv).exists() || !_live_csv.isEmpty())
			? QString::fromUtf8("实时跟随  %1").arg(QFileInfo(_live_csv).fileName())
			: QString::fromUtf8("实时跟随");
	}

	// Frames are recorded even while the view is frozen, so turning 实时跟随
	// back on repaints the whole run.
	appendLiveFrame();
	updateInfo();
}

void PpsWaveWindow::rebuildChannelMenu()
{
	if (_ch_menu == nullptr || _canvas == nullptr)
		return;

	_ch_menu->clear();
	const int n = _canvas->channelCount();
	_ch_btn->setEnabled(n > 0);

	// Head-less check hook: DSVIEW_PPS_WAVE_HIDE=1,3 hides those channels once,
	// right after the channel set becomes known.
	if (!_hide_env_applied && n > 0) {
		_hide_env_applied = true;
		const QStringList parts = qEnvironmentVariable("DSVIEW_PPS_WAVE_HIDE")
			.split(',', Qt::SkipEmptyParts);
		for (int i = 0; i < parts.size(); ++i) {
			const int ch = parts[i].trimmed().toInt();
			if (ch >= 0 && ch < n)
				_canvas->setChannelVisible(ch, false);
		}
	}

	for (int ch = 0; ch < n; ++ch) {
		QAction *act = _ch_menu->addAction(
			QString("CH%1").arg(ch));
		act->setCheckable(true);
		const bool on = _canvas->channelVisible(ch);
		act->setChecked(on);
		const QColor col = channelColor(ch);
		QPixmap swatch(10, 10);
		swatch.fill(col);
		act->setIcon(QIcon(swatch));
		connect(act, &QAction::toggled, this, [this, ch](bool on) {
			_canvas->setChannelVisible(ch, on);
		});
	}
	if (n > 1) {
		_ch_menu->addSeparator();
		QAction *all = _ch_menu->addAction(QString::fromUtf8("全部显示"));
		connect(all, &QAction::triggered, this, [this]() {
			for (int ch = 0; ch < _canvas->channelCount(); ++ch)
				_canvas->setChannelVisible(ch, true);
			rebuildChannelMenu();
		});
	}
}

void PpsWaveWindow::maybeSaveShot()
{
	if (_shot_path.isEmpty() || _shot_done)
		return;
	if (_canvas->frameCount() == 0)
		return;
	// The whole window, not just the canvas: the toolbar carries the Y-range
	// trimming control, and a screenshot of the canvas alone would not show it.
	// A save can fail while the window is still being laid out, so try again
	// instead of silently ending up with no file.
	if (grab().save(_shot_path)) {
		_shot_done = true;
		return;
	}
	if (_shot_tries++ < 6)
		QTimer::singleShot(400, this, [this]() { maybeSaveShot(); });
}

void PpsWaveWindow::appendLiveFrame()
{
	const std::vector<PpsResult> &last = _manager->last_results();
	if (last.empty())
		return;

	QVector<QString> names;
	QVector<double> values;
	QVector<char> valid;
	for (size_t i = 0; i < last.size(); ++i) {
		names.append(QString("CH%1").arg(last[i].channel_index));
		values.append(last[i].delta_ns);
		valid.append(last[i].valid ? 1 : 0);
	}

	const double now_ms = (double)QDateTime::currentMSecsSinceEpoch();
	recordFrame(now_ms, names, values, valid);

	if (!_follow_live)
		return;   // buffered only, the view stays as the user left it

	// Following means auto scale, no matter what the user did before.
	_canvas->setAutoX(true);

	if (_canvas->channelCount() != names.size() || _origin_ms < 0.0) {
		// Channel set changed (or first frame): the canvas is rebuilt around
		// this frame, so this timestamp becomes the new zero.
		_origin_ms = now_ms;
		_canvas->beginSeries(names);
		_flushed = 0;
		_canvas->setTimeBase(_origin_ms);
		rebuildChannelMenu();
	}
	// Frames reach the canvas on the flush timer, at most 10 per second.
	if (!_flush_timer->isActive())
		_flush_timer->start(100);
}

void PpsWaveWindow::recordFrame(double t_ms, const QVector<QString> &names,
		const QVector<double> &values, const QVector<char> &valid)
{
	// A different channel set means a different measurement; start over.
	if (_pend_names != names || _pend_values.size() != names.size()) {
		_pend_names = names;
		_pend_t_ms.clear();
		_pend_values.clear();
		_pend_valid.clear();
		_pend_values.resize(names.size());
		_pend_valid.resize(names.size());
	}

	_pend_t_ms.append(t_ms);
	for (int ch = 0; ch < names.size(); ++ch) {
		_pend_values[ch].append(ch < values.size() ? values[ch] : 0.0);
		_pend_valid[ch].append(ch < valid.size() ? valid[ch] : 0);
	}

	// 1 fps for a day is 86400 frames; keep a hard ceiling anyway so a bug in
	// the feed cannot grow the buffer without bound. Trim 10% at a time (once
	// every ~3.5 h at 4 fps) instead of one frame per tick.
	const int kMaxFrames = 500000;
	if (_pend_t_ms.size() > kMaxFrames) {
		const int drop = std::max(kMaxFrames / 10, (int)_pend_t_ms.size() - kMaxFrames);
		_pend_t_ms.remove(0, drop);
		for (int ch = 0; ch < _pend_values.size(); ++ch)
			_pend_values[ch].remove(0, drop);
		for (int ch = 0; ch < _pend_valid.size(); ++ch)
			_pend_valid[ch].remove(0, drop);
	}
}

void PpsWaveWindow::replayPending()
{
	_canvas->clear();
	if (_pend_t_ms.isEmpty())
		return;

	// Bulk load instead of one appendFrame per buffered frame: appending frame
	// by frame repainted and re-ranged the whole series n times, which is what
	// made "real-time follow" freeze for seconds after a long freeze.
	_origin_ms = _pend_t_ms.first();
	QVector<double> times(_pend_t_ms.size());
	for (int i = 0; i < _pend_t_ms.size(); ++i)
		times[i] = (_pend_t_ms[i] - _origin_ms) / 1000.0;
	_canvas->setData(_pend_names, times, _pend_values, _pend_valid, false);
	_canvas->setTimeBase(_origin_ms);
	_flushed = _pend_t_ms.size();
	rebuildChannelMenu();
	_canvas->setAutoX(true);
}

void PpsWaveWindow::updateInfo()
{
	const QString span = _canvas->frameCount() > 1
		? QString::number(_canvas->timeSpan(), 'f', 2) + " s"
		: QString::fromUtf8("—");
	_info->setText(QString::fromUtf8("%1 · 帧 %2 · 通道 %3 · 跨度 %4")
			.arg(_source)
			.arg(_canvas->frameCount())
			.arg(_canvas->channelCount())
			.arg(span));
}

bool PpsWaveWindow::openCsv(const QString &path, QString &error)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
		error = QString::fromUtf8("无法打开文件：%1").arg(path);
		return false;
	}

	QTextStream in(&f);
	// Qt6: QTextStream 默认 UTF-8，不再需要 setCodec

	QVector<QString> names;
	QVector<double> times;
	QVector<QVector<double> > values;
	QVector<QVector<char> > valid;

	bool time_is_index = false;
	QDateTime base;
	QString first_line;
	int row = 0;

	while (!in.atEnd()) {
		QString line = in.readLine();
		line.remove(QChar(0xFEFF));
		if (line.trimmed().isEmpty())
			continue;

		const QStringList cols = line.split(',');
		if (row == 0) {
			// Header: 触发时间戳(ms),CH0,CH1...
			for (int c = 1; c < cols.size(); ++c) {
				QString n = cols[c].trimmed();
				if (n.isEmpty())
					n = QString("CH%1").arg(c - 1);
				names.append(n);
			}
			values.resize(names.size());
			valid.resize(names.size());
			first_line = line;
			++row;
			continue;
		}

		if (cols.size() < 2) {
			++row;
			continue;
		}

		// The first column is a wall clock stamp; a numeric value means the
		// file was written by another tool, so use the row number instead.
		double t = (double)(row - 1);
		const QString ts = cols[0].trimmed();
		QDateTime dt = QDateTime::fromString(ts, "yyyy-MM-dd HH:mm:ss.zzz");
		if (!dt.isValid())
			dt = QDateTime::fromString(ts, "yyyy-MM-dd HH:mm:ss");
		if (dt.isValid()) {
			if (!base.isValid())
				base = dt;
			t = base.msecsTo(dt) / 1000.0;
		} else {
			time_is_index = true;
		}

		times.append(t);
		for (int ch = 0; ch < names.size(); ++ch) {
			const int col = ch + 1;
			if (col >= cols.size()) {
				values[ch].append(0.0);
				valid[ch].append(0);
				continue;
			}
			const QString v = cols[col].trimmed();
			bool ok = false;
			const double val = v.toDouble(&ok);
			values[ch].append(ok ? val : 0.0);
			valid[ch].append((ok && !v.isEmpty()) ? 1 : 0);
		}
		++row;
	}

	if (names.isEmpty() || times.isEmpty()) {
		error = QString::fromUtf8("文件里没有可识别的 PPS 数据（需要表头 触发时间戳(ms),CHn,...）。");
		return false;
	}

	_canvas->setData(names, times, values, valid, time_is_index);
	for (int ch = 0; ch < names.size(); ++ch)
		_canvas->setChannelVisible(ch, true);
	rebuildChannelMenu();
	// Wall clock of the first row, so the X axis prints HH:mm:ss.
	_canvas->setTimeBase(base.isValid() ? (double)base.toMSecsSinceEpoch() : 0.0);
	dsv_info("PPS wave: loaded %d frame(s), %d channel(s) from %s",
		times.size(), names.size(), QFileInfo(path).fileName().toUtf8().constData());
	_follow_live = false;
	_follow_box->setChecked(false);
	_source = QString::fromUtf8("文件  %1").arg(QFileInfo(path).fileName());
	updateInfo();
	updateSelectionButtons();
	return true;
}

void PpsWaveWindow::onBrowse()
{
	const QString dir = _manager && !_manager->csv_path().isEmpty()
		? QFileInfo(_manager->csv_path()).absolutePath()
		: QString();
	const QString path = QFileDialog::getOpenFileName(this,
			QString::fromUtf8("打开 1PPS 数据 CSV"), dir,
			QString::fromUtf8("CSV 文件 (*.csv);;所有文件 (*.*)"));
	if (path.isEmpty())
		return;

	QString error;
	if (!openCsv(path, error)) {
		_info->setText(error);
		QMessageBox::warning(this, QString::fromUtf8("打开失败"), error);
	}
}

void PpsWaveWindow::onExport()
{
	if (_canvas->frameCount() == 0) {
		_info->setText(QString::fromUtf8("没有数据可导出"));
		return;
	}
	const QString path = QFileDialog::getSaveFileName(this,
			QString::fromUtf8("导出波形图片"), QString("pps_wave.png"),
			QString::fromUtf8("PNG 图片 (*.png);;所有文件 (*.*)"));
	if (path.isEmpty())
		return;
	if (!_canvas->grab().save(path))
		QMessageBox::warning(this, QString::fromUtf8("导出失败"),
				QString::fromUtf8("无法写入：%1").arg(path));
	else
		_info->setText(QString::fromUtf8("已导出 %1").arg(QFileInfo(path).fileName()));
}

void PpsWaveWindow::onClear()
{
	_canvas->clear();
	_origin_ms = -1.0;
	_live_csv.clear();
	_flushed = 0;
	_pend_names.clear();
	_pend_t_ms.clear();
	_pend_values.clear();
	_pend_valid.clear();
	updateInfo();
	updateSelectionButtons();
}

void PpsWaveWindow::updateSelectionButtons()
{
	const bool on = _canvas && _canvas->hasSelection();
	if (_sel_view_act)
		_sel_view_act->setEnabled(on);
	if (_sel_save_act)
		_sel_save_act->setEnabled(on);
	if (_sel_clear_act)
		_sel_clear_act->setEnabled(on);
	if (_sel_visible_act)
		_sel_visible_act->setEnabled(_canvas && _canvas->frameCount() > 0);
}

void PpsWaveWindow::onViewSelection()
{
	if (!_canvas)
		return;
	double t0, t1;
	_canvas->selectionRange(t0, t1);
	_canvas->zoomToSelection();
	_info->setText(QString::fromUtf8("查看选区 %1 s").arg(t1 - t0, 0, 'f', 3));
}

void PpsWaveWindow::onSaveSelection()
{
	if (!_canvas)
		return;
	double t0, t1;
	_canvas->selectionRange(t0, t1);
	const QString path = QFileDialog::getSaveFileName(this,
			QString::fromUtf8("保存选区数据 CSV"), QString("pps_wave_part.csv"),
			QString::fromUtf8("CSV 文件 (*.csv);;所有文件 (*.*)"));
	if (path.isEmpty())
		return;
	int rows = 0;
	QString error;
	if (!writeRangeCsv(path, t0, t1, rows, error))
		QMessageBox::warning(this, QString::fromUtf8("保存失败"), error);
	else
		_info->setText(QString::fromUtf8("已保存选区 %1 行：%2")
			.arg(rows).arg(QFileInfo(path).fileName()));
}

void PpsWaveWindow::onSaveVisible()
{
	if (!_canvas)
		return;
	// "Visible" is the current zoom/pan window: what is on screen right now.
	const double t0 = _canvas->viewStart();
	const double t1 = _canvas->viewEnd();
	const QString path = QFileDialog::getSaveFileName(this,
			QString::fromUtf8("保存当前可见范围 CSV"),
			QString("pps_wave_view.csv"),
			QString::fromUtf8("CSV 文件 (*.csv);;所有文件 (*.*)"));
	if (path.isEmpty())
		return;
	int rows = 0;
	QString error;
	if (!writeRangeCsv(path, t0, t1, rows, error))
		QMessageBox::warning(this, QString::fromUtf8("保存失败"), error);
	else
		_info->setText(QString::fromUtf8("已保存可见范围 %1 行：%2")
			.arg(rows).arg(QFileInfo(path).fileName()));
}

bool PpsWaveWindow::writeRangeCsv(const QString &path, double t0, double t1,
		int &rows, QString &error)
{
	rows = 0;
	if (!_canvas || _canvas->frameCount() == 0) {
		error = QString::fromUtf8("没有数据可保存。");
		return false;
	}
	if (t1 < t0)
		std::swap(t0, t1);

	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		error = QString::fromUtf8("无法写入：%1").arg(path);
		return false;
	}
	QTextStream out(&f);
	// Same layout as the measurement CSV, so the file opens in this window
	// again and stays compatible with the rest of the tooling: a UTF-8 BOM,
	// the wall clock column, then one column per channel.
	out.setGenerateByteOrderMark(true);

	const QVector<QString> &names = _canvas->names();
	out << QString::fromUtf8("触发时间戳(ms)");
	for (int ch = 0; ch < names.size(); ++ch)
		out << "," << names[ch];
	out << "\n";

	const QVector<double> &times = _canvas->times();
	const QVector<QVector<double> > &values = _canvas->values();
	const QVector<QVector<char> > &valid = _canvas->validFlags();
	const double base_ms = _canvas->timeBaseMs();
	const bool is_index = _canvas->timeIsIndex();

	for (int i = 0; i < times.size(); ++i) {
		const double t = times[i];
		if (t < t0 - 1e-9 || t > t1 + 1e-9)
			continue;
		QString stamp;
		if (is_index)
			stamp = QString::number((qint64)t);
		else if (base_ms > 0.0)
			stamp = QDateTime::fromMSecsSinceEpoch(
					(qint64)std::llround(base_ms + t * 1000.0))
				.toString("yyyy-MM-dd HH:mm:ss.zzz");
		else
			stamp = QString::number(t * 1000.0, 'f', 3);
		out << stamp;
		for (int ch = 0; ch < names.size(); ++ch) {
			const bool ok = (ch < valid.size()) && i < valid[ch].size()
				&& valid[ch][i] != 0;
			if (ok && ch < values.size() && i < values[ch].size())
				out << "," << QString::number(values[ch][i], 'f', 1);
			else
				out << ",";
		}
		out << "\n";
		++rows;
	}
	out.flush();
	f.close();
	if (rows == 0) {
		error = QString::fromUtf8("该时间范围内没有任何帧。");
		return false;
	}
	return true;
}

} // namespace pps
} // namespace pv
