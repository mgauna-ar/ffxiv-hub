"""Reads SeStrings, the game's sheet text: UTF-8 with inline macros.

A macro is 0x02, its type, the length of its payload as an SeString integer, the
payload, and 0x03. The payload can hold 0x03 itself (the integer 2, or a nested
macro's end), so a macro is skipped by its length, never by scanning for 0x03.
Every string in Action, Status and ActionTransient parses this way on patch 7.56.
"""

import operator

_START, _END = 0x02, 0x03
_IF, _NEW_LINE, _HYPHEN = 0x08, 0x10, 0x1F

# Expression markers inside a macro's payload.
_COMPARE = {0xE0: operator.ge, 0xE1: operator.gt, 0xE2: operator.le,
            0xE3: operator.lt, 0xE4: operator.eq, 0xE5: operator.ne}
_PLAYER_PARAMETER = 0xE9
_STRING = 0xFF

# The player parameters combat descriptions branch on, as `render` takes them.
CLASS_JOB = 68
LEVEL = 72


def read_integer(raw: bytes, at: int):
    """The SeString integer at raw[at], and the offset past it.

    A byte below 0xD0 is the value plus one. 0xF0 to 0xFE are followed by up to four
    big-endian bytes, which the low four bits of the marker plus one say are there.
    """
    marker = raw[at]
    if marker < 0xD0:
        return marker - 1, at + 1
    if not 0xF0 <= marker <= 0xFE:
        raise ValueError(f"0x{marker:02x} at {at} is an expression, not an integer")
    present = (marker + 1) & 0x0F
    value, at = 0, at + 1
    for bit, shift in ((8, 24), (4, 16), (2, 8), (1, 0)):
        if present & bit:
            value |= raw[at] << shift
            at += 1
    return value, at


def _macro(raw: bytes, at: int):
    """The macro starting at raw[at]: its type, payload and the offset past it."""
    kind = raw[at + 1]
    length, start = read_integer(raw, at + 2)
    end = start + length
    if end >= len(raw) or raw[end] != _END:
        raise ValueError(f"macro 0x{kind:02x} at {at} does not end where its length says")
    return kind, raw[start:end], end + 1


def _text(raw: bytes, on_if) -> str:
    out = bytearray()
    at = 0
    while at < len(raw):
        if raw[at] != _START:
            out.append(raw[at])
            at += 1
            continue
        kind, payload, at = _macro(raw, at)
        # Hyphen is text, not formatting: "Tam-Tara" is stored as Tam<0x1F>Tara.
        if kind == _HYPHEN:
            out += b"-"
        elif kind == _NEW_LINE:
            out += b"\n"
        elif kind == _IF and on_if is not None:
            out += on_if(payload).encode("utf-8")
    return out.decode("utf-8", "replace")


def decode(raw: bytes) -> str:
    """The literal text: Hyphen as "-", NewLine as a line break, every other macro
    dropped whole, conditional text included."""
    return _text(raw, None)


def render(raw: bytes, parameters: dict) -> str:
    """decode, with each If macro replaced by the branch `parameters` pick.

    `parameters` maps a player parameter to its value, such as
    {CLASS_JOB: 33, LEVEL: 100} for an Astrologian at 100. A condition on a
    parameter it lacks raises KeyError.
    """
    def expression(payload, at):
        marker = payload[at]
        if marker < 0xD0 or 0xF0 <= marker <= 0xFE:
            return read_integer(payload, at)
        if marker in _COMPARE:
            left, at = expression(payload, at + 1)
            right, at = expression(payload, at)
            return int(_COMPARE[marker](left, right)), at
        if marker == _PLAYER_PARAMETER:
            index, at = expression(payload, at + 1)
            if index not in parameters:
                raise KeyError(f"no value given for player parameter {index}")
            return parameters[index], at
        if marker == _STRING:
            length, at = read_integer(payload, at + 1)
            return payload[at:at + length], at + length
        raise ValueError(f"unsupported expression 0x{marker:02x}")

    def branch(payload):
        condition, at = expression(payload, 0)
        if_true, at = expression(payload, at)
        if_false, _ = expression(payload, at)
        taken = if_true if condition else if_false
        return render(taken, parameters) if isinstance(taken, bytes) else str(taken)

    return _text(raw, branch)
