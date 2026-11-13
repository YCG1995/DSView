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

// Coarse scanning step. A 1PPS test pulse is normally >= 256 ns wide
// (>= 256 samples at 1GHz, 128 at 500MHz), so a level change between two
// probes 64 samples apart is always a real edge while the probe count for a
// 10ms window stays small enough to be cheap.
static const uint64_t kCoarseStep = 64;
// Hard bound on probes per search, so a pathological snapshot can never hang
// the measurement loop. 200000 probes cover 12.8M samples at this step.
static const int kMaxProbes = 200000;

PpsDecoder::PpsDecoder() {}

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

// Nearest matching edge at or after t0, scanning forward to end.
static bool fwd_matching(LogicSnapshot *snap, int ch, int wanted,
		uint64_t t0, uint64_t end, uint64_t &out)
{
	if (t0 > end)
		return false;

	bool lv = PpsDecoder::level(snap, ch, t0);
	uint64_t i = t0;
	int probes = 0;

	while (i < end && ++probes < kMaxProbes) {
		const uint64_t next = (i + kCoarseStep > end) ? end : i + kCoarseStep;
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
		uint64_t t0, uint64_t &out)
{
	if (t0 == 0)
		return false;

	bool lv = PpsDecoder::level(snap, ch, t0);
	uint64_t i = t0;
	int probes = 0;

	while (i > 0 && ++probes < kMaxProbes) {
		const uint64_t prev = (i <= kCoarseStep) ? 0 : i - kCoarseStep;
		const bool lv_prev = PpsDecoder::level(snap, ch, prev);
		if (lv_prev == lv) {
			i = prev;
			continue;
		}

		uint64_t edge = 0;
		if (!refine(snap, ch, prev, i, edge))
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
		int wanted_edge, uint64_t t0, uint64_t sample_count, uint64_t &edge_sample)
{
	bool have_fwd = false, have_bwd = false;
	uint64_t fwd = 0, bwd = 0;

	if (t0 < sample_count)
		have_fwd = fwd_matching(snapshot, channel_index, wanted_edge, t0,
				sample_count - 1, fwd);
	if (t0 > 0)
		have_bwd = bwd_matching(snapshot, channel_index, wanted_edge, t0, bwd);

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
		vector<uint64_t> &edges, uint64_t max_edges)
{
	edges.clear();
	if (snapshot == nullptr || sample_count < 2)
		return 0;

	uint64_t i = 0;
	int probes = 0;
	// A 1.2 s window at 100MHz is 120M samples: roughly 1.9M coarse probes.
	// Bound the walk by the window size (plus slack for refine() resumes) so a
	// long calibration frame can never spin forever either.
	const int probe_limit = static_cast<int>(std::min<int64_t>(8000000,
			static_cast<int64_t>(sample_count / kCoarseStep) + 100000));
	while (i + 1 < sample_count && ++probes < probe_limit) {
		const uint64_t next = (i + kCoarseStep > sample_count - 1)
				? sample_count - 1 : i + kCoarseStep;
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
			uint64_t edge = 0;
			if (find_nearest_edge(snapshot, ch.channel_index, ch.edge,
					t0, sample_count, edge)) {
				const int64_t diff = static_cast<int64_t>(edge)
						- static_cast<int64_t>(t0);
				r.valid = true;
				r.delta_ns = static_cast<double>(diff) * ns_per_sample;
			}

			// Edge count for the abnormal-pulse check. Only two are needed: the
			// condition is "more than one edge", so find_all_edges stops at 2.
			if (count_edges) {
				vector<uint64_t> edges;
				r.edge_count = static_cast<int>(find_all_edges(snapshot,
						ch.channel_index, ch.edge, sample_count, edges, 2));
			}
		}
		results.push_back(r);
	}
	return results;
}

} // namespace pps
} // namespace pv
