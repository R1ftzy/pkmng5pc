#pragma once

#include <memory>

#include "pkmn/render/backend.h"

namespace pkmn::render {

// Returns a Vulkan presenter when the renderer was built with Vulkan support,
// otherwise null. The factory in backend.cpp falls back to the Null presenter
// so callers never see a missing backend.
std::unique_ptr<Presenter> create_vulkan_presenter();

} // namespace pkmn::render
