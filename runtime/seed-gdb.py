# Pretty printers for Seed values in gdb (seed build -g; docs/guide,
# chapter 14). Load with:
#
#   gdb -x $SEED_ROOT/runtime/seed-gdb.py ./prog
#
# A Seed local `x` is the C variable l_x, or the field L.l_x of the
# function's cleanup frame; `info locals` lists both. With the printers,
# `p l_n` shows an int as a number, `p L.l_name` a string as its text, a
# list as its elements, a map as its entries, and error and interface
# values as their dynamic type.
import gdb

# sd_kind (runtime/seed.h)
K_FLOAT, K_BOOL, K_BYTE, K_FN, K_INT, K_STR, K_LIST, K_MAP = 1, 2, 3, 4, 5, 6, 7, 8
LIMIT = 50  # elements shown at most


def int_text(v):
    w = int(v["w"])
    if w == 1:
        return "null"
    if w & 1 == 0:
        return str(w >> 1)
    return "<big integer>"


def str_text(p):
    if int(p) == 0:
        return "null"
    s = p.dereference()
    n = int(s["len"])
    if n < 0 or n > 1 << 30:
        return "<not a string>"
    raw = bytes(gdb.selected_inferior().read_memory(s["data"].address, min(n, 1000)))
    out = []
    for c in raw:
        ch = chr(c)
        if ch == '"' or ch == "\\":
            out.append("\\" + ch)
        elif 32 <= c < 127:
            out.append(ch)
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        else:
            out.append("\\x%02x" % c)
    return '"' + "".join(out) + ('..."' if n > 1000 else '"')


def elem_text(kind, addr):
    """The text of an element of a built-in kind at addr (a void *)."""
    if kind == K_INT:
        return int_text(addr.cast(gdb.lookup_type("sd_int").pointer()).dereference())
    if kind == K_STR:
        return str_text(addr.cast(gdb.lookup_type("sd_str").pointer()).dereference())
    if kind == K_FLOAT:
        return str(float(addr.cast(gdb.lookup_type("double").pointer()).dereference()))
    if kind == K_BOOL:
        return "true" if int(addr.cast(gdb.lookup_type("char").pointer()).dereference()) else "false"
    if kind == K_BYTE:
        return str(int(addr.cast(gdb.lookup_type("unsigned char").pointer()).dereference()))
    return None


class IntPrinter:
    def __init__(self, v):
        self.v = v

    def to_string(self):
        return int_text(self.v)


class StrPrinter:
    def __init__(self, v):
        self.v = v

    def to_string(self):
        return str_text(self.v)


class ListPrinter:
    def __init__(self, v):
        self.v = v

    def to_string(self):
        if int(self.v) == 0:
            return "null"
        l = self.v.dereference()
        n = int(l["len"])
        et = l["et"].dereference()
        kind, size = int(et["kind"]), int(et["size"])
        base = (self.v + 1).cast(gdb.lookup_type("char").pointer())
        parts = []
        for i in range(min(n, LIMIT)):
            t = elem_text(kind, (base + i * size).cast(gdb.lookup_type("void").pointer()))
            if t is None:
                return "[]%s of length %d" % (et["name"].string(), n)
            parts.append(t)
        if n > LIMIT:
            parts.append("...")
        return "[" + ", ".join(parts) + "] (length %d)" % n


class MapPrinter:
    def __init__(self, v):
        self.v = v

    def to_string(self):
        if int(self.v) == 0:
            return "null"
        m = self.v.dereference()
        n, used = int(m["len"]), int(m["n"])
        kt, vt = m["kt"].dereference(), m["vt"].dereference()
        koff, voff, esz = int(m["koff"]), int(m["voff"]), int(m["esz"])
        ents = m["entries"]
        u64 = gdb.lookup_type("unsigned long").pointer()
        vp = gdb.lookup_type("void").pointer()
        parts = []
        for i in range(used):
            e = ents + i * esz
            if int((e + 8).cast(u64).dereference()) == 0:
                continue  # deleted
            k = elem_text(int(kt["kind"]), (e + koff).cast(vp))
            v = elem_text(int(vt["kind"]), (e + voff).cast(vp))
            parts.append("%s: %s" % (k, v if v is not None else "..."))
            if len(parts) >= LIMIT:
                parts.append("...")
                break
        return "{" + ", ".join(parts) + "} (length %d)" % n


class IfacePrinter:
    def __init__(self, v):
        self.v = v

    def to_string(self):
        vt = self.v["vt"]
        if int(vt) == 0:
            return "null"
        return "<%s>" % vt.dereference()["type_name"].string()


class Safe:
    """A printer that shows <unreadable> for what is not a value yet (a
    variable before its declaration, say)."""

    def __init__(self, p):
        self.p = p

    def to_string(self):
        try:
            return self.p.to_string()
        except (gdb.error, gdb.MemoryError, ValueError, OverflowError):
            return "<unreadable>"


def lookup(v):
    p = find(v)
    return Safe(p) if p else None


def find(v):
    t = v.type.strip_typedefs()
    name = v.type.unqualified().name
    if name == "sd_int" or (t.code == gdb.TYPE_CODE_STRUCT and t.tag == "sd_int"):
        return IntPrinter(v)
    if name == "sd_str":
        return StrPrinter(v)
    if t.code == gdb.TYPE_CODE_PTR:
        target = t.target().strip_typedefs()
        if target.tag == "sd_string":
            return StrPrinter(v)
        if target.tag == "sd_list":
            return ListPrinter(v)
        if target.tag == "sd_map":
            return MapPrinter(v)
    if name in ("sd_err", "sd_iface") or (t.code == gdb.TYPE_CODE_STRUCT and t.tag == "sd_iface"):
        return IfacePrinter(v)
    return None


gdb.pretty_printers.append(lookup)


class SeedLocals(gdb.Command):
    """seed-locals: the Seed variables of the selected frame, by their Seed
    names: the C locals and parameters l_name (o_name for a parameter the
    function assigns), the fields l_name of its cleanup frame L, and inside
    a loop the raw copies r_name_N of its int accumulators. A variable that
    is not declared yet shows whatever its C variable holds."""

    def __init__(self):
        super().__init__("seed-locals", gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        frame = gdb.selected_frame()
        seen = {}
        try:
            block = frame.block()
        except RuntimeError:
            print("no symbols for this frame")
            return
        while block is not None:
            for sym in block:
                if not (sym.is_variable or sym.is_argument):
                    continue
                n = sym.name
                v = sym.value(frame)
                if n == "L" and v.type.strip_typedefs().code == gdb.TYPE_CODE_STRUCT:
                    for f in v.type.strip_typedefs().fields():
                        if f.name and (f.name.startswith("l_") or f.name.startswith("o_")):
                            seen.setdefault(f.name[2:], v[f.name])
                elif n.startswith("l_") or n.startswith("o_"):
                    seen.setdefault(n[2:], v)
                elif n.startswith("r_") and n.rsplit("_", 1)[-1].isdigit():
                    # a loop's raw copy of an int (compiler/README.md,
                    # "Loop accumulators"): the current value
                    seen.setdefault(n[2:].rsplit("_", 1)[0], v)
            if block.function is not None:
                break
            block = block.superblock
        for name in sorted(seen):
            try:
                text = str(seen[name])
            except gdb.error:
                text = "<unreadable>"
            print("%s = %s" % (name, text))


SeedLocals()
