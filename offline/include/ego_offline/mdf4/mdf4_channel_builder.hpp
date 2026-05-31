#pragma once
// MF-01: Fill ChannelDesc::data arrays from aligned pipeline output

#include "ego_offline/mdf4/mdf4_writer.hpp"

namespace ego_offline::mdf4 {

// Fills groups_[].channels[].data from samples / scene_geom / step_metrics.
// groups must already be populated by Mdf4Writer::prepare() (structure only).
void fill_channel_data(
    std::vector<ChannelGroup>&                       groups,
    const std::vector<align::AlignedSample>&         samples,
    const std::vector<SceneGeometry>&                scene_geom,
    const std::vector<scene::StepMetrics>&           step_metrics,
    ns_t                                             t0_ns);

} // namespace ego_offline::mdf4
