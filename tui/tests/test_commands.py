"""Streamed command job tests."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

from tui.commands import Command, Job, LaunchError, _extra_tokens, quickstart_commands
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

