"""Command construction and streamed subprocess execution."""

from __future__ import annotations

import os
import re
import shlex
import signal
import subprocess
import threading
import time
from collections import deque
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any

from tui.model import (
    Repo,
    RomInfo,
    Settings,
    default_bank_out,
    default_overlay_seeds,
    default_title_config,
    find_cmake,
    find_ctest,
    find_powershell,
    read_rom_info,
    split_paths,
)
from tui.terminal import strip_ansi


class LaunchError(RuntimeError):
    pass


@dataclass
class Command:
    title: str
    argv: list[str]
    cwd: Path
    env: dict[str, str] = field(default_factory=dict)
    note: str = ""
    meta: dict[str, Any] = field(default_factory=dict)

    def preview(self) -> str:
        return display_argv(self.argv)


def display_argv(argv: list[str]) -> str:
    if not argv:
        return ""
    if os.name == "nt":
        return subprocess.list2cmdline(argv)
    return shlex.join(argv)


def _require_file(path: Path, what: str) -> None:
    if not path.is_file():
        raise LaunchError(f"{what} not found: {path}")


def _cmake() -> str:
    executable = find_cmake()
    if not executable:
        raise LaunchError("cmake is not on PATH; install CMake >= 3.24 and reopen the launcher")
    return executable


def _ctest() -> str:
    executable = find_ctest()
    if not executable:
        raise LaunchError("ctest is not on PATH; install CMake >= 3.24 and reopen the launcher")
    return executable


def _resolved_extra_bank_dirs(repo: Repo, value: str) -> str:
    parts = split_paths(value)
    return ";".join(str(repo.resolve(part)) for part in parts)


def configure_command(
    repo: Repo,
    settings: Settings,
    rom_sha1: str | None,
    require_banks: bool = True,
) -> Command:
    banks = repo.resolve(settings.banks_dir)
    if require_banks and settings.build_runner and not Repo.has_banks(banks):
        raise LaunchError(
            f"no generated title banks in {banks}. Generate them on the Setup page first."
        )
    if settings.build_runner and settings.native_present and not settings.build_render:
        raise LaunchError("Native present needs Build render; enable it or turn Native present off.")
    extra_dirs = split_paths(settings.extra_bank_dirs)
    extra_sha1s = split_paths(settings.extra_sha1s)
    if bool(extra_dirs) != bool(extra_sha1s):
        raise LaunchError("Extra bank dirs and extra SHA-1s must be filled together.")
    if extra_dirs and len(extra_dirs) != len(extra_sha1s):
        raise LaunchError("Extra bank dirs and extra SHA-1s must have the same count.")

    argv = [
        _cmake(),
        "--preset",
        settings.preset,
        f"-DPKMNG5PC_BUILD_TESTS={'ON' if settings.build_tests else 'OFF'}",
        f"-DPKMNG5PC_BUILD_TOOLS={'ON' if settings.build_tools else 'OFF'}",
        f"-DPKMNG5PC_BUILD_RENDER={'ON' if settings.build_render else 'OFF'}",
        f"-DPKMNG5PC_BUILD_RUNNER={'ON' if settings.build_runner else 'OFF'}",
        f"-DPKMNG5PC_NATIVE_PRESENT={'ON' if settings.native_present else 'OFF'}",
        f"-DPKMNG5PC_BANKS_DIR={banks}",
    ]
    if rom_sha1:
        argv.append(f"-DPKMNG5PC_TITLE_ROM_SHA1={rom_sha1}")
    if extra_dirs:
        argv.append(f"-DNDS_TITLE_BANK_DIRS={_resolved_extra_bank_dirs(repo, settings.extra_bank_dirs)}")
        argv.append(f"-DNDS_TITLE_ROM_SHA1S={';'.join(extra_sha1s)}")
    note = "Options become the CMake cache; changing the compiler later needs a fresh build dir."
    return Command("Configure", argv, repo.root, note=note)


def build_command(repo: Repo, settings: Settings, require_configured: bool = True) -> Command:
    if require_configured and not repo.cache_path(settings.preset).is_file():
        raise LaunchError(
            f"{repo.build_dir(settings.preset)} is not configured. Use Configure & build."
        )
    return Command(
        "Build",
        [_cmake(), "--build", "--preset", settings.preset],
        repo.root,
    )


def test_command(repo: Repo, settings: Settings) -> Command:
    if not repo.cache_path(settings.preset).is_file():
        raise LaunchError(
            f"{repo.build_dir(settings.preset)} is not configured. Use Configure & build."
        )
    return Command(
        "Tests",
        [_ctest(), "--preset", settings.preset, "--output-on-failure"],
        repo.root,
    )


def bootstrap_command(repo: Repo, settings: Settings) -> Command:
    script = repo.bootstrap_script
    _require_file(script, "bootstrap script")
    if os.name == "nt":
        powershell = find_powershell()
        if not powershell:
            raise LaunchError("PowerShell was not found on PATH")
        argv = [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script)]
        if settings.bootstrap_runner:
            argv.append("-BuildRunner")
            if settings.bootstrap_sdl3:
                argv.append("-Sdl3")
        if settings.bootstrap_compiler.strip():
            argv.extend(["-Compiler", settings.bootstrap_compiler.strip()])
    else:
        argv = ["sh", str(script)]
        if settings.bootstrap_runner:
            argv.append("--build-runner")
            if settings.bootstrap_sdl3:
                argv.append("--sdl3")
        if settings.bootstrap_compiler.strip():
            argv.extend(["--compiler", settings.bootstrap_compiler.strip()])
    note = "Prints progress as it initializes the submodule, builds the recompiler and generates BIOS banks."
    return Command("Bootstrap recompiler", argv, repo.root, note=note)


def _rom_info_or_error(repo: Repo, settings: Settings) -> tuple[Path, RomInfo]:
    if not settings.rom_path.strip():
        raise LaunchError("No ROM selected. Choose one on the ROM page.")
    rom = repo.resolve(settings.rom_path)
    if not rom.is_file():
        raise LaunchError(f"ROM not found: {rom}")
    info = read_rom_info(rom)
    if info is None:
        raise LaunchError(f"Could not read the NDS header from {rom}")
    return rom, info


def bankgen_command(repo: Repo, settings: Settings, require_tools: bool = True) -> Command:
    if settings.shards < 2:
        raise LaunchError("Shard count must be at least 2.")
    rom, info = _rom_info_or_error(repo, settings)
    bankgen = repo.bankgen_path(settings.preset)
    if require_tools:
        _require_file(bankgen, "pkmn_bankgen")
    output = repo.resolve(settings.bank_out or default_bank_out(info))

    argv = [str(bankgen), str(rom), str(output)]
    config_text = settings.title_config.strip() or default_title_config(info)
    if config_text:
        config = repo.resolve(config_text)
        if settings.title_config.strip() and not config.is_file():
            raise LaunchError(f"Title config not found: {config}")
        if config.is_file():
            argv.extend(["--title-config", str(config)])
    if settings.overlays:
        seeds_text = settings.overlay_seeds.strip() or default_overlay_seeds(info)
        if not seeds_text:
            raise LaunchError("Overlay seeds directory is not set.")
        seeds = repo.resolve(seeds_text)
        if not seeds.is_dir():
            raise LaunchError(f"Overlay seeds directory not found: {seeds}")
        argv.extend(["--overlays", "--overlay-seeds", str(seeds)])
    argv.extend(["--shards", str(settings.shards)])
    if settings.emit_only:
        argv.append("--emit-only")
    else:
        recompiler = repo.recompiler_path
        if require_tools and not recompiler.is_file():
            raise LaunchError(
                "The recompiler is not built. Run Setup -> Bootstrap recompiler, "
                "or enable Emit only to skip recompilation."
            )
        argv.extend(["--recompiler", str(recompiler)])
    note = "Recompiles the selected ROM into title banks. This can take several minutes."
    return Command(
        "Generate title banks",
        argv,
        repo.root,
        note=note,
        meta={"out_dir": str(output), "rom": str(rom)},
    )


def _split_windows_args(text: str) -> list[str]:
    import ctypes

    shell32 = ctypes.WinDLL("shell32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    parse = shell32.CommandLineToArgvW
    parse.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    parse.restype = ctypes.POINTER(ctypes.c_wchar_p)
    argc = ctypes.c_int()
    argv = parse("pkmng5pc " + text, ctypes.byref(argc))
    if not argv:
        raise OSError(ctypes.get_last_error(), "could not parse extra arguments")
    try:
        return [argv[index] for index in range(1, argc.value)]
    finally:
        kernel32.LocalFree.argtypes = [ctypes.c_void_p]
        kernel32.LocalFree.restype = ctypes.c_void_p
        kernel32.LocalFree(ctypes.cast(argv, ctypes.c_void_p))


def _extra_tokens(text: str) -> list[str]:
    if not text.strip():
        return []
    if os.name == "nt":
        try:
            return _split_windows_args(text)
        except OSError as error:
            raise LaunchError(f"Extra arguments: {error}") from error
    try:
        return shlex.split(text, posix=True)
    except ValueError as error:
        raise LaunchError(f"Extra arguments: {error}") from error


def play_command(repo: Repo, settings: Settings, require_artifacts: bool = True) -> Command:
    rom, info = _rom_info_or_error(repo, settings)
    banks = repo.resolve(settings.banks_dir)
    if require_artifacts and not Repo.has_banks(banks):
        raise LaunchError(f"No generated title banks in {banks}. Generate them on the Setup page.")
    runner, runner_preset = repo.find_runner(settings.preset)
    if runner is None:
        if require_artifacts:
            raise LaunchError(
                "No built runner found. Build page -> enable Build runner -> Configure & build."
            )
        runner = repo.runner_path(settings.preset)
        runner_preset = settings.preset
    script = repo.play_script
    _require_file(script, "play script")
    _require_file(repo.frontend_config, "frontend cartridge config")
    extra = _extra_tokens(settings.extra_args)

    save_path = ""
    if settings.save_mode == "custom":
        if not settings.save_path.strip():
            raise LaunchError("Save mode is Custom but no save file is set.")
        save_path = str(repo.resolve(settings.save_path))
        parent = Path(save_path).parent
        if not parent.is_dir():
            raise LaunchError(f"Save file directory does not exist: {parent}")
    elif settings.save_mode == "default":
        save_path = str(info.save_path)

    if os.name == "nt":
        powershell = find_powershell()
        if not powershell:
            raise LaunchError("PowerShell was not found on PATH")
        argv = [
            powershell,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(script),
            "-Rom",
            str(rom),
            "-Runner",
            str(runner),
            "-Banks",
            str(banks),
        ]
        if settings.play_native:
            argv.append("-Native")
        elif settings.play_layout == "separate":
            argv.extend(["-Layout", "separate"])
        if settings.frames > 0:
            argv.extend(["-Frames", str(settings.frames)])
        if settings.save_mode == "off":
            argv.append("-NoSave")
        elif settings.save_mode == "custom":
            argv.extend(["-Sav", save_path])
        if settings.stats:
            argv.append("-Stats")
        if settings.coverage:
            argv.append("-Coverage")
        if settings.diagnostics:
            argv.append("-Diagnostics")
        if extra:
            argv.append("-Extra")
            argv.extend(extra)
    else:
        argv = [
            "sh",
            str(script),
            "--rom",
            str(rom),
            "--runner",
            str(runner),
            "--banks",
            str(banks),
        ]
        if settings.play_native:
            argv.append("--native")
        elif settings.play_layout == "separate":
            argv.extend(["--layout", "separate"])
        if settings.frames > 0:
            argv.extend(["--frames", str(settings.frames)])
        if settings.save_mode == "off":
            argv.append("--no-save")
        elif settings.save_mode == "custom":
            argv.extend(["--sav", save_path])
        if settings.stats:
            argv.append("--stats")
        if settings.coverage:
            argv.append("--coverage")
        if settings.diagnostics:
            argv.append("--diagnostics")
        if extra:
            argv.append("--")
            argv.extend(extra)
    note = f"Runner from {runner_preset or settings.preset}; close the game window to return here."
    return Command(
        "Play",
        argv,
        repo.root,
        note=note,
        meta={"save": save_path, "runner": str(runner)},
    )


def configure_and_build_commands(
    repo: Repo, settings: Settings, rom_sha1: str | None
) -> list[Command]:
    return [
        configure_command(repo, settings, rom_sha1),
        build_command(repo, settings, require_configured=False),
    ]


def quickstart_commands(repo: Repo, settings: Settings, rom_sha1: str | None) -> list[Command]:
    _rom_info_or_error(repo, settings)
    banks_missing = not Repo.has_banks(repo.resolve(settings.banks_dir))
    runner, _ = repo.find_runner(settings.preset)
    commands: list[Command] = []

    def note(command: Command, text: str) -> Command:
        command.note = f"Quickstart: {text}" + (f" {command.note}" if command.note else "")
        return command

    if banks_missing:
        if not repo.bankgen_path(settings.preset).is_file():
            tools = replace(settings, build_runner=False, build_tools=True)
            commands.append(
                note(
                    configure_command(repo, tools, rom_sha1, require_banks=False),
                    "configure host tools before generating banks.",
                )
            )
            commands.append(
                note(
                    build_command(repo, tools, require_configured=False),
                    "build pkmn_bankgen for the selected preset.",
                )
            )
        bank_settings = replace(settings, bank_out=settings.banks_dir, emit_only=False)
        commands.append(
            note(
                bankgen_command(repo, bank_settings, require_tools=False),
                "generate the missing title banks into the runner banks directory.",
            )
        )
    if banks_missing or runner is None:
        if not repo.recompiler_path.is_file():
            commands.insert(
                0,
                note(
                    bootstrap_command(repo, settings),
                    "the recompiler is missing and the runner must be rebuilt.",
                ),
            )
        runner_settings = settings if settings.build_runner else replace(settings, build_runner=True)
        commands.append(
            note(
                configure_command(repo, runner_settings, rom_sha1, require_banks=False),
                "wire the runner with the current build options.",
            )
        )
        commands.append(
            note(
                build_command(repo, runner_settings, require_configured=False),
                "build the runner used by Play.",
            )
        )
    commands.append(
        note(
            play_command(repo, settings, require_artifacts=False),
            "prerequisites are ready; the game window opens after this step.",
        )
    )
    return commands


def save_label(settings: Settings, repo: Repo, info: RomInfo | None) -> str:
    if settings.save_mode == "off":
        return "off (blank cart, nothing written)"
    if settings.save_mode == "custom":
        return str(repo.resolve(settings.save_path)) if settings.save_path.strip() else "(not set)"
    return str(info.save_path) if info else "(select a ROM)"


LINE_BREAK_RE = re.compile(rb"\r\n|\n|\r")


class Job:
    def __init__(self, commands: list[Command], title: str = ""):
        self.commands = commands
        self.title = title or (commands[0].title if commands else "Command")
        self.lines: deque[str] = deque(maxlen=5000)
        self.revision = 0
        self.status = "running"
        self.returncode: int | None = None
        self.current = 0
        self.current_label = commands[0].title if commands else ""
        self.started_at = time.monotonic()
        self.finished_at: float | None = None
        self.meta: dict[str, Any] = dict(commands[0].meta) if commands else {}
        self.consumed = False
        self._cancel = threading.Event()
        self._done = threading.Event()
        self._process: subprocess.Popen[bytes] | None = None
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self._thread.start()

    def wait(self, timeout: float | None = None) -> bool:
        return self._done.wait(timeout)

    @property
    def active(self) -> bool:
        return self.status == "running"

    @property
    def elapsed(self) -> float:
        end = self.finished_at if self.finished_at is not None else time.monotonic()
        return max(0.0, end - self.started_at)

    def cancel(self) -> None:
        if not self.active:
            return
        self._cancel.set()
        self._append_line("· cancellation requested")
        process = self._process
        if process is None or process.poll() is not None:
            return
        try:
            if os.name == "nt":
                subprocess.run(
                    ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    check=False,
                )
            else:
                os.killpg(os.getpgid(process.pid), signal.SIGTERM)
        except (OSError, ProcessLookupError):
            pass

    def _append_line(self, text: str) -> None:
        text = strip_ansi(text).replace("\t", "    ")
        self.lines.append(text)
        self.revision += 1

    def _run(self) -> None:
        try:
            for index, command in enumerate(self.commands):
                self.current = index
                self.current_label = command.title
                self._append_line("")
                self._append_line(f"$ {command.preview()}")
                if command.note:
                    self._append_line(f"  {command.note}")
                if self._cancel.is_set():
                    self.status = "cancelled"
                    break
                kwargs: dict[str, Any] = {}
                if os.name == "nt":
                    kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
                else:
                    kwargs["start_new_session"] = True
                environment = os.environ.copy()
                environment.update({"PYTHONUNBUFFERED": "1", "CLICOLOR": "0", "NO_COLOR": "1"})
                environment.update(command.env)
                process = subprocess.Popen(
                    command.argv,
                    cwd=str(command.cwd),
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    env=environment,
                    bufsize=0,
                    **kwargs,
                )
                self._process = process
                try:
                    self._pump(process)
                except Exception:
                    if process.poll() is None:
                        try:
                            process.kill()
                        except OSError:
                            pass
                    raise
                finally:
                    returncode = process.wait()
                    self._process = None
                self.returncode = returncode
                if self._cancel.is_set():
                    self.status = "cancelled"
                    break
                if returncode != 0:
                    self._append_line(f"· exited with code {returncode}")
                    self.status = "failed"
                    break
            else:
                self.status = "done"
        except OSError as error:
            self._append_line(f"· could not start command: {error}")
            self.status = "failed"
        except Exception as error:  # keep the launcher alive on unexpected subprocess errors
            self._append_line(f"· internal error: {error}")
            self.status = "failed"
        finally:
            self.finished_at = time.monotonic()
            self._append_line(f"· {self.status} in {self.elapsed:.1f}s")
            self._done.set()

    def _pump(self, process: subprocess.Popen[bytes]) -> None:
        assert process.stdout is not None
        buffer = b""
        try:
            while True:
                chunk = process.stdout.read(4096)
                if not chunk:
                    break
                buffer += chunk
                parts = LINE_BREAK_RE.split(buffer)
                buffer = parts.pop()
                for part in parts:
                    decoded = part.decode("utf-8", "replace")
                    if decoded.strip():
                        self._append_line(decoded)
            if buffer:
                decoded = buffer.decode("utf-8", "replace")
                if decoded.strip():
                    self._append_line(decoded)
        finally:
            process.stdout.close()
