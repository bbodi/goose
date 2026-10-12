"""lldb formatters for Goose programs built with `goose build --backend=tinygen`.

tinygen describes each value under its Goose type name, but in the shapes
DWARF can express: a slice as a record { data, len }, an optional as a
pointer, an ADT as { tag, u = { every payload } }, and lldb reads `i8` and
`u8` through clang's types, as characters. These formatters show the values
as a Goose programmer thinks of them:

    (lldb) command script import /path/to/goose/src/lldb/goose_lldb.py
    (lldb) frame variable
    (i64[:]) s = len=5 { [0] = 3 [1] = 1 ... }
    (u8[]) name = "tiny"
    (i64?) o = null
    (Shape) sh = Shape.Rect(w = 3, h = 4)

Arrays (`T[:]`, `T[]`, `T[>..]` and its fat reference `T[>..]&`, `T[..k]`)
show their live elements, `u8` ones as a quoted string when they hold valid
UTF-8; `i8` and `u8` show as numbers; an optional as `null` or what it refers
to; an ADT as its active variant. Types are matched by their Goose names, so
the formatters apply to every instantiation. They live in the `goose` type
category (`type category disable goose` turns them off).
"""
import re

import lldb

CATEGORY = "goose"
TEXT_MAX = 512        # Bytes of a string shown before "..."
BYTES_SHOWN = 32      # Bytes of a u8 array that is not text its summary lists
CHILD_MAX = 100000    # Elements an array offers lldb at most
DEPTH_MAX = 4         # Nesting of the one-line renderings in summaries

# The array kinds by the suffix of their Goose name: a slice `[:]`, an image
# `[]` (or `[u16]`, its length's width), a resizable `[>..]` and its fat
# reference `[>..]&`, a limited `[..k]`, and a runtime-capacity `[..]`. A
# fixed `T[k]` is a DWARF array and needs nothing.
ARRAY_RE = r"^(const )?.+\[(:|>\.\.|\.\.[0-9]*|u8|u16|u32|u64)?\]&?$"
# The u8 ones, text: a string's summary stands for its elements.
TEXT_RE = r"^(const )?u8\[(:|>\.\.|\.\.[0-9]*|u8|u16|u32|u64)?\]&?$"
OPTIONAL_RE = r"^.+\?$"


def _strip(valobj):
    """The value's own members (not the children these formatters give
    it), through a pointer to it (a `u8[] *`)."""
    valobj = valobj.GetNonSyntheticValue()
    if valobj.GetType().IsPointerType():
        return valobj.Dereference().GetNonSyntheticValue()
    return valobj


def _uint(valobj, name):
    child = valobj.GetChildMemberWithName(name)
    if not child.IsValid():
        return None
    return child.GetValueAsUnsigned(0)


def _addr(valobj):
    addr = valobj.GetLoadAddress()
    return None if addr == lldb.LLDB_INVALID_ADDRESS else addr


class Elements:
    """Where an array value's live elements are: their count, the first's
    address, their type, and the DWARF array member holding them where
    there is one (a limited array's slots). `null` for a null fat
    reference."""

    def __init__(self, valobj):
        self.null = False
        self.count, self.addr, self.elem, self.slots = 0, None, None, None
        v = _strip(valobj)
        hdr = v.GetChildMemberWithName("hdr")
        if hdr.IsValid():   # A fat reference { hdr, stk }: the header it points at.
            if hdr.GetValueAsUnsigned(0) == 0:
                self.null = True
                return
            v = hdr.Dereference().GetNonSyntheticValue()
        self.count = _uint(v, "len") or 0
        for name in ("data", "base", "e"):
            m = v.GetChildMemberWithName(name)
            if not m.IsValid():
                continue
            t = m.GetType()
            if t.IsPointerType():          # A slice's data, a header's base
                self.addr = m.GetValueAsUnsigned(0)
                self.elem = t.GetPointeeType()
            elif t.IsArrayType():          # An image's data, a limited array's slots
                self.elem = t.GetArrayElementType()
                self.addr = _addr(m)
                if name == "e":
                    self.slots = m
                    self.count = min(self.count, m.GetNumChildren())
            break
        else:
            # A runtime-capacity T[..]: { cap: u32, len: u32 }, its slots after.
            if v.GetChildMemberWithName("cap").IsValid():
                name = v.GetType().GetName()
                self.elem = v.GetTarget().FindFirstType(name[:-4]) if name.endswith("[..]") else None
                base = _addr(v)
                self.addr = None if base is None else base + 8
        if self.elem is None or not self.elem.IsValid() or (self.addr is None and not self.slots):
            self.count = 0
        self.count = min(self.count, CHILD_MAX)

    def is_text(self):
        return self.elem is not None and self.elem.GetName() in ("u8", "const u8")

    def bytes(self, process, n):
        """The first n elements' bytes, or None where they cannot be read."""
        if n == 0:
            return b""
        if self.addr is not None and process.IsValid():
            err = lldb.SBError()
            raw = process.ReadMemory(self.addr, n, err)
            if err.Success():
                return raw
        if self.slots is not None:   # A limited array held in registers
            raw = self.slots.GetData().uint8s
            return bytes(raw[:n]) if len(raw) >= n else None
        return None


def _quote(text):
    out = []
    for ch in text:
        if ch == '"' or ch == "\\":
            out.append("\\" + ch)
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            out.append("\\r")
        elif ord(ch) < 0x20 or ord(ch) == 0x7f:
            out.append("\\x%02x" % ord(ch))
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def array_summary(valobj, internal_dict):
    els = Elements(valobj)
    if els.null:
        return "null"
    if els.is_text():
        n = min(els.count, TEXT_MAX)
        raw = els.bytes(valobj.GetProcess(), n)
        text = _utf8(raw, n < els.count)
        if text is not None:
            return _quote(text) + ("..." if n < els.count else "")
        if raw is not None:   # Bytes, not text: the summary lists them.
            shown = ", ".join(str(b) for b in raw[:BYTES_SHOWN])
            return "len=%d (%s%s)" % (els.count, shown, ", ..." if els.count > BYTES_SHOWN else "")
    return "len=%d" % els.count


def _utf8(raw, cut):
    """raw as text if it is valid UTF-8, else None. Where raw was cut
    short, its last character may be cut too and is dropped."""
    if raw is None:
        return None
    for drop in range(4 if cut else 1):
        try:
            return raw[:len(raw) - drop].decode("utf-8")
        except UnicodeDecodeError:
            continue
    return None


class ArrayProvider:
    """An array's live elements as its children [0] .. [len - 1]."""

    def __init__(self, valobj, internal_dict):
        self.valobj = valobj
        self.els = None

    def update(self):
        self.els = Elements(self.valobj)
        return False

    def num_children(self, max_count=CHILD_MAX):
        return min(self.els.count, max_count)

    def has_children(self):
        return True

    def get_child_index(self, name):
        m = re.match(r"^\[(\d+)\]$", name)
        return int(m.group(1)) if m else -1

    def get_child_at_index(self, index):
        els = self.els
        if index < 0 or index >= els.count:
            return None
        if els.slots is not None:
            return els.slots.GetChildAtIndex(index)
        size = els.elem.GetByteSize()
        return self.valobj.CreateValueFromAddress("[%d]" % index, els.addr + index * size,
                                                  els.elem)


# ---- Optionals -----------------------------------------------------------------------------------

def _is_null(valobj):
    valobj = valobj.GetNonSyntheticValue()
    hdr = valobj.GetChildMemberWithName("hdr")   # An optional fat reference
    if hdr.IsValid():
        return hdr.GetValueAsUnsigned(0) == 0
    return valobj.GetValueAsUnsigned(0) == 0


_depth = [0]


def _render(v):
    """A value on one line: its summary, its value, or its children in
    parentheses."""
    summary = v.GetSummary()
    if summary:
        return summary
    value = v.GetValue()
    if value is not None:
        return value
    if _depth[0] >= DEPTH_MAX:
        return "..."
    _depth[0] += 1
    try:
        n = v.GetNumChildren()
        parts = []
        for i in range(min(n, 8)):
            c = v.GetChildAtIndex(i)
            parts.append("%s = %s" % (c.GetName(), _render(c)))
        if n > 8:
            parts.append("...")
        return "(" + ", ".join(parts) + ")"
    finally:
        _depth[0] -= 1


def optional_summary(valobj, internal_dict):
    if _is_null(valobj):
        return "null"
    valobj = valobj.GetNonSyntheticValue()
    if valobj.GetChildMemberWithName("hdr").IsValid():
        return array_summary(valobj, internal_dict)
    if _depth[0] >= DEPTH_MAX:
        return "..."
    _depth[0] += 1
    try:
        return _render(valobj.Dereference())
    finally:
        _depth[0] -= 1


class OptionalProvider:
    """A non-null optional's children are its referent's."""

    def __init__(self, valobj, internal_dict):
        self.valobj = valobj
        self.inner = None

    def update(self):
        self.inner = None
        if _is_null(self.valobj):
            return False
        if self.valobj.GetChildMemberWithName("hdr").IsValid():
            self.inner = ArrayProvider(self.valobj, None)
            self.inner.update()
        else:
            self.inner = self.valobj.Dereference()
        return False

    def num_children(self, max_count=CHILD_MAX):
        if self.inner is None:
            return 0
        if isinstance(self.inner, ArrayProvider):
            return self.inner.num_children(max_count)
        return self.inner.GetNumChildren()

    def has_children(self):
        return self.num_children() > 0

    def get_child_index(self, name):
        if isinstance(self.inner, ArrayProvider):
            return self.inner.get_child_index(name)
        return self.inner.GetIndexOfChildWithName(name) if self.inner else -1

    def get_child_at_index(self, index):
        if isinstance(self.inner, ArrayProvider):
            return self.inner.get_child_at_index(index)
        return self.inner.GetChildAtIndex(index) if self.inner else None


# ---- ADTs ----------------------------------------------------------------------------------------

def is_adt(sbtype, internal_dict):
    """An ADT's record: its first member `tag`, an enum named `<ADT>.tag`."""
    t = sbtype.GetCanonicalType()
    if t.GetTypeClass() != lldb.eTypeClassStruct or t.GetNumberOfFields() == 0:
        return False
    tag = t.GetFieldAtIndex(0)
    return tag.GetName() == "tag" and tag.GetType().GetName() == t.GetName() + ".tag"


def _active(valobj):
    """(the variant's name, its payload or None)."""
    v = _strip(valobj)
    tag = v.GetChildMemberWithName("tag")
    name = tag.GetValue() or str(tag.GetValueAsUnsigned(0))
    payload = v.GetChildMemberWithName("u").GetChildMemberWithName(name)
    if not payload.IsValid():
        return name, None
    if payload.GetNumChildren() == 1 and payload.GetChildAtIndex(0).GetName() == "gs_empty":
        return name, None
    return name, payload


def adt_summary(valobj, internal_dict):
    v = _strip(valobj)
    name, payload = _active(v)
    out = v.GetType().GetCanonicalType().GetName() + "." + name
    if payload is None:
        return out
    if _depth[0] >= DEPTH_MAX:
        return out + "(...)"
    _depth[0] += 1
    try:
        parts = []
        for i in range(payload.GetNumChildren()):
            c = payload.GetChildAtIndex(i)
            parts.append("%s = %s" % (c.GetName(), _render(c)))
        return out + "(" + ", ".join(parts) + ")"
    finally:
        _depth[0] -= 1


class AdtProvider:
    """An ADT's children are its active variant's fields."""

    def __init__(self, valobj, internal_dict):
        self.valobj = valobj
        self.payload = None

    def update(self):
        _, self.payload = _active(self.valobj)
        return False

    def num_children(self, max_count=CHILD_MAX):
        return self.payload.GetNumChildren() if self.payload else 0

    def has_children(self):
        return self.num_children() > 0

    def get_child_index(self, name):
        return self.payload.GetIndexOfChildWithName(name) if self.payload else -1

    def get_child_at_index(self, index):
        return self.payload.GetChildAtIndex(index) if self.payload else None


# ---- Registration --------------------------------------------------------------------------------

def __lldb_init_module(debugger, internal_dict):
    me = __name__
    for cmd in [
        # i8 and u8 as numbers, wherever they appear (a typedef's format
        # cascades to fields and array elements), and a fixed array of them
        # as its elements, where lldb would show a C string.
        "type format add -w %s -f d i8" % CATEGORY,
        "type format add -w %s -f u u8" % CATEGORY,
        "type summary add -w %s -c -x '^[iu]8(\\[[0-9]+\\])+$'" % CATEGORY,
        # Of two regular expressions that match, lldb takes the one added
        # last: text's summary replaces the other arrays' expanded one.
        "type summary add -w %s -e -x '%s' -F %s.array_summary" % (CATEGORY, ARRAY_RE, me),
        "type synthetic add -w %s -x '%s' -l %s.ArrayProvider" % (CATEGORY, ARRAY_RE, me),
        "type summary add -w %s -x '%s' -F %s.array_summary" % (CATEGORY, TEXT_RE, me),
        "type summary add -w %s -v -x '%s' -F %s.optional_summary" % (CATEGORY, OPTIONAL_RE, me),
        "type synthetic add -w %s -x '%s' -l %s.OptionalProvider" % (CATEGORY, OPTIONAL_RE, me),
        "type summary add -w %s --recognizer-function %s.is_adt -F %s.adt_summary"
        % (CATEGORY, me, me),
        "type synthetic add -w %s --recognizer-function %s.is_adt -l %s.AdtProvider"
        % (CATEGORY, me, me),
        "type category enable %s" % CATEGORY,
    ]:
        debugger.HandleCommand(cmd)
