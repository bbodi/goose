"""The tinygen backend (docs/design/tinygen_backend.md): `goose build
--backend=tinygen` against the expected output of the C backends.

Each program is built with the specializations tinygen supports in an object
of its own and the rest in the C, and run; its output must be the blessed
one the C backends are held to. The test/tinygen/ fixtures must go through
tinygen whole (every function compiled by it), and some are also built
with chosen functions left to C, so that calls cross between the two
halves in both directions (structs, arrays and slices, multiple results,
strings and resizable arrays handed in, and built at a destination,
u8[] values, arrays shrunk and resized, free variables of nested
functions, function values, globals on dedicated stacks, `return from`
through frames of both halves);
the other programs are ones the backend covers in part. Where lldb is installed, one program is run under it to see
that the debugger finds a Goose function by name and shows its Goose source
line, its parameters and its locals with their Goose types.

Called by run_tests.py; `python test/tinygen_backend.py <goose>` runs it on
its own."""
from pathlib import Path
import re
import shutil
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "scripts"))
import toolchain as tc  # noqa: E402

# (source, whether every function must go through tinygen, extra build flags)
PROGRAMS = [
    ("test/tinygen/tinygen_scalars.goose", True, []),
    ("test/tinygen/tinygen_scalars.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_structs.goose", True, []),
    ("test/tinygen/tinygen_structs.goose", False, ["--tinygen-skip=main"]),
    ("test/tinygen/tinygen_structs.goose", False, ["--tinygen-skip=add,area,mixed_sum,swap"]),
    ("test/tinygen/tinygen_structs.goose", False, ["--tinygen-skip=scale,dot,grow,mixed"]),
    ("test/tinygen/tinygen_extern.goose", True, []),
    ("test/tinygen/tinygen_divzero.goose", True, []),
    ("test/tinygen/tinygen_divzero.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_arrays.goose", True, []),
    ("test/tinygen/tinygen_arrays.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_arrays.goose", False, ["--tinygen-skip=main"]),
    ("test/tinygen/tinygen_arrays.goose", False,
     ["--tinygen-skip=sum,weighted,bump,first_last,reverse,perimeter,count_bytes,at"]),
    ("test/tinygen/tinygen_slicefail.goose", True, []),
    ("test/tinygen/tinygen_results.goose", True, []),
    ("test/tinygen/tinygen_results.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_results.goose", False, ["--tinygen-skip=main"]),
    ("test/tinygen/tinygen_results.goose", False,
     ["--tinygen-skip=divmod,minmax,split,corners,widen"]),
    ("test/tinygen/tinygen_globals.goose", True, []),
    ("test/tinygen/tinygen_globals.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_globals.goose", False, ["--tinygen-skip=main"]),
    ("test/tinygen/tinygen_globals.goose", False,
     ["--tinygen-skip=start_seed,divmod,bump,next,move_to,sum_primes"]),
    # main builds strings and resizable arrays, which stays in C so far.
    ("test/tinygen/tinygen_strings.goose", False, []),
    ("test/tinygen/tinygen_strings.goose", False, ["--debug"]),
    ("test/tinygen/tinygen_strings.goose", False,
     ["--tinygen-skip=count,first_last,total,sum_view,addto,own"]),
    ("test/tinygen/tinygen_strings.goose", False, ["--tinygen-skip=walk,shout,far"]),
    ("test/tinygen/tinygen_build.goose", True, []),
    ("test/tinygen/tinygen_build.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_build.goose", False, ["--tinygen-skip=main"]),
    ("test/tinygen/tinygen_build.goose", False,
     ["--tinygen-skip=squares,word,evens,label,split,points"]),
    ("test/tinygen/tinygen_build.goose", False, ["--tinygen-skip=sum,own_sum,scan,nested"]),
    ("test/tinygen/tinygen_text.goose", True, []),
    ("test/tinygen/tinygen_text.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_text.goose", False, ["--tinygen-skip=main"]),
    ("test/tinygen/tinygen_text.goose", False,
     ["--tinygen-skip=lit,greet,first,pair,tail,wide,label"]),
    ("test/tinygen/tinygen_text.goose", False,
     ["--tinygen-skip=forward,named,size,same,differ,listing"]),
    ("test/tinygen/tinygen_shrink.goose", True, []),
    ("test/tinygen/tinygen_shrink.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_shrink.goose", False, ["--tinygen-skip=main"]),
    ("test/tinygen/tinygen_shrink.goose", False,
     ["--tinygen-skip=shrink,sized,points,reset,show"]),
    ("test/tinygen/tinygen_nested.goose", True, []),
    ("test/tinygen/tinygen_nested.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_nested.goose", False,
     ["--tinygen-skip=counter,fields,via,words,build,more,outer,fact"]),
    ("test/tinygen/tinygen_nested.goose", False,
     ["--tinygen-skip=bump,grow,twice,total,add,inner,go"]),
    ("test/tinygen/tinygen_nested.goose", False, ["--tinygen-skip=main,mid"]),
    ("test/tinygen/tinygen_funvals.goose", True, []),
    ("test/tinygen/tinygen_funvals.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_funvals.goose", False, ["--tinygen-skip=main"]),
    ("test/tinygen/tinygen_funvals.goose", False,
     ["--tinygen-skip=apply,both,pair,mapp,sum_by,fill,each,countdown"]),
    ("test/tinygen/tinygen_gstacks.goose", True, []),
    ("test/tinygen/tinygen_gstacks.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_gstacks.goose", False, ["--tinygen-skip=main"]),
    ("test/tinygen/tinygen_gstacks.goose", False,
     ["--tinygen-skip=record,sum,copied,add_point,name_len,shout,reassign"]),
    ("test/tinygen/tinygen_retfrom.goose", True, []),
    ("test/tinygen/tinygen_retfrom.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_retfrom.goose", False, ["--tinygen-skip=main"]),
    # Targets in C, the returns and the frames between in tinygen; then
    # the other way round; then the frames between in C.
    ("test/tinygen/tinygen_retfrom.goose", False,
     ["--tinygen-skip=search,check,split,fwd,inner,owner,first_over"]),
    ("test/tinygen/tinygen_retfrom.goose", False,
     ["--tinygen-skip=dig,bad,split_fail,forward,inner_leave,deep2,each,leave_nest"]),
    ("test/tinygen/tinygen_retfrom.goose", False,
     ["--tinygen-skip=deep,outer,after_owner,give,nest,point"]),
    ("test/codegen/loop_invariant_divisors.goose", False, []),
    ("test/codegen/cnames.goose", False, []),
    ("test/codegen/wider_slot_operations.goose", False, []),
    ("samples/21_raytrace.goose", False, []),
]


def expected(source):
    """The blessed stdout of a program, whether its run aborts, and the
    markers its stderr must hold."""
    if source.parent.name == "samples" or source.parent.parent.name == "samples":
        stem = source.stem.split("_", 1)[1]
        out = ROOT / "samples" / "expected" / f"{stem}.out"
        return tc.decode(out.read_bytes()), False, []
    exp = HERE / "expected"
    out = tc.decode((exp / f"{source.stem}.out").read_bytes())
    aborts = (exp / f"{source.stem}.aborts").exists()
    markers = []
    if (exp / f"{source.stem}.stderr").exists():
        markers = [line.strip() for line in
                   tc.decode((exp / f"{source.stem}.stderr").read_bytes()).splitlines()
                   if line.strip()]
    return out, aborts, markers


def build(exe, source, out, flags):
    """Builds source with the tinygen backend; (ok, the compiler's output,
    functions compiled by tinygen, functions in all)."""
    code, o, e = tc.run_capture([exe, "build", "--backend=tinygen", "-v", *flags, "-o", out,
                                 source])
    m = re.search(r"tinygen: (\d+) of (\d+) function", o + e)
    if code != 0 or not m or not Path(out).exists():
        return False, o + e, 0, 0
    return True, o + e, int(m.group(1)), int(m.group(2))


def check_tinygen_backend(exe, base, ok, fail, say):
    base = (Path(base) / "tinygen").resolve()
    base.mkdir(parents=True, exist_ok=True)
    probe = base / "probe.goose"
    tc.write_text(probe, "fn main() { print(1); }\n")
    code, o, e = tc.run_capture([exe, "build", "--backend=tinygen", "--dry-run", probe])
    if "built without tinygen" in o + e:
        say("skip tinygen backend (the compiler was built without tinygen)")
        return
    for i, (rel, whole, flags) in enumerate(PROGRAMS):
        source = ROOT / rel
        label = f"tinygen {source.name}{' ' + ' '.join(flags) if flags else ''}"
        out = base / f"{source.stem}-{i}{tc.EXE_SUFFIX}"
        good, log, done, total = build(exe, source, out, flags)
        if not good:
            fail(label, log)
            continue
        if done == 0 or (whole and done != total):
            fail(f"{label}: {done} of {total} functions through tinygen", log)
            continue
        want, aborts, markers = expected(source)
        code, got, err = tc.run_capture([out], cwd=source.parent)
        if (code != 0) != aborts:
            fail(f"{label} (exit {code})", err)
            continue
        missing = [mk for mk in markers if mk not in err]
        if missing:
            fail(f"{label}: stderr", err)
            continue
        if got.rstrip("\n") != want.rstrip("\n"):
            fail(label, f"--- got:\n{got}\n--- want:\n{want}")
            continue
        ok(f"{label} ({done} of {total} functions)")
    check_lldb(exe, base, ok, fail, say)


def check_lldb(exe, base, ok, fail, say):
    """A breakpoint on a Goose function, its Goose source line, and its
    parameters and locals by their Goose types."""
    lldb = shutil.which("lldb")
    if not lldb:
        say("skip tinygen debug info (no lldb)")
        return
    source = HERE / "tinygen" / "tinygen_structs.goose"
    out = base / f"lldb{tc.EXE_SUFFIX}"
    good, log, _, _ = build(exe, source, out, [])
    if not good:
        fail("tinygen lldb build", log)
        return
    code, o, e = tc.run_capture([lldb, "-b", "-o", "b mixed_sum", "-o", "run",
                                 "-o", "frame variable", "-o", "bt",
                                 "-o", "b tinygen_structs.goose:30", "-o", "continue",
                                 "-o", "frame variable", "-o", "kill", out])
    text = o + e
    wants = [
        r"stop reason = breakpoint",
        r"tinygen_structs\.goose:2[6-9]",          # mixed_sum, in Goose lines
        r"\(Mixed\) m = \(a = .*b = 3000",         # the parameter, a Goose struct
        r"\(f64\) s = 2998\.25",                    # a local, by its Goose type
        r"main_g at tinygen_structs\.goose:\d+",   # the caller, in Goose lines too
    ]
    missing = [w for w in wants if not re.search(w, text)]
    if missing:
        fail("tinygen debug info under lldb", f"missing {missing}:\n{text}")
        return
    ok("tinygen debug info under lldb: Goose function, lines, parameters and locals")
    check_lldb_arrays(exe, base, lldb, ok, fail)


def check_lldb_arrays(exe, base, lldb, ok, fail):
    """A slice parameter as its data and length, a fixed array local with
    its elements, by their Goose types."""
    source = HERE / "tinygen" / "tinygen_arrays.goose"
    out = base / f"lldb-arrays{tc.EXE_SUFFIX}"
    good, log, _, _ = build(exe, source, out, [])
    if not good:
        fail("tinygen lldb arrays build", log)
        return
    code, o, e = tc.run_capture([lldb, "-b", "-o", "b weighted", "-o", "run",
                                 "-o", "frame variable", "-o", "up", "-o", "frame variable a",
                                 "-o", "kill", out])
    text = o + e
    wants = [
        r"weighted_g\(s=.*at tinygen_arrays\.goose:1[45]",
        r"\(i64\[:\]\) s = \{\s*data = 0x[0-9a-f]+\s*len = 5",
        r"\(i64\[5\]\) a = \(\[0\] = 3, \[1\] = 1, \[2\] = 4, \[3\] = 1, \[4\] = 5\)",
    ]
    missing = [w for w in wants if not re.search(w, text)]
    if missing:
        fail("tinygen array debug info under lldb", f"missing {missing}:\n{text}")
        return
    ok("tinygen debug info under lldb: a slice and a fixed array")
    check_lldb_globals(exe, base, lldb, ok, fail)


def check_lldb_globals(exe, base, lldb, ok, fail):
    """The static data tinygen holds a copy of, by its Goose name and type."""
    source = HERE / "tinygen" / "tinygen_globals.goose"
    out = base / f"lldb-globals{tc.EXE_SUFFIX}"
    good, log, _, _ = build(exe, source, out, [])
    if not good:
        fail("tinygen lldb globals build", log)
        return
    code, o, e = tc.run_capture([lldb, "-b", "-o", "b sum_primes", "-o", "run",
                                 "-o", "target variable PRIMES", "-o", "kill", out])
    text = o + e
    want = r"\(i32\[6\]\) PRIMES = \(\[0\] = 2, \[1\] = 3, \[2\] = 5"
    if not re.search(want, text):
        fail("tinygen global debug info under lldb", f"missing {want}:\n{text}")
        return
    ok("tinygen debug info under lldb: a global")
    check_lldb_strings(exe, base, lldb, ok, fail)


def check_lldb_strings(exe, base, lldb, ok, fail):
    """A by-value u8[] as the image it points at, and a resizable array's
    fat reference as its header and stack, the header's elements reached
    through its base, by their Goose types."""
    source = HERE / "tinygen" / "tinygen_strings.goose"
    out = base / f"lldb-strings{tc.EXE_SUFFIX}"
    good, log, _, _ = build(exe, source, out, [])
    if not good:
        fail("tinygen lldb strings build", log)
        return
    code, o, e = tc.run_capture([lldb, "-b", "-o", "b first_last", "-o", "b total", "-o", "run",
                                 "-o", "p *s", "-o", "continue", "-o", "frame variable v",
                                 "-o", "p *v.hdr", "-o", "parray 3 v.hdr->base", "-o", "kill",
                                 out])
    text = o + e
    wants = [
        r"\(u8\[\]\)\s*\(len = 5, data = ",
        r"\(i64\[>\.\.\]&\) v = \{\s*hdr = 0x[0-9a-f]+\s*stk = 0x[0-9a-f]+",
        r"\(i64\[>\.\.\]\) \{\s*base = 0x[0-9a-f]+\s*len = 3",
        r"\[0\] = 1\s*\[1\] = 2\s*\[2\] = 3",
    ]
    missing = [w for w in wants if not re.search(w, text)]
    if missing:
        fail("tinygen string debug info under lldb", f"missing {missing}:\n{text}")
        return
    ok("tinygen debug info under lldb: a string and a resizable array")
    check_lldb_built(exe, base, lldb, ok, fail)


def check_lldb_built(exe, base, lldb, ok, fail):
    """A resizable local and a u8[] local built in tinygen functions: the
    header on the function's own stack with its elements, the image's
    address and what it holds."""
    runs = [
        ("tinygen_build.goose", ["b tinygen_build.goose:31", "run", "frame variable a",
                                 "parray 3 a.base"],
         [r"\(i64\[>\.\.\]\) a = \{\s*base = 0x[0-9a-f]+\s*len = 5",
          r"\[0\] = 0\s*\[1\] = 2\s*\[2\] = 4"]),
        # *hi first: once lldb has shown the pointer, it no longer
        # completes the record behind it (an image of unknown size).
        ("tinygen_text.goose", ["b tinygen_text.goose:14", "run", "frame variable *hi",
                                "frame variable hi"],
         [r"\(u8\[\] \*\) hi = 0x[0-9a-f]+",
          r"\(u8\[\]\) \*hi = \(len = 3, data = "]),
    ]
    for i, (name, cmds, wants) in enumerate(runs):
        out = base / f"lldb-built-{i}{tc.EXE_SUFFIX}"
        good, log, _, _ = build(exe, HERE / "tinygen" / name, out, [])
        if not good:
            fail(f"tinygen lldb {name} build", log)
            return
        args = [lldb, "-b"]
        for c in cmds + ["kill"]:
            args += ["-o", c]
        code, o, e = tc.run_capture([*args, out])
        missing = [w for w in wants if not re.search(w, o + e)]
        if missing:
            fail("tinygen built-array debug info under lldb", f"missing {missing}:\n{o + e}")
            return
    ok("tinygen debug info under lldb: a resizable and a u8[] local built in place")
    check_lldb_freevars(exe, base, lldb, ok, fail)


def check_lldb_freevars(exe, base, lldb, ok, fail):
    """A nested function's free variables as the variables themselves,
    reached through the addresses it is handed: scalars at a breakpoint
    on the function, a struct and a fixed array at a line."""
    source = HERE / "tinygen" / "tinygen_nested.goose"
    out = base / f"lldb-nested{tc.EXE_SUFFIX}"
    good, log, _, _ = build(exe, source, out, [])
    if not good:
        fail("tinygen lldb nested build", log)
        return
    code, o, e = tc.run_capture([lldb, "-b", "-o", "b bump", "-o", "run", "-o", "frame variable",
                                 "-o", "br del 1", "-o", "b tinygen_nested.goose:28",
                                 "-o", "continue", "-o", "frame variable", "-o", "kill", out])
    text = o + e
    wants = [
        r"\(i64\) n = 5\s*\(i32\) calls = 0\s*\(i64\) by = 3",
        r"\(P2\) p = \(x = 3, y = 2\)",
        r"\(i64\[3\]\) w = \(\[0\] = 5, \[1\] = 6, \[2\] = 7\)",
    ]
    missing = [w for w in wants if not re.search(w, text)]
    if missing:
        fail("tinygen free-variable debug info under lldb", f"missing {missing}:\n{text}")
        return
    ok("tinygen debug info under lldb: free variables through their addresses")


if __name__ == "__main__":
    fails = []
    check_tinygen_backend(sys.argv[1], ROOT / "build" / "gen", lambda m: print("ok", m),
                          lambda m, d="": (fails.append(m), print("FAIL", m, d)), print)
    sys.exit(1 if fails else 0)
