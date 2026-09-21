"""Reads files out of a FFXIV SqPack archive.

Only what the table generator needs: look a path up in the .index and pull the
file out of the matching .datN. Verified against exd/root.exl on patch 7.56.
"""

import struct
import zlib
from pathlib import Path

SQPACK_MAGIC = b"SqPack\0\0"

# A block whose header reports this or more is stored uncompressed.
UNCOMPRESSED_MARKER = 32000


def path_hash(text: str) -> int:
    """CRC32 of the lowercased name without the final inversion zlib applies."""
    return (~zlib.crc32(text.lower().encode())) & 0xFFFFFFFF


class SqPack:
    def __init__(self, repo_dir, category="0a0000", platform="win32"):
        # Not Path.with_suffix: "0a0000.win32" already looks like it has one.
        self._base = str(Path(repo_dir) / f"{category}.{platform}")
        self._index = Path(self._base + ".index").read_bytes()
        self._dats = {}
        self._entries = self._read_index()

    def _read_index(self):
        if self._index[:8] != SQPACK_MAGIC:
            raise ValueError(f"{self._base}.index is not a SqPack file")
        header_size, = struct.unpack_from("<I", self._index, 0x0C)
        data_offset, data_size = struct.unpack_from("<II", self._index, header_size + 0x08)

        entries = {}
        for i in range(data_size // 16):
            file_hash, dir_hash, packed, _ = struct.unpack_from(
                "<IIII", self._index, data_offset + i * 16
            )
            entries[(dir_hash, file_hash)] = ((packed & ~0xF) * 8, (packed >> 1) & 0x7)
        return entries

    def _dat(self, data_file_id: int) -> bytes:
        if data_file_id not in self._dats:
            self._dats[data_file_id] = Path(f"{self._base}.dat{data_file_id}").read_bytes()
        return self._dats[data_file_id]

    def exists(self, path: str) -> bool:
        folder, _, name = path.rpartition("/")
        return (path_hash(folder), path_hash(name)) in self._entries

    def read(self, path: str) -> bytes:
        folder, _, name = path.rpartition("/")
        key = (path_hash(folder), path_hash(name))
        if key not in self._entries:
            raise KeyError(f"not in archive: {path}")
        offset, data_file_id = self._entries[key]
        return self._read_file(self._dat(data_file_id), offset)

    @staticmethod
    def _read_file(dat: bytes, offset: int) -> bytes:
        header_size, file_type, raw_size = struct.unpack_from("<III", dat, offset)
        if file_type != 2:
            # Only binary files; models and textures use a different block layout.
            raise ValueError(f"unsupported file type {file_type} at 0x{offset:x}")
        block_count, = struct.unpack_from("<I", dat, offset + 0x14)

        out = bytearray()
        for i in range(block_count):
            block_offset, _, _ = struct.unpack_from("<IHH", dat, offset + 0x18 + i * 8)
            block = offset + header_size + block_offset
            block_header_size, _, compressed_size, uncompressed_size = struct.unpack_from(
                "<IIII", dat, block
            )
            if compressed_size >= UNCOMPRESSED_MARKER:
                body = dat[block + block_header_size:][:uncompressed_size]
                out += body
            else:
                body = dat[block + block_header_size:][:compressed_size]
                out += zlib.decompress(body, -15)

        if len(out) != raw_size:
            raise ValueError(f"size mismatch at 0x{offset:x}: {len(out)} != {raw_size}")
        return bytes(out)

    def sheet_names(self):
        """Sheet names from exd/root.exl, which also proves the reader works."""
        text = self.read("exd/root.exl").decode("utf-8")
        return [line.split(",")[0] for line in text.splitlines()[1:] if line]
