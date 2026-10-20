#!/usr/bin/env python3
"""Entry point for the pkmng5pc terminal launcher."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tui.app import PAGE_ORDER, App
from tui.model import (  # noqa: E402
    Repo,
    Settings,
    default_state_path,
    describe_size,
    host_family,
    platform_label,
    preferred_preset_names,
    read_rom_info,
    scan_roms,
)
from tui.terminal import Terminal, TerminalError  # noqa: E402

VERSION = "1.0"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Terminal launcher for pkmng5pc: ROM selection, build options, bank setup and play flags."
    )
    parser.add_argument("--version", action="version", version=f"pkmn tui {VERSION}")
    parser.add_argument(
        "--state",
        type=Path,
        default=default_state_path(),
        help="settings file (default: tui/.state.json)",
    )
    parser.add_argument("--rom", help="select this ROM for the session")
    parser.add_argument("--preset", help="select this build preset for the session")
    parser.add_argument("--no-color", action="store_true", help="disable ANSI colors")
    parser.add_argument("--ascii", action="store_true", help="use ASCII glyphs only")
    parser.add_argument(
        "--snapshot",
        nargs="?",
        const="home",
        choices=PAGE_ORDER,
        metavar="PAGE",
        help="render one page to stdout and exit (home, rom, build, play, setup, help)",
    )
    parser.add_argument("--width", type=int, default=100, help="snapshot width (default: 100)")
    parser.add_argument("--height", type=int, default=32, help="snapshot height (default: 32)")
    parser.add_argument(
        "--check",
        action="store_true",
        help="print detected tools, ROM, banks and runner paths, then exit",
    )
    return parser


def load_settings(args: argparse.Namespace, repo: Repo) -> Settings:
    settings = Settings.load(args.state, repo.presets)
    if args.rom:
        path = repo.resolve(args.rom)
        info = read_rom_info(path)
        if info is None:
            raise SystemExit(f"not a readable NDS ROM: {path}")
        settings.rom_path = repo.store(path)
        settings.rom_dir = repo.store(path.parent)
    if args.preset:
        if args.preset not in repo.presets:
            raise SystemExit(
                f"unknown preset: {args.preset}\nknown: {', '.join(repo.presets)}"
            )
        settings.preset = args.preset
    settings.normalize(repo.presets)
    return settings


def check(repo: Repo, settings: Settings, state_path: Path) -> int:
    rom_dir = repo.resolve(settings.rom_dir)
    info = read_rom_info(repo.resolve(settings.rom_path)) if settings.rom_path else None
    source = "selected"
    roms = scan_roms(rom_dir)
    if info is None and roms:
        info = next((rom for rom in roms if rom.code == "IRBO"), None)
        if info is None:
            info = next((rom for rom in roms if rom.supported), roms[0])
        source = "auto"
    banks = repo.resolve(settings.banks_dir)
    runner, runner_preset = repo.find_runner(settings.preset)
    bankgen = repo.bankgen_path(settings.preset)
    print(f"root        : {repo.root}")
    print(f"platform    : {platform_label()} ({host_family()})")
    print(f"python      : {sys.version.split()[0]}")
    print(f"presets     : {', '.join(repo.presets) or '(none)'}")
    print(f"selected    : {settings.preset}")
    print(f"preferred   : {', '.join(preferred_preset_names())}")
    print(f"rom dir     : {rom_dir}  ({len(roms)} ROM(s))")
    print(f"rom         : {info.path if info else '(none)'}" + (f"  ({source})" if info else ""))
    if info:
        print(
            f"rom title   : {info.display_name}  code={info.code or '????'}  "
            f"{describe_size(info.size)}"
        )
    print(f"bank dir    : {banks}  ({len(Repo.bank_files(banks))} shard files)")
    print(f"bankgen     : {bankgen}  ({'yes' if bankgen.is_file() else 'no'})")
    print(f"runner      : {runner or '(none)'}" + (f"  ({runner_preset})" if runner else ""))
    print(f"recompiler  : {repo.recompiler_path}  ({'yes' if repo.recompiler_path.is_file() else 'no'})")
    print(f"state       : {state_path}")
    print(f"cmake       : {_which('cmake') or '(not found)'}")
    print(f"ctest       : {_which('ctest') or '(not found)'}")
    return 0 if info else 1


def _which(name: str) -> str | None:
    import shutil

    return shutil.which(name)


def configure_stdout() -> None:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    repo = Repo.discover()
    if not (repo.root / "CMakePresets.json").is_file():
        print(f"not a pkmng5pc checkout: {repo.root}", file=sys.stderr)
        return 1
    try:
        settings = load_settings(args, repo)
    except SystemExit as error:
        print(error, file=sys.stderr)
        return 1
    if args.check:
        configure_stdout()
        return check(repo, settings, args.state)

    if args.snapshot:
        configure_stdout()
    terminal = Terminal(color=not args.no_color, ascii_only=args.ascii)
    if args.snapshot:
        app = App(repo, settings, terminal, args.state, persist=False)
        app.set_page(args.snapshot)
        lines = app.render(max(60, args.width), max(18, args.height))
        sys.stdout.write("\n".join(lines) + "\n")
        return 0
    try:
        app = App(repo, settings, terminal, args.state)
        app.run()
    except TerminalError as error:
        print(f"launcher: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
