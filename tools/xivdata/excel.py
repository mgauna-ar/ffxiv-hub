"""Parses FFXIV Excel sheets (.exh headers and .exd row pages).

Both formats are big-endian, unlike the SqPack container around them.
Verified against ClassJob on patch 7.56.
"""

import struct

EXH_MAGIC = b"EXHF"
EXD_MAGIC = b"EXDF"

LANGUAGE_NONE = 0
LANGUAGE_EN = 2

# Column kinds this reader understands. 0x19..0x20 are single bits packed into one
# byte, bit 0 first.
_STRING = 0x00
_PACKED_BOOL_FIRST, _PACKED_BOOL_LAST = 0x19, 0x20
_INT_FORMATS = {
    0x01: "?",   # bool
    0x02: "b",   # int8
    0x03: "B",   # uint8
    0x04: "h",   # int16
    0x05: "H",   # uint16
    0x06: "i",   # int32
    0x07: "I",   # uint32
    0x09: "f",   # float32
    0x0A: "q",   # int64
    0x0B: "Q",   # uint64
}


def decode_sestring(raw: bytes) -> str:
    """Game strings carry inline macro payloads; keep only the literal text."""
    out = bytearray()
    i = 0
    while i < len(raw):
        byte = raw[i]
        if byte == 0x02:  # macro start: skip to its 0x03 terminator
            end = raw.find(b"\x03", i)
            if end == -1:
                break
            i = end + 1
            continue
        out.append(byte)
        i += 1
    return out.decode("utf-8", "replace")


class Sheet:
    """One Excel sheet, read lazily out of a SqPack archive."""

    def __init__(self, pack, name: str, language: int = LANGUAGE_EN):
        self.name = name
        self._pack = pack
        header = pack.read(f"exd/{name}.exh")
        if header[:4] != EXH_MAGIC:
            raise ValueError(f"{name}.exh is not an EXH file")

        (self.row_size, self.column_count, page_count, language_count) = struct.unpack_from(
            ">HHHH", header, 0x06
        )
        self.row_count, = struct.unpack_from(">I", header, 0x14)

        self.columns = [
            struct.unpack_from(">HH", header, 0x20 + i * 4) for i in range(self.column_count)
        ]
        pages_at = 0x20 + self.column_count * 4
        self._pages = [
            struct.unpack_from(">II", header, pages_at + i * 8)[0] for i in range(page_count)
        ]
        languages_at = pages_at + page_count * 8
        available = {header[languages_at + i * 2] for i in range(language_count)}

        # Sheets without translations are stored with no language suffix.
        self.language = language if language in available else LANGUAGE_NONE

    def _page_path(self, start_id: int) -> str:
        suffix = {LANGUAGE_NONE: "", 2: "_en", 1: "_ja", 3: "_de", 4: "_fr"}[self.language]
        return f"exd/{self.name}_{start_id}{suffix}.exd"

    def rows(self):
        """Yields (row_id, accessor) for every row, in file order."""
        for start_id in self._pages:
            path = self._page_path(start_id)
            if not self._pack.exists(path):
                continue
            data = self._pack.read(path)
            if data[:4] != EXD_MAGIC:
                raise ValueError(f"{path} is not an EXD file")
            index_size, = struct.unpack_from(">I", data, 0x08)
            for i in range(index_size // 8):
                row_id, offset = struct.unpack_from(">II", data, 0x20 + i * 8)
                yield row_id, _Row(self, data, offset + 6)

    def as_dict(self, *columns):
        """{row_id: (col, ...)} for the given column indices."""
        return {rid: tuple(row[c] for c in columns) for rid, row in self.rows()}


class _Row:
    __slots__ = ("_sheet", "_data", "_base")

    def __init__(self, sheet, data, base):
        self._sheet = sheet
        self._data = data
        self._base = base

    def __getitem__(self, column: int):
        kind, offset = self._sheet.columns[column]
        at = self._base + offset
        if kind == _STRING:
            heap = self._base + self._sheet.row_size
            start, = struct.unpack_from(">I", self._data, at)
            end = self._data.index(b"\0", heap + start)
            return decode_sestring(self._data[heap + start:end])
        if kind in _INT_FORMATS:
            return struct.unpack_from(">" + _INT_FORMATS[kind], self._data, at)[0]
        if _PACKED_BOOL_FIRST <= kind <= _PACKED_BOOL_LAST:
            return bool((self._data[at] >> (kind - _PACKED_BOOL_FIRST)) & 1)
        raise ValueError(f"unsupported column kind 0x{kind:02x} in {self._sheet.name}")
