"""Runner frame-rate sampling tests."""

from __future__ import annotations

import json
import socket
import threading
import time
import unittest

from tui.fps import (
    EmuAttrib,
    FrameAttrib,
    FramePhases,
    FrontendStats,
    RunnerFpsProbe,
    attrib_between,
    fps_between,
    parse_emu_attrib,
    parse_frontend_stats,
    phases_between,
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

    def test_phase_ticks_are_parsed_when_present(self) -> None:
        stats = parse_frontend_stats(
            json.dumps(
                {
                    "active": 1,
                    "frames": 600,
                    "now_ticks": 9000,
                    "freq": 1000,
                    "emu_ticks": 120,
                    "present_ticks": 30,
                    "adaptive_ticks": 4,
                    "upload_ticks": 5,
                    "draw_ticks": 6,
                    "swap_ticks": 7,
                    "drain_ticks": 80,
                }
            )
        )

        assert stats is not None
        self.assertEqual(stats.emu_ticks, 120)
        self.assertEqual(stats.present_ticks, 30)
        self.assertEqual(stats.drain_ticks, 80)
        self.assertEqual(stats.swap_ticks, 7)


class PhaseTests(unittest.TestCase):
    def test_phases_scale_by_the_presented_frames(self) -> None:
        before = FrontendStats(100, 1000, 1000)
        after = FrontendStats(
            300,
            2000,
            1000,
            emu_ticks=1_000,
            present_ticks=20,
            drain_ticks=10,
        )

        phases = phases_between(before, after)

        assert phases is not None
        self.assertAlmostEqual(phases.emu_ms, 5.0)
        self.assertAlmostEqual(phases.present_ms, 0.1)
        self.assertAlmostEqual(phases.drain_ms, 0.05)
        self.assertAlmostEqual(phases.gpu_ms, 0.1)
        self.assertAlmostEqual(phases.total_ms, 5.15)

    def test_phases_need_a_forward_window(self) -> None:
        sample = FrontendStats(10, 100, 1000)
        self.assertIsNone(phases_between(sample, sample))

    def test_bottleneck_names_the_dominant_phase(self) -> None:
        cpu_bound = FramePhases(20.0, 0.5, 0.0, 0.0, 0.0, 0.0, 0.5)
        gpu_bound = FramePhases(2.0, 18.0, 0.0, 0.0, 0.0, 0.0, 0.5)

        self.assertEqual(cpu_bound.bottleneck(40.0), "emulation (CPU) (20.0 ms)")
        self.assertEqual(gpu_bound.bottleneck(40.0), "GPU / present (18.0 ms)")
        self.assertEqual(cpu_bound.bottleneck(59.8), "at target")


class EmuAttribTests(unittest.TestCase):
    def test_parses_the_runner_buckets(self) -> None:
        attrib = parse_emu_attrib(
            json.dumps(
                {
                    "buckets": {
                        "tier3_arm9": {"ns": 1_000_000, "entries": 10},
                        "raster2d": {"ns": 200_000, "entries": 5},
                        "unknown": {"ns": 9_000_000},
                    }
                }
            )
        )

        assert attrib is not None
        self.assertEqual(attrib.ns("tier3_arm9"), 1_000_000)
        self.assertEqual(attrib.ns("raster2d"), 200_000)
        self.assertEqual(attrib.ns("unknown"), 0)

    def test_broken_attrib_payloads_are_ignored(self) -> None:
        self.assertIsNone(parse_emu_attrib("not json"))
        self.assertIsNone(parse_emu_attrib('{"buckets":{}}'))

    def test_attrib_scales_by_the_presented_frames(self) -> None:
        before = EmuAttrib({"tier3_arm9": 1_000_000, "raster2d": 0})
        after = EmuAttrib({"tier3_arm9": 61_000_000, "raster2d": 6_000_000})

        attrib = attrib_between(before, after, 60)

        assert attrib is not None
        self.assertAlmostEqual(attrib.tier3_arm9_ms, 1.0)
        self.assertAlmostEqual(attrib.raster2d_ms, 0.1)

    def test_attrib_needs_a_frame_window(self) -> None:
        sample = EmuAttrib({"tier3_arm9": 1_000_000})
        self.assertIsNone(attrib_between(sample, sample, 0))


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
            {
                "active": 1,
                "frames": 100,
                "now_ticks": 1000,
                "freq": 1000,
                "emu_ticks": 10_000,
                "present_ticks": 400,
            },
            {
                "active": 1,
                "frames": 160,
                "now_ticks": 2000,
                "freq": 1000,
                "emu_ticks": 10_100,
                "present_ticks": 410,
            },
        ]
        attrib_samples = [
            {
                "buckets": {
                    "tier3_arm9": {"ns": 1_000_000},
                    "raster2d": {"ns": 0},
                }
            },
            {
                "buckets": {
                    "tier3_arm9": {"ns": 61_000_000},
                    "raster2d": {"ns": 6_000_000},
                }
            },
        ]

        def serve() -> None:
            try:
                connection, _ = server.accept()
            except OSError:
                return
            with connection:
                stream = connection.makefile("rwb")
                index = 0
                attrib_index = 0
                while True:
                    request = stream.readline()
                    if not request:
                        return
                    if b'"emu_attrib"' in request:
                        sample = attrib_samples[
                            min(attrib_index, len(attrib_samples) - 1)
                        ]
                        attrib_index += 1
                    else:
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
            assert probe.phases is not None
            self.assertAlmostEqual(probe.phases.emu_ms, 100.0 / 60.0, places=3)
            self.assertAlmostEqual(probe.phases.present_ms, 10.0 / 60.0, places=3)
            assert probe.attrib is not None
            self.assertAlmostEqual(probe.attrib.tier3_arm9_ms, 1.0, places=3)
            self.assertAlmostEqual(probe.attrib.raster2d_ms, 0.1, places=3)
        finally:
            probe.stop()
            server.close()

        self.assertIsNone(probe.fps)
        self.assertIsNone(probe.frames)
        self.assertIsNone(probe.phases)
        self.assertIsNone(probe.attrib)
        self.assertFalse(probe.connected)


if __name__ == "__main__":
    unittest.main()
