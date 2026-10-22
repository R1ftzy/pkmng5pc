# pkmng5pc

A native PC reimplementation of the Generation V Pokémon games, driven by a
user-supplied ROM and built on statically recompiled game code plus a hardware
layer.

## Language

**Static recompilation**:
Translating a program's machine code ahead of time into native C++ that is
compiled and linked like any other code. The bulk of the guest's code runs as
translated native code; the guest's instructions are not interpreted on the
normal path, but the runner keeps a bounded interpreter as its correctness
floor (**Tier 3**).
_Avoid_: emulation, transpilation

**Recompiler**:
The tool that performs static recompilation, turning a ROM's ARM9/ARM7/overlay
code into native translation units.
_Avoid_: engine

**Runner**:
The runtime that hosts recompiled code, supplying the memory map, CPU services,
devices, and present loop those translated units expect. It hosts translated
banks and a bounded interpreter (**Tier 3**) that runs only where no bank covers
a PC. It is not an emulator core: it never interprets the whole machine.
_Avoid_: emulator, engine

**Live presentation**:
Displaying the game on screen as it executes, so the player watches the port
rather than reading captured frames afterwards. Contrast the headless path,
which advances as fast as the host allows and is observed only through the
debug protocol. Live presentation does **not** imply the port keeps up with the
guest's own frame rate.
_Avoid_: realtime display, interactive mode

**Present loop**:
The live frontend's per-frame cycle: advance one guest frame, upload both
screens, present. One iteration is one presented frame, so presented frames and
elapsed guest frames are the same count.
_Avoid_: render loop

**Present clock**:
The host time source the present loop is paced against. In this port it is the
audio output queue, not a timer or a display vsync, so no audio device means no
pacing and the port runs flat out. Guest time advancing at exactly wall-clock
rate is a separate, stronger property — see **Realtime (1:1)**.
_Avoid_: frame limiter, vsync

**Realtime (1:1)**:
Emulated time advancing at the guest's own rate (59.8261 frames/second), neither
dropped nor delayed. Distinct from **live presentation**: a port can be live
while running slower or faster than 1:1.
_Avoid_: full speed, real-time display

**Tier 1**:
Execution of a statically recompiled bank function, chosen by a per-CPU
guest-PC → function dispatch table. Verifies the expected guest bytes are live
before it runs.
_Avoid_: fast path, native tier

**Tier 2**:
The dirty-RAM JIT shard: code copied into RAM at boot, recompiled on first miss.
Designed but **not implemented** in the adopted runner; deferred behind Tiers 1
and 3.
_Avoid_: JIT

**Tier 3**:
The bounded interpreter inside the runner. It executes the guest's own bytes at
a PC that no bank covers, and is the always-correct floor — it is never fed
high-level emulation. A PC's tier is decided per dispatch, not per CPU.
_Avoid_: emulator core, fallback tier

**Interpreter coverage**:
The share of executed guest instructions served by Tier 3 rather than Tier 1.
High coverage means the port is running the game's bytes rather than its
translated code.
_Avoid_: interpretation rate

**Engine**:
_Deprecated as a bare term._ It ambiguously meant both the recompiler+runner
and our own format library. Say **recompiler**, **runner**, or **ROM layer**.

**Oracle**:
An independent, known-good implementation of the same hardware (e.g. stock
melonDS), run separately to compare frames, RAM, or saves against our port.
_Avoid_: reference emulator

**ROM layer**:
Our portable library for reading a user's ROM — the NDS header, NitroFS, NARC,
overlay tables, and the Nitro compression codecs. It is an **offline** layer
(bank generation, save I/O, launcher ROM detection): the running game reads its
cartridge through the runner's emulated cart, never through this library.
Contains no game logic and ships no game content.
_Avoid_: engine, core

**Bank**:
The generated native translation unit produced for one recompiled image (the
ARM9 binary, one overlay, or the ARM7 image), generated from the user's own ROM
and never committed.
_Avoid_: module, blob

**Title bank**:
The complete set of banks for one game title; producing them needs a recompiler
configuration for that title.

**Bank group**:
The set of generated banks the runner registers together for one ROM, held in a
single directory and gated by that ROM's SHA-1.
_Avoid_: bank dir, bank set

**Live-overlay shard**:
A translation unit the runner generates from guest bytes **at run time** and
compiles with a host C compiler before loading it. The adopted runner's
alternative to banking an overlay ahead of time; it puts a compiler in the
runtime, so it is not part of the default configuration.
_Avoid_: JIT overlay, runtime bank

**ROM-at-runtime**:
The shipped program contains translated game **code** and none of the game's
**content**; the user's ROM is read at run time for data (text, scripts, models,
audio). Content lives only in the user's ROM, never in a build artifact.
_Avoid_: bundled assets

**HAL** (hardware abstraction layer):
The seam through which game logic reaches hardware devices; game logic never
touches a device or the GPU directly.

**HLE** (high-level emulation):
Implementing a device's or BIOS's behaviour directly instead of executing its
original code.

**DS GE**:
The Nintendo DS's fixed-function 3D "Geometry Engine".

**DS pipeline**:
The default rendering path: the DS GPU device models rasterize and composite
on the CPU (the 3D soft rasterizer and the 2D scanline compositor), and only
presentation happens on the host GPU.
_Avoid_: software renderer, emulator pipeline

**Sink**:
A host-GPU endpoint that replaces one of the DS's rasterization stages. A sink
consumes state captured from the guest's GPU device model and produces pixels
on the host GPU; it owns no game logic and no guest-visible state.
_Avoid_: renderer hook, capture

**2D sink**:
The sink that replaces the DS 2D scanline compositor. It consumes latched
per-scanline layer state (backgrounds, objects, windows, effects), plus the 3D
surface, and produces the final screen image.
_Avoid_: 2D renderer, compositor

**3D sink**:
The sink that replaces the DS GE's rasterizer. It consumes the already-decoded
geometry (transformed vertices, polygons, latched render state) and produces
the per-line 3D surface. Geometry decode stays in the device model.
_Avoid_: GE emulator, 3D renderer

**Native render path**:
The opt-in, alpha rendering mode in which the 2D and 3D sinks rasterize on the
host GPU. Off by default; the DS pipeline remains the reference and fallback.
_Avoid_: GPU mode, hardware renderer

**HD split**:
The existing host-side split in which the CPU 2D compositor emits a
`NdsGpu2dHdFrame` with the 3D layer removed and the presenter re-inserts the
hi-res 3D surface using the DS's priority/order and blend rules. The first
native-render slice reuses it; the 2D sink later replaces the CPU compositor
with a GPU one.
_Avoid_: HD mode, upscale path

**Scene widening**:
The widescreen policy that widens the 3D frustum and samples 2D backgrounds
beyond the DS's 256-pixel clip, while keeping authored UI inside a 4:3 safe
area. Distinct from stretching and from pillarboxing.
_Avoid_: widescreen hack, stretch

**Safe area**:
The 4:3 region of a widened frame in which the game's authored UI and gameplay
framing are preserved.
