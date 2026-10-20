"""Repository discovery, preset loading and bank detection tests."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from tui.model import Repo, host_family, load_presets, preferred_preset_names


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
