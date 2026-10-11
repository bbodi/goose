"""The tinygen backend (docs/design/tinygen_backend.md): `goose build
--backend=tinygen` against the expected output of the C backends.

Each program is built with the specializations tinygen supports in an object
of its own and the rest in the C, and run; its output must be the blessed
one the C backends are held to. The test/tinygen/ fixtures must go through
tinygen whole (every function compiled by it), and some are also built
with chosen functions left to C, so that calls cross between the two
halves in both directions (structs, arrays and slices, multiple results);
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


if __name__ == "__main__":
    fails = []
    check_tinygen_backend(sys.argv[1], ROOT / "build" / "gen", lambda m: print("ok", m),
                          lambda m, d="": (fails.append(m), print("FAIL", m, d)), print)
    sys.exit(1 if fails else 0)
