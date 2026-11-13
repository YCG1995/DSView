/*
 * 1PPS measurement decoder — dedicated, synchronous, no Python/generic decode.
 * For each measured channel it locates the edge (selectable rising/falling)
 * nearest to the reference trigger position and returns the signed time
 * difference in nanoseconds. Channels with no matching edge yield no value.
 */
#ifndef DSVIEW_PV_PPS_PPSDECODER_H
#define DSVIEW_PV_PPS_PPSDECODER_H

#include <stdint.h>
#include <vector>
#include <QString>

namespace pv {

namespace data {
class LogicSnapshot;
}

namespace pps {

enum PPS_EDGE
{
    PPS_EDGE_RISING = 0,
    PPS_EDGE_FALLING = 1,
};

// One measured channel. channel_index is the global logic channel index.
struct PpsChannel
{
    int channel_index;
    int edge;          // PPS_EDGE
    bool enabled;
    // Keep the raw frame as a .dsl file when this channel yields no edge.
    bool save_on_loss;
};

struct PpsResult
{
    int channel_index;
    bool valid;
    double delta_ns;   // measured time - reference time, signed
    // Number of edges of the configured direction inside the capture window
    // (capped at 2). Only filled when PpsDecoder::decode() was asked to count
    // them, which the "异常脉冲" DSL save needs; 0 means "not counted".
    int edge_count;
};

class PpsDecoder
{
public:
    PpsDecoder();

    // Decode one captured frame.
    // snapshot          : completed logic snapshot for the frame
    // sample_count      : number of samples in the frame
    // sample_rate       : Hz
    // ref_trigger_sample: reference trigger position t0 (sample index)
    // channels          : measured channels (reference channel already excluded)
    // count_edges       : also fill PpsResult::edge_count (needed by the
    //                     "abnormal pulse" DSL save; costs one extra scan)
    std::vector<PpsResult> decode(
            data::LogicSnapshot *snapshot,
            uint64_t sample_count,
            uint64_t sample_rate,
            uint64_t ref_trigger_sample,
            const std::vector<PpsChannel> &channels,
            bool count_edges = false);

    // Level of a channel at a sample (false=low, true=high).
    static bool level(data::LogicSnapshot *snapshot,
                      int channel_index, uint64_t sample);

    // Collect every edge of the requested direction in [0, sample_count), in
    // ascending order. Used by the calibration run, which needs the spacing
    // between many consecutive edges of one channel.
    static uint64_t find_all_edges(
            data::LogicSnapshot *snapshot,
            int channel_index,
            int wanted_edge,
            uint64_t sample_count,
            std::vector<uint64_t> &edges,
            uint64_t max_edges = 0);

private:
    // Find the nearest edge of the requested direction for one channel.
    // Returns true and sets edge_sample if found within [0, sample_count).
    bool find_nearest_edge(
            data::LogicSnapshot *snapshot,
            int channel_index,
            int wanted_edge,
            uint64_t t0,
            uint64_t sample_count,
            uint64_t &edge_sample);
};

} // namespace pps
} // namespace pv

#endif // DSVIEW_PV_PPS_PPSDECODER_H
