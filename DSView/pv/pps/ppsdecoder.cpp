/*
 * 1PPS measurement decoder implementation.
 *
 * Edge search is implemented here with a bounded coarse+fine scan over
 * LogicSnapshot::get_sample instead of using LogicSnapshot::get_nxt_edge /
 * get_pre_edge. The library search helpers run their hierarchical walk
 * unlocked on the caller's index and can spin forever on a snapshot whose
 * ring/loop state changed after the capture, which froze the GUI thread in
 * the middle of a 1PPS frame.
 */
#include "ppsdecoder.h"
#include "../data/logicsnapshot.h"

#include <cmath>
#include <algorithm>

using std::vector;
using pv::data::LogicSnapshot;

namespace pv {
namespace pps {

// Coarse step used once a channel has been measured at least once: the first
// pass runs sample by sample (step 1) because a narrow test pulse may be
// shorter than any safe fixed step, and the frame after that steps by half the
// pulse width the first pass measured. That keeps a pulse at least two probes
// wide, so a level change can never be skipped over.
static const uint64_t kFineStep = 1;
static const uint64_t kFallbackStep = 64;
// The learned step is also used by find_all_edges(), the abnormal-pulse check.
// A step wider than the old fixed 64 would let a glitch narrower than that step
// sit entirely inside one probe interval - both endpoints read the same level, so
// the edge is never seen. Capping it here keeps narrow pulses fast without ever
// making the glitch hunt worse than the fixed step it replaced.
static const uint64_t kMaxStep = 64;
// Upper bound for the width measurement itself: a pulse wider than this is not
// a 1PPS test pulse, and scanning a whole second of samples would defeat the
// purpose of the measurement.
static const uint64_t kMaxWidthProbe = 1u << 20;

// Hard bound on probes per search, so a pathological snapshot can never hang
// the measurement loop. It scales with the window, so a small fixed count
// cannot silently cut the search short on long windows and report the far
// edges as lost.
static int64_t probe_limit(uint64_t sample_count, uint64_t step)
{
	if (step == 0)
		step = 1;
	return std::min<int64_t>(8000000,
		static_cast<int64_t>(sample_count / step) + 100000);
}

PpsDecoder::PpsDecoder() {}

void PpsDecoder::reset_steps()
{
	_steps.clear();
	_measured.clear();
}

uint64_t PpsDecoder::step_for(int channel_index) const
{
	// First frame of a run: exact scan. Channels that already had their first
	// chance but produced nothing ("数据为空") fall back to the fixed step
	// instead of scanning every sample of every frame forever.
	auto it = _steps.find(channel_index);
	if (it != _steps.end())
		return it->second;
	auto tried = _measured.find(channel_index);
	if (tried != _measured.end() && tried->second)
		return kFallbackStep;
	return kFineStep;
}

bool PpsDecoder::level(LogicSnapshot *snapshot, int channel_index, uint64_t sample)
{
	return snapshot->get_sample(sample, channel_index);
}

// Direction of the transition located at edge_sample:
// compare the level at the edge with the level just before it.
static int edge_direction(LogicSnapshot *snap, int ch, uint64_t edge)
{
	if (edge == 0)
		return PPS_EDGE_RISING; // no prior sample; treat first level as rising
	const bool before = PpsDecoder::level(snap, ch, edge - 1);
	const bool at = PpsDecoder::level(snap, ch, edge);
	if (!before && at)
		return PPS_EDGE_RISING;
	if (before && !at)
		return PPS_EDGE_FALLING;
	return PPS_EDGE_RISING;
}

// First sample in (lo, hi] whose level differs from the level at lo.
static bool refine(LogicSnapshot *snap, int ch, uint64_t lo, uint64_t hi,
		uint64_t &edge)
{
	const bool lv_lo = PpsDecoder::level(snap, ch, lo);
	for (uint64_t i = lo + 1; i <= hi; ++i) {
		if (PpsDecoder::level(snap, ch, i) != lv_lo) {
			edge = i;
			return true;
		}
	}
	return false;
}

// Last sample in (lo, hi] whose level differs from the sample before it, i.e.
// the transition closest to hi.
static bool refine_last(LogicSnapshot *snap, int ch, uint64_t lo, uint64_t hi,
		uint64_t &edge)
{
	bool lv = PpsDecoder::level(snap, ch, hi);
	for (uint64_t i = hi; i > lo; --i) {
		const bool lv_prev = PpsDecoder::level(snap, ch, i - 1);
		if (lv_prev != lv) {
			edge = i;
			return true;
		}
		lv = lv_prev;
	}
	return false;
}

// Width in samples of the level that follows `edge`, i.e. the distance to the
// next transition. Nothing is returned when the level never changes inside the
// probe budget: that is an empty measurement, not a pulse.
static bool pulse_width(LogicSnapshot *snap, int ch, uint64_t edge,
		uint64_t sample_count, uint64_t &width)
{
	if (edge + 1 >= sample_count)
		return false;
	const bool lv = PpsDecoder::level(snap, ch, edge);
	const uint64_t stop = std::min<uint64_t>(sample_count,
			edge + kMaxWidthProbe);
	for (uint64_t i = edge + 1; i < stop; ++i) {
		if (PpsDecoder::level(snap, ch, i) != lv) {
			width = i - edge;
			return true;
		}
	}
	return false;
}

// Nearest matching edge after t0, scanning forward to end.
static bool fwd_matching(LogicSnapshot *snap, int ch, int wanted,
		uint64_t t0, uint64_t end, uint64_t step, uint64_t &out)
{
	if (t0 > end)
		return false;
	if (step == 0)
		step = 1;

	bool lv = PpsDecoder::level(snap, ch, t0);
	uint64_t i = t0;
	int64_t probes = 0;
	const int64_t limit = probe_limit(end - t0 + 1, step);

	while (i < end && ++probes < limit) {
		const uint64_t next = (i + step > end) ? end : i + step;
		const bool lv_next = PpsDecoder::level(snap, ch, next);
		if (lv_next == lv) {
			i = next;
			continue;
		}

		uint64_t edge = 0;
		if (!refine(snap, ch, i, next, edge))
			return false;
		if (edge_direction(snap, ch, edge) == wanted) {
			out = edge;
			return true;
		}
		// Opposite direction: resume right at this edge.
		i = edge;
		lv = PpsDecoder::level(snap, ch, i);
	}
	return false;
}

// Nearest matching edge at or before t0, scanning backward to sample 0.
static bool bwd_matching(LogicSnapshot *snap, int ch, int wanted,
		uint64_t t0, uint64_t step, uint64_t &out)
{
	if (t0 == 0)
		return false;
	if (step == 0)
		step = 1;

	bool lv = PpsDecoder::level(snap, ch, t0);
	uint64_t i = t0;
	int64_t probes = 0;
	const int64_t limit = probe_limit(t0 + 1, step);

	while (i > 0 && ++probes < limit) {
		const uint64_t prev = (i <= step) ? 0 : i - step;
		const bool lv_prev = PpsDecoder::level(snap, ch, prev);
		if (lv_prev == lv) {
			i = prev;
			continue;
		}

		// The transition closest to t0 inside this step, not the first one:
		// with two edges in one step the earlier one is the wrong answer.
		uint64_t edge = 0;
		if (!refine_last(snap, ch, prev, i, edge))
			return false;
		if (edge_direction(snap, ch, edge) == wanted) {
			out = edge;
			return true;
		}
		// Opposite direction: resume right before this edge.
		if (edge == 0)
			return false;
		i = edge - 1;
		lv = PpsDecoder::level(snap, ch, i);
	}
	return false;
}

bool PpsDecoder::find_nearest_edge(LogicSnapshot *snapshot, int channel_index,
		int wanted_edge, uint64_t t0, uint64_t sample_count, uint64_t step,
		uint64_t &edge_sample)
{
	bool have_fwd = false, have_bwd = false;
	uint64_t fwd = 0, bwd = 0;

	if (t0 < sample_count)
		have_fwd = fwd_matching(snapshot, channel_index, wanted_edge, t0,
				sample_count - 1, step, fwd);
	if (t0 > 0)
		have_bwd = bwd_matching(snapshot, channel_index, wanted_edge, t0,
				step, bwd);

	if (have_fwd && have_bwd) {
		const uint64_t df = fwd - t0;
		const uint64_t db = t0 - bwd;
		edge_sample = (df < db) ? fwd : bwd;
		return true;
	}
	if (have_fwd) { edge_sample = fwd; return true; }
	if (have_bwd) { edge_sample = bwd; return true; }
	return false;
}

uint64_t PpsDecoder::find_all_edges(LogicSnapshot *snapshot, int channel_index,
		int wanted_edge, uint64_t sample_count,
		vector<uint64_t> &edges, uint64_t max_edges, uint64_t step)
{
	edges.clear();
	if (snapshot == nullptr || sample_count < 2)
		return 0;
	if (step == 0)
		step = 1;

	uint64_t i = 0;
	int64_t probes = 0;
	// A 1.2 s window at 100MHz is 120M samples: roughly 1.9M coarse probes.
	// Bound the walk by the window size (plus slack for refine() resumes) so a
	// long calibration frame can never spin forever either.
	const int64_t limit = probe_limit(sample_count, step);
	while (i + 1 < sample_count && ++probes < limit) {
		const uint64_t next = (i + step > sample_count - 1)
				? sample_count - 1 : i + step;
		if (PpsDecoder::level(snapshot, channel_index, next) ==
				PpsDecoder::level(snapshot, channel_index, i)) {
			i = next;
			continue;
		}

		uint64_t edge = 0;
		if (!refine(snapshot, channel_index, i, next, edge))
			break;
		if (edge_direction(snapshot, channel_index, edge) == wanted_edge)
			edges.push_back(edge);
		if (max_edges != 0 && edges.size() >= max_edges)
			break;
		i = edge;
	}
	return edges.size();
}

vector<PpsResult> PpsDecoder::decode(LogicSnapshot *snapshot,
		uint64_t sample_count, uint64_t sample_rate,
		uint64_t ref_trigger_sample, const vector<PpsChannel> &channels,
		bool count_edges)
{
	vector<PpsResult> results;
	if (snapshot == nullptr || sample_rate == 0 || sample_count == 0)
		return results;

	const uint64_t t0 = (ref_trigger_sample < sample_count)
			? ref_trigger_sample : sample_count / 2;
	const double ns_per_sample = 1.0e9 / static_cast<double>(sample_rate);

	for (const PpsChannel &ch : channels) {
		PpsResult r;
		r.channel_index = ch.channel_index;
		r.valid = false;
		r.delta_ns = 0.0;
		r.edge_count = 0;

		if (ch.enabled) {
			const int idx = ch.channel_index;
			// The very first frame of a run is scanned sample by sample; after
			// that the step comes from the pulse width that frame measured.
			const uint64_t step = step_for(idx);

			uint64_t edge = 0;
			if (find_nearest_edge(snapshot, idx, ch.edge,
					t0, sample_count, step, edge)) {
				const int64_t diff = static_cast<int64_t>(edge)
						- static_cast<int64_t>(t0);
				r.valid = true;
				r.delta_ns = static_cast<double>(diff) * ns_per_sample;

				// First measurement of this channel: learn its pulse width and
				// derive the coarse step for the following frames. An empty
				// measurement (no second transition) is ignored - the channel
				// simply keeps the fallback step instead of a learned one.
				if (step == kFineStep) {
					uint64_t width = 0;
					if (pulse_width(snapshot, idx, edge, sample_count, width)) {
						const uint64_t learned =
							std::min<uint64_t>(kMaxStep,
								std::max<uint64_t>(1, width / 2));
						_steps[idx] = learned;
					}
				}
			}
			// Remember that this channel has had its fine pass, whether it found
			// an edge or not, so a permanently empty channel cannot force a
			// sample-by-sample scan on every frame.
			_measured[idx] = true;

			// Edge count for the abnormal-pulse check. Only two are needed: the
			// condition is "more than one edge", so find_all_edges stops at 2.
			if (count_edges) {
				vector<uint64_t> edges;
				r.edge_count = static_cast<int>(find_all_edges(snapshot,
						idx, ch.edge, sample_count, edges, 2, step));
			}
		}
		results.push_back(r);
	}
	return results;
}

} // namespace pps
} // namespace pv
