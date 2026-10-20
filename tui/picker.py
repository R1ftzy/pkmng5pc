"""Interactive folder and ROM file browser."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from tui.model import ROM_EXTENSIONS, clean_path_text, describe_size, read_rom_info
from tui.terminal import MouseEvent, Terminal, clip, truncate_middle, visible_len


@dataclass
class Entry:
    kind: str
    path: Path
    label: str
    value: str = ""


class Picker:
    def __init__(
        self,
        terminal: Terminal,
        cwd: Path,
        mode: str,
        title: str,
        on_pick: Callable[[Path], None],
        file_extensions: tuple[str, ...] = ROM_EXTENSIONS,
    ):
        self.terminal = terminal
        self.cwd = cwd.resolve(strict=False)
        self.mode = mode
        self.title = title
        self.on_pick = on_pick
        self.file_extensions = tuple(extension.lower() for extension in file_extensions)
        self.show_hidden = False
        self.index = 0
        self._scroll = 0
        self.error = ""
        self.entries: list[Entry] = []
        self.done = False
        self.editing_path = False
        self.path_text = ""
        self.path_cursor = 0
        self.refresh()

    def refresh(self, keep: Path | None = None) -> None:
        selected_path = keep
        if selected_path is None and self.entries and 0 <= self.index < len(self.entries):
            selected_path = self.entries[self.index].path
        entries: list[Entry] = []
        if self.mode == "folder":
            entries.append(Entry("use", self.cwd, "Use this folder", str(self.cwd)))
        if self.cwd.parent != self.cwd:
            entries.append(Entry("parent", self.cwd.parent, "..", ""))
        directories: list[Path] = []
        files: list[Path] = []
        try:
            children = sorted(self.cwd.iterdir(), key=lambda item: item.name.casefold())
        except OSError as error:
            self.error = str(error)
            children = []
        else:
            self.error = ""
        for child in children:
            if not self.show_hidden and child.name.startswith("."):
                continue
            try:
                if child.is_dir():
                    directories.append(child)
                    continue
                if (
                    self.mode == "file"
                    and child.is_file()
                    and child.suffix.lower() in self.file_extensions
                ):
                    files.append(child)
            except OSError:
                continue
        for directory in directories:
            entries.append(Entry("dir", directory, f"{directory.name}/", "directory"))
        for path in files:
            try:
                size = path.stat().st_size
            except OSError:
                continue
            info = read_rom_info(path) if path.suffix.lower() in ROM_EXTENSIONS else None
            if info:
                value = f"{info.display_name} · {info.code or '????'} · {describe_size(size)}"
            else:
                value = describe_size(size)
            entries.append(Entry("file", path, path.name, value))
        self.entries = entries
        self.index = 0
        if selected_path is not None:
            for position, entry in enumerate(entries):
                if entry.path == selected_path:
                    self.index = position
                    break
        if self.index >= len(entries):
            self.index = max(0, len(entries) - 1)

    def _current(self) -> Entry | None:
        if not self.entries:
            return None
        return self.entries[max(0, min(self.index, len(self.entries) - 1))]

    def _move(self, delta: int) -> None:
        if not self.entries:
            return
        self.index = (self.index + delta) % len(self.entries)

    def _chdir(self, path: Path) -> None:
        self.cwd = path.resolve(strict=False)
        self.refresh()
        self.error = ""
        if self.mode == "file":
            for position, entry in enumerate(self.entries):
                if entry.kind == "file":
                    self.index = position
                    break

    def _pick(self, path: Path) -> None:
        try:
            self.on_pick(path)
        except Exception as error:  # a bad callback must not take down the browser
            self.error = str(error)
            return
        self.done = True

    def handle_key(self, key: str) -> str | None:
        if self.editing_path:
            return self._handle_path_edit(key)
        if key in ("escape", "ctrl-c"):
            return "cancel"
        if key == "up":
            self._move(-1)
        elif key == "down":
            self._move(1)
        elif key == "pageup":
            self._move(-10)
        elif key == "pagedown":
            self._move(10)
        elif key == "home":
            self.index = 0
        elif key == "end":
            self.index = max(0, len(self.entries) - 1)
        elif key in ("backspace", "left"):
            if self.cwd.parent != self.cwd:
                self._chdir(self.cwd.parent)
        elif key == "right":
            entry = self._current()
            if entry and entry.kind == "dir":
                self._chdir(entry.path)
        elif key == "enter":
            return self._activate_current()
        elif key in ("s", "S") and self.mode == "folder":
            self._pick(self.cwd)
            return "picked" if self.done else None
        elif key in ("g", "G"):
            self.editing_path = True
            self.path_text = str(self.cwd)
            self.path_cursor = len(self.path_text)
            self.error = ""
        elif key in ("h", "H"):
            self.show_hidden = not self.show_hidden
            self.refresh()
        elif key in ("r", "R"):
            self.refresh()
        return None

    def _activate_current(self) -> str | None:
        entry = self._current()
        if entry is None:
            return None
        if entry.kind == "use":
            self._pick(self.cwd)
            return "picked" if self.done else None
        if entry.kind in ("dir", "parent"):
            self._chdir(entry.path)
            return None
        if entry.kind == "file":
            self._pick(entry.path)
            return "picked" if self.done else None
        return None

    def handle_mouse(self, event: MouseEvent) -> str | None:
        if self.editing_path:
            return None
        if event.kind == "wheel":
            if event.button in ("up", "down"):
                self._move(-3 if event.button == "up" else 3)
            return None
        if event.kind != "press" or event.button != "left":
            return None
        if event.y < 5:
            return None
        body_height = max(3, self.terminal.size()[1] - 7)
        index = self.scroll_top(body_height) + event.y - 5
        if not 0 <= index < len(self.entries):
            return None
        if index == self.index:
            return self._activate_current()
        self.index = index
        return None

    def _handle_path_edit(self, key: str) -> str | None:
        if key in ("escape", "ctrl-c"):
            self.editing_path = False
            self.error = ""
            return None
        if key == "enter":
            text = clean_path_text(self.path_text)
            if not text:
                self.editing_path = False
                return None
            target = Path(text)
            if not target.is_absolute():
                target = self.cwd / target
            target = target.resolve(strict=False)
            if target.is_dir():
                self.editing_path = False
                self._chdir(target)
                return None
            if (
                self.mode == "file"
                and target.is_file()
                and target.suffix.lower() in self.file_extensions
            ):
                self.editing_path = False
                self._pick(target)
                return "picked" if self.done else None
            self.error = f"Not a {self._target_word()}: {target}"
            return None
        if key in ("backspace", "ctrl-h"):
            if self.path_cursor > 0:
                self.path_text = (
                    self.path_text[: self.path_cursor - 1] + self.path_text[self.path_cursor :]
                )
                self.path_cursor -= 1
        elif key == "delete":
            self.path_text = (
                self.path_text[: self.path_cursor] + self.path_text[self.path_cursor + 1 :]
            )
        elif key == "left":
            self.path_cursor = max(0, self.path_cursor - 1)
        elif key == "right":
            self.path_cursor = min(len(self.path_text), self.path_cursor + 1)
        elif key == "home":
            self.path_cursor = 0
        elif key == "end":
            self.path_cursor = len(self.path_text)
        elif key == "ctrl-u":
            self.path_text = ""
            self.path_cursor = 0
        elif len(key) == 1 and key >= " ":
            self.path_text = (
                self.path_text[: self.path_cursor] + key + self.path_text[self.path_cursor :]
            )
            self.path_cursor += 1
        return None

    def _target_word(self) -> str:
        return "folder" if self.mode == "folder" else "ROM file"

    def _path_display(self, width: int) -> str:
        raw = self.path_text if self.editing_path else str(self.cwd)
        prefix = "  Path  "
        room = max(4, width - visible_len(prefix) - 2)
        if len(raw) <= room:
            shown = raw
            start = 0
        else:
            start = max(0, self.path_cursor - room + 8) if self.editing_path else len(raw) - room
            shown = raw[start : start + room]
        if self.editing_path:
            local = self.path_cursor - start
            local = max(0, min(local, len(shown)))
            before = shown[:local]
            at = shown[local] if local < len(shown) else " "
            after = shown[local + 1 :] if local < len(shown) else ""
            shown = before + self.terminal.style(at, reverse=True) + after
            if start > 0:
                shown = self.terminal.grey("‹") + shown
            if start + room < len(raw):
                shown = shown + self.terminal.grey("›")
        return prefix + shown

    def draw(self, width: int, height: int) -> list[str]:
        term = self.terminal
        lines: list[str] = []
        brand = term.bold(term.cyan("pkmng5pc")) + term.dim("  ::  ") + term.bold(self.title)
        lines.append(clip("  " + brand, width))
        lines.append("  " + term.dim(term.rule(width - 4)))
        lines.append(self._path_display(width))
        lines.append("  " + term.dim(term.rule(width - 4)))
        body_height = max(3, height - 7)
        body: list[str] = []
        if self.entries:
            start = self.scroll_top(body_height)
            self._scroll = start
            visible = self.entries[start : start + body_height]
            for position, entry in enumerate(visible, start=start):
                selected = position == self.index
                marker = term.marker(selected)
                label_room = max(8, width - 4 - visible_len(marker))
                if entry.value:
                    label_room = max(8, label_room - min(30, max(12, width // 3)))
                text = truncate_middle(entry.label, label_room)
                if entry.kind == "use":
                    label = term.style(text, color="bright_green", bold=True)
                elif entry.kind == "dir":
                    label = term.style(text, color="cyan", bold=selected)
                elif entry.kind == "parent":
                    label = term.dim(text)
                else:
                    label = term.bold(text) if selected else text
                line = "  " + marker + label
                if entry.value:
                    room = width - 4 - visible_len(marker) - visible_len(label) - 2
                    if room > 8:
                        line += "  " + term.dim(clip(entry.value, room))
                body.append(line)
        else:
            self._scroll = 0
            body.append("  " + term.dim("(empty)"))
        body.extend("" for _ in range(body_height - len(body)))
        lines.extend(body)
        current = self._current()
        if self.editing_path:
            hint = "Enter go · Esc cancel · Ctrl+U clear"
        elif self.error:
            hint = term.red(self.error)
        elif current is None:
            hint = "This folder has no matching entries."
        elif current.kind == "use":
            hint = "Enter chooses this folder. Use G to type a path, H for hidden entries."
        elif current.kind == "dir":
            hint = "Enter opens this folder."
        elif current.kind == "parent":
            hint = "Enter goes up one level."
        elif current.kind == "file":
            hint = f"{current.path}"
        else:
            hint = ""
        lines.append("  " + clip(hint, width - 4))
        lines.append("  " + term.dim(term.rule(width - 4)))
        if self.editing_path:
            keys = "Type a path · Enter go · Esc cancel"
        elif self.mode == "folder":
            keys = "↑↓ move · Enter open · S use folder · G go to · H hidden · R rescan · Esc cancel"
        else:
            keys = "↑↓ move · Enter open/choose · G go to · H hidden · R rescan · Esc cancel"
        lines.append("  " + term.dim(clip(keys, width - 2)))
        while len(lines) < height:
            lines.append("")
        return lines[:height]

    def scroll_top(self, body_height: int) -> int:
        start = self._scroll
        if self.index < start:
            start = self.index
        if self.index >= start + body_height:
            start = self.index - body_height + 1
        return max(0, start)
