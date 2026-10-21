"""Host system and Vulkan adapter information for the launcher."""

from __future__ import annotations

import ctypes
import os
import platform
import subprocess
from dataclasses import dataclass
from pathlib import Path

from tui.model import platform_label


@dataclass(frozen=True)
class GpuSpec:
    index: int
    name: str
    kind: str
    api: str
    memory_mib: int
    driver: str


GPU_RANK = {"discrete": 4, "integrated": 2, "virtual": 1, "other": 1, "cpu": 0}
GPU_POLICIES = ("", "auto", "discrete", "integrated")


def cpu_name() -> str:
    if os.name == "nt":
        try:
            import winreg

            with winreg.OpenKey(
                winreg.HKEY_LOCAL_MACHINE,
                r"HARDWARE\DESCRIPTION\System\CentralProcessor\0",
            ) as key:
                value, _ = winreg.QueryValueEx(key, "ProcessorNameString")
                text = " ".join(str(value).split())
                if text:
                    return text
        except (OSError, ImportError):
            pass
    try:
        for line in Path("/proc/cpuinfo").read_text(
            encoding="utf-8", errors="replace"
        ).splitlines():
            if line.lower().startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or platform.machine() or "unknown"


class _MemoryStatusEx(ctypes.Structure):
    _fields_ = [
        ("dwLength", ctypes.c_ulong),
        ("dwMemoryLoad", ctypes.c_ulong),
        ("ullTotalPhys", ctypes.c_ulonglong),
        ("ullAvailPhys", ctypes.c_ulonglong),
        ("ullTotalPageFile", ctypes.c_ulonglong),
        ("ullAvailPageFile", ctypes.c_ulonglong),
        ("ullTotalVirtual", ctypes.c_ulonglong),
        ("ullAvailVirtual", ctypes.c_ulonglong),
        ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
    ]


def total_memory_bytes() -> int:
    if os.name == "nt":
        status = _MemoryStatusEx()
        status.dwLength = ctypes.sizeof(status)
        try:
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
                return int(status.ullTotalPhys)
        except (AttributeError, OSError):
            pass
    try:
        return int(os.sysconf("SC_PAGE_SIZE")) * int(os.sysconf("SC_PHYS_PAGES"))
    except (AttributeError, OSError, ValueError):
        return 0


def format_bytes(value: int) -> str:
    if value <= 0:
        return "unknown"
    gib = value / (1024**3)
    if gib >= 1:
        return f"{gib:.1f} GiB"
    mib = value / (1024**2)
    return f"{mib:.0f} MiB"


def list_gpus(tool: Path | str | None, timeout: float = 5.0) -> list[GpuSpec]:
    if tool is None:
        return []
    path = Path(tool)
    if not path.is_file():
        return []
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) if os.name == "nt" else 0
    try:
        result = subprocess.run(
            [str(path), "--list-gpus"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            check=False,
            creationflags=flags,
        )
    except (OSError, subprocess.SubprocessError):
        return []
    return parse_gpu_tsv(result.stdout)


def parse_gpu_tsv(text: str) -> list[GpuSpec]:
    gpus: list[GpuSpec] = []
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) < 6:
            continue
        try:
            index = int(fields[0])
            memory_mib = int(fields[4])
        except ValueError:
            continue
        gpus.append(
            GpuSpec(
                index=index,
                name=fields[1],
                kind=fields[2],
                api=fields[3],
                memory_mib=memory_mib,
                driver=fields[5],
            )
        )
    return gpus


def gpu_value(spec: GpuSpec) -> str:
    return f"index:{spec.index}"


def gpu_label(spec: GpuSpec) -> str:
    return f"#{spec.index} {spec.name} ({spec.kind})"


def _best(specs: list[GpuSpec]) -> GpuSpec | None:
    if not specs:
        return None
    return max(
        specs,
        key=lambda spec: (GPU_RANK.get(spec.kind.lower(), 1), spec.memory_mib),
    )


def resolve_gpu(value: str, gpus: list[GpuSpec]) -> GpuSpec | None:
    if not gpus:
        return None
    preference = value.strip().lower()
    if preference in ("", "auto"):
        return _best(gpus)
    if preference in ("discrete", "integrated"):
        matches = [spec for spec in gpus if spec.kind.lower() == preference]
        return _best(matches) or _best(gpus)
    if preference.startswith("index:"):
        preference = preference[6:]
    if preference.isascii() and preference.isdigit():
        index = int(preference)
        return next((spec for spec in gpus if spec.index == index), _best(gpus))
    named = [spec for spec in gpus if preference in spec.name.lower()]
    return _best(named) or _best(gpus)


def selected_gpu_text(value: str, gpus: list[GpuSpec]) -> str:
    if value in ("", "auto"):
        base = "Auto (prefer discrete)"
    elif value == "discrete":
        base = "Discrete only"
    elif value == "integrated":
        base = "Integrated only"
    else:
        for spec in gpus:
            if gpu_value(spec) == value or str(spec.index) == value:
                return gpu_label(spec)
        named = [spec for spec in gpus if value.strip().lower() in spec.name.lower()]
        if named:
            spec = _best(named)
            return f"{value}  ·  {spec.name}" if spec is not None else value
        return value
    spec = resolve_gpu(value, gpus)
    if spec is not None:
        return f"{base}  ·  {spec.name}"
    return base


def system_specs_lines(
    *,
    preset: str,
    layout: str,
    native: bool,
    gpu_preference: str,
    gpus: list[GpuSpec],
    render_tool: Path | None,
) -> list[str]:
    lines = [
        f"CPU: {cpu_name()}",
        f"Memory: {format_bytes(total_memory_bytes())}",
        f"OS: {platform_label()} ({platform.machine() or 'unknown'})",
        f"Preset: {preset}",
        f"Presentation: {'native present (Vulkan)' if native else 'runner SDL blit'}",
        f"Layout: {layout}",
        f"GPU preference: {selected_gpu_text(gpu_preference, gpus)}",
    ]
    if gpus:
        lines.append("Vulkan adapters:")
        for spec in gpus:
            lines.append(
                f"  #{spec.index} {spec.name}  ·  {spec.kind}  ·  "
                f"{format_bytes(spec.memory_mib * 1024**2)}  ·  "
                f"Vulkan {spec.api}  ·  driver {spec.driver}"
            )
    elif render_tool is None:
        lines.append("Vulkan adapters: render smoke tool not built")
    else:
        lines.append("Vulkan adapters: none reported")
    if native:
        lines.append("The Vulkan adapter choice applies to Native present.")
    else:
        lines.append(
            "The runner's SDL path has no adapter-selection hook; "
            "GPU choice applies to Native present."
        )
    return lines
