"""ANSI screen, styling and raw keyboard input for the launcher."""

from __future__ import annotations

import os
import re
import select
import shutil
import sys
import textwrap
import time
from dataclasses import dataclass

ANSI_RE = re.compile(r"\x1b(?:\[[0-9;?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\))")
CONTROL_RE = re.compile(r"[\x00-\x08\x0b-\x1f\x7f]")

COLORS = {
    "black": 30,
    "red": 31,
    "green": 32,
    "yellow": 33,
    "blue": 34,
    "magenta": 35,
    "cyan": 36,
    "white": 37,
    "grey": 90,
    "bright_red": 91,
    "bright_green": 92,
    "bright_yellow": 93,
    "bright_cyan": 96,
    "bright_white": 97,
}
BACKGROUNDS = {
    "black": 40,
    "red": 41,
    "green": 42,
    "yellow": 43,
    "blue": 44,
    "magenta": 45,
    "cyan": 46,
    "white": 47,
    "grey": 100,
}

RESET = "\x1b[0m"
MOUSE_ON = "\x1b[?1000h\x1b[?1006h"
MOUSE_OFF = "\x1b[?1000l\x1b[?1006l"


@dataclass(frozen=True)
class MouseEvent:
    kind: str
    button: str
    x: int
    y: int


class TerminalError(RuntimeError):
    pass


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def sanitize(text: str, width: int | None = None) -> str:
    text = strip_ansi(text).replace("\t", "    ")
    text = CONTROL_RE.sub("", text.replace("\r", " "))
    if width is not None:
        text = text[:width]
    return text


def visible_len(text: str) -> int:
    return len(strip_ansi(text))


def clip(text: str, width: int) -> str:
    if width <= 0:
        return ""
    if visible_len(text) <= width:
        return text
    output: list[str] = []
    used = 0
    position = 0
    for match in ANSI_RE.finditer(text):
        plain = text[position : match.start()]
        take = max(0, width - used)
        output.append(plain[:take])
        used += min(len(plain), take)
        if used >= width:
            break
        output.append(match.group(0))
        position = match.end()
    else:
        plain = text[position:]
        output.append(plain[: max(0, width - used)])
    result = "".join(output)
    return result + (RESET if "\x1b" in text else "")


def pad(text: str, width: int) -> str:
    text = clip(text, width)
    missing = width - visible_len(text)
    return text + (" " * max(0, missing))


def pad_between(left: str, right: str, width: int) -> str:
    right = clip(right, max(0, width // 2))
    room = width - visible_len(left) - visible_len(right)
    if room <= 0:
        return pad(left, width)
    return left + (" " * room) + right


def truncate_middle(text: str, width: int) -> str:
    if width <= 1:
        return clip(text, max(0, width))
    if visible_len(text) <= width:
        return text
    keep = width - 1
    left = (keep + 1) // 2
    right = keep - left
    plain = strip_ansi(text)
    return plain[:left] + "…" + (plain[-right:] if right else "")


def wrap_text(text: str, width: int) -> list[str]:
    if width <= 1:
        return [text]
    lines: list[str] = []
    for paragraph in text.splitlines() or [""]:
        if not paragraph:
            lines.append("")
            continue
        lines.extend(
            textwrap.wrap(
                paragraph,
                width=width,
                break_long_words=True,
                break_on_hyphens=False,
                replace_whitespace=False,
            )
            or [""]
        )
    return lines


class Terminal:
    def __init__(self, color: bool = True, ascii_only: bool = False):
        self.color = color and not os.environ.get("NO_COLOR")
        self.ascii = ascii_only
        self._saved_stty = None
        self._saved_windows_modes: tuple[int, int] | None = None
        self._saved_code_page: int | None = None
        self._windows_input = None
        self._windows_read = None
        self._windows_buttons = 0
        self._input_buffer = bytearray()
        self._fd = None
        self._opened = False

    def __enter__(self) -> "Terminal":
        self.open()
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()

    def open(self) -> None:
        if not sys.stdin.isatty() or not sys.stdout.isatty():
            raise TerminalError("the launcher needs an interactive terminal")
        try:
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
        if os.name == "nt":
            self._open_windows()
        else:
            self._open_posix()
        self._opened = True
        mouse = "" if os.name == "nt" else MOUSE_ON
        self.write("\x1b[?1049h\x1b[?7l\x1b[?25l" + mouse + "\x1b[2J\x1b[H")

    def _open_windows(self) -> None:
        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.GetStdHandle.argtypes = [wintypes.DWORD]
        kernel32.GetStdHandle.restype = wintypes.HANDLE
        kernel32.GetConsoleMode.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel32.SetConsoleMode.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        output_handle = kernel32.GetStdHandle(-11)
        input_handle = kernel32.GetStdHandle(-10)
        output_mode = ctypes.c_uint32()
        input_mode = ctypes.c_uint32()
        if not kernel32.GetConsoleMode(output_handle, ctypes.byref(output_mode)):
            raise TerminalError("could not read the Windows console mode")
        if not kernel32.GetConsoleMode(input_handle, ctypes.byref(input_mode)):
            raise TerminalError("could not read the Windows console input mode")
        self._saved_windows_modes = (output_mode.value, input_mode.value)
        kernel32.GetConsoleOutputCP.restype = wintypes.UINT
        self._saved_code_page = kernel32.GetConsoleOutputCP()
        kernel32.SetConsoleOutputCP(65001)
        kernel32.SetConsoleMode(output_handle, output_mode.value | 0x0004)
        enabled = 0x0008 | 0x0010 | 0x0080
        disabled = 0x0001 | 0x0002 | 0x0004 | 0x0040
        new_input = (input_mode.value & ~disabled) | enabled
        kernel32.SetConsoleMode(input_handle, new_input)
        self._windows_input = input_handle
        self._windows_read = self._windows_reader(kernel32, ctypes, wintypes)

    @staticmethod
    def _windows_reader(kernel32, ctypes, wintypes):
        class Coord(ctypes.Structure):
            _fields_ = [("X", wintypes.SHORT), ("Y", wintypes.SHORT)]

        class CharUnion(ctypes.Union):
            _fields_ = [("UnicodeChar", wintypes.WCHAR), ("AsciiChar", wintypes.CHAR)]

        class KeyEvent(ctypes.Structure):
            _fields_ = [
                ("bKeyDown", wintypes.BOOL),
                ("wRepeatCount", wintypes.WORD),
                ("wVirtualKeyCode", wintypes.WORD),
                ("wVirtualScanCode", wintypes.WORD),
                ("uChar", CharUnion),
                ("dwControlKeyState", wintypes.DWORD),
            ]

        class MouseEventRecord(ctypes.Structure):
            _fields_ = [
                ("dwMousePosition", Coord),
                ("dwButtonState", wintypes.DWORD),
                ("dwControlKeyState", wintypes.DWORD),
                ("dwEventFlags", wintypes.DWORD),
            ]

        class WindowSizeEvent(ctypes.Structure):
            _fields_ = [("dwSize", Coord)]

        class EventUnion(ctypes.Union):
            _fields_ = [
                ("KeyEvent", KeyEvent),
                ("MouseEvent", MouseEventRecord),
                ("WindowBufferSizeEvent", WindowSizeEvent),
            ]

        class InputRecord(ctypes.Structure):
            _fields_ = [("EventType", wintypes.WORD), ("Event", EventUnion)]

        kernel32.ReadConsoleInputW.argtypes = [
            wintypes.HANDLE,
            ctypes.POINTER(InputRecord),
            wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
        ]
        kernel32.ReadConsoleInputW.restype = wintypes.BOOL
        kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel32.WaitForSingleObject.restype = wintypes.DWORD
        record = (InputRecord * 1)()
        count = wintypes.DWORD()
        return kernel32, record, count

    def _open_posix(self) -> None:
        import termios
        import tty

        self._fd = sys.stdin.fileno()
        self._saved_stty = termios.tcgetattr(self._fd)
        tty.setraw(self._fd)

    def close(self) -> None:
        if not self._opened:
            return
        self.write(RESET + MOUSE_OFF + "\x1b[?7h\x1b[?25h\x1b[?1049l")
        if os.name == "nt" and self._saved_windows_modes:
            import ctypes
            from ctypes import wintypes

            kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel32.GetStdHandle.argtypes = [wintypes.DWORD]
            kernel32.GetStdHandle.restype = wintypes.HANDLE
            kernel32.SetConsoleMode.argtypes = [wintypes.HANDLE, wintypes.DWORD]
            kernel32.SetConsoleMode(kernel32.GetStdHandle(-11), self._saved_windows_modes[0])
            kernel32.SetConsoleMode(kernel32.GetStdHandle(-10), self._saved_windows_modes[1])
            if self._saved_code_page is not None:
                kernel32.SetConsoleOutputCP(self._saved_code_page)
        elif self._saved_stty is not None and self._fd is not None:
            import termios

            termios.tcsetattr(self._fd, termios.TCSADRAIN, self._saved_stty)
        self._opened = False

    def size(self) -> tuple[int, int]:
        size = shutil.get_terminal_size(fallback=(100, 30))
        return max(20, size.columns), max(8, size.lines)

    def write(self, text: str) -> None:
        sys.stdout.write(text)
        sys.stdout.flush()

    def draw(self, lines: list[str]) -> None:
        width, height = self.size()
        visible = list(lines[:height])
        while len(visible) < height:
            visible.append("")
        output = ["\x1b[H"]
        for index, line in enumerate(visible):
            output.append(pad(line, width))
            output.append("\x1b[K")
            if index + 1 < height:
                output.append("\r\n")
        output.append("\x1b[J")
        self.write("".join(output))

    def style(
        self,
        text: str,
        color: str | None = None,
        background: str | None = None,
        bold: bool = False,
        dim: bool = False,
        reverse: bool = False,
    ) -> str:
        if not self.color:
            return text
        codes: list[str] = []
        if bold:
            codes.append("1")
        if dim:
            codes.append("2")
        if reverse:
            codes.append("7")
        if color in COLORS:
            codes.append(str(COLORS[color]))
        if background in BACKGROUNDS:
            codes.append(str(BACKGROUNDS[background]))
        if not codes:
            return text
        return f"\x1b[{';'.join(codes)}m{text}{RESET}"

    def bold(self, text: str) -> str:
        return self.style(text, bold=True)

    def dim(self, text: str) -> str:
        return self.style(text, dim=True)

    def cyan(self, text: str) -> str:
        return self.style(text, color="cyan")

    def green(self, text: str) -> str:
        return self.style(text, color="bright_green")

    def yellow(self, text: str) -> str:
        return self.style(text, color="bright_yellow")

    def red(self, text: str) -> str:
        return self.style(text, color="bright_red")

    def magenta(self, text: str) -> str:
        return self.style(text, color="magenta")

    def grey(self, text: str) -> str:
        return self.style(text, color="grey")

    def marker(self, selected: bool) -> str:
        if not selected:
            return "  "
        return "> " if self.ascii else "▸ "

    def glyph(self, unicode_value: str, ascii_value: str) -> str:
        return ascii_value if self.ascii else unicode_value

    def rule(self, width: int) -> str:
        char = "-" if self.ascii else "─"
        return char * max(0, width)

    def get_key(self, timeout: float = 0.1) -> str | MouseEvent | None:
        if os.name == "nt":
            return self._get_key_windows(timeout)
        return self._get_key_posix(timeout)

    def _get_key_windows(self, timeout: float) -> str | MouseEvent | None:
        if self._windows_read is None or self._windows_input is None:
            return None
        import ctypes

        kernel32, record, count = self._windows_read
        deadline = time.monotonic() + timeout
        while True:
            remaining = max(0, int((deadline - time.monotonic()) * 1000))
            if kernel32.WaitForSingleObject(self._windows_input, remaining) != 0:
                return None
            if not kernel32.ReadConsoleInputW(
                self._windows_input, record, 1, ctypes.byref(count)
            ):
                return None
            if count.value == 0:
                continue
            event = record[0]
            if event.EventType == 0x0004:
                return "resize"
            if event.EventType == 0x0002:
                token, self._windows_buttons = self._windows_mouse(
                    event.Event.MouseEvent, self._windows_buttons
                )
                if token is not None:
                    return token
            else:
                if event.EventType == 0x0001:
                    token = self._windows_key(event.Event.KeyEvent)
                    if token is not None:
                        return token
            if time.monotonic() >= deadline:
                return None

    @staticmethod
    def _windows_key(event) -> str | None:
        if not event.bKeyDown:
            return None
        virtual = event.wVirtualKeyCode
        names = {
            0x26: "up",
            0x28: "down",
            0x25: "left",
            0x27: "right",
            0x24: "home",
            0x23: "end",
            0x21: "pageup",
            0x22: "pagedown",
            0x2E: "delete",
            0x2D: "insert",
        }
        if virtual in names:
            return names[virtual]
        if virtual == 0x09 and event.dwControlKeyState & 0x0010:
            return "backtab"
        char = event.uChar.UnicodeChar
        if not char:
            return None
        return Terminal._translate_control(char)

    @staticmethod
    def _windows_mouse(event, previous: int) -> tuple[MouseEvent | None, int]:
        x = event.dwMousePosition.X + 1
        y = event.dwMousePosition.Y + 1
        flags = event.dwEventFlags
        state = event.dwButtonState
        buttons = state & 0x0007
        if flags & 0x0004:
            delta = (state >> 16) & 0xFFFF
            if delta >= 0x8000:
                delta -= 0x10000
            if delta == 0:
                return None, buttons
            return MouseEvent("wheel", "up" if delta > 0 else "down", x, y), buttons
        if flags & 0x0008:
            delta = (state >> 16) & 0xFFFF
            if delta == 0:
                return None, buttons
            direction = "right" if delta < 0x8000 else "left"
            return MouseEvent("wheel", direction, x, y), buttons
        if flags & 0x0001:
            return None, buttons
        newly = buttons if flags & 0x0002 else buttons & ~previous
        if newly & 0x0001:
            return MouseEvent("press", "left", x, y), buttons
        if newly & 0x0004:
            return MouseEvent("press", "middle", x, y), buttons
        if newly & 0x0002:
            return MouseEvent("press", "right", x, y), buttons
        return None, buttons

    @staticmethod
    def _translate_control(char: str) -> str | None:
        mapping = {
            "\r": "enter",
            "\n": "enter",
            "\t": "tab",
            "\x1b": "escape",
            "\x08": "backspace",
            "\x7f": "backspace",
            "\x03": "ctrl-c",
            "\x01": "ctrl-a",
            "\x05": "ctrl-e",
            "\x15": "ctrl-u",
            "\x04": "ctrl-d",
        }
        return mapping.get(char, char)

    def _get_key_posix(self, timeout: float) -> str | MouseEvent | None:
        while True:
            if self._input_buffer:
                key, consumed = self._parse_posix(self._input_buffer)
                if consumed:
                    del self._input_buffer[:consumed]
                    return key
            ready, _, _ = select.select([self._fd], [], [], timeout)
            if not ready:
                if self._input_buffer:
                    key, consumed = self._parse_posix(self._input_buffer, force=True)
                    if consumed:
                        del self._input_buffer[:consumed]
                        return key
                return None
            try:
                chunk = os.read(self._fd, 128)
            except OSError:
                return None
            if not chunk:
                return None
            self._input_buffer.extend(chunk)

    @staticmethod
    def _mouse_sgr(sequence: str, final: str) -> MouseEvent | None:
        parts = sequence.split(";")
        if len(parts) != 3:
            return None
        try:
            code, x, y = (int(part) for part in parts)
        except ValueError:
            return None
        if code & 64:
            direction = {0: "up", 1: "down", 2: "left", 3: "right"}.get(code & 3)
            if direction is None:
                return None
            return MouseEvent("wheel", direction, x, y)
        if final == "m" or code & 32:
            return None
        button = {0: "left", 1: "middle", 2: "right"}.get(code & 3)
        if button is None:
            return None
        return MouseEvent("press", button, x, y)

    def _parse_posix(
        self, data: bytearray, force: bool = False
    ) -> tuple[str | MouseEvent | None, int]:
        first = data[0]
        if first == 0x1B:
            if len(data) == 1:
                return ("escape", 1) if force else (None, 0)
            if data[1] in (ord("["), ord("O")):
                if data[1] == ord("[") and len(data) > 2 and data[2] == ord("M"):
                    if len(data) < 6:
                        return ("escape", 1) if force else (None, 0)
                    code = data[3] - 32
                    x = data[4] - 32
                    y = data[5] - 32
                    return self._mouse_sgr(f"{code};{x};{y}", "M"), 6
                index = 2
                while index < len(data) and not (0x40 <= data[index] <= 0x7E):
                    index += 1
                if index >= len(data):
                    return ("escape", 1) if force else (None, 0)
                sequence = bytes(data[2:index]).decode("ascii", "ignore")
                final = chr(data[index])
                consumed = index + 1
                if sequence.startswith("<"):
                    return self._mouse_sgr(sequence[1:], final), consumed
                mapping = {
                    "A": "up",
                    "B": "down",
                    "C": "right",
                    "D": "left",
                    "H": "home",
                    "F": "end",
                    "Z": "backtab",
                }
                if final in mapping:
                    return mapping[final], consumed
                tilde = {
                    "1": "home",
                    "2": "insert",
                    "3": "delete",
                    "4": "end",
                    "5": "pageup",
                    "6": "pagedown",
                    "7": "home",
                    "8": "end",
                }
                if final == "~" and sequence in tilde:
                    return tilde[sequence], consumed
                return None, consumed
            return "escape", 1
        if first < 0x20 or first == 0x7F:
            return self._translate_control(chr(first)), 1
        length = 1
        if first & 0xE0 == 0xC0:
            length = 2
        elif first & 0xF0 == 0xE0:
            length = 3
        elif first & 0xF8 == 0xF0:
            length = 4
        if len(data) < length:
            if not force:
                return None, 0
            return None, 1
        try:
            character = bytes(data[:length]).decode("utf-8")
        except UnicodeDecodeError:
            return None, 1
        return character, length


def make_box(title: str, body: list[str], width: int, terminal: Terminal) -> list[str]:
    inner = max(4, width - 4)
    if terminal.ascii:
        top_left = top_right = bottom_left = bottom_right = "+"
        horizontal = "-"
        vertical = "|"
    else:
        top_left, top_right = "┌", "┐"
        bottom_left, bottom_right = "└", "┘"
        horizontal, vertical = "─", "│"
    heading = f" {title} " if title else ""
    fill = max(0, inner - visible_len(heading))
    lines = [f"  {top_left}{heading}{horizontal * fill}{top_right}"]
    for line in body:
        lines.append(f"  {vertical} {pad(line, inner - 2)} {vertical}")
    lines.append(f"  {bottom_left}{horizontal * inner}{bottom_right}")
    return lines
