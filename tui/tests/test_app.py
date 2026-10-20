"""Launcher state and cache comparison tests."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

from tui.app import App
from tui.commands import Command
from tui.model import Repo, Settings
from tui.terminal import MouseEvent, Terminal, strip_ansi


def make_app(temp: Path, settings: Settings) -> tuple[App, Repo]:
    repo = Repo(temp)
    app = App(repo, settings, Terminal(color=False), temp / "state.json", persist=False)
    return app, repo


def in_sync_cache(repo: Repo) -> dict[str, str]:
    return {
        "PKMNG5PC_BUILD_TESTS": "ON",
        "PKMNG5PC_BUILD_TOOLS": "ON",
        "PKMNG5PC_BUILD_RENDER": "ON",
        "PKMNG5PC_BUILD_RUNNER": "OFF",
        "PKMNG5PC_NATIVE_PRESENT": "ON",
        "PKMNG5PC_BANKS_DIR": str(repo.resolve("banks/black")),
    }


class CacheMismatchTests(unittest.TestCase):
    def test_matching_cache_has_no_problems(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            app, repo = make_app(Path(temp), Settings(preset="test-debug"))
            self.assertEqual(app.cache_mismatches(in_sync_cache(repo)), [])

    def test_missing_cache_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            app, _ = make_app(Path(temp), Settings(preset="test-debug"))
            self.assertEqual(app.cache_mismatches({}), ["build directory is not configured"])

    def test_option_and_banks_changes_are_reported(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            app, repo = make_app(Path(temp), Settings(preset="test-debug"))
            cache = in_sync_cache(repo)
            cache["PKMNG5PC_BUILD_TESTS"] = "OFF"
            cache["PKMNG5PC_BANKS_DIR"] = str(repo.resolve("banks/white"))

            problems = app.cache_mismatches(cache)

            self.assertIn("PKMNG5PC_BUILD_TESTS is OFF in the cache", problems)
            self.assertIn("banks directory differs from the cache", problems)

    def test_extra_title_settings_are_compared(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            settings = Settings(
                preset="test-debug",
                extra_bank_dirs="banks/white;banks/black2",
                extra_sha1s=f"{'a' * 40};{'b' * 40}",
            )
            app, repo = make_app(Path(temp), settings)
            cache = in_sync_cache(repo)
            cache["NDS_TITLE_BANK_DIRS"] = str(repo.resolve("banks/white"))
            cache["NDS_TITLE_ROM_SHA1S"] = "a" * 40

            problems = app.cache_mismatches(cache)

            self.assertIn("extra title bank dirs differ from the cache", problems)
            self.assertIn("extra title SHA-1s differ from the cache", problems)


class ReviewTests(unittest.TestCase):
    def command(self, temp: Path) -> Command:
        return Command("demo", [sys.executable, "-c", "pass"], temp)

    def test_action_shows_review_before_starting(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            app, _ = make_app(Path(temp), Settings(preset="test-debug"))
            app.run_or_message(lambda: [self.command(Path(temp))], "Demo")

            self.assertEqual(app.mode, "review")
            self.assertIsNone(app.job)
            app.cancel_review()
            self.assertEqual(app.mode, "page")

    def test_cancel_runs_review_cancel_callback(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            app, _ = make_app(Path(temp), Settings(preset="test-debug"))
            cancelled: list[bool] = []
            app.show_review(
                "Demo",
                [self.command(Path(temp))],
                None,
                on_cancel=lambda: cancelled.append(True),
            )
            app.handle_key("escape")

            self.assertEqual(cancelled, [True])
            self.assertEqual(app.mode, "page")

    def test_mouse_click_runs_the_review(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            app, _ = make_app(Path(temp), Settings(preset="test-debug"))
            app.show_review("Demo", [self.command(Path(temp))], None)
            app.render(100, 34)
            hit = next(item for item in app.review_hits if item[3] == "run")

            app.handle_mouse(MouseEvent("press", "left", (hit[1] + hit[2]) // 2, hit[0]))

            self.assertEqual(app.mode, "job")
            self.assertIsNotNone(app.job)
            self.assertTrue(app.job.wait(15))
            self.assertEqual(app.job.status, "done")

    def test_enter_confirms_review_and_starts_job(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            app, _ = make_app(Path(temp), Settings(preset="test-debug"))
            app.show_review("Demo", [self.command(Path(temp))], None)
            app.handle_key("enter")

            self.assertEqual(app.mode, "job")
            self.assertIsNotNone(app.job)
            self.assertTrue(app.job.wait(15))
            self.assertEqual(app.job.status, "done")

    def test_review_lists_every_configuration_group(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "roms").mkdir()
            (root / "roms" / "test.nds").write_bytes(
                b"TEST ROM" + b"\x00" * 4 + b"IRBO" + b"\x00" * 16
            )
            settings = Settings(
                preset="test-debug",
                rom_path="roms/test.nds",
                banks_dir="banks/white",
                extra_bank_dirs="banks/black2",
                extra_sha1s="a" * 40,
                play_layout="separate",
                save_mode="custom",
                save_path="saves/custom.sav",
                frames=120,
                stats=True,
                bootstrap_runner=True,
                bootstrap_compiler="clang",
                bank_out="banks/out",
                title_config="re/config/white.toml",
                shards=4,
                overlays=True,
                overlay_seeds="re/config/white/seeds",
            )
            app, _ = make_app(root, settings)
            sections = app.review_sections("Play", [self.command(root)])
            names = [name for name, _ in sections]
            text = "\n".join(row for _, rows in sections for row in rows)

            self.assertEqual(
                names,
                [
                    "ACTION",
                    "ROM",
                    "BUILD",
                    "RECOMPILATION",
                    "TITLE BANKS",
                    "BOOTSTRAP",
                    "PLAY",
                    "COMMANDS",
                    "FINAL CHECK",
                ],
            )
            for expected in [
                "Preset: Test Debug  [test-debug]",
                "Build type: Debug",
                "Profile:",
                "Recompiled functions: no generated banks",
                "Runtime dispatch: not measured",
                "ROM folder: roms",
                "Build tests: ON",
                "host tools: ON",
                "Build render: ON",
                "runner: OFF",
                "Native present: ON",
                "Banks directory: banks/white",
                "Extra bank dirs: banks/black2",
                "Extra SHA-1s: " + "a" * 40,
                "Build directory:",
                "CMake cache:",
                "bankgen:",
                "Runner:",
                "Recompiler:",
                "CMake:",
                "CTest:",
                "Output: banks/out",
                "Title config: re/config/white.toml",
                "Overlay seeds: re/config/white/seeds",
                "Shards: 4",
                "overlays: ON",
                "emit only: OFF",
                "Standalone runner: ON",
                "SDL3: OFF",
                "Compiler: clang",
                "Layout: separate",
                "Frame limit: 120",
                "Save mode: custom",
                "resolved save:",
                "Stats: ON",
                "coverage: OFF",
                "diagnostics: OFF",
                "Extra args: (none)",
                "Runner used:",
                "Action: Play",
                "State file:",
                "SHA-1:",
                "Title:",
                "Default save:",
            ]:
                self.assertIn(expected, text)
            self.assertIn("$ " + self.command(Path(temp)).preview(), text)

    def test_mouse_click_switches_tabs(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            app, _ = make_app(Path(temp), Settings(preset="test-debug"))
            app.render(100, 34)
            hit = next(hit for hit in app.tab_hits if hit[3] == "rom")
            _, x0, x1, _ = hit

            app.handle_mouse(MouseEvent("press", "left", (x0 + x1) // 2, 3))

            self.assertEqual(app.page, "rom")

    def test_mouse_click_toggles_a_page_row(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            app, _ = make_app(Path(temp), Settings(preset="test-debug"))
            app.set_page("build")
            app.render(100, 34)
            y = next(row for row, key in app.row_hits.items() if key == "build-tests")
            before = app.s.build_tests

            app.handle_mouse(MouseEvent("press", "left", 5, y))

            self.assertEqual(app.s.build_tests, not before)


class RecompDisplayTests(unittest.TestCase):
    def test_home_and_review_show_bank_and_runtime_state(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            banks = root / "banks" / "black"
            banks.mkdir(parents=True)
            (banks / "irbo_arm9_main.h").write_text(
                "/* Total functions: 12 */\n", encoding="utf-8"
            )
            (banks / "irbo_arm9_main_00.c").write_text("", encoding="utf-8")
            (banks / "irbo_arm9_main_dispatch.c").write_text(
                "const unsigned g_dispatch_irbo_arm9_main_len = 20u;\n",
                encoding="utf-8",
            )
            (banks / "irbo_arm7_main.h").write_text(
                "/* Total functions: 3 */\n", encoding="utf-8"
            )
            (banks / "irbo_arm7_main_00.c").write_text("", encoding="utf-8")
            (banks / "irbo_arm7_main_dispatch.c").write_text(
                "const unsigned g_dispatch_irbo_arm7_main_len = 5u;\n",
                encoding="utf-8",
            )
            rom = root / "roms" / "test.nds"
            rom.parent.mkdir()
            rom.write_bytes(b"TEST ROM" + b"\x00" * 4 + b"IRBO" + b"\x00" * 16)
            play = root / "banks" / "play"
            play.mkdir()
            sha1 = hashlib.sha1(rom.read_bytes()).hexdigest()
            coverage = {
                "kind": "ndsrecomp-tier3-coverage",
                "schema": 4,
                "rom_sha1": sha1,
                "build_id": "test-build",
                "static_coverage": {
                    "tier3_entries9": 10,
                    "tier3_entries7": 0,
                    "tier3_insns9": 100,
                    "tier3_insns7": 0,
                },
                "entry_points_arm9": [
                    {"addr": "0x02000000", "mode": "arm"},
                    {"addr": "0x02000000", "mode": "arm"},
                ],
                "entry_points_arm7": [],
            }
            coverage_path = play / "test-coverage-20260101-120000.json"
            coverage_path.write_text(json.dumps(coverage), encoding="utf-8")
            performance = [
                {
                    "kind": "session",
                    "rom_sha1": sha1,
                    "build_id": "test-build",
                },
                {
                    "kind": "perf",
                    "tier3_delta": {
                        "arm9": {"entries": 10, "instructions": 100},
                        "arm7": {"entries": 0, "instructions": 0},
                    },
                    "dispatch_delta": {
                        "arm9": {"dispatch_total": 990},
                        "arm7": {"dispatch_total": 0},
                    },
                },
            ]
            (play / "performance-20260101-120000.jsonl").write_text(
                "\n".join(json.dumps(row) for row in performance) + "\n",
                encoding="utf-8",
            )
            settings = Settings(
                preset="test-debug",
                rom_path="roms/test.nds",
                banks_dir="banks/black",
            )
            app, _ = make_app(root, settings)
            app.wait_for_sha1()
            self.assertEqual(app.current_sha1(), sha1)
            app.invalidate_recomp()

            stats = app.recomp_stats()
            self.assertEqual(stats.banks.total_functions, 15)
            self.assertEqual(stats.banks.dispatch_entries, 25)
            self.assertAlmostEqual(stats.interpreter_share or 0.0, 1.0)

            app.set_page("home")
            home = "\n".join(strip_ansi(line) for line in app.render(100, 40))
            self.assertIn("Recompiled functions", home)
            self.assertIn("15 functions", home)
            self.assertIn("99.00% native", home)
            self.assertIn("1.00% interpreter", home)
            self.assertIn("Interpreter coverage", home)
            self.assertIn("Last evidence", home)

            app.show_review(
                "Play",
                [Command("demo", [sys.executable, "-c", "pass"], root)],
                None,
            )
            review = "\n".join(strip_ansi(line) for line in app.render(100, 40))
            self.assertIn("RECOMPILATION", review)
            self.assertIn("Recompiled functions: 15", review)
            rows = dict(
                app.review_sections(
                    "Play",
                    [Command("demo", [sys.executable, "-c", "pass"], root)],
                )
            )["RECOMPILATION"]
            self.assertTrue(
                any("Runtime dispatch: 99.00% native" in row for row in rows)
            )

    def test_build_profile_shows_a_readable_preset_label(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            app, _ = make_app(Path(temp), Settings(preset="test-debug"))
            app.set_page("build")

            rendered = "\n".join(strip_ansi(line) for line in app.render(100, 44))

            self.assertIn("BUILD PROFILE", rendered)
            self.assertIn("Test Debug", rendered)
            self.assertIn("Build type", rendered)
            self.assertIn("Debug", rendered)
            self.assertIn("Preset ID", rendered)
            self.assertIn("test-debug", rendered)
            self.assertIn("system default", rendered)


class QuickstartActionTests(unittest.TestCase):
    def test_cancel_restores_settings_prepared_for_the_run(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            repo = Repo(root)
            (root / "roms").mkdir()
            rom = root / "roms" / "test.nds"
            rom.write_bytes(b"TEST ROM" + b"\x00" * 4 + b"IRBO" + b"\x00" * 16)
            for script in (repo.play_script, repo.bootstrap_script):
                script.parent.mkdir(parents=True, exist_ok=True)
                script.write_text("", encoding="utf-8")
            repo.frontend_config.parent.mkdir(parents=True, exist_ok=True)
            repo.frontend_config.write_text("", encoding="utf-8")
            repo.recompiler_path.parent.mkdir(parents=True, exist_ok=True)
            repo.recompiler_path.write_text("", encoding="utf-8")
            settings = Settings(
                preset="test-debug",
                rom_path=repo.store(rom),
                banks_dir="banks/white",
                emit_only=True,
                build_tools=False,
                build_render=False,
            )
            app = App(repo, settings, Terminal(color=False), root / "state.json", persist=False)
            key = app.sha1_key()
            assert key is not None
            app.sha1_cache[key] = "a" * 40

            app.quickstart_action()

            self.assertEqual(app.mode, "review")
            self.assertEqual(app.s.banks_dir, "banks/black")
            self.assertFalse(app.s.emit_only)
            self.assertTrue(app.s.build_tools)
            self.assertTrue(app.s.build_render)
            self.assertTrue(app.s.build_runner)
            app.cancel_review()

            self.assertEqual(app.mode, "page")
            self.assertEqual(app.s.banks_dir, "banks/white")
            self.assertTrue(app.s.emit_only)
            self.assertFalse(app.s.build_tools)
            self.assertFalse(app.s.build_render)
            self.assertFalse(app.s.build_runner)


if __name__ == "__main__":
    unittest.main()

