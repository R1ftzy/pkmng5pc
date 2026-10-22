# patches/ndsrecomp/ — local modifications to the pinned submodule

`third_party/ndsrecomp` is a pinned git submodule (tag `v0.6.10`) that this
project does not own. These `*.patch` files are the only modifications we make
to it, and they are **applied at configure time** by
`cmake/NdsRecompPatches.cmake::pkmng5pc_apply_ndsrecomp_patches()` —
idempotently, when `PKMNG5PC_NATIVE_RENDERER=ON`, or when
`PKMNG5PC_BUILD_RUNNER=ON` and `PKMNG5PC_NATIVE_PRESENT=ON` (its default).

Applying a patch leaves the submodule checkout dirty (`git status` in the
superproject reports `modified content`). That is intended and reversible:

```sh
git -C third_party/ndsrecomp status          # what is applied
git -C third_party/ndsrecomp checkout .      # revert, then re-configure
```

Configure detects a reverted submodule (`git apply --check` succeeds) and
re-applies; it detects an already-applied one (`git apply --check --reverse`
succeeds) and leaves it alone. If neither direction applies, configure fails
with the restore command rather than guessing, because that means the submodule
moved or carries unrelated edits.

This is the same convention ndsrecomp itself uses for its vendored melonDS tree
(`third_party/ndsrecomp/runner/vendor/melonds/patches/README.md`): the patch
files stay in-tree as the GPLv3 §5(a) record of what changed, and the change is
kept as small as the seam allows.

Every hook added here must be **compiled out** unless the port's build defines
its guard macro, so the patched tree still builds standalone as plain
ndsrecomp. `nds_runner` gets the guard macro only from `cmake/Runner.cmake`.

## 0001-native-present-hook.patch

`runner/src/frontend.cpp`: lets `tools/play -Native` present the two DS screens
through this project's own presenter (`render/`) instead of the frontend's
fixed 512×768 SDL blit. Recorded in
`docs/adr/0006-ndsrecomp-native-present-hook.md`.

Six small sites, all behind `#if defined(PKMNG5PC_NATIVE_PRESENT)`:

1. Forward declarations of the bridge (`render/src/native_present.cpp`).
2. `create_presentation()`: force the stacked single-window layout, create that
   window with `SDL_WINDOW_VULKAN`, and skip the SDL renderer/texture setup —
   the presenter owns those. Window ids are still registered so the existing
   input and touch plumbing is untouched.
3. `convert_mouse_event_to_logical_coordinates()`: ask the presenter for the
   window-to-logical conversion instead of `SDL_RenderCoordinatesFromWindow`,
   which needs the renderer this path does not create. The presenter inverts the
   same placement rule it composed with, so the touchscreen is clickable exactly
   where it is drawn.
4. `present_screens()`: forward the raw surfaces to the bridge and return,
   before any SDL work.
5. `nds_run_interactive_frontend()`: bring the presenter up after the window
   exists, and fail loudly if it cannot.
6. The same function's teardown: shut the presenter down before the window is
   destroyed.

Nothing above changes guest execution, input, audio, pacing or the
presented-frame digest ring: it is presentation, plus the pointer-coordinate
conversion the replaced renderer used to perform.

## 0002-libslirp-msvc-clang.patch

`runner/vendor/melonds/net/libslirp/src/`: two upstream portability bugs in the
vendored libslirp. With MSVC/UCRT headers (an unsupported toolchain) the POSIX
names are not declared, and `-Wimplicit-function-declaration` is an error under
C99 and later, so the `slirp` target does not compile:

1. `tcp_subr.c`, `tcp_connect()`: the accepted socket in the "guest address not
   available yet" path is closed with `close(s)`. Every other socket close in
   the tree goes through `closesocket`, which `util.h` maps to the Winsock
   wrapper on Windows; `close` on a socket is wrong there (and warns on POSIX).
2. `tftp.c`: the read-only TFTP server works on POSIX file descriptors
   (`open`/`lseek`/`read`/`close`). Under `_WIN32` they are aliased to the
   underscored UCRT forms from `<io.h>` (`_open`/`_lseek`/`_read`/`_close`)
   rather than relying on the deprecated non-ANSI aliases, which UCRT does not
   declare for a `-std=gnu11` translation unit.

Both changes are what upstream would take: they are correct on POSIX as well,
and they need no guard macro. The vendored tree is GPL-3.0 through the melonDS
GPL subtree, so the patch carries the same §5(a) purpose as 0001.

## 0005-melonds-net-win32.patch

`runner/vendor/melonds/`: the vendored melonDS Wi-Fi sources select their
Windows branches with `__WIN32__`, a MinGW/Borland macro that plain LLVM Clang
targeting MSVC does not define. Under the MSVC/UCRT headers they take the POSIX
branch and fail on `<poll.h>`, `<sys/socket.h>` and `<netdb.h>`;
`Net_Slirp.h`/`Net_Slirp.cpp` also declare the slirp send callback's return
type as bare `ssize_t`, which those headers do not provide.

The patch switches the guards in `Net_Slirp.h`, `Net_Slirp.cpp`, `WifiAP.cpp`
and `Net_PCap.cpp` to `_WIN32`, and uses libslirp's own `slirp_ssize_t` for the
callback. `_WIN32` is defined by both MinGW and MSVC-target Clang and is absent
on POSIX, so the branch selection stays correct everywhere; `slirp_ssize_t` is
`SSIZE_T` on Windows and `ssize_t` on POSIX. `Net_PCap.cpp` is only compiled
when the optional PCAP backend is enabled, but its guard has the same bug. No
guard macro is needed, and the patched tree still builds standalone as plain
ndsrecomp.
