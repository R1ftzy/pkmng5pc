#include "pkmn/render/backend.h"

#include "vulkan_presenter.h"

namespace pkmn::render {

const char* to_string(BackendKind kind)
{
    switch (kind) {
    case BackendKind::Null:
        return "null";
    case BackendKind::Vulkan:
        return "vulkan";
    }
    return "unknown";
}

namespace {

    // A do-nothing device. It is the documented fallback when no GPU backend can
    // be brought up: the port still boots and steps guest logic, it just does not
    // put pixels on screen. Never returned for a successful Vulkan init.
    class NullPresenter final : public Presenter {
    public:
        bool init(const Window&, const Config&) override { return true; }
        void shutdown() override { }
        bool resize(uint32_t width, uint32_t height) override
        {
            width_ = width;
            height_ = height;
            return true;
        }
        uint32_t width() const override { return width_; }
        uint32_t height() const override { return height_; }
        bool present(const Canvas&) override { return true; }
        BackendKind kind() const override { return BackendKind::Null; }

    private:
        uint32_t width_ = 0;
        uint32_t height_ = 0;
    };

} // namespace

std::unique_ptr<Presenter> create_presenter(BackendKind kind)
{
    if (kind == BackendKind::Vulkan) {
        if (std::unique_ptr<Presenter> vulkan = create_vulkan_presenter()) {
            return vulkan;
        }
    }
    return std::make_unique<NullPresenter>();
}

} // namespace pkmn::render
