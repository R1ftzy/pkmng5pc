"""ROM header parsing and folder scanning tests."""

from __future__ import annotations

import hashlib
import tempfile
import unittest
from pathlib import Path

from tui.model import TITLE_BY_CODE, read_rom_info, scan_roms, sha1_file


def write_rom(
    path: Path,
    code: str = "IRBO",
    title: str = "POKEMON B",
    size: int = 0x1000,
) -> Path:
    header = bytearray(0x20)
    encoded_title = title.encode("ascii")[:12]
    header[: len(encoded_title)] = encoded_title
    header[0x0C:0x10] = code.encode("ascii")[:4].ljust(4, b" ")
    path.write_bytes(bytes(header) + b"\x00" * max(0, size - 0x20))
    return path


class RomHeaderTests(unittest.TestCase):
    def test_reads_a_supported_header(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = write_rom(Path(temp) / "game.nds", code="IRBO", size=0x40000)
            info = read_rom_info(path)

        self.assertIsNotNone(info)
        assert info is not None
        self.assertEqual(info.title, "POKEMON B")
        self.assertEqual(info.code, "IRBO")
        self.assertEqual(info.title_id, "black")
        self.assertTrue(info.supported)
        self.assertEqual(info.display_name, "Pokémon Black")
        self.assertEqual(info.size, 0x40000)
        self.assertEqual(info.save_path, path.with_suffix(".sav"))

    def test_reads_every_supported_code(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            for code, (title_id, display_name) in TITLE_BY_CODE.items():
                with self.subTest(code=code):
                    info = read_rom_info(write_rom(Path(temp) / f"{code}.nds", code=code))
                    assert info is not None
                    self.assertEqual(info.title_id, title_id)
                    self.assertEqual(info.display_name, display_name)

    def test_unknown_code_keeps_the_raw_title(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            info = read_rom_info(
                write_rom(Path(temp) / "homebrew.srl", code="HMBR", title="HOMEBREW")
            )

        assert info is not None
        self.assertEqual(info.code, "HMBR")
        self.assertIsNone(info.title_id)
        self.assertFalse(info.supported)
        self.assertEqual(info.display_name, "HOMEBREW")

    def test_non_alphanumeric_code_is_dropped(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            info = read_rom_info(write_rom(Path(temp) / "game.nds", code="IR O"))

        assert info is not None
        self.assertEqual(info.code, "")
        self.assertIsNone(info.title_id)
        self.assertEqual(info.display_name, "POKEMON B")

    def test_short_and_missing_files_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            short = Path(temp) / "short.nds"
            short.write_bytes(b"\x00" * 8)
            self.assertIsNone(read_rom_info(short))
            self.assertIsNone(read_rom_info(Path(temp) / "missing.nds"))


class RomScanTests(unittest.TestCase):
    def test_scans_supported_extensions_in_top_level_only(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            write_rom(root / "b.srl", code="IRAO", title="POKEMON W")
            write_rom(root / "A.NDS", code="IRBO", title="POKEMON B")
            (root / "notes.txt").write_text("not a ROM", encoding="utf-8")
            nested = root / "nested"
            nested.mkdir()
            write_rom(nested / "c.nds", code="IREO")
            found = scan_roms(root)

        self.assertEqual([item.path.name for item in found], ["A.NDS", "b.srl"])
        self.assertEqual([item.code for item in found], ["IRBO", "IRAO"])

    def test_missing_or_empty_folders_scan_as_empty(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.assertEqual(scan_roms(root / "missing"), [])
            self.assertEqual(scan_roms(root), [])


class Sha1Tests(unittest.TestCase):
    def test_chunked_hash_matches_hashlib(self) -> None:
        payload = b"pkmng5pc" * 1000
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "game.nds"
            path.write_bytes(payload)
            self.assertEqual(sha1_file(path, chunk_size=7), hashlib.sha1(payload).hexdigest())
