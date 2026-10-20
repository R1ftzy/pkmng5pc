"""Terminal input parsing tests."""

from __future__ import annotations

import os
import unittest
from types import SimpleNamespace

from tui.terminal import MouseEvent, Terminal


class MouseParsingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.terminal = Terminal(color=False)

    def parse(self, sequence: bytes):
        data = bytearray(sequence)
        return self.terminal._parse_posix(data)

    def test_left_press(self) -> None:
        event, consumed = self.parse(b"\x1b[<0;12;5M")

        self.assertEqual(event, MouseEvent("press", "left", 12, 5))
        self.assertEqual(consumed, len(b"\x1b[<0;12;5M"))

    def test_wheel_up_and_down(self) -> None:
        self.assertEqual(self.parse(b"\x1b[<64;3;4M")[0], MouseEvent("wheel", "up", 3, 4))
        self.assertEqual(self.parse(b"\x1b[<65;3;4M")[0], MouseEvent("wheel", "down", 3, 4))

    def test_release_and_motion_are_ignored(self) -> None:
        self.assertIsNone(self.parse(b"\x1b[<0;12;5m")[0])
        self.assertIsNone(self.parse(b"\x1b[<32;12;5M")[0])

    def test_plain_arrows_still_parse(self) -> None:
        self.assertEqual(self.parse(b"\x1b[A"), ("up", 3))
        self.assertEqual(self.parse(b"\x1b[6~"), ("pagedown", 4))

    def test_x10_mouse_fallback(self) -> None:
        event, consumed = self.parse(b"\x1b[M" + bytes([32, 43, 36]))
        self.assertEqual(event, MouseEvent("press", "left", 11, 4))
        self.assertEqual(consumed, 6)
        self.assertEqual(
            self.parse(b"\x1b[M" + bytes([96, 34, 35]))[0],
            MouseEvent("wheel", "up", 2, 3),
        )


class WindowsRecordTests(unittest.TestCase):
    def key(self, virtual: int, char: str = "", shift: bool = False):
        return SimpleNamespace(
            bKeyDown=True,
            wVirtualKeyCode=virtual,
            dwControlKeyState=0x0010 if shift else 0,
            uChar=SimpleNamespace(UnicodeChar=char),
        )

    def mouse(self, x: int, y: int, state: int, flags: int):
        return SimpleNamespace(
            dwMousePosition=SimpleNamespace(X=x, Y=y),
            dwButtonState=state,
            dwEventFlags=flags,
        )

    def test_special_keys_and_shift_tab(self) -> None:
        self.assertEqual(Terminal._windows_key(self.key(0x26)), "up")
        self.assertEqual(Terminal._windows_key(self.key(0x21)), "pageup")
        self.assertEqual(Terminal._windows_key(self.key(0x09, "\t", shift=True)), "backtab")
        self.assertEqual(Terminal._windows_key(self.key(0x0D, "\r")), "enter")

    def test_press_release_and_wheel(self) -> None:
        self.assertEqual(
            Terminal._windows_mouse(self.mouse(9, 3, 0x0001, 0), 0),
            (MouseEvent("press", "left", 10, 4), 1),
        )
        self.assertEqual(
            Terminal._windows_mouse(self.mouse(9, 3, 0, 0), 1),
            (None, 0),
        )
        self.assertEqual(
            Terminal._windows_mouse(self.mouse(9, 3, 120 << 16, 0x0004), 0),
            (MouseEvent("wheel", "up", 10, 4), 0),
        )
        self.assertEqual(
            Terminal._windows_mouse(self.mouse(9, 3, (-120 & 0xFFFF) << 16, 0x0004), 0),
            (MouseEvent("wheel", "down", 10, 4), 0),
        )
        self.assertEqual(
            Terminal._windows_mouse(self.mouse(9, 3, 0, 0x0004), 0),
            (None, 0),
        )

    @unittest.skipUnless(os.name == "nt", "Windows console records")
    def test_reader_structures_exist(self) -> None:
        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        _, record, count = Terminal._windows_reader(kernel32, ctypes, wintypes)
        record[0].EventType = 0x0002
        record[0].Event.MouseEvent.dwMousePosition.X = 4
        record[0].Event.MouseEvent.dwMousePosition.Y = 7

        self.assertEqual(record[0].Event.MouseEvent.dwMousePosition.X, 4)
        self.assertEqual(record[0].Event.MouseEvent.dwMousePosition.Y, 7)
        self.assertEqual(count.value, 0)


if __name__ == "__main__":
    unittest.main()
