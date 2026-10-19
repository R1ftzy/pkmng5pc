#if !defined(PKMN_RENDER_VULKAN)

#include "vulkan_presenter.h"

namespace pkmn::render {

// The renderer was built without Vulkan support; the factory falls back to the
// Null presenter. Kept as a separate translation unit so the Vulkan sources
// stay out of non-Vulkan builds without scattering #ifdefs.
std::unique_ptr<Presenter> create_vulkan_presenter() { return nullptr; }

} // namespace pkmn::render

#endif // !PKMN_RENDER_VULKAN
