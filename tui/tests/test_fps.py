"""Runner frame-rate sampling tests."""

from __future__ import annotations

import json
import socket
import threading
import time
import unittest

from tui.fps import (
    FrontendStats,
    RunnerFpsProbe,
    fps_between,
    parse_frontend_stats,
)


class FrontendStatsTests(unittest.TestCase):
    def test_parses_an_active_sample(self) -> None:
        stats = parse_frontend_stats(
            json.dumps({"active": 1, "frames": 600, "now_ticks": 9000, "freq": 1000})
        )

        self.assertEqual(stats, FrontendStats(600, 9000, 1000))

    def test_inactive_and_broken_samples_are_ignored(self) -> None:
        self.assertIsNone(parse_frontend_stats('{"active":0,"frames":1}'))
        self.assertIsNone(parse_frontend_stats("not json"))
        self.assertIsNone(
            parse_frontend_stats('{"active":1,"frames":1,"now_ticks":2,"freq":0}')
        )
        self.assertIsNone(parse_frontend_stats('{"active":1,"frames":1}'))

    def test_fps_uses_the_counter_delta(self) -> None:
        before = FrontendStats(100, 1000, 1000)
        after = FrontendStats(160, 2000, 1000)

        self.assertAlmostEqual(fps_between(before, after), 60.0)

    def test_fps_needs_a_forward_window(self) -> None:
        sample = FrontendStats(10, 100, 1000)

        self.assertIsNone(fps_between(sample, sample))
        self.assertIsNone(fps_between(sample, FrontendStats(5, 200, 1000)))


class ProbeTests(unittest.TestCase):
    def test_restart_replaces_a_stopped_but_live_probe(self) -> None:
        probe = RunnerFpsProbe(interval=0.01)
        started: list[int] = []
        release = threading.Event()

        def fake_run() -> None:
            started.append(threading.get_ident())
            probe._stop.wait()
            release.wait(1.0)

        probe._run = fake_run  # type: ignore[method-assign]
        try:
            probe.start()
            deadline = time.monotonic() + 1.0
            while len(started) < 1 and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertEqual(len(started), 1)

            probe.stop()
            probe.start()
            deadline = time.monotonic() + 1.0
            while len(started) < 2 and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertEqual(len(started), 2)
        finally:
            release.set()
            probe.stop()

    def test_probe_samples_the_runner_socket(self) -> None:
        server = socket.socket()
        server.bind(("127.0.0.1", 0))
        server.listen(1)
        port = server.getsockname()[1]
        samples = [
            {"active": 1, "frames": 100, "now_ticks": 1000, "freq": 1000},
            {"active": 1, "frames": 160, "now_ticks": 2000, "freq": 1000},
        ]

        def serve() -> None:
            try:
                connection, _ = server.accept()
            except OSError:
                return
            with connection:
                stream = connection.makefile("rwb")
                index = 0
                while True:
                    if not stream.readline():
                        return
                    sample = samples[min(index, len(samples) - 1)]
                    index += 1
                    stream.write((json.dumps(sample) + "\n").encode())
                    stream.flush()

        threading.Thread(target=serve, name="fake-runner", daemon=True).start()
        probe = RunnerFpsProbe(port=port, interval=0.01)
        try:
            probe.start()
            deadline = time.monotonic() + 3.0
            while probe.fps is None and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertIsNotNone(probe.fps)
            self.assertAlmostEqual(probe.fps or 0.0, 60.0, places=3)
            self.assertTrue(probe.connected)
            self.assertEqual(probe.frames, 160)
        finally:
            probe.stop()
            server.close()

        self.assertIsNone(probe.fps)
        self.assertIsNone(probe.frames)
        self.assertFalse(probe.connected)


if __name__ == "__main__":
    unittest.main()
