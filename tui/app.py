"""Interactive launcher screens, navigation and actions."""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

from tui.commands import (
    Command,
    Job,
    LaunchError,
    bankgen_command,
    bootstrap_command,
    build_command,
    configure_and_build_commands,
    configure_command,
    play_command,
    quickstart_commands,
    save_label,
    test_command,
)
from tui.model import (
    PLAY_LAYOUTS,
    SAVE_MODES,
    Repo,
    RomInfo,
    Settings,
    cache_bool,
    default_bank_out,
    default_overlay_seeds,
    default_title_config,
    describe_size,
    find_cmake,
    find_ctest,
    platform_label,
    read_rom_info,
    same_path,
    scan_roms,
    sha1_file,
    split_paths,
    title_identity,
)
from tui.picker import Picker
from tui.terminal import (
    MouseEvent,
    Terminal,
    clip,
    make_box,
    pad_between,
    sanitize,
    strip_ansi,
    truncate_middle,
    visible_len,
    wrap_text,
)

PAGE_ORDER = ["home", "rom", "build", "play", "setup", "help"]
PAGE_NAMES = {
    "home": "Home",
    "rom": "ROM",
    "build": "Build",
    "play": "Play",
    "setup": "Setup",
    "help": "Help",
}
SELECTABLE = {"toggle", "choice", "text", "folder", "action", "rom"}
BUILDER = Callable[[], Command | list[Command]]


def column_widths(width: int) -> tuple[int, int]:
    label_width = min(30, max(17, (width - 12) // 2))
    return label_width, max(4, width - 6 - label_width)


@dataclass
class Item:
    key: str
    label: str = ""
    kind: str = "info"
    value: str = ""
    hint: str = ""
    on_activate: Callable[[], None] | None = None
    on_adjust: Callable[[int], None] | None = None
    enabled: bool = True
    preview: list[str] = field(default_factory=list)
    data: Any = None


@dataclass
class PageView:
    title: str
    subtitle: str
    items: list[Item]
    panel: str = "Preview"


@dataclass
class InputState:
    title: str
    prompt: str
    value: str
    cursor: int
    on_submit: Callable[[str], None]
    validator: Callable[[str], str | None] | None = None
    error: str = ""
    allow_empty: bool = True


@dataclass
class MessageState:
    title: str
    body: str
    kind: str = "info"


@dataclass
class ReviewState:
    title: str
    commands: list[Command]
    sections: list[tuple[str, list[str]]]
    on_success: Callable[[Job], None] | None
    on_cancel: Callable[[], None] | None = None
    scroll: int = 0


class App:
    def __init__(
        self,
        repo: Repo,
        settings: Settings,
        terminal: Terminal,
        state_path: Path,
        persist: bool = True,
    ):
        self.repo = repo
        self.settings = settings
        self.terminal = terminal
        self.state_path = state_path
        self.persist = persist
        self.page = "home"
        self.mode = "page"
        self.selected: dict[str, str] = {}
        self.scroll: dict[str, int] = {page: 0 for page in PAGE_ORDER}
        self.roms: list[RomInfo] = []
        self.picker: Picker | None = None
        self.input: InputState | None = None
        self.message: MessageState | None = None
        self.review: ReviewState | None = None
        self.job: Job | None = None
        self.last_job: Job | None = None
        self.job_success: Callable[[Job], None] | None = None
        self.tab_hits: list[tuple[int, int, int, str]] = []
        self.row_hits: dict[int, str] = {}
        self.review_hits: list[tuple[int, int, int, str]] = []
        self.log_follow = True
        self.log_back = 0
        self.toast: tuple[str, str, float] | None = None
        self.running = True
        self.dirty = True
        self.settings_dirty = False
        self.last_save = 0.0
        self.last_size = (0, 0)
        self._job_revision = 0
        self.sha1_cache: dict[tuple[str, int, int], str] = {}
        self.sha1_pending: set[tuple[str, int, int]] = set()
        self.sha1_errors: dict[tuple[str, int, int], str] = {}
        self.refresh_roms(auto=True)
        self.ensure_sha1()

    @property
    def s(self) -> Settings:
        return self.settings

    def touch(self) -> None:
        self.dirty = True
        self.settings_dirty = True

    def set_toast(self, text: str, kind: str = "info", seconds: float = 4.0) -> None:
        self.toast = (text, kind, time.monotonic() + seconds)
        self.dirty = True

    def refresh_roms(self, auto: bool = False) -> None:
        folder = self.repo.resolve(self.s.rom_dir)
        self.roms = scan_roms(folder)
        if auto and not self.s.rom_path and self.roms:
            preferred = next((info for info in self.roms if info.code == "IRBO"), None)
            if preferred is None:
                supported = [info for info in self.roms if info.supported]
                preferred = supported[0] if supported else self.roms[0]
            self.select_rom(preferred, announce=False)
        self.dirty = True

    def select_rom(self, info: RomInfo, announce: bool = True) -> None:
        self.s.rom_path = self.repo.store(info.path)
        self.s.rom_dir = self.repo.store(info.path.parent)
        self.ensure_sha1()
        if announce:
            self.set_toast(f"Selected {info.display_name}", "success")
        self.touch()

    def selected_rom(self) -> RomInfo | None:
        if not self.s.rom_path.strip():
            return None
        path = self.repo.resolve(self.s.rom_path)
        return read_rom_info(path)

    def ensure_sha1(self) -> None:
        info = self.selected_rom()
        if info is None:
            return
        try:
            stat = info.path.stat()
        except OSError:
            return
        key = (str(info.path), stat.st_size, stat.st_mtime_ns)
        if key in self.sha1_cache or key in self.sha1_pending:
            return
        self.sha1_pending.add(key)

        def work() -> None:
            try:
                self.sha1_cache[key] = sha1_file(info.path)
            except OSError as error:
                self.sha1_errors[key] = str(error)
            finally:
                self.sha1_pending.discard(key)
                self.dirty = True

        import threading

        threading.Thread(target=work, daemon=True).start()

    def sha1_key(self) -> tuple[str, int, int] | None:
        info = self.selected_rom()
        if info is None:
            return None
        try:
            stat = info.path.stat()
        except OSError:
            return None
        return (str(info.path), stat.st_size, stat.st_mtime_ns)

    def current_sha1(self) -> str | None:
        key = self.sha1_key()
        return self.sha1_cache.get(key) if key else None

    def sha1_state(self) -> tuple[str, str]:
        key = self.sha1_key()
        if key is None:
            return "none", "no ROM selected"
        if key in self.sha1_cache:
            return "ready", self.sha1_cache[key]
        if key in self.sha1_pending:
            return "pending", "hashing selected ROM…"
        if key in self.sha1_errors:
            return "error", self.sha1_errors[key]
        return "none", "not computed"

    def sha1_for_configure(self) -> str | None:
        if not self.s.rom_path.strip():
            return None
        self.ensure_sha1()
        state, value = self.sha1_state()
        if state == "ready":
            return value
        if state == "pending":
            raise LaunchError("The selected ROM is still being hashed; try again in a moment.")
        if state == "error":
            raise LaunchError(f"Could not hash the selected ROM: {value}")
        raise LaunchError("No ROM hash available; reselect the ROM and try again.")

    def set_page(self, page: str) -> None:
        if page not in PAGE_NAMES:
            return
        self.page = page
        self.mode = "page"
        self.scroll[page] = 0
        self.dirty = True

    def cycle_page(self, delta: int) -> None:
        index = PAGE_ORDER.index(self.page)
        self.set_page(PAGE_ORDER[(index + delta) % len(PAGE_ORDER)])

    def next_step(self) -> str:
        if self.selected_rom() is None:
            return "Choose a ROM on the ROM page."
        if not self.repo.bankgen_path(self.s.preset).is_file():
            return "Build the host tools: Build page -> Configure & build."
        if not self.repo.recompiler_path.is_file():
            return "Bootstrap the recompiler on the Setup page."
        if not Repo.has_banks(self.repo.resolve(self.s.banks_dir)):
            return "Generate title banks on the Setup page."
        if self.repo.find_runner(self.s.preset)[0] is None:
            return "Enable Build runner on the Build page, then Configure & build."
        return "Ready. Press Quickstart or Play now, or tune flags first."

    def cache(self) -> dict[str, str]:
        return self.repo.read_cache(self.s.preset)

    def cache_mismatches(self, cache: dict[str, str] | None = None) -> list[str]:
        cache = self.cache() if cache is None else cache
        if not cache:
            return ["build directory is not configured"]
        problems: list[str] = []
        checks = [
            ("build_tests", "PKMNG5PC_BUILD_TESTS"),
            ("build_tools", "PKMNG5PC_BUILD_TOOLS"),
            ("build_render", "PKMNG5PC_BUILD_RENDER"),
            ("build_runner", "PKMNG5PC_BUILD_RUNNER"),
            ("native_present", "PKMNG5PC_NATIVE_PRESENT"),
        ]
        for attribute, key in checks:
            cached = cache_bool(cache, key)
            if cached is not None and cached != bool(getattr(self.s, attribute)):
                problems.append(f"{key} is {'ON' if cached else 'OFF'} in the cache")
        cached_banks = cache.get("PKMNG5PC_BANKS_DIR")
        if cached_banks and not same_path(cached_banks, self.repo.resolve(self.s.banks_dir)):
            problems.append("banks directory differs from the cache")
        digest = self.current_sha1()
        cached_sha1 = cache.get("PKMNG5PC_TITLE_ROM_SHA1")
        if digest and cached_sha1 and cached_sha1 != digest:
            problems.append("title ROM SHA-1 differs from the cache")
        if self.s.extra_bank_dirs.strip() or self.s.extra_sha1s.strip():
            requested_dirs = [self.repo.resolve(path) for path in split_paths(self.s.extra_bank_dirs)]
            cached_dirs = [
                self.repo.resolve(path)
                for path in cache.get("NDS_TITLE_BANK_DIRS", "").split(";")
                if path.strip()
            ]
            if len(requested_dirs) != len(cached_dirs) or any(
                not same_path(left, right) for left, right in zip(requested_dirs, cached_dirs)
            ):
                problems.append("extra title bank dirs differ from the cache")
            requested_sha1s = split_paths(self.s.extra_sha1s)
            cached_sha1s = [
                path.strip()
                for path in cache.get("NDS_TITLE_ROM_SHA1S", "").split(";")
                if path.strip()
            ]
            if requested_sha1s != cached_sha1s:
                problems.append("extra title SHA-1s differ from the cache")
        return problems

    def _choice(
        self,
        key: str,
        label: str,
        attribute: str,
        options: list[tuple[str, str]],
        hint: str,
        on_change: Callable[[], None] | None = None,
    ) -> Item:
        values = [value for value, _ in options]
        labels = dict(options)
        current = getattr(self.s, attribute)

        def set_value(value: str) -> None:
            setattr(self.s, attribute, value)
            if on_change:
                on_change()
            self.touch()

        def adjust(delta: int) -> None:
            index = values.index(current) if current in values else 0
            set_value(values[(index + delta) % len(values)])

        return Item(
            key,
            label,
            kind="choice",
            value=labels.get(current, current),
            hint=hint,
            on_activate=lambda: adjust(1),
            on_adjust=adjust,
        )

    def _toggle(
        self,
        key: str,
        label: str,
        attribute: str,
        hint: str,
        enabled: bool = True,
    ) -> Item:
        value = bool(getattr(self.s, attribute))

        def adjust(delta: int) -> None:
            setattr(self.s, attribute, delta > 0)
            self.touch()

        def activate() -> None:
            if enabled:
                adjust(not value)
            else:
                self.set_toast(hint, "warning")

        return Item(
            key,
            label,
            kind="toggle",
            value="ON" if value else "OFF",
            hint=hint,
            enabled=enabled,
            on_activate=activate,
            on_adjust=adjust if enabled else None,
        )

    def _text(
        self,
        key: str,
        label: str,
        getter: Callable[[], str],
        setter: Callable[[str], None],
        prompt: str,
        hint: str,
        validator: Callable[[str], str | None] | None = None,
        empty_label: str = "(not set)",
    ) -> Item:
        value = getter()

        def activate() -> None:
            self.open_input(prompt, value, setter, validator=validator, allow_empty=True)

        return Item(
            key,
            label,
            kind="text",
            value=value if value else empty_label,
            hint=hint,
            on_activate=activate,
        )

    def _folder(
        self,
        key: str,
        label: str,
        getter: Callable[[], str],
        setter: Callable[[str], None],
        prompt: str,
        hint: str,
    ) -> Item:
        value = getter()

        def activate() -> None:
            start = self.repo.resolve(value) if value else self.repo.root
            self.open_folder_picker(prompt, start, setter)

        return Item(
            key,
            label,
            kind="folder",
            value=value if value else "(not set)",
            hint=hint,
            on_activate=activate,
        )

    def _action(self, key: str, label: str, builder: BUILDER, hint: str, on_success=None) -> Item:
        def activate() -> None:
            self.run_or_message(builder, label, on_success)

        return Item(
            key,
            label,
            kind="action",
            hint=hint,
            preview=self.preview_commands(builder),
            on_activate=activate,
        )

    def _nav(self, key: str, label: str, page: str, hint: str) -> Item:
        return Item(
            key,
            label,
            kind="action",
            hint=hint,
            on_activate=lambda: self.set_page(page),
        )

    def preview_commands(self, builder: BUILDER) -> list[str]:
        try:
            result = builder()
            commands = result if isinstance(result, list) else [result]
        except (LaunchError, OSError, ValueError) as error:
            return [f"! {error}"]
        lines: list[str] = []
        for command in commands:
            lines.append(f"$ {command.preview()}")
            if command.note:
                lines.append(command.note)
        return lines

    def run_or_message(self, builder: BUILDER, title: str, on_success=None) -> None:
        try:
            result = builder()
            commands = result if isinstance(result, list) else [result]
            if not commands:
                raise LaunchError("Nothing to run.")
            self.show_review(title, commands, on_success)
        except (LaunchError, OSError, ValueError) as error:
            self.show_message("Cannot start", str(error), "error")

    def quickstart_action(self) -> None:
        info = self.selected_rom()
        if info is None:
            self.show_message(
                "Quickstart",
                "Choose a ROM on the ROM page first. Quickstart builds the missing pieces and then plays it.",
                "warning",
            )
            return
        previous = (
            self.s.banks_dir,
            self.s.emit_only,
            self.s.build_runner,
            self.s.build_tools,
            self.s.build_render,
        )
        banks_missing = not Repo.has_banks(self.repo.resolve(self.s.banks_dir))
        if banks_missing:
            output = self.s.bank_out.strip() or default_bank_out(info)
            self.s.banks_dir = self.repo.store(self.repo.resolve(output))
            self.s.emit_only = False
            if not self.repo.bankgen_path(self.s.preset).is_file():
                self.s.build_tools = True
        runner, _ = self.repo.find_runner(self.s.preset)
        if banks_missing or runner is None:
            self.s.build_runner = True
            if self.s.native_present and not self.s.build_render:
                self.s.build_render = True
        self.touch()

        def restore() -> None:
            (
                self.s.banks_dir,
                self.s.emit_only,
                self.s.build_runner,
                self.s.build_tools,
                self.s.build_render,
            ) = previous
            self.touch()

        try:
            sha1 = self.current_sha1()
            commands = quickstart_commands(self.repo, self.s, sha1)
            if any(command.title == "Configure" for command in commands):
                commands = quickstart_commands(
                    self.repo, self.s, self.sha1_for_configure()
                )
        except (LaunchError, OSError, ValueError) as error:
            restore()
            self.show_message("Cannot start", str(error), "error")
            return
        if not commands:
            restore()
            self.show_message("Cannot start", "Nothing to run.", "error")
            return
        self.show_review(
            "Quickstart", commands, self.quickstart_finished, on_cancel=restore
        )

    def quickstart_finished(self, job: Job) -> None:
        self.set_toast("Quickstart finished", "success", seconds=7)

    def show_review(
        self,
        title: str,
        commands: list[Command],
        on_success: Callable[[Job], None] | None,
        on_cancel: Callable[[], None] | None = None,
    ) -> None:
        self.review = ReviewState(
            title=title,
            commands=commands,
            sections=self.review_sections(title, commands),
            on_success=on_success,
            on_cancel=on_cancel,
        )
        self.mode = "review"
        self.dirty = True

    def review_sections(self, title: str, commands: list[Command]) -> list[tuple[str, list[str]]]:
        info = self.selected_rom()
        banks = self.repo.resolve(self.s.banks_dir)
        bank_count = len(Repo.bank_files(banks))
        bankgen = self.repo.bankgen_path(self.s.preset)
        runner, runner_preset = self.repo.find_runner(self.s.preset)
        recompiler = self.repo.recompiler_path
        sha_state, sha_value = self.sha1_state()
        cache = self.cache()
        mismatches = self.cache_mismatches(cache)
        cache_state = (
            "not configured"
            if not cache
            else "in sync"
            if not mismatches
            else "configure needed"
        )
        default_config = default_title_config(info)
        default_seeds = default_overlay_seeds(info)
        on_off = lambda value: "ON" if value else "OFF"
        extra_dirs = self.s.extra_bank_dirs.strip() or "(none)"
        extra_sha1s = self.s.extra_sha1s.strip() or "(none)"
        runner_label = (
            self.repo.display(runner) + f"  ({runner_preset})" if runner else "missing"
        )
        play_runner_label = (
            self.repo.display(runner) + f"  ({runner_preset})"
            if runner
            else "will be built by this run"
        )
        if not info:
            sha_label = "(none selected)"
        elif sha_state == "ready":
            sha_label = sha_value
        elif sha_state == "pending":
            sha_label = "still hashing"
        elif sha_state == "error":
            sha_label = f"unavailable ({sha_value})"
        else:
            sha_label = sha_value

        rom_rows = [
            f"ROM: {self.repo.display(info.path) if info else '(none selected)'}",
            f"ROM folder: {self.s.rom_dir or '(not set)'}",
        ]
        if info:
            rom_rows.extend(
                [
                    f"Title: {info.display_name}  ·  code {info.code or '????'}  ·  {describe_size(info.size)}",
                    f"Default save: {self.repo.display(info.save_path)}",
                ]
            )
        rom_rows.append("SHA-1: " + sha_label)

        build_rows = [
            f"Preset: {self.s.preset or '(none)'}",
            f"Build tests: {on_off(self.s.build_tests)}  ·  host tools: {on_off(self.s.build_tools)}",
            f"Build render: {on_off(self.s.build_render)}  ·  runner: {on_off(self.s.build_runner)}",
            f"Native present: {on_off(self.s.native_present)}",
            f"Banks directory: {self.repo.display(banks)}  ({bank_count} shard files)",
            f"Extra bank dirs: {extra_dirs}",
            f"Extra SHA-1s: {extra_sha1s}",
            f"Build directory: {self.repo.display(self.repo.build_dir(self.s.preset))}",
            f"CMake cache: {cache_state}",
        ]
        if mismatches:
            build_rows.append("Cache notes: " + "; ".join(mismatches))
        build_rows.extend(
            [
                f"bankgen: {self.repo.display(bankgen)}  ({'found' if bankgen.is_file() else 'missing'})",
                f"Runner: {runner_label}",
                f"Recompiler: {self.repo.display(recompiler)}  ({'found' if recompiler.is_file() else 'missing'})",
                f"CMake: {find_cmake() or '(not found)'}",
                f"CTest: {find_ctest() or '(not found)'}",
            ]
        )

        bank_rows = [
            f"Output: {self.s.bank_out or default_bank_out(info)}",
            f"Title config: {self.s.title_config or default_config or '(none)'}",
            f"Overlay seeds: {self.s.overlay_seeds or default_seeds or '(none)'}",
            f"Shards: {self.s.shards}  ·  overlays: {on_off(self.s.overlays)}  ·  emit only: {on_off(self.s.emit_only)}",
        ]

        bootstrap_rows = [
            f"Standalone runner: {on_off(self.s.bootstrap_runner)}  ·  SDL3: {on_off(self.s.bootstrap_sdl3)}",
            f"Compiler: {self.s.bootstrap_compiler.strip() or '(script default)'}",
        ]

        play_rows = [
            f"Layout: {self.s.play_layout}  ·  native present: {on_off(self.s.play_native)}",
            f"Frame limit: {self.s.frames if self.s.frames else '0 (until closed)'}",
            f"Save mode: {self.s.save_mode}  ·  resolved save: {save_label(self.s, self.repo, info)}",
            f"Stats: {on_off(self.s.stats)}  ·  coverage: {on_off(self.s.coverage)}  ·  diagnostics: {on_off(self.s.diagnostics)}",
            f"Extra args: {self.s.extra_args.strip() or '(none)'}",
            f"Runner used: {play_runner_label}",
        ]

        action_rows = [f"Action: {title}", f"State file: {self.repo.display(self.state_path)}"]
        if title == "Quickstart":
            action_rows.append(
                "Quickstart may update Banks directory, Emit only, Build host tools, Build render, and Build runner; cancelling restores them."
            )

        command_rows: list[str] = []
        for index, command in enumerate(commands, start=1):
            command_rows.append(f"{index}. $ {command.preview()}")
            command_rows.append(f"   cwd: {self.repo.display(command.cwd)}")
            if command.note:
                command_rows.append(f"   note: {command.note}")

        warning_rows = [
            "This is the final check. Enter or click Run starts every step above in order.",
            "Esc or click Cancel returns without starting anything.",
            "Build and bank generation can take several minutes. Output stays local and git-ignored.",
        ]

        return [
            ("ACTION", action_rows),
            ("ROM", rom_rows),
            ("BUILD", build_rows),
            ("TITLE BANKS", bank_rows),
            ("BOOTSTRAP", bootstrap_rows),
            ("PLAY", play_rows),
            ("COMMANDS", command_rows),
            ("FINAL CHECK", warning_rows),
        ]

    def start_job(
        self, commands: list[Command], title: str, on_success: Callable[[Job], None] | None = None
    ) -> None:
        self.job = Job(commands, title)
        self.last_job = self.job
        self.job_success = on_success
        self.mode = "job"
        self.log_follow = True
        self.log_back = 0
        self._job_revision = 0
        self.job.start()
        self.dirty = True

    def open_input(
        self,
        title: str,
        initial: str,
        on_submit: Callable[[str], None],
        validator: Callable[[str], str | None] | None = None,
        allow_empty: bool = True,
    ) -> None:
        self.input = InputState(
            title=title,
            prompt=title,
            value=initial,
            cursor=len(initial),
            on_submit=on_submit,
            validator=validator,
            allow_empty=allow_empty,
        )
        self.mode = "input"
        self.dirty = True

    def open_folder_picker(self, title: str, start: Path, setter: Callable[[str], None]) -> None:
        def picked(path: Path) -> None:
            setter(self.repo.store(path))
            self.touch()
            self.set_toast(f"Set to {self.repo.store(path)}", "success")

        if not start.is_dir():
            start = start.parent if start.parent.is_dir() else self.repo.root
        self.picker = Picker(self.terminal, start, "folder", title, picked)
        self.mode = "picker"
        self.dirty = True

    def open_rom_picker(self) -> None:
        start = self.repo.resolve(self.s.rom_dir)
        if self.s.rom_path.strip():
            candidate = self.repo.resolve(self.s.rom_path).parent
            if candidate.is_dir():
                start = candidate
        if not start.is_dir():
            start = self.repo.root

        def picked(path: Path) -> None:
            info = read_rom_info(path)
            self.s.rom_path = self.repo.store(path)
            self.s.rom_dir = self.repo.store(path.parent)
            self.ensure_sha1()
            self.set_toast(
                f"Selected {info.display_name}" if info else f"Selected {path.name}", "success"
            )
            self.touch()

        self.picker = Picker(self.terminal, start, "file", "Choose ROM file", picked)
        self.mode = "picker"
        self.dirty = True

    def show_message(self, title: str, body: str, kind: str = "info") -> None:
        self.message = MessageState(title, body, kind)
        self.mode = "message"
        self.dirty = True

    def run(self) -> None:
        with self.terminal:
            while self.running:
                self.poll()
                self.draw()
                timeout = 0.08 if self.active_job else 0.4
                key = self.terminal.get_key(timeout)
                if isinstance(key, MouseEvent):
                    self.handle_mouse(key)
                elif key is not None:
                    self.handle_key(key)
                self.maybe_save()

    @property
    def active_job(self) -> Job | None:
        return self.job if self.job and self.job.active else None

    def poll(self) -> None:
        now = time.monotonic()
        if self.toast and now >= self.toast[2]:
            self.toast = None
            self.dirty = True
        if self.job:
            if self.job.revision != self._job_revision:
                self._job_revision = self.job.revision
                self.dirty = True
            if self.job.status != "running" and not self.job.consumed:
                self.job.consumed = True
                self.last_job = self.job
                if self.job.status == "done" and self.job_success:
                    callback = self.job_success
                    self.job_success = None
                    try:
                        callback(self.job)
                    except Exception as error:
                        self.show_message("Post-run step failed", str(error), "error")
                self.dirty = True
            if self.job.active:
                self.dirty = True
        size = self.terminal.size()
        if size != self.last_size:
            self.last_size = size
            self.dirty = True

    def maybe_save(self) -> None:
        if not self.persist or not self.settings_dirty:
            return
        if self.review is not None and self.review.on_cancel is not None:
            return
        now = time.monotonic()
        if now - self.last_save < 0.75:
            return
        try:
            self.s.save(self.state_path)
            self.last_save = now
            self.settings_dirty = False
        except OSError:
            pass

    def draw(self) -> None:
        if not self.dirty:
            return
        width, height = self.terminal.size()
        self.terminal.draw(self.render(width, height))
        self.dirty = False

    def render(self, width: int | None = None, height: int | None = None) -> list[str]:
        width, height = width or self.terminal.size()[0], height or self.terminal.size()[1]
        self.tab_hits.clear()
        self.row_hits.clear()
        self.review_hits.clear()
        if width < 60 or height < 18:
            return self.render_too_small(width, height)
        if self.mode == "picker" and self.picker:
            return self.picker.draw(width, height)
        if self.mode == "input" and self.input:
            return self.render_input(width, height)
        if self.mode == "message" and self.message:
            return self.render_message(width, height)
        if self.mode == "review" and self.review:
            return self.render_review(width, height)
        if self.mode == "job" and self.job:
            return self.render_job(width, height)
        return self.render_page(width, height)

    def render_too_small(self, width: int, height: int) -> list[str]:
        lines = ["", "", "  pkmng5pc launcher", "", "  Please resize the terminal to at least 60 x 18."]
        while len(lines) < height:
            lines.append("")
        return lines[:height]

    def render_chrome(self, width: int) -> list[str]:
        term = self.terminal
        left = "  " + term.bold(term.cyan("pkmng5pc")) + term.dim("  launcher")
        right = term.dim(f"{platform_label()} · {self.s.preset or 'no preset'}")
        if visible_len(right) > width // 2:
            right = ""
        lines = [pad_between(left, right, width), "  " + term.dim(term.rule(width - 4))]
        tabs: list[str] = []
        x = 3
        for index, page in enumerate(PAGE_ORDER):
            label = f" {index + 1} {PAGE_NAMES[page]} "
            if page == self.page:
                if term.color:
                    tab = term.style(label, color="black", background="cyan", bold=True)
                else:
                    tab = f"[{label.strip()}]"
            else:
                tab = term.dim(label)
            width_here = visible_len(tab)
            self.tab_hits.append((3, x, x + width_here - 1, page))
            x += width_here + 1
            tabs.append(tab)
        lines.append(clip("  " + " ".join(tabs), width))
        lines.append("  " + term.dim(term.rule(width - 4)))
        return lines

    def page_view(self, width: int) -> PageView:
        if self.page == "rom":
            return self.rom_view(width)
        if self.page == "build":
            return self.build_view(width)
        if self.page == "play":
            return self.play_view(width)
        if self.page == "setup":
            return self.setup_view(width)
        if self.page == "help":
            return self.help_view(width)
        return self.home_view(width)

    def rom_status(self) -> tuple[str, str, str]:
        info = self.selected_rom()
        if info is None:
            if self.s.rom_path.strip():
                path = self.repo.resolve(self.s.rom_path)
                return "error", "missing", str(path)
            return "warning", "not selected", "Choose a ROM to enable the other pages."
        if info.path.is_file():
            code = f" · {info.code}" if info.code else ""
            return "ok", f"{info.display_name}{code}", str(info.path)
        return "error", info.display_name, str(info.path)

    def home_view(self, width: int) -> PageView:
        term = self.terminal
        rom_state, rom_value, rom_hint = self.rom_status()
        banks = self.repo.resolve(self.s.banks_dir)
        bank_count = len(Repo.bank_files(banks))
        runner, runner_preset = self.repo.find_runner(self.s.preset)
        recompiler = self.repo.recompiler_path
        rom_color = {"ok": term.green, "warning": term.yellow, "error": term.red}[rom_state]
        items = [
            Item("next", "", kind="notice", value=term.cyan(self.next_step())),
            Item("status", "STATUS", kind="section"),
            Item("rom", "ROM", kind="info", value=rom_color(rom_value), hint=rom_hint),
            Item("folder", "ROM folder", kind="info", value=self.s.rom_dir),
            Item("preset", "Build preset", kind="info", value=self.s.preset),
            Item(
                "banks",
                "Title banks",
                kind="info",
                value=(
                    term.green(f"{bank_count} shard files")
                    if bank_count
                    else term.yellow("not generated")
                )
                + term.dim(f"  {self.repo.display(banks)}"),
                hint="Generate them on the Setup page from the selected ROM.",
            ),
            Item(
                "runner",
                "Runner",
                kind="info",
                value=(
                    term.green(self.repo.display(runner)) + term.dim(f"  ({runner_preset})")
                    if runner
                    else term.yellow("not built")
                ),
                hint="Enable Build runner on the Build page, then Configure & build.",
            ),
            Item(
                "recompiler",
                "Recompiler",
                kind="info",
                value=(
                    term.green(self.repo.display(recompiler))
                    if recompiler.is_file()
                    else term.yellow("not bootstrapped")
                ),
                hint="Setup page -> Bootstrap recompiler.",
            ),
            Item("quick", "QUICK ACTIONS", kind="section"),
            Item(
                "quickstart",
                "Quickstart",
                kind="action",
                hint="Build only the missing pieces, then play. You review every command before it runs.",
                preview=["Finish setup and play in one run.", "Missing steps are added automatically."],
                on_activate=self.quickstart_action,
            ),
            self._action(
                "play",
                "Play now",
                lambda: play_command(self.repo, self.s),
                "Start the game with the current Play settings.",
                self.play_finished,
            ),
            self._action(
                "configure-build",
                "Configure & build",
                lambda: configure_and_build_commands(
                    self.repo, self.s, self.sha1_for_configure()
                ),
                "Configure CMake with the current Build options, then build the selected preset.",
                self.build_finished,
            ),
            Item(
                "choose-rom",
                "Choose ROM…",
                kind="action",
                hint="Browse folders and pick a .nds/.srl file.",
                on_activate=self.open_rom_picker,
            ),
            self._nav("setup", "Generate title banks…", "setup", "Open Setup for bank generation."),
            self._nav("build", "Build settings…", "build", "Open the Build options page."),
        ]
        subtitle = f"{len(self.roms)} ROM(s) · {self.s.preset}"
        return PageView("Dashboard", subtitle, items)

    def rom_view(self, width: int) -> PageView:
        term = self.terminal
        info = self.selected_rom()
        selected_key = self.repo.store(info.path) if info else ""
        items: list[Item] = [
            self._folder(
                "rom-dir",
                "ROM folder",
                lambda: self.s.rom_dir,
                self.set_rom_folder,
                "Choose ROM folder",
                "Enter browses folders; S in the browser uses the current folder.",
            ),
            Item(
                "scan",
                "Rescan folder",
                kind="action",
                hint="Read the .nds/.srl headers in the ROM folder again.",
                on_activate=lambda: (self.refresh_roms(), self.set_toast("ROM folder rescanned")),
            ),
            Item(
                "choose-file",
                "Choose ROM file…",
                kind="action",
                hint="Interactive folder navigation with header preview.",
                on_activate=self.open_rom_picker,
            ),
            Item("selected", "SELECTED ROM", kind="section"),
        ]
        _, value_room = column_widths(width)
        if info:
            save_state = "present" if info.save_path.is_file() else "new"
            sha_state, sha_value = self.sha1_state()
            sha_text = (
                sha_value
                if sha_state == "ready"
                else term.yellow(sha_value)
                if sha_state in ("pending", "error")
                else term.dim(sha_value)
            )
            items.extend(
                [
                    Item("title", "Title", kind="info", value=info.display_name),
                    Item("code", "Game code", kind="info", value=info.code or "unknown"),
                    Item("size", "ROM size", kind="info", value=describe_size(info.size)),
                    Item(
                        "path",
                        "Path",
                        kind="info",
                        value=truncate_middle(self.repo.display(info.path), value_room),
                    ),
                    Item(
                        "sha1",
                        "ROM SHA-1",
                        kind="info",
                        value=sha_text,
                        hint="Configure passes this hash so the runner registers the matching banks.",
                    ),
                    Item(
                        "save",
                        "Save file",
                        kind="info",
                        value=truncate_middle(
                            self.repo.display(info.save_path),
                            max(8, value_room - visible_len(save_state) - 2),
                        )
                        + term.dim(f"  {save_state}"),
                        hint="The save file is created on the first in-game save.",
                    ),
                ]
            )
        else:
            items.append(
                Item(
                    "no-rom",
                    "",
                    kind="info",
                    value=term.yellow("No ROM selected. Browse to one below."),
                )
            )
        items.extend(
            [
                Item(
                    "hash",
                    "Compute SHA-1",
                    kind="action",
                    hint="Hash the selected ROM in the background for the runner title key.",
                    on_activate=self.compute_sha1_action,
                ),
                Item("detected", f"DETECTED ROMS ({len(self.roms)})", kind="section"),
            ]
        )
        if not self.roms:
            items.append(
                Item(
                    "empty",
                    "",
                    kind="info",
                    value=term.dim("No .nds or .srl files in this folder."),
                )
            )
        for rom in self.roms:
            is_selected = bool(selected_key) and self.repo.store(rom.path) == selected_key
            label = ("● " if is_selected else "○ ") + rom.display_name
            if not rom.supported:
                label += "  (unknown code)"
            meta = f" · {rom.code or '????'} · {describe_size(rom.size)}"
            save_mark = " · save" if rom.save_path.is_file() else ""
            name_room = max(8, value_room - visible_len(meta) - visible_len(save_mark) - 1)
            if name_room < 12:
                save_mark = ""
                name_room = max(8, value_room - visible_len(meta) - 1)
            value = f"{truncate_middle(rom.path.name, name_room)}{meta}{save_mark}"
            items.append(
                Item(
                    f"rom:{self.repo.store(rom.path)}",
                    label,
                    kind="rom",
                    value=value,
                    hint=str(rom.path),
                    on_activate=lambda rom=rom: self.select_rom(rom),
                    data=rom,
                )
            )
        subtitle = f"{len(self.roms)} detected · {self.s.rom_dir}"
        return PageView("ROM", subtitle, items)

    def set_rom_folder(self, value: str) -> None:
        self.s.rom_dir = value
        self.touch()
        self.refresh_roms()
        self.set_toast(f"ROM folder set to {value}", "success")

    def compute_sha1_action(self) -> None:
        self.ensure_sha1()
        state, value = self.sha1_state()
        if state == "ready":
            self.set_toast(value, "success", seconds=8)
        elif state == "pending":
            self.set_toast("Hashing the selected ROM…", "info")
        elif state == "error":
            self.show_message("Hash failed", value, "error")
        else:
            self.set_toast("No ROM selected", "warning")

    def build_view(self, width: int) -> PageView:
        term = self.terminal
        cache = self.cache()
        banks = self.repo.resolve(self.s.banks_dir)
        bank_count = len(Repo.bank_files(banks))
        bankgen = self.repo.bankgen_path(self.s.preset)
        runner = self.repo.runner_path(self.s.preset)
        sha_state, sha_value = self.sha1_state()
        digest = self.current_sha1()
        mismatches = self.cache_mismatches(cache)
        items: list[Item] = [
            Item("preset-label", "BUILD PRESET", kind="section"),
            self._choice(
                "preset",
                "Preset",
                "preset",
                [(name, name) for name in self.repo.presets],
                "Configure/build preset. RelWithDebInfo is the recommended runner build.",
            ),
            Item("options-label", "CMAKE OPTIONS", kind="section"),
            self._toggle(
                "build-tests",
                "Build unit tests",
                "build_tests",
                "Adds tests/ and enables ctest for the selected preset.",
            ),
            self._toggle(
                "build-tools",
                "Build host tools",
                "build_tools",
                "Builds pkmn_bankgen and pkmn_romdump under build/<preset>/tools.",
            ),
            self._toggle(
                "build-render",
                "Build render layer",
                "build_render",
                "Builds render/ and the GPU backends; native present needs this.",
            ),
            self._toggle(
                "build-runner",
                "Build ndsrecomp runner",
                "build_runner",
                "Builds the runner and SDL3. Banks must exist before configuring.",
            ),
            self._toggle(
                "native-present",
                "Native present",
                "native_present",
                "Let the runner present through render/ (Vulkan). Requires render and runner.",
                enabled=self.s.build_render,
            ),
            Item("runner-label", "RUNNER WIRING", kind="section"),
            self._folder(
                "banks-dir",
                "Banks directory",
                lambda: self.s.banks_dir,
                lambda value: (setattr(self.s, "banks_dir", value), self.touch()),
                "Choose banks directory",
                "The bank directory compiled into the runner; must contain *_[0-9][0-9].c shards.",
            ),
            Item(
                "banks-status",
                "Bank set",
                kind="info",
                value=(
                    term.green(f"{bank_count} shard files") + term.dim(f"  {self.repo.display(banks)}")
                    if bank_count
                    else term.red(f"missing  {self.repo.display(banks)}")
                ),
                hint="Generate title banks on the Setup page if this is empty.",
            ),
            Item(
                "sha1",
                "Runner title SHA-1",
                kind="info",
                value=(
                    term.green(sha_value) + term.dim("  auto from selected ROM")
                    if sha_state == "ready"
                    else term.yellow(sha_value)
                    if sha_state == "pending"
                    else term.dim("preset default (no ROM selected)")
                ),
                hint="Configure passes the selected ROM's SHA-1 automatically.",
            ),
            Item("advanced", "MULTI-TITLE (ADVANCED)", kind="section"),
            self._text(
                "extra-banks",
                "Extra bank dirs",
                lambda: self.s.extra_bank_dirs,
                lambda value: (setattr(self.s, "extra_bank_dirs", value), self.touch()),
                "Extra title bank dirs (semicolon separated)",
                "Passed as NDS_TITLE_BANK_DIRS; leave empty to keep the current cache value.",
                empty_label="(cache default)",
            ),
            self._text(
                "extra-sha1s",
                "Extra SHA-1s",
                lambda: self.s.extra_sha1s,
                lambda value: (setattr(self.s, "extra_sha1s", value), self.touch()),
                "Extra title SHA-1s (semicolon separated)",
                "Parallel to Extra bank dirs; passed as NDS_TITLE_ROM_SHA1S.",
                empty_label="(cache default)",
            ),
            Item("actions", "ACTIONS", kind="section"),
        ]
        configure_builder = lambda: configure_command(self.repo, self.s, self.sha1_for_configure())
        items.extend(
            [
                self._action(
                    "configure",
                    "Configure",
                    configure_builder,
                    "Write the options above into the CMake cache.",
                    self.configure_finished,
                ),
                self._action(
                    "build",
                    "Build",
                    lambda: build_command(self.repo, self.s),
                    "Incremental build using the existing CMake cache.",
                    self.build_finished,
                ),
                self._action(
                    "configure-build",
                    "Configure & build",
                    lambda: configure_and_build_commands(
                        self.repo, self.s, self.sha1_for_configure()
                    ),
                    "Configure with these options, then build. Use this after changing options.",
                    self.build_finished,
                ),
                self._action(
                    "test",
                    "Run tests",
                    lambda: test_command(self.repo, self.s),
                    "Run ctest for the selected preset with output on failure.",
                ),
            ]
        )
        items.extend(
            [
                Item("state", "BUILD STATE", kind="section"),
                Item(
                    "cache-state",
                    "Options vs cache",
                    kind="info",
                    value=(
                        term.green("in sync")
                        if cache and not mismatches
                        else term.yellow("configure needed")
                        if cache
                        else term.yellow("not configured")
                    ),
                    hint="Configure & build applies option changes.",
                    preview=mismatches or ["The cache matches the options above."],
                ),
                Item(
                    "cache-path",
                    "Build directory",
                    kind="info",
                    value=self.repo.display(self.repo.build_dir(self.s.preset)),
                ),
                Item(
                    "artifacts",
                    "Artifacts",
                    kind="info",
                    value=(
                        term.green("bankgen") if bankgen.is_file() else term.red("bankgen missing")
                    )
                    + "  ·  "
                    + (term.green("runner") if runner.is_file() else term.yellow("runner missing")),
                    hint="bankgen comes from Build host tools; the runner comes from Build runner.",
                ),
                Item(
                    "sha1-note",
                    "",
                    kind="notice",
                    value=term.dim(
                        "Options are passed explicitly on every Configure, so toggles apply immediately."
                    ),
                ),
            ]
        )
        subtitle = f"{self.s.preset} · {self.repo.display(banks)}"
        return PageView("Build", subtitle, items)

    def play_view(self, width: int) -> PageView:
        term = self.terminal
        info = self.selected_rom()
        cache = self.cache()
        runner, runner_preset = self.repo.find_runner(self.s.preset)
        banks = self.repo.resolve(self.s.banks_dir)
        native_cached = cache_bool(cache, "PKMNG5PC_NATIVE_PRESENT")
        items: list[Item] = [
            Item("presentation", "PRESENTATION", kind="section"),
            self._choice(
                "layout",
                "Screen layout",
                "play_layout",
                [(value, value) for value in PLAY_LAYOUTS],
                "Stacked is one window; separate uses two windows. Native present is always stacked.",
            ),
            self._toggle(
                "play-native",
                "Native present",
                "play_native",
                "Present through render/ (Vulkan). The layout is forced to stacked.",
            ),
            self._text(
                "frames",
                "Frame limit",
                lambda: str(self.s.frames),
                self.set_frames,
                "Frames to present (0 = run until closed)",
                "A positive value presents that many frames, prints stats and exits.",
                validator=validate_frames,
            ),
            Item("save-section", "SAVE", kind="section"),
            self._choice(
                "save-mode",
                "Save mode",
                "save_mode",
                [(value, value) for value in SAVE_MODES],
                "Default uses <rom>.sav beside the ROM; Custom picks a file; Off writes nothing.",
                on_change=self.ensure_default_save_path,
            ),
        ]
        if self.s.save_mode == "custom":
            items.append(
                self._text(
                    "save-path",
                    "Save file",
                    lambda: self.s.save_path,
                    lambda value: (setattr(self.s, "save_path", value), self.touch()),
                    "Save file path",
                    "A 512 KiB Gen 5 flash image, or a new file that will be created.",
                    empty_label="(not set)",
                )
            )
        items.append(
            Item(
                "save-resolved",
                "Resolved save",
                kind="info",
                value=save_label(self.s, self.repo, info),
                hint="The runner writes this file whenever the game saves.",
            )
        )
        items.extend(
            [
                Item("diagnostics", "DIAGNOSTICS", kind="section"),
                self._toggle("stats", "Print stats", "stats", "Print the runner performance summary on exit."),
                self._toggle(
                    "coverage",
                    "Keep coverage manifest",
                    "coverage",
                    "Keep the coverage manifest instead of passing --no-coverage-manifest.",
                ),
                self._toggle(
                    "diagnostics-flag",
                    "Diagnostics bundle",
                    "diagnostics",
                    "Write a performance bundle into banks/play.",
                ),
                Item("advanced", "ADVANCED", kind="section"),
                self._text(
                    "extra-args",
                    "Extra runner args",
                    lambda: self.s.extra_args,
                    lambda value: (setattr(self.s, "extra_args", value), self.touch()),
                    "Extra runner arguments",
                    "Shell-style tokens appended to the play command.",
                    empty_label="(none)",
                ),
                Item("run", "RUN", kind="section"),
            ]
        )
        native_warning = ""
        if self.s.play_native and native_cached is False:
            native_warning = "  ·  cache has native present OFF"
        items.append(
            Item(
                "runner",
                "Runner",
                kind="info",
                value=(
                    term.green(self.repo.display(runner)) + term.dim(f"  ({runner_preset})")
                    if runner
                    else term.red("not built")
                )
                + term.yellow(native_warning),
                hint="Play passes this runner explicitly; build it from the Build page.",
            )
        )
        title_warning = ""
        if info and info.title_id and Path(self.s.banks_dir).name != info.title_id:
            title_warning = f"  ·  bank dir is {Path(self.s.banks_dir).name}"
        items.append(
            Item(
                "banks",
                "Title banks",
                kind="info",
                value=(
                    term.green(f"{len(Repo.bank_files(banks))} shard files")
                    if Repo.has_banks(banks)
                    else term.red("missing")
                )
                + term.dim(f"  {self.repo.display(banks)}")
                + term.yellow(title_warning),
            )
        )
        items.append(
            self._action(
                "play",
                "Play",
                lambda: play_command(self.repo, self.s),
                "Launch the live SDL window. Close it or press Esc there to return.",
                self.play_finished,
            )
        )
        subtitle = (info.display_name if info else "no ROM") + f" · {self.s.play_layout}"
        return PageView("Play", subtitle, items)

    def set_frames(self, value: str) -> None:
        self.s.frames = int(value) if value.strip() else 0
        self.touch()

    def ensure_default_save_path(self) -> None:
        if self.s.save_mode != "custom" or self.s.save_path.strip():
            return
        info = self.selected_rom()
        if info:
            self.s.save_path = self.repo.store(info.save_path)
        self.touch()

    def setup_view(self, width: int) -> PageView:
        term = self.terminal
        info = self.selected_rom()
        _, label = title_identity(info)
        default_out = default_bank_out(info)
        default_config = default_title_config(info)
        default_seeds = default_overlay_seeds(info)
        bankgen = self.repo.bankgen_path(self.s.preset)
        recompiler = self.repo.recompiler_path
        items: list[Item] = [
            Item("bootstrap-label", "RECOMPILER BOOTSTRAP", kind="section"),
            self._toggle(
                "bootstrap-runner",
                "Build standalone runner",
                "bootstrap_runner",
                "Also configure/build the standalone runner under third_party/ndsrecomp.",
            ),
            self._toggle(
                "bootstrap-sdl3",
                "SDL3 backend",
                "bootstrap_sdl3",
                "Only with Build standalone runner; otherwise the standalone runner is headless.",
                enabled=self.s.bootstrap_runner,
            ),
            self._text(
                "bootstrap-compiler",
                "Compiler",
                lambda: self.s.bootstrap_compiler,
                lambda value: (setattr(self.s, "bootstrap_compiler", value), self.touch()),
                "Bootstrap compiler (blank = script default)",
                "clang, gcc, clang-18, or a path to the C compiler.",
                empty_label="(script default)",
            ),
            self._action(
                "bootstrap",
                "Bootstrap recompiler",
                lambda: bootstrap_command(self.repo, self.s),
                "Initialize ndsrecomp, build nds_recompile and generate the BIOS banks.",
                self.bootstrap_finished,
            ),
            Item(
                "recompiler",
                "Recompiler",
                kind="info",
                value=(
                    term.green(self.repo.display(recompiler))
                    if recompiler.is_file()
                    else term.yellow("not built")
                ),
                hint="Generated under third_party/ndsrecomp/recompiler/build.",
            ),
            Item("banks-label", "TITLE BANKS", kind="section"),
            Item(
                "rom",
                "Source ROM",
                kind="info",
                value=(
                    term.green(info.display_name) + term.dim(f"  {info.code}")
                    if info
                    else term.yellow("no ROM selected (ROM page)")
                ),
                hint="Bank output and config default to this ROM's title.",
            ),
            self._text(
                "bank-out",
                "Output directory",
                lambda: self.s.bank_out or default_out,
                lambda value: (setattr(self.s, "bank_out", value), self.touch()),
                "Bank output directory",
                "Blank resets to the derived banks/<title> directory.",
            ),
            self._text(
                "title-config",
                "Title config",
                lambda: self.s.title_config or default_config,
                lambda value: (
                    setattr(self.s, "title_config", "" if value == default_config else value),
                    self.touch(),
                ),
                "Title config path",
                "Per-title [[alias]]/[[seed_file]] config; blank uses the derived default.",
                empty_label="(derived default)" if default_config else "(none)",
            ),
            self._text(
                "shards",
                "Shard count",
                lambda: str(self.s.shards),
                self.set_shards,
                "Bank shard count (>= 2)",
                "The runner globs *_[0-9][0-9].c, so at least two shards are required.",
                validator=validate_shards,
            ),
            self._toggle(
                "overlays",
                "Include overlays",
                "overlays",
                "Add one ARM9 bank per overlay that has an observed seed file.",
            ),
        ]
        if self.s.overlays:
            items.append(
                self._folder(
                    "overlay-seeds",
                    "Overlay seeds",
                    lambda: self.s.overlay_seeds or default_seeds,
                    lambda value: (setattr(self.s, "overlay_seeds", value), self.touch()),
                    "Choose overlay seeds directory",
                    "Directory holding arm9_ov*.toml seed files.",
                )
            )
        items.extend(
            [
                self._toggle(
                    "emit-only",
                    "Emit only (no recompile)",
                    "emit_only",
                    "Write the bank scaffolding without running nds_recompile.",
                ),
                self._action(
                    "generate",
                    "Generate title banks",
                    lambda: bankgen_command(self.repo, self.s),
                    "Recompile the selected ROM into the output directory; existing shards are replaced. This can take minutes.",
                    self.bankgen_finished,
                ),
                Item(
                    "bankgen",
                    "bankgen",
                    kind="info",
                    value=(
                        term.green(self.repo.display(bankgen))
                        if bankgen.is_file()
                        else term.red("missing (build host tools first)")
                    ),
                ),
                Item("after", "AFTER GENERATION", kind="section"),
                Item(
                    "use-output",
                    "Use output as build banks dir",
                    kind="action",
                    hint="Point the Build page at the directory generated above.",
                    on_activate=self.use_generated_banks,
                ),
            ]
        )
        subtitle = (info.display_name if info else "no ROM") + f" · {self.s.banks_dir}"
        return PageView("Setup", subtitle, items)

    def set_shards(self, value: str) -> None:
        self.s.shards = int(value)
        self.touch()

    def use_generated_banks(self) -> None:
        info = self.selected_rom()
        output = self.s.bank_out or default_bank_out(info)
        self.s.banks_dir = self.repo.store(self.repo.resolve(output))
        self.touch()
        self.set_toast(f"Build banks dir set to {self.s.banks_dir}", "success")

    def help_view(self, width: int) -> PageView:
        term = self.terminal
        text_width = max(20, width - 8)
        items: list[Item] = [
            Item("keys", "KEYS", kind="section"),
        ]
        key_lines = [
            "Up/Down move · Left/Right change · Enter activate · Space toggle",
            "Mouse: click a tab or row, wheel scrolls; click a selected browser row to open it",
            "Final review: Enter Run, Esc Cancel, Up/Down scroll; Run/Cancel are clickable",
            "Tab / Shift+Tab change page · 1-6 jump · H/R/B/P/S shortcuts",
            "? help · L last log · Q quit · Esc closes a dialog",
            "In the ROM browser: Enter open, S use folder, G type a path, H hidden",
        ]
        for line in key_lines:
            for wrapped in wrap_text(line, text_width):
                items.append(Item("", "", kind="raw", value=wrapped))
        items.append(Item("workflow", "WORKFLOW", kind="section"))
        workflow = [
            "1. ROM: pick the folder, then the .nds/.srl. Titles and game codes are read from the header.",
            "2. Build: keep Build host tools on, then Configure & build to get pkmn_bankgen.",
            "3. Setup: bootstrap the recompiler once, then generate title banks from the ROM.",
            "4. Build: enable Build runner, then Configure & build again.",
            "5. Play: choose presentation, save and diagnostic flags, then Play.",
        ]
        for line in workflow:
            for wrapped in wrap_text(line, text_width):
                items.append(Item("", "", kind="raw", value=wrapped))
        items.append(Item("notes", "NOTES", kind="section"))
        notes = [
            "Quickstart on Home builds only the missing pieces, then plays; every action opens a final review of the full configuration first.",
            "The launcher never writes game content. Builds go to build/, banks to banks/, and the local UI state to tui/.state.json (git-ignored).",
            "ROMs are read for their header and, when configuring, their SHA-1. The game window belongs to the runner; close it to return to the log.",
            "Configuration changes are passed explicitly on every Configure, so toggles do not depend on old cache values.",
        ]
        for line in notes:
            for wrapped in wrap_text(line, text_width):
                items.append(Item("", "", kind="raw", value=wrapped))
        items.append(Item("legal", "LEGAL", kind="section"))
        for line in [
            "No ROM, save, extracted asset or generated bank is committed. Supply your own legally obtained ROMs.",
        ]:
            for wrapped in wrap_text(line, text_width):
                items.append(Item("", "", kind="raw", value=wrapped))
        return PageView("Help", "keys, workflow and file locations", items, panel="")

    def configure_finished(self, job: Job) -> None:
        self.set_toast(f"Configured {self.s.preset}", "success")

    def build_finished(self, job: Job) -> None:
        self.set_toast(f"Build finished for {self.s.preset}", "success")

    def play_finished(self, job: Job) -> None:
        self.set_toast("Play session ended", "success")

    def bootstrap_finished(self, job: Job) -> None:
        self.set_toast("Bootstrap finished", "success")

    def bankgen_finished(self, job: Job) -> None:
        output = job.meta.get("out_dir")
        if output:
            self.s.banks_dir = self.repo.store(output)
            self.touch()
            self.set_toast(f"Banks generated in {self.s.banks_dir}", "success", seconds=7)

    def render_page(self, width: int, height: int) -> list[str]:
        term = self.terminal
        view = self.page_view(width)
        items = view.items
        selectable = [index for index, item in enumerate(items) if item.kind in SELECTABLE]
        selected_index = self.selection_index(items, selectable)
        selected_item = items[selected_index] if selected_index >= 0 else None
        panel_height = (
            5 if height >= 28 and view.panel and selected_item and selected_item.preview else 0
        )
        body_height = height - 8 - panel_height
        body_height = max(2, body_height)
        scroll = self.scroll.get(self.page, 0)
        if selected_index >= 0:
            if selected_index < scroll:
                scroll = selected_index
            elif selected_index >= scroll + body_height:
                scroll = selected_index - body_height + 1
        max_scroll = max(0, len(items) - body_height)
        scroll = max(0, min(scroll, max_scroll))
        self.scroll[self.page] = scroll

        lines = self.render_chrome(width)
        title_line = "  " + term.bold(view.title)
        right = term.dim(view.subtitle)
        if visible_len(right) > width // 2:
            right = ""
        lines.append(pad_between(title_line, right, width))

        for index in range(scroll, min(len(items), scroll + body_height)):
            if items[index].kind in SELECTABLE:
                self.row_hits[6 + index - scroll] = items[index].key
            lines.append(self.render_item(items[index], index == selected_index, width))
        while len(lines) < 5 + body_height:
            lines.append("")

        if self.toast:
            text, kind, _ = self.toast
            color = {"success": term.green, "warning": term.yellow, "error": term.red}.get(
                kind, term.cyan
            )
            hint = color(text)
        elif selected_item and selected_item.hint:
            hint = term.dim(selected_item.hint)
        else:
            hint = term.dim(view.subtitle)
        lines.append("  " + truncate_middle(hint, width - 4))

        if panel_height and selected_item:
            preview = selected_item.preview
            wrapped: list[str] = []
            for line in preview:
                wrapped.extend(wrap_text(sanitize(line), width - 8))
            if len(wrapped) > 3:
                wrapped = wrapped[:3]
                wrapped[-1] = wrapped[-1] + " …"
            while len(wrapped) < 3:
                wrapped.append("")
            lines.extend(make_box(view.panel, wrapped, width, term))

        lines.append("  " + term.dim(term.rule(width - 4)))
        lines.append("  " + term.dim(clip(self.footer_text(width), width - 2)))
        while len(lines) < height:
            lines.append("")
        return lines[:height]

    def selection_index(self, items: list[Item], selectable: list[int] | None = None) -> int:
        if selectable is None:
            selectable = [index for index, item in enumerate(items) if item.kind in SELECTABLE]
        if not selectable:
            return -1
        desired = self.selected.get(self.page)
        if desired:
            for index in selectable:
                if items[index].key == desired:
                    return index
        selected = selectable[0]
        self.selected[self.page] = items[selected].key
        return selected

    def render_item(self, item: Item, selected: bool, width: int) -> str:
        term = self.terminal
        if item.kind == "section":
            label = f"── {item.label} "
            fill = max(0, width - 4 - visible_len(label))
            return "  " + term.dim(label + term.rule(fill))
        if item.kind == "raw":
            return "  " + clip(item.value, width - 2)
        if item.kind == "blank":
            return ""
        if item.kind == "notice":
            return "  " + clip(item.value, width - 2)

        marker = term.marker(selected)
        label_width, value_room = column_widths(width)
        label = item.label
        if visible_len(label) > label_width:
            label = clip(label, label_width)
        label_room = label_width - visible_len(label)
        if selected and item.enabled:
            label_styled = term.style(label, color="bright_cyan", bold=True)
        elif item.enabled:
            label_styled = label
        else:
            label_styled = term.dim(label)
        value = item.value
        if item.kind == "toggle":
            value = (
                term.green("[ ON ]")
                if item.value == "ON" and item.enabled
                else term.grey("[ OFF ]")
                if item.enabled
                else term.dim("[ OFF ]")
            )
        elif item.kind == "choice":
            value = (
                term.cyan(f"‹ {value} ›")
                if selected and item.enabled
                else term.dim(f"‹ {value} ›")
                if item.enabled
                else term.dim(value)
            )
        elif item.kind == "action":
            if selected and item.enabled:
                value = term.dim("Enter")
            elif not item.enabled:
                value = term.dim(item.value)
        elif item.kind == "rom":
            if selected:
                value = term.cyan(value)
            else:
                value = term.dim(value)
        elif item.kind == "text" and item.value == "(not set)":
            value = term.dim(item.value)
        elif item.kind == "folder" and item.value == "(not set)":
            value = term.dim(item.value)
        if not item.enabled and item.kind != "toggle":
            value = term.dim(strip_ansi(value))
        return "  " + marker + label_styled + (" " * max(0, label_room)) + "  " + clip(value, value_room)

    def footer_text(self, width: int) -> str:
        if self.page == "help":
            text = "↑↓ scroll · Tab page · 1-6 jump · Q quit"
        else:
            text = "↑↓ move · ←→ change · Enter activate · mouse · Tab page · ? help · Q quit"
        if self.terminal.ascii:
            text = (
                text.replace("↑↓", "up/down")
                .replace("←→", "left/right")
                .replace("·", "-")
            )
        return text

    def render_job(self, width: int, height: int) -> list[str]:
        term = self.terminal
        job = self.job
        assert job is not None
        lines = self.render_chrome(width)
        spinner = "|/-\\"[int(time.monotonic() * 8) % 4] if job.active else " "
        if job.active:
            status = term.yellow(f"{spinner} running")
        elif job.status == "done":
            status = term.green("finished")
        elif job.status == "cancelled":
            status = term.yellow("cancelled")
        else:
            status = term.red(f"failed (exit {job.returncode})")
        header = "  " + status + "  " + term.bold(job.title)
        if job.current_label and job.active:
            header += term.dim(f"  ·  {job.current_label}")
        header += term.dim(f"  ·  {job.elapsed:.1f}s")
        lines.append(clip(header, width))
        lines.append("  " + term.dim(term.rule(width - 4)))
        log_height = max(3, height - 9)
        total = len(job.lines)
        if self.log_follow:
            start = max(0, total - log_height)
        else:
            start = max(0, total - log_height - self.log_back)
        visible = list(job.lines)[start : start + log_height]
        for line in visible:
            lines.append("  " + clip(sanitize(line), width - 4))
        while len(lines) < 5 + log_height:
            lines.append("")
        if job.active:
            hint = "Esc / Ctrl+C cancel · ↑↓ scroll · End follow"
        else:
            hint = "Enter return · ↑↓ scroll · End follow"
        if not self.log_follow:
            hint += term.dim(f"  ·  {self.log_back} line(s) back")
        lines.append("  " + term.dim(clip(hint, width - 2)))
        lines.append("  " + term.dim(term.rule(width - 4)))
        lines.append(
            "  " + term.dim(clip("The game window, when open, owns its own keyboard focus.", width - 2))
        )
        while len(lines) < height:
            lines.append("")
        return lines[:height]

    def render_input(self, width: int, height: int) -> list[str]:
        term = self.terminal
        state = self.input
        assert state is not None
        lines = self.render_chrome(width)
        lines.append(clip("  " + term.bold(term.cyan("Edit")) + "  " + term.dim(state.title), width))
        lines.append("  " + term.dim(term.rule(width - 4)))
        lines.append("  " + clip(state.prompt, width - 2))
        field_width = max(12, width - 8)
        start = 0
        if len(state.value) > field_width:
            start = max(0, state.cursor - field_width + 12)
            start = min(start, len(state.value) - field_width)
        shown = state.value[start : start + field_width]
        local_cursor = max(0, min(state.cursor - start, len(shown)))
        before = shown[:local_cursor]
        at = shown[local_cursor] if local_cursor < len(shown) else " "
        after = shown[local_cursor + 1 :] if local_cursor < len(shown) else ""
        field = before + term.style(at, reverse=True) + after
        if start > 0:
            field = term.grey("‹") + field
        if start + field_width < len(state.value):
            field += term.grey("›")
        lines.extend(make_box("Value", [field], width, term))
        if state.error:
            lines.append("  " + term.red(clip(state.error, width - 2)))
        else:
            lines.append("")
        lines.append(
            "  " + term.dim(clip("Enter accept · Esc cancel · ←→ move · Ctrl+U clear", width - 2))
        )
        lines.append("  " + term.dim(term.rule(width - 4)))
        lines.append(
            "  " + term.dim(clip("Blank input resets a derived path in some fields.", width - 2))
        )
        while len(lines) < height:
            lines.append("")
        return lines[:height]

    def render_message(self, width: int, height: int) -> list[str]:
        term = self.terminal
        state = self.message
        assert state is not None
        lines = self.render_chrome(width)
        color = {"error": term.red, "success": term.green, "warning": term.yellow}.get(
            state.kind, term.cyan
        )
        lines.append(clip("  " + term.bold(color(state.title)), width))
        lines.append("  " + term.dim(term.rule(width - 4)))
        wrapped: list[str] = []
        for paragraph in state.body.splitlines() or [""]:
            wrapped.extend(wrap_text(paragraph, width - 8))
        max_body = max(2, height - 12)
        if len(wrapped) > max_body:
            wrapped = wrapped[:max_body]
            wrapped[-1] += " …"
        lines.extend(make_box("Message", wrapped, width, term))
        lines.append("")
        lines.append("  " + term.dim(clip("Enter close · Esc close", width - 2)))
        lines.append("  " + term.dim(term.rule(width - 4)))
        lines.append("  " + term.dim(clip("The launcher keeps your settings.", width - 2)))
        while len(lines) < height:
            lines.append("")
        return lines[:height]

    def render_review(self, width: int, height: int) -> list[str]:
        term = self.terminal
        state = self.review
        assert state is not None
        body: list[tuple[str, str]] = []
        for section, rows in state.sections:
            body.append(("heading", section))
            warning = section == "FINAL CHECK"
            for row in rows:
                indent = len(row) - len(row.lstrip())
                prefix = " " * indent
                for wrapped in wrap_text(sanitize(row.strip()), max(10, width - 6 - indent)) or [""]:
                    body.append(("warning" if warning else "text", prefix + wrapped))
            body.append(("blank", ""))
        body_height = max(2, height - 8)
        max_scroll = max(0, len(body) - body_height)
        state.scroll = max(0, min(state.scroll, max_scroll))
        top = state.scroll

        lines = self.render_chrome(width)
        lines.append(
            "  " + term.bold(term.yellow("FINAL REVIEW")) + "  " + term.bold(state.title)
        )
        lines.append("  " + term.dim(term.rule(width - 4)))
        for kind, text in body[top : top + body_height]:
            if kind == "heading":
                label = f"{'-- ' if term.ascii else '── '}{text} "
                fill = max(0, width - 4 - visible_len(label))
                lines.append("  " + term.bold(term.cyan(label + term.rule(fill))))
            elif kind == "warning":
                lines.append("  " + term.yellow(clip(text, width - 4)))
            elif kind == "blank":
                lines.append("")
            else:
                lines.append("  " + clip(text, width - 4))
        while len(lines) < 6 + body_height:
            lines.append("")

        run = "[ Run ]"
        cancel = "[ Cancel ]"
        run_x = 3
        cancel_x = run_x + len(run) + 2
        self.review_hits.append((height - 1, run_x, run_x + len(run) - 1, "run"))
        self.review_hits.append((height - 1, cancel_x, cancel_x + len(cancel) - 1, "cancel"))
        if term.color:
            run = term.style(run, color="black", background="green", bold=True)
            cancel = term.style(cancel, color="black", background="yellow", bold=True)
        position = (
            f"lines {top + 1}-{min(len(body), top + body_height)}/{len(body)}"
            if body
            else "no lines"
        )
        if term.ascii:
            buttons = "  " + run + "  " + cancel + "   Enter run - Esc cancel - up/down scroll"
        else:
            buttons = "  " + run + "  " + cancel + "   Enter run · Esc cancel · ↑↓ scroll"
        lines.append(pad_between(buttons, term.dim(position), width))
        lines.append("  " + term.dim(term.rule(width - 4)))
        while len(lines) < height:
            lines.append("")
        return lines[:height]

    def handle_key(self, key: str) -> None:
        if self.mode == "job":
            self.handle_job_key(key)
        elif self.mode == "picker":
            self.handle_picker_key(key)
        elif self.mode == "input":
            self.handle_input_key(key)
        elif self.mode == "review":
            self.handle_review_key(key)
        elif self.mode == "message":
            if key in ("enter", "escape", "space", "ctrl-c"):
                self.message = None
                self.mode = "page"
                self.dirty = True
        else:
            self.handle_page_key(key)

    def handle_review_key(self, key: str) -> None:
        state = self.review
        if state is None:
            return
        if key == "enter":
            self.confirm_review()
            return
        if key in ("escape", "ctrl-c", "q", "Q"):
            self.cancel_review()
            return
        if key == "up":
            state.scroll = max(0, state.scroll - 1)
        elif key == "down":
            state.scroll += 1
        elif key == "pageup":
            state.scroll = max(0, state.scroll - 8)
        elif key == "pagedown":
            state.scroll += 8
        elif key == "home":
            state.scroll = 0
        elif key == "end":
            state.scroll = 100000
        self.dirty = True

    def confirm_review(self) -> None:
        state = self.review
        if state is None:
            return
        self.review = None
        self.mode = "page"
        self.start_job(state.commands, state.title, state.on_success)

    def cancel_review(self) -> None:
        state = self.review
        self.review = None
        self.mode = "page"
        if state and state.on_cancel:
            state.on_cancel()
        self.set_toast("Cancelled — nothing was run", "info")

    def handle_mouse(self, event: MouseEvent) -> None:
        if self.mode == "picker" and self.picker:
            result = self.picker.handle_mouse(event)
            self.finish_picker(result)
            self.dirty = True
            return
        if event.kind == "wheel":
            if event.button not in ("up", "down"):
                return
            direction = "up" if event.button == "up" else "down"
            if self.mode == "page":
                if self.page == "help":
                    for _ in range(3):
                        self.handle_help_scroll(direction)
                else:
                    items = self.page_view(self.terminal.size()[0]).items
                    selectable = [
                        index for index, item in enumerate(items) if item.kind in SELECTABLE
                    ]
                    if selectable:
                        current = self.selection_index(items, selectable)
                        self.move_selection(
                            items, selectable, current, -3 if direction == "up" else 3
                        )
            elif self.mode == "review" and self.review:
                self.review.scroll = max(
                    0, self.review.scroll + (-3 if direction == "up" else 3)
                )
                self.dirty = True
            elif self.mode == "job":
                for _ in range(3):
                    self.handle_job_key(direction)
            return
        if event.kind != "press" or event.button != "left":
            return
        if self.mode == "page":
            self.handle_page_click(event)
        elif self.mode == "review" and self.review:
            for y, x0, x1, action in self.review_hits:
                if event.y == y and x0 <= event.x <= x1:
                    if action == "run":
                        self.confirm_review()
                    else:
                        self.cancel_review()
                    return
        elif self.mode == "message" and self.message:
            self.message = None
            self.mode = "page"
            self.dirty = True

    def handle_page_click(self, event: MouseEvent) -> None:
        for y, x0, x1, page in self.tab_hits:
            if event.y == y and x0 <= event.x <= x1:
                self.set_page(page)
                return
        key = self.row_hits.get(event.y)
        if key is None:
            return
        items = self.page_view(self.terminal.size()[0]).items
        index = next((position for position, item in enumerate(items) if item.key == key), None)
        if index is None:
            return
        item = items[index]
        self.select_index(items, index)
        if item.on_activate:
            item.on_activate()
        elif item.on_adjust:
            item.on_adjust(1)

    def handle_page_key(self, key: str) -> None:
        if key in ("q", "Q", "ctrl-c"):
            self.quit()
            return
        if key in ("?", "help"):
            self.set_page("help")
            return
        if key in ("tab",):
            self.cycle_page(1)
            return
        if key == "backtab":
            self.cycle_page(-1)
            return
        if key in "123456" and len(key) == 1:
            self.set_page(PAGE_ORDER[int(key) - 1])
            return
        shortcuts = {"h": "home", "r": "rom", "b": "build", "p": "play", "s": "setup"}
        if key.lower() in shortcuts:
            self.set_page(shortcuts[key.lower()])
            return
        if key in ("l", "L"):
            if self.last_job:
                self.job = self.last_job
                self.mode = "job"
                self.log_follow = True
                self.log_back = 0
                self.dirty = True
            return
        if self.page == "help":
            self.handle_help_scroll(key)
            return
        items = self.page_view(self.terminal.size()[0]).items
        selectable = [index for index, item in enumerate(items) if item.kind in SELECTABLE]
        if not selectable:
            return
        current = self.selection_index(items, selectable)
        if key == "up":
            self.move_selection(items, selectable, current, -1)
        elif key == "down":
            self.move_selection(items, selectable, current, 1)
        elif key == "pageup":
            self.move_selection(items, selectable, current, -8)
        elif key == "pagedown":
            self.move_selection(items, selectable, current, 8)
        elif key == "home":
            self.select_index(items, selectable[0])
        elif key == "end":
            self.select_index(items, selectable[-1])
        elif key == "left":
            if items[current].on_adjust:
                items[current].on_adjust(-1)
        elif key == "right":
            if items[current].on_adjust:
                items[current].on_adjust(1)
        elif key in ("enter", " "):
            if items[current].on_activate:
                items[current].on_activate()
            elif items[current].on_adjust:
                items[current].on_adjust(1)

    def handle_help_scroll(self, key: str) -> None:
        current = self.scroll.get("help", 0)
        if key == "up":
            self.scroll["help"] = max(0, current - 1)
        elif key == "down":
            self.scroll["help"] = current + 1
        elif key == "pageup":
            self.scroll["help"] = max(0, current - 8)
        elif key == "pagedown":
            self.scroll["help"] = current + 8
        elif key == "home":
            self.scroll["help"] = 0
        elif key == "end":
            self.scroll["help"] = 100000
        self.dirty = True

    def move_selection(
        self, items: list[Item], selectable: list[int], current: int, delta: int
    ) -> None:
        position = selectable.index(current) if current in selectable else 0
        position = max(0, min(position + delta, len(selectable) - 1))
        self.select_index(items, selectable[position])

    def select_index(self, items: list[Item], index: int) -> None:
        self.selected[self.page] = items[index].key
        self.dirty = True

    def handle_picker_key(self, key: str) -> None:
        assert self.picker is not None
        self.finish_picker(self.picker.handle_key(key))

    def finish_picker(self, result: str | None) -> None:
        if result in ("cancel", "picked"):
            self.picker = None
            self.mode = "page"
            self.refresh_roms()
            self.dirty = True

    def handle_input_key(self, key: str) -> None:
        state = self.input
        assert state is not None
        if key in ("escape", "ctrl-c"):
            self.input = None
            self.mode = "page"
            self.dirty = True
            return
        if key == "enter":
            value = state.value
            if not state.allow_empty and not value.strip():
                state.error = "A value is required."
                self.dirty = True
                return
            if state.validator:
                error = state.validator(value)
                if error:
                    state.error = error
                    self.dirty = True
                    return
            try:
                state.on_submit(value)
            except (ValueError, OSError) as error:
                state.error = str(error)
                self.dirty = True
                return
            self.input = None
            self.mode = "page"
            self.dirty = True
            return
        if key == "backspace":
            if state.cursor > 0:
                state.value = state.value[: state.cursor - 1] + state.value[state.cursor :]
                state.cursor -= 1
        elif key == "delete":
            state.value = state.value[: state.cursor] + state.value[state.cursor + 1 :]
        elif key == "left":
            state.cursor = max(0, state.cursor - 1)
        elif key == "right":
            state.cursor = min(len(state.value), state.cursor + 1)
        elif key == "home":
            state.cursor = 0
        elif key == "end":
            state.cursor = len(state.value)
        elif key == "ctrl-u":
            state.value = ""
            state.cursor = 0
        elif len(key) == 1 and key >= " ":
            state.value = state.value[: state.cursor] + key + state.value[state.cursor :]
            state.cursor += 1
        state.error = ""
        self.dirty = True

    def handle_job_key(self, key: str) -> None:
        job = self.job
        assert job is not None
        if key in ("escape", "ctrl-c"):
            if job.active:
                job.cancel()
                self.set_toast("Cancelling…", "warning")
            else:
                self.mode = "page"
                self.dirty = True
            return
        if not job.active and key in ("enter", " ", "q", "Q"):
            self.mode = "page"
            self.dirty = True
            return
        if key in ("up", "down", "pageup", "pagedown", "home", "end", "f", "F"):
            total = len(job.lines)
            height = max(3, self.terminal.size()[1] - 9)
            if key == "up":
                self.log_follow = False
                self.log_back = min(total, self.log_back + 1)
            elif key == "down":
                self.log_back = max(0, self.log_back - 1)
                if self.log_back == 0:
                    self.log_follow = True
            elif key == "pageup":
                self.log_follow = False
                self.log_back = min(total, self.log_back + height)
            elif key == "pagedown":
                self.log_back = max(0, self.log_back - height)
                if self.log_back == 0:
                    self.log_follow = True
            elif key == "home":
                self.log_follow = False
                self.log_back = max(0, total - height)
            elif key in ("end", "f", "F"):
                self.log_follow = True
                self.log_back = 0
            self.dirty = True

    def quit(self) -> None:
        if self.persist:
            try:
                self.s.save(self.state_path)
                self.settings_dirty = False
            except OSError:
                pass
        self.running = False


def validate_frames(value: str) -> str | None:
    text = value.strip()
    if not text:
        return None
    try:
        number = int(text)
    except ValueError:
        return "Enter a whole number."
    if number < 0:
        return "Frames cannot be negative."
    return None


def validate_shards(value: str) -> str | None:
    text = value.strip()
    if not text:
        return "Enter a shard count (>= 2)."
    try:
        number = int(text)
    except ValueError:
        return "Enter a whole number."
    if number < 2:
        return "The minimum is 2 shards."
    if number > 64:
        return "Pick 64 or fewer shards."
    return None
