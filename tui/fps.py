"""Live presented-frame rate from the runner's play-mode debug surface."""

from __future__ import annotations

import json
import socket
import threading
from dataclasses import dataclass

RUNNER_PORT = 19842
REQUEST = b'{"cmd":"frontend_stats"}\n'
ATTRIB_REQUEST = b'{"cmd":"emu_attrib"}\n'
CONNECT_TIMEOUT = 1.0
RETRY_DELAY = 0.5
ATTRIB_BUCKETS = (
    "tier3_arm9",
    "tier3_arm7",
    "raster2d",
    "geometry",
    "dma_arm9",
    "gpu3d_frame",
)


PHASE_TICKS = (
    "emu_ticks",
    "present_ticks",
    "adaptive_ticks",
    "upload_ticks",
    "draw_ticks",
    "swap_ticks",
    "drain_ticks",
)
FRAME_BUDGET_MS = 1000.0 / 60.0


@dataclass(frozen=True)
class FrontendStats:
    frames: int
    now_ticks: int
    freq: int
    emu_ticks: int = 0
    present_ticks: int = 0
    adaptive_ticks: int = 0
    upload_ticks: int = 0
    draw_ticks: int = 0
    swap_ticks: int = 0
    drain_ticks: int = 0


@dataclass(frozen=True)
class EmuAttrib:
    buckets: dict[str, int]

    def ns(self, name: str) -> int:
        return int(self.buckets.get(name, 0))


@dataclass(frozen=True)
class FrameAttrib:
    tier3_arm9_ms: float
    tier3_arm7_ms: float
    raster2d_ms: float
    geometry_ms: float
    dma_arm9_ms: float
    gpu3d_ms: float


@dataclass(frozen=True)
class FramePhases:
    emu_ms: float
    present_ms: float
    adaptive_ms: float
    upload_ms: float
    draw_ms: float
    swap_ms: float
    drain_ms: float

    @property
    def gpu_ms(self) -> float:
        return self.present_ms

    @property
    def total_ms(self) -> float:
        return self.emu_ms + self.gpu_ms + self.drain_ms

    def bottleneck(self, fps: float | None) -> str:
        if fps is not None and fps >= 55.0:
            return "at target"
        phases = (
            ("emulation (CPU)", self.emu_ms),
            ("GPU / present", self.gpu_ms),
            ("pacing / audio wait", self.drain_ms),
        )
        label, value = max(phases, key=lambda item: item[1])
        if label == "pacing / audio wait" and fps is not None and fps >= 50.0:
            return "frame pacing"
        return f"{label} ({value:.1f} ms)"


def _optional_int(value: object) -> int | None:
    try:
        return int(value or 0)
    except (TypeError, ValueError):
        return None


def parse_frontend_stats(line: str) -> FrontendStats | None:
    try:
        data = json.loads(line)
    except ValueError:
        return None
    if not isinstance(data, dict) or not data.get("active"):
        return None
    try:
        frames = int(data["frames"])
        now_ticks = int(data["now_ticks"])
        freq = int(data["freq"])
    except (KeyError, TypeError, ValueError):
        return None
    if freq <= 0:
        return None
    phases: dict[str, int] = {}
    for name in PHASE_TICKS:
        value = _optional_int(data.get(name))
        if value is None:
            phases[name] = 0
        else:
            phases[name] = value
    return FrontendStats(frames=frames, now_ticks=now_ticks, freq=freq, **phases)


def fps_between(before: FrontendStats, after: FrontendStats) -> float | None:
    frames = after.frames - before.frames
    ticks = after.now_ticks - before.now_ticks
    if ticks <= 0 or frames < 0:
        return None
    return frames * after.freq / ticks


def phases_between(before: FrontendStats, after: FrontendStats) -> FramePhases | None:
    frames = after.frames - before.frames
    if frames <= 0 or after.freq <= 0:
        return None
    scale = 1000.0 / (after.freq * frames)

    def per_frame(name: str) -> float:
        delta = getattr(after, name) - getattr(before, name)
        return max(0.0, delta * scale)

    return FramePhases(
        emu_ms=per_frame("emu_ticks"),
        present_ms=per_frame("present_ticks"),
        adaptive_ms=per_frame("adaptive_ticks"),
        upload_ms=per_frame("upload_ticks"),
        draw_ms=per_frame("draw_ticks"),
        swap_ms=per_frame("swap_ticks"),
        drain_ms=per_frame("drain_ticks"),
    )


def parse_emu_attrib(line: str) -> EmuAttrib | None:
    try:
        data = json.loads(line)
    except ValueError:
        return None
    if not isinstance(data, dict):
        return None
    buckets = data.get("buckets")
    if not isinstance(buckets, dict):
        return None
    values: dict[str, int] = {}
    for name in ATTRIB_BUCKETS:
        bucket = buckets.get(name)
        if not isinstance(bucket, dict):
            continue
        value = _optional_int(bucket.get("ns"))
        if value is not None:
            values[name] = value
    if not values:
        return None
    return EmuAttrib(buckets=values)


def attrib_between(
    before: EmuAttrib, after: EmuAttrib, frames: int
) -> FrameAttrib | None:
    if frames <= 0:
        return None

    def per_frame(name: str) -> float:
        delta = after.ns(name) - before.ns(name)
        return max(0.0, delta / 1e6 / frames)

    return FrameAttrib(
        tier3_arm9_ms=per_frame("tier3_arm9"),
        tier3_arm7_ms=per_frame("tier3_arm7"),
        raster2d_ms=per_frame("raster2d"),
        geometry_ms=per_frame("geometry"),
        dma_arm9_ms=per_frame("dma_arm9"),
        gpu3d_ms=per_frame("gpu3d_frame"),
    )


class RunnerFpsProbe:
    def __init__(self, port: int = RUNNER_PORT, interval: float = 1.0):
        self.port = port
        self.interval = interval
        self.fps: float | None = None
        self.frames: int | None = None
        self.phases: FramePhases | None = None
        self.attrib: FrameAttrib | None = None
        self.connected = False
        self.revision = 0
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        thread = self._thread
        if thread is not None and thread.is_alive() and not self._stop.is_set():
            return
        self._reset()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="runner-fps", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._reset()

    def _reset(self) -> None:
        if (self.fps, self.frames, self.phases, self.attrib, self.connected) == (
            None,
            None,
            None,
            None,
            False,
        ):
            return
        self.fps = None
        self.frames = None
        self.phases = None
        self.attrib = None
        self.connected = False
        self.revision += 1

    def _run(self) -> None:
        stop = self._stop
        while not stop.is_set():
            previous: FrontendStats | None = None
            previous_attrib: EmuAttrib | None = None
            try:
                with socket.create_connection(
                    ("127.0.0.1", self.port), timeout=CONNECT_TIMEOUT
                ) as connection:
                    connection.settimeout(CONNECT_TIMEOUT)
                    stream = connection.makefile("rwb")
                    try:
                        while not stop.is_set():
                            stream.write(REQUEST)
                            stream.flush()
                            line = stream.readline()
                            if not line:
                                break
                            stats = parse_frontend_stats(line.decode("utf-8", "replace"))
                            stream.write(ATTRIB_REQUEST)
                            stream.flush()
                            attrib_line = stream.readline()
                            if not attrib_line:
                                break
                            attrib = parse_emu_attrib(
                                attrib_line.decode("utf-8", "replace")
                            )
                            if stats is None:
                                previous = None
                                previous_attrib = None
                                continue
                            frames = (
                                stats.frames - previous.frames if previous else 0
                            )
                            fps = fps_between(previous, stats) if previous else None
                            phases = phases_between(previous, stats) if previous else None
                            attrib_delta = (
                                attrib_between(previous_attrib, attrib, frames)
                                if previous_attrib and attrib
                                else None
                            )
                            previous = stats
                            previous_attrib = attrib
                            if stop.is_set():
                                return
                            self._publish(
                                stop, fps, stats.frames, phases, attrib_delta, True
                            )
                            if stop.wait(self.interval):
                                return
                    finally:
                        stream.close()
            except OSError:
                pass
            finally:
                self._publish(stop, None, None, None, None, False)
            if stop.wait(RETRY_DELAY):
                return

    def _publish(
        self,
        stop: threading.Event,
        fps: float | None,
        frames: int | None,
        phases: FramePhases | None,
        attrib: FrameAttrib | None,
        connected: bool,
    ) -> None:
        if stop is not self._stop:
            return
        if (self.fps, self.frames, self.phases, self.attrib, self.connected) == (
            fps,
            frames,
            phases,
            attrib,
            connected,
        ):
            return
        self.fps = fps
        self.frames = frames
        self.phases = phases
        self.attrib = attrib
        self.connected = connected
        self.revision += 1
