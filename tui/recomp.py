"""Read recompilation and interpreter-coverage state from local artifacts."""

from __future__ import annotations

import json
import re
import time
from dataclasses import dataclass, field
from pathlib import Path

FUNCTION_TOTAL_RE = re.compile(r"Total functions:\s*(\d+)")
FUNCTION_DECL_RE = re.compile(r"\b\w+_[at]func_[0-9A-Fa-f]+\s*\(void\);")
DISPATCH_LEN_RE = re.compile(r"g_dispatch_\w+_len\s*=\s*(\d+)u")
RUN_STAMP_RE = re.compile(r"(\d{8}-\d{6})")


@dataclass(frozen=True)
class BankGroup:
    bank_id: str
    cpu: str
    functions: int
    dispatch_entries: int
    generated_at: float
    header: Path


@dataclass(frozen=True)
class BankStats:
    folder: Path
    groups: tuple[BankGroup, ...] = ()
    generated_at: float = 0.0

    @property
    def total_functions(self) -> int:
        return sum(group.functions for group in self.groups)

    @property
    def arm9_functions(self) -> int:
        return sum(group.functions for group in self.groups if group.cpu == "arm9")

    @property
    def arm7_functions(self) -> int:
        return sum(group.functions for group in self.groups if group.cpu == "arm7")

    @property
    def dispatch_entries(self) -> int:
        return sum(group.dispatch_entries for group in self.groups)

    @property
    def code(self) -> str | None:
        codes = {
            group.bank_id.split("_", 1)[0].upper()
            for group in self.groups
            if re.fullmatch(r"[A-Za-z0-9]{4}", group.bank_id.split("_", 1)[0])
        }
        return codes.pop() if len(codes) == 1 else None


@dataclass(frozen=True)
class CoverageStats:
    path: Path
    observed_at: float
    rom_sha1: str
    build_id: str
    run_stamp: str
    entries9: int
    entries7: int
    insns9: int
    insns7: int
    distinct9: int
    distinct7: int

    @property
    def entries(self) -> int:
        return self.entries9 + self.entries7

    @property
    def insns(self) -> int:
        return self.insns9 + self.insns7

    @property
    def distinct(self) -> int:
        return self.distinct9 + self.distinct7


@dataclass(frozen=True)
class PerfStats:
    path: Path
    observed_at: float
    rom_sha1: str
    build_id: str
    run_stamp: str
    entries9: int
    entries7: int
    insns9: int
    insns7: int
    dispatched9: int
    dispatched7: int

    @property
    def entries(self) -> int:
        return self.entries9 + self.entries7

    @property
    def dispatches(self) -> int:
        return self.dispatched9 + self.dispatched7

    @property
    def interpreter_share(self) -> float | None:
        total = self.entries + self.dispatches
        if total <= 0:
            return None
        return 100.0 * self.entries / total


@dataclass(frozen=True)
class RecompStats:
    banks: BankStats
    coverage: CoverageStats | None = None
    perf: PerfStats | None = None
    warnings: tuple[str, ...] = field(default_factory=tuple)

    @property
    def interpreter_share(self) -> float | None:
        if self.coverage and self.perf:
            total = self.coverage.entries + self.perf.dispatches
            return 100.0 * self.coverage.entries / total if total else None
        return self.perf.interpreter_share if self.perf else None

    @property
    def observed_at(self) -> float | None:
        if self.coverage:
            return self.coverage.observed_at
        if self.perf:
            return self.perf.observed_at
        return None

    @property
    def build_id(self) -> str:
        if self.coverage and self.coverage.build_id:
            return self.coverage.build_id
        if self.perf:
            return self.perf.build_id
        return ""


def _read_header(path: Path) -> tuple[int, float]:
    try:
        stat = path.stat()
        head = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return 0, 0.0
    match = FUNCTION_TOTAL_RE.search(head[:16384])
    if match:
        return int(match.group(1)), stat.st_mtime
    return len(FUNCTION_DECL_RE.findall(head)), stat.st_mtime


def _read_dispatch_count(path: Path) -> int:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return 0
    match = DISPATCH_LEN_RE.search(text)
    return int(match.group(1)) if match else 0


def read_bank_stats(folder: Path | str) -> BankStats:
    folder = Path(folder)
    if not folder.is_dir():
        return BankStats(folder=folder)
    groups: list[BankGroup] = []
    try:
        headers = sorted(folder.glob("*.h"))
    except OSError:
        return BankStats(folder=folder)
    for header in headers:
        bank_id = header.stem
        cpu = "arm9" if "_arm9_" in bank_id else "arm7" if "_arm7_" in bank_id else ""
        if not cpu:
            continue
        functions, generated_at = _read_header(header)
        if functions <= 0:
            continue
        dispatch = _read_dispatch_count(folder / f"{bank_id}_dispatch.c")
        groups.append(
            BankGroup(
                bank_id=bank_id,
                cpu=cpu,
                functions=functions,
                dispatch_entries=dispatch,
                generated_at=generated_at,
                header=header,
            )
        )
    generated_at = max((group.generated_at for group in groups), default=0.0)
    return BankStats(folder=folder, groups=tuple(groups), generated_at=generated_at)


def _run_stamp(path: Path) -> str:
    match = RUN_STAMP_RE.search(path.name)
    return match.group(1) if match else ""


def _stamp_time(stamp: str) -> float | None:
    try:
        return time.mktime(time.strptime(stamp, "%Y%m%d-%H%M%S"))
    except (OverflowError, ValueError):
        return None


def _to_int(value: object) -> int:
    if isinstance(value, bool):
        return 0
    if isinstance(value, (int, float)):
        return int(value)
    if isinstance(value, str):
        try:
            return int(value.strip())
        except ValueError:
            return 0
    return 0


def _unique_points(rows: object) -> int:
    if not isinstance(rows, list):
        return 0
    points = set()
    for row in rows:
        if not isinstance(row, dict):
            continue
        address = row.get("addr")
        if isinstance(address, str):
            points.add((address.lower(), str(row.get("mode", ""))))
    return len(points)


def read_coverage(path: Path | str) -> CoverageStats | None:
    path = Path(path)
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        modified = path.stat().st_mtime
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict) or data.get("kind") != "ndsrecomp-tier3-coverage":
        return None
    run_stamp = _run_stamp(path)
    observed_at = _stamp_time(run_stamp) or modified
    static = data.get("static_coverage")
    static = static if isinstance(static, dict) else {}

    def number(key: str) -> int:
        return _to_int(static.get(key, 0))

    return CoverageStats(
        path=path,
        observed_at=observed_at,
        rom_sha1=str(data.get("rom_sha1") or ""),
        build_id=str(data.get("build_id") or ""),
        run_stamp=run_stamp,
        entries9=number("tier3_entries9"),
        entries7=number("tier3_entries7"),
        insns9=number("tier3_insns9"),
        insns7=number("tier3_insns7"),
        distinct9=_unique_points(data.get("entry_points_arm9")),
        distinct7=_unique_points(data.get("entry_points_arm7")),
    )


def read_performance(path: Path | str) -> PerfStats | None:
    path = Path(path)
    rom_sha1 = ""
    build_id = ""
    entries9 = entries7 = insns9 = insns7 = 0
    dispatched9 = dispatched7 = 0
    saw_perf = False
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None
    for line in lines:
        if not line.strip():
            continue
        try:
            data = json.loads(line)
        except ValueError:
            continue
        if not isinstance(data, dict):
            continue
        if data.get("kind") == "session":
            rom_sha1 = str(data.get("rom_sha1") or rom_sha1)
            build_id = str(data.get("build_id") or build_id)
            continue
        if data.get("kind") != "perf":
            continue
        saw_perf = True
        for cpu, entries_attr, insns_attr, dispatch_attr in (
            ("arm9", "entries9", "insns9", "dispatched9"),
            ("arm7", "entries7", "insns7", "dispatched7"),
        ):
            tier3 = data.get("tier3_delta")
            tier3 = tier3.get(cpu) if isinstance(tier3, dict) else None
            if isinstance(tier3, dict):
                entries = _to_int(tier3.get("entries", 0))
                instructions = _to_int(tier3.get("instructions", 0))
                if entries_attr == "entries9":
                    entries9 += entries
                    insns9 += instructions
                else:
                    entries7 += entries
                    insns7 += instructions
            dispatch = data.get("dispatch_delta")
            dispatch = dispatch.get(cpu) if isinstance(dispatch, dict) else None
            if isinstance(dispatch, dict):
                total = _to_int(dispatch.get("dispatch_total", 0))
                if dispatch_attr == "dispatched9":
                    dispatched9 += total
                else:
                    dispatched7 += total
    if not saw_perf:
        return None
    run_stamp = _run_stamp(path)
    try:
        modified = path.stat().st_mtime
    except OSError:
        modified = 0.0
    return PerfStats(
        path=path,
        observed_at=_stamp_time(run_stamp) or modified,
        rom_sha1=rom_sha1,
        build_id=build_id,
        run_stamp=run_stamp,
        entries9=entries9,
        entries7=entries7,
        insns9=insns9,
        insns7=insns7,
        dispatched9=dispatched9,
        dispatched7=dispatched7,
    )


def _newest(paths: list[Path]) -> list[Path]:
    def key(path: Path) -> tuple[str, float]:
        try:
            modified = path.stat().st_mtime
        except OSError:
            modified = 0.0
        return (_run_stamp(path), modified)

    return sorted(paths, key=key, reverse=True)


def _latest_coverage(paths: list[Path], sha1: str | None) -> CoverageStats | None:
    fallback: CoverageStats | None = None
    for path in _newest(paths)[:8]:
        item = read_coverage(path)
        if item is None:
            continue
        if sha1 and item.rom_sha1 == sha1:
            return item
        if fallback is None:
            fallback = item
    return fallback


def _latest_performance(
    paths: list[Path],
    sha1: str | None,
    build_id: str | None = None,
    run_stamp: str = "",
    require_stamp: bool = False,
) -> PerfStats | None:
    fallback: PerfStats | None = None
    for path in _newest(paths)[:8]:
        item = read_performance(path)
        if item is None:
            continue
        if sha1 and item.rom_sha1 != sha1:
            continue
        if build_id and item.build_id and item.build_id != build_id:
            continue
        if run_stamp and item.run_stamp == run_stamp:
            return item
        if require_stamp and run_stamp:
            continue
        if fallback is None:
            fallback = item
    return fallback


def collect_recomp_stats(
    root: Path | str,
    banks_dir: Path | str,
    sha1: str | None = None,
    selected_code: str | None = None,
    rom_dir: Path | str | None = None,
) -> RecompStats:
    root = Path(root)
    banks = read_bank_stats(banks_dir)
    play_dir = root / "banks" / "play"
    coverage_dirs = [play_dir]
    if rom_dir:
        coverage_dirs.append(Path(rom_dir))
    seen_dirs: set[Path] = set()
    coverage_files: list[Path] = []
    for folder in coverage_dirs:
        resolved = folder.resolve()
        if resolved in seen_dirs or not folder.is_dir():
            continue
        seen_dirs.add(resolved)
        try:
            candidates = list(folder.glob("*.json"))
        except OSError:
            continue
        coverage_files.extend(
            path for path in candidates if "coverage" in path.name.lower()
        )
    try:
        performance_files = (
            sorted(play_dir.glob("performance-*.jsonl")) if play_dir.is_dir() else []
        )
    except OSError:
        performance_files = []
    coverage = _latest_coverage(coverage_files, sha1)
    perf: PerfStats | None = None
    if coverage and (not sha1 or coverage.rom_sha1 == sha1):
        perf = _latest_performance(
            performance_files,
            coverage.rom_sha1 or sha1,
            coverage.build_id or None,
            coverage.run_stamp,
            require_stamp=bool(coverage.run_stamp),
        )
    elif not coverage:
        perf = _latest_performance(performance_files, sha1)

    warnings: list[str] = []
    if selected_code and banks.code and banks.code != selected_code:
        warnings.append(f"banks are for {banks.code}, selected ROM is {selected_code}")
    if sha1 and coverage and coverage.rom_sha1 and coverage.rom_sha1 != sha1:
        warnings.append("latest coverage is from a different ROM")
    if (
        coverage
        and banks.generated_at
        and coverage.observed_at + 1.0 < banks.generated_at
    ):
        warnings.append("coverage predates the current bank set")
    if perf and not coverage:
        warnings.append("coverage manifest missing; fallback targets unavailable")
    if (
        coverage
        and not perf
        and not (sha1 and coverage.rom_sha1 and coverage.rom_sha1 != sha1)
    ):
        warnings.append("no performance log matches the coverage run; interpreter share unavailable")
    if (
        coverage
        and perf
        and coverage.run_stamp
        and perf.run_stamp
        and coverage.run_stamp != perf.run_stamp
    ):
        warnings.append("performance log is from a different run than the coverage manifest")
    return RecompStats(
        banks=banks,
        coverage=coverage,
        perf=perf,
        warnings=tuple(warnings),
    )
