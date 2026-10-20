"""Recompilation bank, coverage and performance evidence tests."""

from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path

from tui.recomp import (
    collect_recomp_stats,
    read_bank_stats,
    read_coverage,
    read_performance,
)


def write_bank(
    folder: Path,
    bank_id: str,
    functions: int,
    dispatch_rows: int,
) -> None:
    (folder / f"{bank_id}_00.c").write_text("", encoding="utf-8")
    (folder / f"{bank_id}.h").write_text(
        f"/* Total functions: {functions} */\n", encoding="utf-8"
    )
    (folder / f"{bank_id}_dispatch.c").write_text(
        f"const unsigned g_dispatch_{bank_id}_len = {dispatch_rows}u;\n",
        encoding="utf-8",
    )


def write_coverage(
    folder: Path,
    name: str,
    sha1: str,
    build_id: str,
    entries9: int,
    entries7: int,
    insns9: int,
    insns7: int,
) -> Path:
    path = folder / name
    payload = {
        "kind": "ndsrecomp-tier3-coverage",
        "schema": 4,
        "rom_sha1": sha1,
        "build_id": build_id,
        "static_coverage": {
            "tier3_entries9": entries9,
            "tier3_entries7": entries7,
            "tier3_insns9": insns9,
            "tier3_insns7": insns7,
        },
        "entry_points_arm9": [
            {"addr": "0x02000000", "mode": "arm"},
            {"addr": "0x02000000", "mode": "arm"},
            {"addr": "0x02000004", "mode": "thumb"},
        ],
        "entry_points_arm7": [{"addr": "0x02380000", "mode": "arm"}],
    }
    path.write_text(json.dumps(payload), encoding="utf-8")
    return path


def set_bank_time(folder: Path, timestamp: float) -> None:
    for header in folder.glob("*.h"):
        os.utime(header, (timestamp, timestamp))


def write_performance(
    folder: Path,
    name: str,
    sha1: str,
    build_id: str,
    entries9: int,
    entries7: int,
    dispatched9: int,
    dispatched7: int,
) -> Path:
    path = folder / name
    rows = [
        {"kind": "session", "rom_sha1": sha1, "build_id": build_id},
        {
            "kind": "perf",
            "tier3_delta": {
                "arm9": {"entries": entries9, "instructions": 100},
                "arm7": {"entries": entries7, "instructions": 20},
            },
            "dispatch_delta": {
                "arm9": {"dispatch_total": dispatched9},
                "arm7": {"dispatch_total": dispatched7},
            },
        },
    ]
    path.write_text(
        "\n".join(json.dumps(row) for row in rows) + "\n", encoding="utf-8"
    )
    return path


class BankStatsTests(unittest.TestCase):
    def test_totals_come_from_headers_and_dispatch_lengths(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            write_bank(folder, "irbo_arm9_main", 12, 20)
            write_bank(folder, "irbo_arm7_main", 3, 5)

            stats = read_bank_stats(folder)

            self.assertEqual(stats.total_functions, 15)
            self.assertEqual(stats.arm9_functions, 12)
            self.assertEqual(stats.arm7_functions, 3)
            self.assertEqual(stats.dispatch_entries, 25)
            self.assertEqual(stats.code, "IRBO")
            self.assertEqual(len(stats.groups), 2)

    def test_header_without_marker_counts_function_declarations(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            (folder / "irbo_arm9_main.h").write_text(
                "void irbo_afunc_02000000(void);\n"
                "void irbo_afunc_02000004(void);\n"
                "void irbo_afunc_02000008(void);\n",
                encoding="utf-8",
            )

            stats = read_bank_stats(folder)

            self.assertEqual(stats.total_functions, 3)


class EvidenceTests(unittest.TestCase):
    def test_matching_coverage_and_performance_are_paired(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            banks = root / "banks" / "black"
            banks.mkdir(parents=True)
            write_bank(banks, "irbo_arm9_main", 12, 20)
            write_bank(banks, "irbo_arm7_main", 3, 5)
            set_bank_time(banks, 1_577_880_000)
            play = root / "banks" / "play"
            play.mkdir()
            sha1 = "a" * 40
            coverage_path = write_coverage(
                play,
                "game-coverage-20260101-120000.json",
                sha1,
                "build-one",
                entries9=10,
                entries7=5,
                insns9=100,
                insns7=20,
            )
            performance_path = write_performance(
                play,
                "performance-20260101-120000.jsonl",
                sha1,
                "build-one",
                entries9=10,
                entries7=5,
                dispatched9=990,
                dispatched7=100,
            )

            coverage = read_coverage(coverage_path)
            performance = read_performance(performance_path)
            stats = collect_recomp_stats(root, banks, sha1, "IRBO")

            self.assertIsNotNone(coverage)
            self.assertIsNotNone(performance)
            assert coverage is not None and performance is not None
            self.assertEqual(coverage.run_stamp, "20260101-120000")
            self.assertEqual(performance.run_stamp, "20260101-120000")
            self.assertEqual(coverage.distinct, 3)
            self.assertEqual(performance.dispatches, 1090)
            self.assertEqual(stats.banks.total_functions, 15)
            self.assertEqual(stats.coverage, coverage)
            self.assertEqual(stats.perf, performance)
            self.assertEqual(stats.build_id, "build-one")
            self.assertAlmostEqual(stats.interpreter_share or 0.0, 15 / 1105 * 100)
            self.assertEqual(stats.warnings, ())

    def test_coverage_written_next_to_the_rom_is_found(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            banks = root / "banks" / "black"
            banks.mkdir(parents=True)
            write_bank(banks, "irbo_arm9_main", 12, 20)
            roms = root / "roms"
            roms.mkdir()
            play = root / "banks" / "play"
            play.mkdir()
            sha1 = "a" * 40
            write_coverage(
                roms,
                "game-coverage-20260101-120000.json",
                sha1,
                "build-one",
                entries9=10,
                entries7=0,
                insns9=100,
                insns7=0,
            )
            write_performance(
                play,
                "performance-20260101-120000.jsonl",
                sha1,
                "build-one",
                entries9=10,
                entries7=0,
                dispatched9=990,
                dispatched7=0,
            )

            stats = collect_recomp_stats(root, banks, sha1, "IRBO", roms)

            self.assertIsNotNone(stats.coverage)
            self.assertIsNotNone(stats.perf)
            self.assertAlmostEqual(stats.interpreter_share or 0.0, 1.0)

    def test_performance_without_coverage_still_reports_a_share(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            banks = root / "banks" / "black"
            banks.mkdir(parents=True)
            write_bank(banks, "irbo_arm9_main", 12, 20)
            play = root / "banks" / "play"
            play.mkdir()
            sha1 = "a" * 40
            write_performance(
                play,
                "performance-20260101-120000.jsonl",
                sha1,
                "build-one",
                entries9=10,
                entries7=0,
                dispatched9=990,
                dispatched7=0,
            )

            stats = collect_recomp_stats(root, banks, sha1, "IRBO")

            self.assertIsNone(stats.coverage)
            self.assertIsNotNone(stats.perf)
            self.assertAlmostEqual(stats.interpreter_share or 0.0, 1.0)
            self.assertTrue(
                any("coverage manifest missing" in warning for warning in stats.warnings)
            )

    def test_mismatched_run_stamps_do_not_mix_metrics(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            banks = root / "banks" / "black"
            banks.mkdir(parents=True)
            write_bank(banks, "irbo_arm9_main", 12, 20)
            play = root / "banks" / "play"
            play.mkdir()
            sha1 = "a" * 40
            write_coverage(
                play,
                "game-coverage-20260101-120000.json",
                sha1,
                "build-one",
                entries9=10,
                entries7=0,
                insns9=100,
                insns7=0,
            )
            write_performance(
                play,
                "performance-20260101-130000.jsonl",
                sha1,
                "build-one",
                entries9=10,
                entries7=0,
                dispatched9=990,
                dispatched7=0,
            )

            stats = collect_recomp_stats(root, banks, sha1, "IRBO")

            self.assertIsNotNone(stats.coverage)
            self.assertIsNone(stats.perf)
            self.assertIsNone(stats.interpreter_share)
            self.assertTrue(
                any("no performance log matches" in warning for warning in stats.warnings)
            )

    def test_coverage_from_another_rom_is_flagged(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            banks = root / "banks" / "black"
            banks.mkdir(parents=True)
            write_bank(banks, "irbo_arm9_main", 12, 20)
            play = root / "banks" / "play"
            play.mkdir()
            write_coverage(
                play,
                "game-coverage-20260101-120000.json",
                "a" * 40,
                "build-one",
                entries9=10,
                entries7=0,
                insns9=100,
                insns7=0,
            )

            stats = collect_recomp_stats(root, banks, "b" * 40, "IRBO")

            self.assertIsNotNone(stats.coverage)
            self.assertIsNone(stats.perf)
            self.assertIn("latest coverage is from a different ROM", stats.warnings)

    def test_missing_evidence_leaves_runtime_share_unknown(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            banks = root / "banks" / "black"
            banks.mkdir(parents=True)
            write_bank(banks, "irbo_arm9_main", 12, 20)

            stats = collect_recomp_stats(root, banks, "a" * 40, "IRBO")

            self.assertEqual(stats.banks.total_functions, 12)
            self.assertIsNone(stats.coverage)
            self.assertIsNone(stats.perf)
            self.assertIsNone(stats.interpreter_share)

    def test_coverage_predating_banks_is_flagged(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            banks = root / "banks" / "black"
            banks.mkdir(parents=True)
            play = root / "banks" / "play"
            play.mkdir()
            coverage_path = write_coverage(
                play,
                "game-coverage-20260101-120000.json",
                "a" * 40,
                "build-one",
                entries9=10,
                entries7=0,
                insns9=100,
                insns7=0,
            )
            write_bank(banks, "irbo_arm9_main", 12, 20)
            generated = coverage_path.stat().st_mtime + 10
            header = banks / "irbo_arm9_main.h"
            os.utime(header, (generated, generated))

            stats = collect_recomp_stats(root, banks, "a" * 40, "IRBO")

            self.assertIn("coverage predates the current bank set", stats.warnings)
