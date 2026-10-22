#pragma once

namespace pkmn::render::ds {

// The native 3D sink owns rasterization only while its GPU resources are live
// and the guest is not consuming pixels through a CPU-exact path. `capture`
// covers display capture, whose VRAM bytes must match the reference renderer;
// `failed` keeps a broken sink from presenting suspect pixels for the rest of
// the run.
struct SinkFrameState {
    bool initialized = false;
    bool failed = false;
    bool capture = false;
};

constexpr bool sink_frame_needs_cpu(const SinkFrameState& state)
{
    return !state.initialized || state.failed || state.capture;
}

} // namespace pkmn::render::ds
