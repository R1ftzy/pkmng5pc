"""Streamed command job tests."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

from tui.commands import (
    Command,
    Job,
    LaunchError,
    _extra_tokens,
    configure_command,
    play_command,
    quickstart_commands,
)
from tui.model import Repo, Settings


def run_job(job: Job, timeout: float = 15.0) -> None:
    job.start()
    if not job.wait(timeout):
        raise AssertionError("job did not finish within the timeout")


class JobTests(unittest.TestCase):
    def test_streams_output_and_finishes(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            command = Command(
                "demo",
                [sys.executable, "-c", "print('hello'); print('world')"],
                Path(temp),
            )
            job = Job([command], "demo")
            run_job(job)

        self.assertEqual(job.status, "done")
        self.assertEqual(job.returncode, 0)
        output = "\n".join(job.lines)
        self.assertIn("hello", output)
        self.assertIn("world", output)
        self.assertGreater(job.revision, 0)

    def test_failed_command_keeps_the_exit_code(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            command = Command(
                "fail",
                [sys.executable, "-c", "raise SystemExit(3)"],
                Path(temp),
            )
            job = Job([command], "fail")
            run_job(job)

        self.assertEqual(job.status, "failed")
        self.assertEqual(job.returncode, 3)

    def test_revision_advances_after_the_log_buffer_wraps(self) -> None:
        job = Job([], "buffer")
        for index in range(5100):
            job._append_line(str(index))

        self.assertEqual(job.revision, 5100)
        self.assertEqual(len(job.lines), 5000)
        self.assertEqual(job.lines[0], "100")

    def test_command_environment_reaches_the_child(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            command = Command(
                "env",
                [
                    sys.executable,
                    "-c",
                    "import os; print(os.environ.get('PKMNG5PC_RENDERER', ''))",
                ],
                Path(temp),
                env={"PKMNG5PC_RENDERER": "native"},
            )
            job = Job([command], "env")
            run_job(job)

        self.assertEqual(job.status, "done")
        self.assertIn("native", job.lines)


class ExtraArgumentTests(unittest.TestCase):
    def test_blank_input_has_no_tokens(self) -> None:
        self.assertEqual(_extra_tokens("   "), [])

    def test_quoted_values_keep_spaces(self) -> None:
        if os.name == "nt":
            text = '--bios-dir "C:\\Program Files\\bios" --flag "a b"'
            expected = ["--bios-dir", "C:\\Program Files\\bios", "--flag", "a b"]
        else:
            text = "--bios-dir '/tmp/a b' --flag 'a b'"
            expected = ["--bios-dir", "/tmp/a b", "--flag", "a b"]

        self.assertEqual(_extra_tokens(text), expected)

    def test_unbalanced_quotes_are_reported(self) -> None:
        if os.name == "nt":
            self.skipTest("CommandLineToArgvW accepts unbalanced quotes")
        with self.assertRaises(LaunchError):
            _extra_tokens("'unterminated")


def make_quickstart_repo(temp: Path) -> tuple[Repo, Path]:
    repo = Repo(temp)
    (temp / "roms").mkdir()
    rom = temp / "roms" / "test.nds"
    rom.write_bytes(b"TEST ROM" + b"\x00" * 4 + b"IRBO" + b"\x00" * 16)
    for script in (repo.play_script, repo.bootstrap_script):
        script.parent.mkdir(parents=True, exist_ok=True)
        script.write_text("", encoding="utf-8")
    repo.frontend_config.parent.mkdir(parents=True, exist_ok=True)
    repo.frontend_config.write_text("", encoding="utf-8")
    repo.recompiler_path.parent.mkdir(parents=True, exist_ok=True)
    repo.recompiler_path.write_text("", encoding="utf-8")
    return repo, rom


def write_build_cache(
    repo: Repo,
    native_renderer: str | None = "ON",
    native_present: str | None = "ON",
) -> None:
    path = repo.cache_path("test-debug")
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = ["PKMNG5PC_BUILD_RENDER:BOOL=ON"]
    if native_present is not None:
        lines.append(f"PKMNG5PC_NATIVE_PRESENT:BOOL={native_present}")
    if native_renderer is not None:
        lines.append(f"PKMNG5PC_NATIVE_RENDERER:BOOL={native_renderer}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


class ConfigureCommandTests(unittest.TestCase):
    def make_command(self, **changes: object) -> Command:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        repo, rom = make_quickstart_repo(Path(temp.name))
        settings = Settings(
            preset="test-debug",
            rom_path=repo.store(rom),
            banks_dir="banks/black",
            **changes,
        )
        return configure_command(repo, settings, None, require_banks=False)

    def test_native_renderer_build_option_is_passed(self) -> None:
        command = self.make_command(native_renderer=True)
        self.assertIn("-DPKMNG5PC_NATIVE_RENDERER=ON", command.argv)

        command = self.make_command(native_renderer=False)
        self.assertIn("-DPKMNG5PC_NATIVE_RENDERER=OFF", command.argv)

    def test_native_renderer_build_requires_the_render_layer(self) -> None:
        with self.assertRaises(LaunchError) as raised:
            self.make_command(native_renderer=True, build_render=False)
        self.assertIn("Build render", str(raised.exception))

    def test_native_renderer_runner_build_requires_native_present(self) -> None:
        with self.assertRaises(LaunchError) as raised:
            self.make_command(
                native_renderer=True, native_present=False, build_runner=True
            )
        self.assertIn("Native present", str(raised.exception))


class PlayCommandTests(unittest.TestCase):
    def make_command(self, **changes: object) -> tuple[Command, Repo]:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        repo, rom = make_quickstart_repo(Path(temp.name))
        settings = Settings(
            preset="test-debug",
            rom_path=repo.store(rom),
            banks_dir="banks/black",
            **changes,
        )
        return play_command(repo, settings, require_artifacts=False), repo

    def test_native_separate_keeps_the_layout_and_passes_the_gpu(self) -> None:
        command, _ = self.make_command(
            play_native=True, play_layout="separate", play_gpu="index:1"
        )
        if os.name == "nt":
            self.assertIn("-Native", command.argv)
            self.assertIn("-Layout", command.argv)
            self.assertEqual(command.argv[command.argv.index("-Layout") + 1], "separate")
            self.assertEqual(command.argv[command.argv.index("-Gpu") + 1], "index:1")
        else:
            self.assertIn("--native", command.argv)
            self.assertIn("--layout", command.argv)
            self.assertEqual(command.argv[command.argv.index("--layout") + 1], "separate")
            self.assertEqual(command.argv[command.argv.index("--gpu") + 1], "index:1")

    def test_auto_gpu_is_passed_and_stacked_layout_adds_no_layout_flag(self) -> None:
        command, _ = self.make_command(play_native=True, play_gpu="auto")
        if os.name == "nt":
            self.assertEqual(command.argv[command.argv.index("-Gpu") + 1], "auto")
        else:
            self.assertEqual(command.argv[command.argv.index("--gpu") + 1], "auto")
        self.assertNotIn("-Layout", command.argv)
        self.assertNotIn("--layout", command.argv)

    def test_gpu_is_not_passed_without_native_present(self) -> None:
        command, _ = self.make_command(play_native=False, play_gpu="discrete")
        self.assertNotIn("-Gpu", command.argv)
        self.assertNotIn("--gpu", command.argv)


class NativeRenderCommandTests(unittest.TestCase):
    def make_command(
        self, native_renderer: str | None = "ON", **changes: object
    ) -> tuple[Command, Repo]:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        repo, rom = make_quickstart_repo(Path(temp.name))
        write_build_cache(repo, native_renderer)
        settings = Settings(
            preset="test-debug",
            rom_path=repo.store(rom),
            banks_dir="banks/black",
            **changes,
        )
        return play_command(repo, settings, require_artifacts=False), repo

    def test_renderer_off_sets_no_environment(self) -> None:
        command, _ = self.make_command(
            render_native=False, render_scale=4, render_aspect="21:9"
        )
        self.assertEqual(command.env, {})
        self.assertNotIn("-Native", command.argv)
        self.assertNotIn("--native", command.argv)

    def test_native_renderer_passes_launch_flags(self) -> None:
        command, _ = self.make_command(
            render_native=True, render_scale=1, render_aspect="4:3"
        )
        self.assertEqual(command.env, {})
        if os.name == "nt":
            self.assertIn("-Native", command.argv)
            self.assertEqual(command.argv[command.argv.index("-Renderer") + 1], "native")
            self.assertEqual(command.argv[command.argv.index("-Aspect") + 1], "4:3")
            self.assertEqual(command.argv[command.argv.index("-RenderScale") + 1], "1")
        else:
            self.assertIn("--native", command.argv)
            self.assertEqual(
                command.argv[command.argv.index("--renderer") + 1], "native"
            )
            self.assertEqual(command.argv[command.argv.index("--aspect") + 1], "4:3")
            self.assertEqual(
                command.argv[command.argv.index("--render-scale") + 1], "1"
            )

    def test_native_renderer_forces_native_presentation(self) -> None:
        command, _ = self.make_command(render_native=True, play_native=False)
        self.assertIn("-Native" if os.name == "nt" else "--native", command.argv)

    def test_native_renderer_refuses_non_4_by_3(self) -> None:
        with self.assertRaises(LaunchError) as raised:
            self.make_command(render_native=True, render_aspect="21:9")
        self.assertIn("4:3 only", str(raised.exception))

    def test_native_renderer_refuses_non_1x(self) -> None:
        with self.assertRaises(LaunchError) as raised:
            self.make_command(render_native=True, render_scale=3)
        self.assertIn("1x only", str(raised.exception))

    def test_verify_only_adds_the_debug_environment(self) -> None:
        command, _ = self.make_command(render_native=True, render_verify=True)
        self.assertEqual(command.env["PKMNG5PC_DS3D_VERIFY"], "1")

    def test_missing_native_build_refuses_to_launch(self) -> None:
        with self.assertRaises(LaunchError) as raised:
            self.make_command(native_renderer=None, render_native=True)
        self.assertIn("unavailable", str(raised.exception))
        self.assertIn("PKMNG5PC_NATIVE_RENDERER", str(raised.exception))

    def test_native_build_off_refuses_to_launch(self) -> None:
        with self.assertRaises(LaunchError) as raised:
            self.make_command(native_renderer="OFF", render_native=True)
        self.assertIn("unavailable", str(raised.exception))
        self.assertIn("PKMNG5PC_NATIVE_RENDERER=OFF", str(raised.exception))

    def test_build_state_reports_missing_and_disabled(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        repo, _ = make_quickstart_repo(Path(temp.name))

        available, reason = repo.native_renderer_state("test-debug")
        self.assertFalse(available)
        self.assertIn("not configured", reason)

        write_build_cache(repo, None)
        available, reason = repo.native_renderer_state("test-debug")
        self.assertFalse(available)
        self.assertIn("PKMNG5PC_NATIVE_RENDERER=ON", reason)

        write_build_cache(repo, "OFF")
        available, reason = repo.native_renderer_state("test-debug")
        self.assertFalse(available)
        self.assertIn("rebuild", reason)

        write_build_cache(repo, "ON", "OFF")
        available, reason = repo.native_renderer_state("test-debug")
        self.assertFalse(available)
        self.assertIn("PKMNG5PC_NATIVE_PRESENT=OFF", reason)

        write_build_cache(repo, "ON")
        self.assertEqual(repo.native_renderer_state("test-debug"), (True, ""))


class QuickstartTests(unittest.TestCase):
    def test_missing_pieces_are_planned_in_order(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            repo, rom = make_quickstart_repo(root)
            settings = Settings(
                preset="test-debug",
                rom_path=repo.store(rom),
                banks_dir="banks/black",
            )

            commands = quickstart_commands(repo, settings, "a" * 40)

            self.assertEqual(
                [command.title for command in commands],
                ["Configure", "Build", "Generate title banks", "Configure", "Build", "Play"],
            )
            self.assertTrue(any("PKMNG5PC_BUILD_RUNNER=OFF" in arg for arg in commands[0].argv))
            self.assertTrue(any("PKMNG5PC_BUILD_RUNNER=ON" in arg for arg in commands[3].argv))
            self.assertIn(str(repo.resolve("banks/black")), commands[2].argv)
            self.assertTrue(all(command.note.startswith("Quickstart:") for command in commands))

    def test_missing_recompiler_is_bootstrapped_before_runner_rebuild(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            repo, rom = make_quickstart_repo(root)
            repo.recompiler_path.unlink()
            banks = repo.resolve("banks/black")
            banks.mkdir(parents=True)
            (banks / "irbo_arm9_main_00.c").write_text("", encoding="utf-8")
            settings = Settings(
                preset="test-debug",
                rom_path=repo.store(rom),
                banks_dir="banks/black",
            )

            commands = quickstart_commands(repo, settings, "a" * 40)

            self.assertEqual(
                [command.title for command in commands],
                ["Bootstrap recompiler", "Configure", "Build", "Play"],
            )

    def test_ready_setup_does_not_bootstrap_an_unused_recompiler(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            repo, rom = make_quickstart_repo(root)
            repo.recompiler_path.unlink()
            banks = repo.resolve("banks/black")
            banks.mkdir(parents=True)
            (banks / "irbo_arm9_main_00.c").write_text("", encoding="utf-8")
            runner = repo.runner_path("test-debug")
            runner.parent.mkdir(parents=True)
            runner.write_text("", encoding="utf-8")
            settings = Settings(
                preset="test-debug",
                rom_path=repo.store(rom),
                banks_dir="banks/black",
            )

            commands = quickstart_commands(repo, settings, "a" * 40)

            self.assertEqual([command.title for command in commands], ["Play"])

    def test_ready_setup_only_plays(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            repo, rom = make_quickstart_repo(root)
            banks = repo.resolve("banks/black")
            banks.mkdir(parents=True)
            (banks / "irbo_arm9_main_00.c").write_text("", encoding="utf-8")
            runner = repo.runner_path("test-debug")
            runner.parent.mkdir(parents=True)
            runner.write_text("", encoding="utf-8")
            settings = Settings(
                preset="test-debug",
                rom_path=repo.store(rom),
                banks_dir="banks/black",
            )

            commands = quickstart_commands(repo, settings, "a" * 40)

            self.assertEqual([command.title for command in commands], ["Play"])
            self.assertTrue(commands[0].note.startswith("Quickstart:"))

