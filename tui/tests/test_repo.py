"""Repository discovery, preset loading and bank detection tests."""

from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path

from tui.model import (
    Repo,
    fallback_preset_info,
    host_family,
    load_preset_infos,
    load_presets,
    preferred_preset_names,
)


class RepoPathTests(unittest.TestCase):
    def test_store_and_resolve_round_trip_inside_the_root(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / "roms" / "game.nds"
            target.parent.mkdir(parents=True)
            target.write_bytes(b"")
            repo = Repo(root)
            stored = repo.store(target)

            self.assertEqual(stored, "roms/game.nds")
            self.assertEqual(repo.resolve(stored), target.resolve())
            self.assertEqual(repo.display(target), "roms/game.nds")

    def test_paths_outside_the_root_stay_absolute(self) -> None:
        with tempfile.TemporaryDirectory() as temp, tempfile.TemporaryDirectory() as other:
            outside = Path(other) / "game.nds"
            outside.write_bytes(b"")
            repo = Repo(temp)

            self.assertEqual(repo.store(outside), str(outside.resolve()))


class BankDetectionTests(unittest.TestCase):
    def test_only_two_digit_shards_are_banks(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp) / "banks"
            folder.mkdir()
            for name in (
                "black_00.c",
                "black_01.c",
                "black_1.c",
                "black_100.c",
                "black_00.h",
                "notes.c",
            ):
                (folder / name).write_text("", encoding="utf-8")
            nested = folder / "nested"
            nested.mkdir()
            (nested / "black_02.c").write_text("", encoding="utf-8")

            files = Repo.bank_files(folder)
            self.assertEqual([path.name for path in files], ["black_00.c", "black_01.c"])
            self.assertEqual([path.parent for path in files], [folder, folder])
            self.assertTrue(Repo.has_banks(folder))
            self.assertFalse(Repo.has_banks(folder / "missing"))
            self.assertFalse(Repo.has_banks(Path(temp) / "empty"))


class RunnerDiscoveryTests(unittest.TestCase):
    def test_selected_preset_wins(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            repo = Repo(temp)
            for preset in ("test-debug", "test-release"):
                runner = repo.runner_path(preset)
                runner.parent.mkdir(parents=True)
                runner.write_bytes(b"")

            found, preset = repo.find_runner("test-release")

            self.assertEqual(found, repo.runner_path("test-release"))
            self.assertEqual(preset, "test-release")

    def test_unknown_preset_falls_back_to_the_build_tree(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            repo = Repo(temp)
            runner = repo.runner_path("test-debug")
            runner.parent.mkdir(parents=True)
            runner.write_bytes(b"")

            found, preset = repo.find_runner("missing-preset")

            self.assertEqual(found, runner)
            self.assertEqual(preset, "test-debug")


class RenderToolDiscoveryTests(unittest.TestCase):
    def test_falls_back_to_another_build_preset(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            suffix = ".exe" if os.name == "nt" else ""
            tool = root / "build" / "custom-preset" / "render" / f"pkmn_render_smoke{suffix}"
            tool.parent.mkdir(parents=True)
            tool.write_bytes(b"")
            repo = Repo(root)

            found, preset = repo.find_render_smoke("missing-preset")

            self.assertEqual(found, tool)
            self.assertEqual(preset, "custom-preset")


class PresetLoadingTests(unittest.TestCase):
    def test_filters_other_hosts_hidden_and_duplicates(self) -> None:
        family = host_family()
        payload = {
            "configurePresets": [
                {"name": f"{family}-debug"},
                {"name": f"{family}-release"},
                {"name": f"{family}-debug"},
                {"name": "other-debug"},
                {"name": f"{family}-hidden", "hidden": True},
                {"name": 7},
                "not-a-dict",
            ]
        }
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "CMakePresets.json"
            path.write_text(json.dumps(payload), encoding="utf-8")
            names = load_presets(path)

        self.assertEqual(names, [f"{family}-debug", f"{family}-release"])

    def test_missing_file_falls_back_to_preferred_names(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            names = load_presets(Path(temp) / "CMakePresets.json")

        self.assertEqual(names, preferred_preset_names())

    def test_infos_carry_readable_labels_descriptions_and_compiler(self) -> None:
        family = host_family()
        payload = {
            "configurePresets": [
                {
                    "name": "base",
                    "hidden": True,
                    "cacheVariables": {
                        "CMAKE_C_COMPILER": {
                            "type": "FILEPATH",
                            "value": "clang",
                        },
                        "CMAKE_CXX_COMPILER": "clang++",
                    },
                },
                {
                    "name": f"{family}-relwithdebinfo",
                    "displayName": "Fast + Symbols",
                    "description": "Best for play and profiling.",
                    "inherits": "base",
                    "cacheVariables": {"CMAKE_BUILD_TYPE": "RelWithDebInfo"},
                },
                {
                    "name": f"{family}-debug",
                    "displayName": "Slow + Symbols",
                    "cacheVariables": {"CMAKE_BUILD_TYPE": "Debug"},
                },
            ]
        }
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "CMakePresets.json"
            path.write_text(json.dumps(payload), encoding="utf-8")
            infos = load_preset_infos(path)

        self.assertEqual(
            [info.name for info in infos],
            [f"{family}-relwithdebinfo", f"{family}-debug"],
        )
        fast, debug = infos
        self.assertEqual(fast.display_name, "Fast + Symbols")
        self.assertEqual(fast.build_type, "RelWithDebInfo")
        self.assertEqual(fast.compiler, "clang / clang++")
        self.assertEqual(fast.description, "Best for play and profiling.")
        self.assertTrue(fast.recommended)
        self.assertEqual(debug.display_name, "Slow + Symbols")
        self.assertEqual(debug.build_type, "Debug")
        self.assertEqual(debug.compiler, "system default")
        self.assertIn("Unoptimised", debug.description)
        self.assertFalse(debug.recommended)

    def test_missing_preset_file_has_readable_fallback_info(self) -> None:
        info = fallback_preset_info("linux-relwithdebinfo")

        self.assertEqual(info.display_name, "Linux RelWithDebInfo")
        self.assertEqual(info.build_type, "RelWithDebInfo")
        self.assertEqual(info.compiler, "system default")
        self.assertTrue(info.recommended)
        self.assertIn("Recommended", info.description)
