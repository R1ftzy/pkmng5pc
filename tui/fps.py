"""Live presented-frame rate from the runner's play-mode debug surface."""

from __future__ import annotations

import json
import socket
import threading
from dataclasses import dataclass

RUNNER_PORT = 19842
REQUEST = b'{"cmd":"frontend_stats"}\n'
CONNECT_TIMEOUT = 1.0
RETRY_DELAY = 0.5


@dataclass(frozen=True)
class FrontendStats:
    frames: int
    now_ticks: int
    freq: int


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
    return FrontendStats(frames=frames, now_ticks=now_ticks, freq=freq)


def fps_between(before: FrontendStats, after: FrontendStats) -> float | None:
    frames = after.frames - before.frames
    ticks = after.now_ticks - before.now_ticks
    if ticks <= 0 or frames < 0:
        return None
    return frames * after.freq / ticks


class RunnerFpsProbe:
    def __init__(self, port: int = RUNNER_PORT, interval: float = 1.0):
        self.port = port
        self.interval = interval
        self.fps: float | None = None
        self.frames: int | None = None
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
        if (self.fps, self.frames, self.connected) == (None, None, False):
            return
        self.fps = None
        self.frames = None
        self.connected = False
        self.revision += 1

    def _run(self) -> None:
        stop = self._stop
        while not stop.is_set():
            previous: FrontendStats | None = None
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
                            if stats is None:
                                previous = None
                                continue
                            fps = fps_between(previous, stats) if previous else None
                            previous = stats
                            if stop.is_set():
                                return
                            self._publish(stop, fps, stats.frames, True)
                            if stop.wait(self.interval):
                                return
                    finally:
                        stream.close()
            except OSError:
                pass
            finally:
                self._publish(stop, None, None, False)
            if stop.wait(RETRY_DELAY):
                return

    def _publish(
        self,
        stop: threading.Event,
        fps: float | None,
        frames: int | None,
        connected: bool,
    ) -> None:
        if stop is not self._stop:
            return
        if (self.fps, self.frames, self.connected) == (fps, frames, connected):
            return
        self.fps = fps
        self.frames = frames
        self.connected = connected
        self.revision += 1
