"""Project, ROM and persisted-settings model for the launcher."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import sys
from dataclasses import dataclass, asdict, fields
from pathlib import Path

TITLE_BY_CODE: dict[str, tuple[str, str]] = {
    "IRBO": ("black", "Pokémon Black"),
    "IRAO": ("white", "Pokémon White"),
    "IREO": ("black2", "Pokémon Black 2"),
    "IRDO": ("white2", "Pokémon White 2"),
}
ROM_EXTENSIONS = (".nds", ".srl")
PLAY_LAYOUTS = ("stacked", "separate")
SAVE_MODES = ("default", "custom", "off")
PRESET_FLAVOURS = ("relwithdebinfo", "release", "debug")
STATE_VERSION = 1


def host_family() -> str:
    return "windows" if os.name == "nt" else "linux"


def platform_label() -> str:
    if os.name == "nt":
        return f"Windows {sys.getwindowsversion().major}"
    return sys.platform


def preferred_preset_names() -> list[str]:
    return [f"{host_family()}-{flavour}" for flavour in PRESET_FLAVOURS]


def default_preset(presets: list[str]) -> str:
    for name in preferred_preset_names():
        if name in presets:
            return name
    return presets[0] if presets else ""


@dataclass(frozen=True)
class RomInfo:
    path: Path
    title: str
    code: str
    size: int
    title_id: str | None

    @property
    def supported(self) -> bool:
        return self.title_id is not None

    @property
    def display_name(self) -> str:
        if self.title_id:
            return TITLE_BY_CODE[self.code][1]
        return self.title or self.path.name

    @property
    def save_path(self) -> Path:
        return self.path.with_suffix(".sav")


def _clean_ascii(raw: bytes) -> str:
    text = raw.decode("ascii", "replace")
    return "".join(ch if ch.isprintable() else " " for ch in text).strip()


def read_rom_info(path: Path | str) -> RomInfo | None:
    path = Path(path)
    try:
        with path.open("rb") as handle:
            header = handle.read(0x20)
        size = path.stat().st_size
    except OSError:
        return None
    if len(header) < 0x10:
        return None
    title = _clean_ascii(header[0x00:0x0C].split(b"\x00", 1)[0])
    code = _clean_ascii(header[0x0C:0x10])
    if len(code) != 4 or not code.isalnum():
        code = ""
    title_id = TITLE_BY_CODE.get(code, (None, ""))[0]
    return RomInfo(path=path, title=title, code=code, size=size, title_id=title_id)


def scan_roms(folder: Path | str) -> list[RomInfo]:
    folder = Path(folder)
    if not folder.is_dir():
        return []
    found: list[RomInfo] = []
    try:
        children = sorted(folder.iterdir(), key=lambda item: item.name.casefold())
    except OSError:
        return []
    for child in children:
        if not child.is_file() or child.suffix.lower() not in ROM_EXTENSIONS:
            continue
        info = read_rom_info(child)
        if info:
            found.append(info)
    return found


def sha1_file(path: Path | str, chunk_size: int = 1 << 20) -> str:
    digest = hashlib.sha1()
    with Path(path).open("rb") as handle:
        while True:
            block = handle.read(chunk_size)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def describe_size(size: int) -> str:
    if size >= 1 << 30:
        return f"{size / (1 << 30):.2f} GiB"
    if size >= 1 << 20:
        return f"{size / (1 << 20):.0f} MiB"
    if size >= 1 << 10:
        return f"{size / (1 << 10):.0f} KiB"
    return f"{size} B"


def clean_path_text(text: str) -> str:
    text = text.strip()
    if len(text) >= 2 and text[0] == text[-1] and text[0] in {'"', "'"}:
        text = text[1:-1]
    return os.path.expandvars(os.path.expanduser(text))


def title_identity(info: RomInfo | None) -> tuple[str | None, str]:
    if not info:
        return None, "custom"
    if info.title_id:
        return info.title_id, info.title_id
    if info.code:
        return None, re.sub(r"[^a-z0-9]+", "", info.code.lower()) or "custom"
    return None, "custom"


def default_bank_out(info: RomInfo | None) -> str:
    _, label = title_identity(info)
    return f"banks/{label}"


def default_title_config(info: RomInfo | None) -> str:
    if info and info.title_id:
        return f"re/config/{info.title_id}.toml"
    return ""


def default_overlay_seeds(info: RomInfo | None) -> str:
    if info and info.title_id:
        return f"re/config/{info.title_id}/seeds"
    return ""


def load_presets(path: Path, fallback: list[str] | None = None) -> list[str]:
    names: list[str] = []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        data = {}
    for entry in data.get("configurePresets", []):
        if not isinstance(entry, dict):
            continue
        name = entry.get("name")
        if not isinstance(name, str) or entry.get("hidden"):
            continue
        if name.startswith(f"{host_family()}-"):
            names.append(name)
    if not names:
        names = list(fallback or preferred_preset_names())
    seen: set[str] = set()
    return [name for name in names if not (name in seen or seen.add(name))]


class Repo:
    def __init__(self, root: Path | str):
        self.root = Path(root).resolve()

    @classmethod
    def discover(cls) -> "Repo":
        return cls(Path(__file__).resolve().parent.parent)

    @property
    def presets(self) -> list[str]:
        return load_presets(self.root / "CMakePresets.json")

    @property
    def default_preset(self) -> str:
        return default_preset(self.presets)

    def resolve(self, value: Path | str) -> Path:
        text = clean_path_text(str(value))
        path = Path(text)
        if not path.is_absolute():
            path = self.root / path
        return path.resolve(strict=False)

    def store(self, path: Path | str) -> str:
        target = Path(path).resolve(strict=False)
        try:
            return target.relative_to(self.root).as_posix()
        except ValueError:
            return str(target)

    def display(self, value: Path | str) -> str:
        return self.store(self.resolve(value))

    def build_dir(self, preset: str) -> Path:
        return self.root / "build" / preset

    def cache_path(self, preset: str) -> Path:
        return self.build_dir(preset) / "CMakeCache.txt"

    def bankgen_path(self, preset: str) -> Path:
        suffix = ".exe" if os.name == "nt" else ""
        return self.build_dir(preset) / "tools" / f"pkmn_bankgen{suffix}"

    def runner_path(self, preset: str) -> Path:
        suffix = ".exe" if os.name == "nt" else ""
        return (
            self.build_dir(preset)
            / "third_party"
            / "ndsrecomp"
            / "runner"
            / f"nds_runner{suffix}"
        )

    def find_runner(self, preset: str) -> tuple[Path | None, str | None]:
        order: list[str] = []
        for name in [preset, *preferred_preset_names()]:
            if name and name not in order:
                order.append(name)
        for name in order:
            candidate = self.runner_path(name)
            if candidate.is_file():
                return candidate, name
        for candidate in sorted(
            (self.root / "build").glob("*/third_party/ndsrecomp/runner/nds_runner*")
        ):
            if candidate.is_file():
                return candidate, candidate.parents[3].name
        return None, None

    @property
    def recompiler_path(self) -> Path:
        suffix = ".exe" if os.name == "nt" else ""
        return (
            self.root
            / "third_party"
            / "ndsrecomp"
            / "recompiler"
            / "build"
            / f"nds_recompile{suffix}"
        )

    @property
    def play_script(self) -> Path:
        name = "play.ps1" if os.name == "nt" else "play.sh"
        return self.root / "tools" / name

    @property
    def bootstrap_script(self) -> Path:
        name = "bootstrap_ndsrecomp.ps1" if os.name == "nt" else "bootstrap_ndsrecomp.sh"
        return self.root / "tools" / name

    @property
    def frontend_config(self) -> Path:
        return self.root / "re" / "config" / "frontend.toml"

    def title_config(self, title_id: str | None) -> Path | None:
        if not title_id:
            return None
        path = self.root / "re" / "config" / f"{title_id}.toml"
        return path if path.is_file() else None

    def read_cache(self, preset: str) -> dict[str, str]:
        values: dict[str, str] = {}
        try:
            lines = self.cache_path(preset).read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            return values
        for line in lines:
            if not line or line.startswith(("#", "//")):
                continue
            key, separator, value = line.partition("=")
            if not separator:
                continue
            values[key.split(":", 1)[0]] = value
        return values

    @staticmethod
    def bank_files(folder: Path | str) -> list[Path]:
        folder = Path(folder)
        if not folder.is_dir():
            return []
        return sorted(folder.glob("*_[0-9][0-9].c"))

    @classmethod
    def has_banks(cls, folder: Path | str) -> bool:
        return bool(cls.bank_files(folder))


@dataclass
class Settings:
    preset: str = ""
    rom_dir: str = "roms"
    rom_path: str = ""
    build_tests: bool = True
    build_tools: bool = True
    build_render: bool = True
    build_runner: bool = False
    native_present: bool = True
    banks_dir: str = "banks/black"
    extra_bank_dirs: str = ""
    extra_sha1s: str = ""
    play_layout: str = "stacked"
    play_native: bool = False
    save_mode: str = "default"
    save_path: str = ""
    frames: int = 0
    stats: bool = False
    coverage: bool = False
    diagnostics: bool = False
    extra_args: str = ""
    bootstrap_runner: bool = False
    bootstrap_sdl3: bool = False
    bootstrap_compiler: str = ""
    bank_out: str = ""
    title_config: str = ""
    shards: int = 8
    overlays: bool = False
    overlay_seeds: str = ""
    emit_only: bool = False

    @classmethod
    def load(cls, path: Path, presets: list[str]) -> "Settings":
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            data = {}
        if not isinstance(data, dict):
            data = {}
        defaults = cls()
        settings = cls()
        for field in fields(cls):
            if field.name not in data:
                continue
            value = data[field.name]
            default_value = getattr(defaults, field.name)
            try:
                if isinstance(default_value, bool):
                    value = bool(value)
                elif isinstance(default_value, int):
                    value = int(value)
                elif isinstance(default_value, str):
                    value = str(value)
            except (TypeError, ValueError):
                continue
            setattr(settings, field.name, value)
        settings.normalize(presets)
        return settings

    def normalize(self, presets: list[str]) -> None:
        if self.preset not in presets:
            self.preset = default_preset(presets)
        if not self.rom_dir:
            self.rom_dir = "roms"
        if not self.banks_dir:
            self.banks_dir = "banks/black"
        if self.play_layout not in PLAY_LAYOUTS:
            self.play_layout = "stacked"
        if self.save_mode not in SAVE_MODES:
            self.save_mode = "default"
        self.frames = max(0, min(int(self.frames), 10_000_000))
        self.shards = max(2, min(int(self.shards), 64))

    def save(self, path: Path) -> None:
        payload = {"version": STATE_VERSION, **asdict(self)}
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix(path.suffix + ".tmp")
        temporary.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        try:
            temporary.replace(path)
        except OSError:
            path.write_text(temporary.read_text(encoding="utf-8"), encoding="utf-8")
            temporary.unlink(missing_ok=True)


def default_state_path() -> Path:
    override = os.environ.get("PKMNG5PC_TUI_STATE")
    if override:
        return Path(clean_path_text(override)).resolve(strict=False)
    return Path(__file__).resolve().parent / ".state.json"


def find_cmake() -> str | None:
    return shutil.which("cmake")


def find_ctest() -> str | None:
    return shutil.which("ctest")


def find_powershell() -> str | None:
    for name in ("pwsh", "powershell"):
        candidate = shutil.which(name)
        if candidate:
            return candidate
    return None


def cache_bool(cache: dict[str, str], key: str) -> bool | None:
    value = cache.get(key)
    if value is None:
        return None
    return value.upper() in {"ON", "TRUE", "1", "YES"}


def same_path(left: Path | str, right: Path | str) -> bool:
    return os.path.normcase(str(Path(left).resolve(strict=False))) == os.path.normcase(
        str(Path(right).resolve(strict=False))
    )


def split_paths(value: str) -> list[str]:
    return [part.strip() for part in value.split(";") if part.strip()]
