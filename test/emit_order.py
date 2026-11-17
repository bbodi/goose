"""The generated C must not depend on which C++ compiler built goose. The code
generator's emitting calls (CT and the other type namers, Gen*, LoadLoc, Top,
LocArgs, StrRaw, ...) append to an output stream or take names and numbers
as they go, so the order of two of them is the order of what they emit -- and
C++ leaves the order of a call's arguments, and of most operators' operands,
to the compiler: clang evaluates left to right, gcc and MSVC right to left.

This check reads src/codegen*.h and src/tinygen_backend.h and reports every
expression in which two different calls that emit to a common stream are
arguments of one call or operands of one unsequenced operator (goose#122).
Which members emit where is computed, not listed: a member that appends to an
output stream, numbers a temporary or registers a stack's top emits there, and
so does every member that calls one, and a lambda a function names likewise
inside that function. The branches of ?: and the operands of && and || are
sequenced or exclusive, and two spellings of the same call emit the same thing
once, so those are not reported.

The tinygen backend emits through the tinygen library: a tyg_ call given the
procedure numbers a value, a local or a block or appends an instruction (the
"ir" stream), one given the module numbers a symbol or a type or adds data
("module"). Every value is numbered, a constant too, and tinygen's C names
values by number, so there two spellings of the same call are two emissions.
A member without parameters emits one thing to the module, once (SliceAT, the
record every header shares): two calls that reach the module only through the
same such member emit it whichever runs first, and are not reported."""
import glob
import os
import re
import sys

# What a member writes directly, by stream: the output strings (CodeGen's
# members of those names), the temporaries' numbering (the body's) and the
# cached tops' numbering.
WRITES = {
    "tdecls": ["tdecls"],
    "body": ["body", "paramcopies"],
    "data": ["data", "gblock"],
    "pdata": ["pdata"],
    "protos": ["protos", "exportprotos"],
    "code": ["code"],
}
DIRECT = {stream: rf"(?<![\w.>])(?:{'|'.join(names)})\s*\+=|\bAppend\(\s*(?:{'|'.join(names)})\b"
          for stream, names in WRITES.items()}
DIRECT["body"] += r"|\btmpn\+\+"
DIRECT["protos"] += r"|\b(?:erqueue|renderqueue)\.push_back"
DIRECT["tops"] = r"\btoporder\.push_back"
# tinygen: a tyg_ call given the procedure, or the module; a query of either emits nothing.
DIRECT["ir"] = r"\btyg_(?!(?:val_type|param)\b)\w+\(\s*(?:\w+(?:\.|->))?p\s*[,)]"
DIRECT["module"] = (r"\btyg_(?!error\b|type_(?:ptr|int|scalar)\b)\w+\(\s*(?:\w+(?:\.|->))?m\s*[,)]"
                    r"|\b(?:shims|externs|direct)\.(?:push|emplace)_back")
# Streams whose calls emit every time, not once per spelling; those a member without
# parameters writes once.
EVERY = {"ir"}
ONCE = {"module"}
SOURCES = ("codegen*.h", "tinygen_backend.h")
BASE = {"L": {"body"}, "T": {"body"}, "Lbl": {"body"}}
CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
# Operators whose operands C++ leaves unsequenced (so not && || ?: = , << >>).
UNSEQ = re.compile(r"==|!=|<=|>=|(?<!-)>(?![>=])|<(?![<=])|[-+*/%&|^](?![-+=&|>])")
# Where one piece is sequenced before the next, or only one of them runs.
SEQ = re.compile(r"&&|\|\||\?|:(?!:)|,|;|\s=\s|\+=|-=|\|=|&=")
CONTROL = re.compile(r"\b(?:if|while|for|switch|return|case|else)\b")
KEYWORDS = {"if", "while", "for", "switch", "return", "case", "else", "catch", "sizeof"}
LAMBDA = re.compile(r"[\])]\s*(?:mutable\s*)?(?:->\s*[\w:<>*& ]+?)?\s*$")
NAMED = re.compile(r"\bauto\s+(\w+)\s*=\s*\[")
RETURNED = re.compile(r"\breturn\s*\[[^\]]*\]\s*(?:\([^()]*\))?\s*\{(?!\})")
HEAD = re.compile(r"^(?:inline |    )(?:template<[^\n]*>\s*)?(?:static |friend )?"
                  r"[^\n;{}=]*?\b(\w+)\s*\(", re.M)


def strip(text):
    """Comments gone, string and character literals emptied; line breaks kept."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(c + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def matching(s, i, open="(", close=")"):
    depth = 0
    for j in range(i, len(s)):
        if s[j] == open:
            depth += 1
        elif s[j] == close:
            depth -= 1
            if depth == 0:
                return j
    return -1


def functions(text):
    """(name, body, line, parameters) for every function defined at file or class scope."""
    for m in HEAD.finditer(text):
        close = matching(text, m.end() - 1)
        if close < 0 or m.group(1) in KEYWORDS:
            continue
        b = re.match(r"\s*(?:const\s*)?(?:->\s*[\w:<>*& ]+)?\s*(?::[^{;]*)?\{", text[close + 1:])
        if not b:
            continue
        start = close + b.end()
        end = matching(text, start, "{", "}")
        if end >= 0:
            yield (m.group(1), text[start:end + 1], text.count("\n", 0, m.start()) + 1,
                   text[m.end():close].strip())


def statements(body, base=0):
    """(statement, offset in the function) for each statement of body. A lambda's
    body inside an argument list is statements of its own, blank where it stood: it
    runs when it is called, not where it is written."""
    chars, depth, i = list(body), 0, 0
    while i < len(body):
        c = body[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif c == "{" and depth > 0 and LAMBDA.search(body, max(0, i - 80), i):
            end = matching(body, i, "{", "}")
            if end < 0:
                break
            yield from statements(body[i + 1:end], base + i + 1)
            for j in range(i + 1, end):
                if chars[j] != "\n":
                    chars[j] = " "
            i = end
        i += 1
    text, depth, pos = "".join(chars), 0, 0
    for i, c in enumerate(text):
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif c in ";{}" and depth <= 0:
            depth = 0
            yield text[pos:i], base + pos
            pos = i + 1


def lambdas(body):
    """name -> body of each lambda the function body names (auto name = [...] ...),
    less a lambda it returns, which calling it does not run."""
    found = {}
    for m in NAMED.finditer(body):
        open = body.find("{", m.end())
        end = matching(body, open, "{", "}") if open >= 0 else -1
        if end < 0:
            continue
        text = body[open:end + 1]
        while (r := RETURNED.search(text)):
            close = matching(text, r.end() - 1, "{", "}")
            if close < 0:
                break
            text = text[:r.end()] + text[close:]
        found[m.group(1)] = text
    return found


def split_top(s, seps):
    """s split at the depth-0 matches of seps."""
    parts, depth, last, i = [], 0, 0, 0
    while i < len(s):
        c = s[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif depth == 0:
            m = seps.match(s, i)
            if m:
                parts.append(s[last:i])
                i = last = m.end()
                continue
        i += 1
    parts.append(s[last:])
    return parts


class Checker:
    def __init__(self, streams):
        self.streams = streams   # member name -> the streams it emits to, each with its sources
        self.found = []

    def emitting(self, expr):
        """Each emitting call inside expr, as its spelling, with its streams."""
        calls, pos = {}, 0
        while (m := CALL.search(expr, pos)):
            pos = m.end()
            close = matching(expr, m.end() - 1)
            if close < 0:
                continue
            # A member's streams, or those the call writes itself (a tyg_ call).
            streams = self.streams.get(m.group(1)) or {
                s: {""} for s, rx in DIRECT.items() if re.match(rx, expr[m.start():close + 1])}
            if streams:
                calls[" ".join(expr[m.start():close + 1].split())] = streams
        return calls

    def check(self, expr, where):
        pieces = split_top(expr, SEQ)
        if len(pieces) > 1:
            for p in pieces:
                self.check(p, where)
            return
        operands = split_top(expr, UNSEQ)
        if len(operands) > 1:
            self.pair(operands, where, expr)
            for o in operands:
                self.check(o, where)
            return
        # Each parenthesized group: a call's arguments are unsequenced with
        # respect to each other; a grouping is checked as a whole.
        pos = 0
        while (open := expr.find("(", pos)) >= 0:
            close = matching(expr, open)
            if close < 0:
                break
            call = re.search(r"([A-Za-z_]\w*)\s*$", expr[:open])
            if call and call.group(1) not in KEYWORDS:
                args = split_top(expr[open + 1:close], re.compile(r","))
                self.pair(args, where, expr[call.start():close + 1])
                for a in args:
                    self.check(a, where)
            else:
                self.check(expr[open + 1:close], where)
            pos = close + 1

    def pair(self, parts, where, text):
        """Two parts evaluated in no particular order: a problem when a call of
        one and a different call of another write the same stream."""
        calls = [self.emitting(p) for p in parts]
        for i in range(len(calls)):
            for j in range(i + 1, len(calls)):
                for a, sa in calls[i].items():
                    for b, sb in calls[j].items():
                        if any(s in EVERY or a != b and not (s in ONCE and once(sa[s], sb[s]))
                               for s in sa.keys() & sb.keys()):
                            self.found.append((where, " ".join(text.split()), a, b))
                            return


def once(a, b):
    """Whether two calls' sources of one stream are the same single member without parameters."""
    return a == b and len(a) == 1 and "" not in a


def emitters(bodies, single=(), known=None):
    """The streams each member writes, itself or through what it calls, and for each
    stream its sources: the members of `single` (those without parameters) it is
    reached through, "" for any other way. `known` are the members' when the bodies
    are one function's lambdas."""
    streams = dict(known) if known else {name: {s: {""} for s in ss} for name, ss in BASE.items()}
    for name, bs in bodies.items():
        streams[name] = {s: set(v) for s, v in streams.get(name, {}).items()}
        for stream, rx in DIRECT.items():
            if any(re.search(rx, b) for b in bs):
                streams[name].setdefault(stream, set()).add(name if name in single else "")
    calls = {name: set(c for b in bs for c in CALL.findall(b)) - KEYWORDS
             for name, bs in bodies.items()}
    changed = True
    while changed:
        changed = False
        for name, cs in calls.items():
            mine = streams[name]
            for c in cs:
                for stream, sources in streams.get(c, {}).items():
                    via = {name} if name in single else sources
                    if not via <= mine.get(stream, set()):
                        mine.setdefault(stream, set()).update(via)
                        changed = True
    return streams


def members(texts):
    """Every function's bodies by name, and the names never defined with parameters."""
    bodies, params = {}, {}
    for t in texts:
        for name, body, line, ps in functions(t):
            bodies.setdefault(name, []).append(body)
            params.setdefault(name, []).append(ps)
    return bodies, {name for name, ps in params.items() if not any(ps)}


def sources(src):
    return sorted(f for g in SOURCES for f in glob.glob(os.path.join(src, g)))


def check_emit_order(src):
    """Raises RuntimeError naming every offending expression."""
    files = sources(src)
    texts = {f: strip(open(f, encoding="utf-8").read()) for f in files}
    known = emitters(*members(texts.values()))
    checker = Checker(known)
    for f, t in texts.items():
        for name, body, line, _ in functions(t):
            local = lambdas(body)
            checker.streams = emitters({n: [b] for n, b in local.items()}, known=known) if local else known
            for stmt, pos in statements(body):
                at = line + body.count("\n", 0, pos + len(stmt) - len(stmt.lstrip()))
                for e in split_top(stmt, CONTROL):
                    checker.check(e, f"{os.path.basename(f)}:{at}")
    if checker.found:
        lines = [f"{where}: {text}\n    unsequenced: {a} and {b}"
                 for where, text, a, b in checker.found]
        raise RuntimeError("two emitting calls in one unsequenced expression:\n" +
                           "\n".join(lines))
    return sum(1 for s in known.values() if s)


if __name__ == "__main__":
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "src")
    if len(sys.argv) > 2:
        # --why NAME: the streams a member reaches and what it calls that does.
        bodies, single = members(strip(open(f, encoding="utf-8").read()) for f in sources(src))
        streams = emitters(bodies, single)
        for name in sys.argv[3:]:
            via = {c: sorted(streams[c]) for b in bodies.get(name, []) for c in set(CALL.findall(b))
                   if streams.get(c)}
            print(name, sorted(streams.get(name, ())), via)
        sys.exit(0)
    try:
        n = check_emit_order(src)
        print(f"ok: {n} emitting members, no unsequenced pair")
    except RuntimeError as e:
        print(e)
        sys.exit(1)
