#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The single seam between the port and the host GPU (`AGENTS.md` §5): nothing
// above this header may name Vulkan/D3D/GL, and nothing below it may name the
// game. A `Presenter` owns a device and one output surface; the port hands it
// a finished, laid-out canvas and it puts pixels on the screen.
//
// The first backend is Vulkan and is deliberately shader-free: the canvas is
// uploaded through a staging buffer and blitted into the swapchain image, so
// the seam can be exercised (and the DS's CPU-side 2D/3D output shown)
// before any shader-based compositing exists. See
// `docs/adr/0005-vulkan-first-renderer.md`.

namespace pkmn::render {

enum class BackendKind {
    Null,
    Vulkan,
};

const char* to_string(BackendKind kind);

// Native DS screen size, canonical internal resolution.
inline constexpr uint32_t kScreenWidth = 256;
inline constexpr uint32_t kScreenHeight = 192;

// Host window, opaque so this header carries no SDL/Win32/Xlib types. The
// caller (the frontend) owns the window's lifetime.
struct Window {
    void* sdl_window = nullptr; // SDL_Window*
};

struct Config {
    BackendKind kind = BackendKind::Vulkan;
    // FIFO present (true) vs. lowest-latency available mode (false).
    bool vsync = true;
    // Request the Vulkan validation layer when it is installed.
    bool validation = false;
    // Vulkan device preference. `auto` (or empty) prefers a discrete GPU and
    // then falls back by device type; `discrete` and `integrated` restrict the
    // choice; `index:<n>` selects the nth physical device; any other value is
    // a case-insensitive substring of the device name.
    std::string gpu = "auto";
};

// Where a presented canvas lands in the output surface, in output pixels.
// The presenter clears the rest of the surface to the letterbox colour, so a
// native-resolution canvas can be presented into a larger window without a
// CPU-side full-window composite.
struct PresentTarget {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;

    bool empty() const { return width == 0 || height == 0; }
};

// BGRA8 bytes: B=16, G=12, R=12, A=255. The compositor and the presenter use
// the same value for the bars around the DS image.
inline constexpr uint32_t kLetterboxBgra = 0xFF0C0C10;

// A BGRA8 (8-bit per channel, sRGB-encoded optional) canvas ready to present.
// `pitch` is in bytes; `width * 4` when tightly packed. This is a non-owning
// view; the owner keeps the storage alive for the duration of present().
struct Canvas {
    const uint8_t* data = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
};

// Owning BGRA8 canvas, tightly packed. Returned by the CPU compositor.
struct CanvasBuffer {
    std::vector<uint8_t> bytes;
    uint32_t width = 0;
    uint32_t height = 0;

    Canvas view() const { return Canvas { bytes.data(), width, height, width * 4 }; }
};

class Presenter {
public:
    virtual ~Presenter() = default;

    // Create the device and output surface for `window`. Must be called on the
    // thread that owns the window. Returns false and leaves no device behind
    // on failure, so the caller can fall back to another backend.
    virtual bool init(const Window& window, const Config& config) = 0;

    virtual void shutdown() = 0;

    // Recreate the swapchain at this output size. Called on window resize and
    // after an out-of-date present. The canvas passed to present() may be any
    // size; it is scaled by the GPU into the target rectangle.
    virtual bool resize(uint32_t width, uint32_t height) = 0;

    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;

    // Present a canvas into `target` (output pixels). The target must lie
    // inside the output surface; everything outside it is cleared to the
    // letterbox colour and the canvas is scaled to the target with nearest
    // filtering. `canvas` is a tightly packed or pitched BGRA8 view.
    // Returns false on a recoverable failure (the caller resizes and retries);
    // a surface that cannot be recreated is reported by the next call.
    virtual bool present(const Canvas& canvas, const PresentTarget& target) = 0;

    // Present the canvas over the whole output surface (stretch to fit).
    bool present(const Canvas& canvas)
    {
        return present(canvas, PresentTarget { 0, 0, width(), height() });
    }

    virtual BackendKind kind() const = 0;
};

// A physical GPU that can host the Vulkan backend. `memory_bytes` is the sum
// of device-local heaps (VRAM for a discrete card, shared memory otherwise).
enum class GpuType {
    Other,
    Integrated,
    Discrete,
    Virtual,
    Cpu,
};

const char* to_string(GpuType type);

struct GpuInfo {
    uint32_t index = 0;
    std::string name;
    GpuType type = GpuType::Other;
    uint32_t api_version = 0;
    uint32_t driver_version = 0;
    uint64_t memory_bytes = 0;
};

// Enumerate Vulkan physical devices without creating a device or a window.
// Returns an empty list when no Vulkan loader or no device is available.
std::vector<GpuInfo> enumerate_gpus();

// Factory. The returned object is never null; an unavailable backend degrades
// to the Null presenter rather than failing construction (init() reports
// whether a real device came up).
std::unique_ptr<Presenter> create_presenter(BackendKind kind);

// CPU layout/compositor: pack the two native BGR555 DS screens into one BGRA8
// canvas of the given output size. This is the single home for layout
// (`Plan.md` M8): stacked (top over bottom) or side-by-side (left/right),
// centered with a letterbox in the unused area. `top`/`bottom` are 256x192
// BGR555 rows, tightly packed.
enum class ScreenLayout {
    Stacked,
    SideBySide,
};

CanvasBuffer compose_screens(const uint16_t* top, const uint16_t* bottom, ScreenLayout layout,
    uint32_t output_width, uint32_t output_height);

// Same layout, over screens that are already BGRA8. This is the form the
// runner's frontend hands out (`nds_gpu2d_framebuffer()` is ARGB8888, i.e.
// BGRA8 in memory), so the bridge into the frontend needs no per-pixel
// conversion. `*_width` may exceed the native 256 (adaptive wide output);
// height is always the DS screen height. Both the placement rule and the
// letterbox come from the same implementation `compose_screens()` uses, so
// layout still lives in exactly one place.
CanvasBuffer compose_screens_bgra8(const uint8_t* top, uint32_t top_width, const uint8_t* bottom,
    uint32_t bottom_width, ScreenLayout layout, uint32_t output_width, uint32_t output_height);

// In-place variants that reuse the canvas storage when its size has not
// changed. The native presenter keeps one canvas for the life of the run, so
// resizing a window no longer reallocates or recomposes at window size.
void compose_screens_bgra8_into(CanvasBuffer& canvas, const uint8_t* top, uint32_t top_width,
    const uint8_t* bottom, uint32_t bottom_width, ScreenLayout layout, uint32_t output_width,
    uint32_t output_height);

// One screen, letterboxed into an output canvas. Used by the native two-window
// layout, where each window owns one screen.
CanvasBuffer compose_screen_bgra8(
    const uint8_t* screen, uint32_t width, uint32_t output_width, uint32_t output_height);
void compose_screen_bgra8_into(CanvasBuffer& canvas, const uint8_t* screen, uint32_t width,
    uint32_t output_width, uint32_t output_height);

// Where the composed two-screen stack lands in the output canvas: the integer
// scale, and the centred, letterboxed rectangle both screens are drawn inside.
// Composition and the frontend's pointer mapping both read this, so the
// inverse of the layout is never a second, drifting copy of the rule.
struct StackPlacement {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t scale = 1;
    // False when the output is too small to show the stack at all.
    bool visible = false;
};

StackPlacement stack_placement(uint32_t top_width, uint32_t bottom_width, ScreenLayout layout,
    uint32_t output_width, uint32_t output_height);

// The same integer-scale placement rule for one source rectangle. The native
// separate layout uses it to size each screen's window target.
StackPlacement fit_placement(
    uint32_t source_width, uint32_t source_height, uint32_t output_width, uint32_t output_height);

// Expose the BGR555 -> BGRA8 conversion for the compositor and its tests.
void bgr555_to_bgra8(const uint16_t* src, uint8_t* dst, uint32_t pixels);

} // namespace pkmn::render
