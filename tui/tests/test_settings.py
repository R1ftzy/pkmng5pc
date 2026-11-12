"""Persisted launcher settings tests."""

from __future__ import annotations

import json
import tempfile
import unittest
from dataclasses import asdict
from pathlib import Path

from tui.model import STATE_VERSION, Settings, play_window_size, play_window_text

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
        self.assertTrue(settings.play_native)
        self.assertTrue(settings.render_native)
        self.assertFalse(settings.compatibility_mode)
        self.assertEqual(settings.render_scale, 1)
        self.assertEqual(settings.render_aspect, "4:3")
        self.assertEqual(settings.renderer_3d, "faithful")
        self.assertEqual(settings.texture_upscale, 1)
        self.assertEqual(settings.window_size, "native")
        self.assertEqual(settings.window_width, 1024)
        self.assertEqual(settings.window_height, 768)
        self.assertEqual(settings.present_scaling, "integer")
        self.assertFalse(settings.render_verify)

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
            "render_native": "on",
            "compatibility_mode": "on",
            "render_scale": 9,
            "render_aspect": "32:9",
            "renderer_3d": "warp",
            "texture_upscale": 3,
            "window_size": "warp",
            "window_width": 10,
            "window_height": 100_000,
            "present_scaling": "stretch",
            "render_verify": "yes",
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
        self.assertTrue(settings.render_native)
        self.assertFalse(settings.compatibility_mode)
        self.assertEqual(settings.render_scale, 1)
        self.assertEqual(settings.render_aspect, "4:3")
        self.assertEqual(settings.renderer_3d, "faithful")
        self.assertEqual(settings.texture_upscale, 1)
        self.assertEqual(settings.window_size, "native")
        self.assertEqual(settings.window_width, 160)
        self.assertEqual(settings.window_height, 4320)
        self.assertEqual(settings.present_scaling, "integer")
        self.assertFalse(settings.render_verify)

    def test_window_size_presets_follow_aspect_and_layout(self) -> None:
        settings = Settings(render_aspect="16:9", window_size="2x")
        self.assertEqual(play_window_size(settings), (684, 768))

        settings = Settings(render_aspect="21:9", window_size="1x", play_layout="separate")
        self.assertEqual(play_window_size(settings), (448, 192))

        settings = Settings(window_size="custom", window_width=1600, window_height=900)
        self.assertEqual(play_window_size(settings), (1600, 900))
        self.assertEqual(play_window_text(settings), "1600x900")

        settings = Settings(window_size="native")
        self.assertIsNone(play_window_size(settings))
        self.assertEqual(play_window_text(settings), "runner default")

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

    def test_missing_render_keys_use_defaults_without_touching_present(self) -> None:
        payload = {"play_native": True, "play_layout": "separate"}
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "state.json"
            path.write_text(json.dumps(payload), encoding="utf-8")
            settings = Settings.load(path, PRESETS)

        self.assertTrue(settings.play_native)
        self.assertTrue(settings.render_native)
        self.assertEqual(settings.render_scale, 1)
        self.assertEqual(settings.render_aspect, "4:3")
        self.assertFalse(settings.render_verify)

    def test_native_renderer_forces_native_presentation(self) -> None:
        payload = {"render_native": True, "play_native": False}
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "state.json"
            path.write_text(json.dumps(payload), encoding="utf-8")
            settings = Settings.load(path, PRESETS)

        self.assertTrue(settings.render_native)
        self.assertTrue(settings.play_native)

    def test_native_renderer_off_leaves_native_presentation_alone(self) -> None:
        settings = Settings(render_native=False, play_native=False)
        settings.normalize(PRESETS)
        self.assertFalse(settings.play_native)

        settings = Settings(render_native=False, play_native=True)
        settings.normalize(PRESETS)
        self.assertTrue(settings.play_native)

    def test_compatibility_mode_does_not_force_native_presentation(self) -> None:
        settings = Settings(render_native=True, play_native=False, compatibility_mode=True)
        settings.normalize(PRESETS)
        self.assertFalse(settings.play_native)


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
            native_renderer=False,
            banks_dir="banks/white2",
            extra_bank_dirs="banks/black;banks/white",
            extra_sha1s=f"{'a' * 40};{'b' * 40}",
            play_layout="separate",
            play_native=True,
            play_gpu="index:1",
            render_native=True,
            renderer_3d="compute",
            render_scale=3,
            texture_upscale=4,
            render_aspect="21:9",
            window_size="custom",
            window_width=1600,
            window_height=1000,
            present_scaling="fit",
            compatibility_mode=True,
            render_verify=True,
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
