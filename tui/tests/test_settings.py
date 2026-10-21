"""Persisted launcher settings tests."""

from __future__ import annotations

import json
import tempfile
import unittest
from dataclasses import asdict
from pathlib import Path

from tui.model import STATE_VERSION, Settings

PRESETS = ["test-debug", "test-release"]


class SettingsLoadTests(unittest.TestCase):
    def test_missing_file_gives_defaults(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            settings = Settings.load(Path(temp) / "missing.json", PRESETS)

        self.assertEqual(settings.preset, PRESETS[0])
        self.assertEqual(settings.rom_dir, "roms")
        self.assertEqual(settings.banks_dir, "banks/black")
        self.assertEqual(settings.play_layout, "stacked")
        self.assertEqual(settings.play_gpu, "auto")
        self.assertEqual(settings.save_mode, "default")
        self.assertEqual(settings.frames, 0)
        self.assertEqual(settings.shards, 8)

    def test_malformed_json_gives_defaults(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "state.json"
            path.write_text("{not json", encoding="utf-8")
            settings = Settings.load(path, PRESETS)

        self.assertEqual(settings.preset, PRESETS[0])
        self.assertEqual(settings.rom_dir, "roms")

    def test_invalid_values_are_normalized(self) -> None:
        payload = {
            "preset": "bogus",
            "rom_dir": "",
            "banks_dir": "",
            "play_layout": "wide",
            "play_gpu": "   ",
            "save_mode": "sometimes",
            "frames": -4,
            "shards": 1,
        }
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "state.json"
            path.write_text(json.dumps(payload), encoding="utf-8")
            settings = Settings.load(path, PRESETS)

        self.assertEqual(settings.preset, PRESETS[0])
        self.assertEqual(settings.rom_dir, "roms")
        self.assertEqual(settings.banks_dir, "banks/black")
        self.assertEqual(settings.play_layout, "stacked")
        self.assertEqual(settings.play_gpu, "auto")
        self.assertEqual(settings.save_mode, "default")
        self.assertEqual(settings.frames, 0)
        self.assertEqual(settings.shards, 2)

    def test_out_of_range_values_are_clamped(self) -> None:
        payload = {"frames": 20_000_000, "shards": 1_000}
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "state.json"
            path.write_text(json.dumps(payload), encoding="utf-8")
            settings = Settings.load(path, PRESETS)

        self.assertEqual(settings.frames, 10_000_000)
        self.assertEqual(settings.shards, 64)

    def test_non_integer_numbers_keep_their_defaults(self) -> None:
        payload = {"frames": "many", "shards": None}
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "state.json"
            path.write_text(json.dumps(payload), encoding="utf-8")
            settings = Settings.load(path, PRESETS)

        self.assertEqual(settings.frames, 0)
        self.assertEqual(settings.shards, 8)


class SettingsRoundTripTests(unittest.TestCase):
    def test_save_and_load_preserve_every_field(self) -> None:
        original = Settings(
            preset="test-release",
            rom_dir="roms/alt",
            rom_path="roms/alt/game.nds",
            build_tests=False,
            build_tools=True,
            build_render=False,
            build_runner=True,
            native_present=False,
            banks_dir="banks/white2",
            extra_bank_dirs="banks/black;banks/white",
            extra_sha1s=f"{'a' * 40};{'b' * 40}",
            play_layout="separate",
            play_native=True,
            play_gpu="index:1",
            save_mode="custom",
            save_path="saves/run.sav",
            frames=120,
            stats=True,
            coverage=True,
            diagnostics=True,
            extra_args="--foo bar",
            bootstrap_runner=True,
            bootstrap_sdl3=True,
            bootstrap_compiler="clang",
            bank_out="banks/custom",
            title_config="re/config/black.toml",
            shards=16,
            overlays=True,
            overlay_seeds="re/config/black/seeds",
            emit_only=True,
        )
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "state.json"
            original.save(path)
            payload = json.loads(path.read_text(encoding="utf-8"))
            loaded = Settings.load(path, PRESETS)

        self.assertEqual(payload["version"], STATE_VERSION)
        self.assertEqual(asdict(loaded), asdict(original))
