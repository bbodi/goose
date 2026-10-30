"""The backend: `goose build --backend=tinygen` against the blessed
expected output.

Each program in PROGRAMS is built and run; its output must be the blessed
one. Every program is built whole: every function through tinygen, main in
tinygen's object, no C of the program compiled but tinygen's. The gfx, ui,
audio and physics fixtures run headless; the windowed samples (BUILD_ONLY)
are only built. All of them are built and run again with
--backend=tinygen-c, through tinygen's C and the C compiler. Where lldb is
installed, several fixtures run under it to check that the debugger finds
Goose functions by name and shows their Goose source lines, and their
parameters, locals and globals by their Goose types, and, with
src/lldb/goose_lldb.py imported, as Goose values.

Called by run_tests.py; `python test/tinygen_backend.py <goose>` runs it on
its own."""
from pathlib import Path
import os
import re
import shutil
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "scripts"))
import toolchain as tc  # noqa: E402

# (source, extra build flags); every one is built whole, main and all, with no C of the program
# compiled but tinygen's
PROGRAMS = [
    ("test/tinygen/tinygen_scalars.goose", []),
    ("test/tinygen/tinygen_scalars.goose", ["--debug"]),
    ("test/tinygen/tinygen_structs.goose", []),
    # Packed structs by value across the C calling convention, both ways: C returns to
    # tinygen, tinygen passes to C and back, C calls tinygen past the registers.
    ("test/tinygen/tinygen_abi.goose", []),
    ("test/tinygen/tinygen_extern.goose", []),
    ("test/tinygen/tinygen_divzero.goose", []),
    ("test/tinygen/tinygen_divzero.goose", ["--debug"]),
    ("test/tinygen/tinygen_arrays.goose", []),
    ("test/tinygen/tinygen_arrays.goose", ["--debug"]),
    ("test/tinygen/tinygen_slicefail.goose", []),
    ("test/tinygen/tinygen_results.goose", []),
    ("test/tinygen/tinygen_results.goose", ["--debug"]),
    ("test/tinygen/tinygen_globals.goose", []),
    ("test/tinygen/tinygen_globals.goose", ["--debug"]),
    ("test/tinygen/tinygen_strings.goose", []),
    ("test/tinygen/tinygen_strings.goose", ["--debug"]),
    ("test/tinygen/tinygen_build.goose", []),
    ("test/tinygen/tinygen_build.goose", ["--debug"]),
    # The stack settings reach the support object at run time, so they keep a whole build.
    ("test/tinygen/tinygen_build.goose", ["--stack-reserve", "64M"]),
    ("test/tinygen/tinygen_text.goose", []),
    ("test/tinygen/tinygen_text.goose", ["--debug"]),
    ("test/tinygen/tinygen_shrink.goose", []),
    ("test/tinygen/tinygen_shrink.goose", ["--debug"]),
    ("test/tinygen/tinygen_nested.goose", []),
    ("test/tinygen/tinygen_nested.goose", ["--debug"]),
    ("test/tinygen/tinygen_funvals.goose", []),
    ("test/tinygen/tinygen_funvals.goose", ["--debug"]),
    ("test/tinygen/tinygen_gstacks.goose", []),
    ("test/tinygen/tinygen_gstacks.goose", ["--debug"]),
    ("test/tinygen/tinygen_retfrom.goose", []),
    ("test/tinygen/tinygen_retfrom.goose", ["--debug"]),
    # Targets in C, the returns and the frames between in tinygen; then
    # the other way round; then the frames between in C.
    # An --include header reads a struct its extern takes by reference, while every Goose
    # function, the extern's callers among them, is tinygen's: whole, the header in the extern unit.
    ("samples/24_call_c.goose", ["--include", str(ROOT / "samples" / "call_c.h")]),
    ("test/tinygen/tinygen_render.goose", []),
    ("test/tinygen/tinygen_render.goose", ["--debug"]),
    ("test/tinygen/tinygen_control.goose", []),
    ("test/tinygen/tinygen_control.goose", ["--debug"]),
    ("test/tinygen/tinygen_elemwise.goose", []),
    ("test/tinygen/tinygen_elemwise.goose", ["--debug"]),
    ("test/codegen/loop_invariant_divisors.goose", []),
    ("test/codegen/cnames.goose", []),
    ("test/codegen/wider_slot_operations.goose", []),
    ("samples/21_raytrace.goose", []),
    # Inlined code (-O2, the default; -O0 inlines nothing): a body spliced into its caller, its
    # returns leaving the block. Whole programs, so a fallback to C fails.
    ("samples/21_raytrace.goose", ["-O2"]),
    ("test/tinygen/tinygen_inline.goose", ["-O2"]),
    ("test/tinygen/tinygen_inline.goose", ["-O1"]),
    ("test/tinygen/tinygen_inline.goose", ["-O0"]),
    ("test/tinygen/tinygen_inline.goose", ["-O2", "--debug"]),
    ("test/codegen/recursive_loop_walk.goose", ["-O2"]),
    ("test/codegen/exit_value_destination.goose", ["-O2"]),
    ("test/codegen/exit_value_destination.goose", ["-O0"]),
    ("samples/20_life.goose", []),
]


# ---- ADTs: fixed-mode values, variants, match and case-function dispatch,
# plain and --debug; and a sample of variable-mode ADTs.
PROGRAMS += [
    ("test/tinygen/tinygen_adts.goose", []),
    ("test/tinygen/tinygen_adts.goose", ["--debug"]),
    ("samples/05_shapes.goose", []),
]


# Limited arrays T[..k]: by value and by reference, alone and in a struct, plain and --debug.
PROGRAMS += [
    ("test/tinygen/tinygen_limited.goose", []),
    ("test/tinygen/tinygen_limited.goose", ["--debug"]),
    # A return nested in another return's T[] value as it is built.
    ("test/codegen/exit_value_destination.goose", []),
]

# Variable-size structs (tinygen_varsize): built in place, read behind
# their variable-size fields, passed and returned by value and by
# reference.
PROGRAMS += [
    ("test/tinygen/tinygen_varsize.goose", []),
    ("test/tinygen/tinygen_varsize.goose", ["--debug"]),
]



# On macOS a data stack commits its pages from the fault handler; lldb must
# hand those faults to the program instead of stopping on them (as goose
# debug does).
LLDB_PASS_FAULTS = ["-o", "settings set platform.plugin.darwin.ignored-exceptions EXC_BAD_ACCESS",
                    "-o", "process handle SIGBUS -s false -n false -p true",
                    "-o", "process handle SIGSEGV -s false -n false -p true"] \
    if sys.platform == "darwin" else []

def check_lldb_varsize(exe, base, lldb, ok, fail):
    """A variable-size struct parameter as the image it points at, its
    fields up to the first variable-size one by their Goose types."""
    source = HERE / "tinygen" / "tinygen_varsize.goose"
    out = base / f"lldb-varsize{tc.EXE_SUFFIX}"
    good, log, _, _ = build(exe, source, out, [])
    if not good:
        fail("tinygen lldb varsize build", log)
        return
    code, o, e = tc.run_capture([lldb, "-b", *LLDB_PASS_FAULTS, "-o", "b pay", "-o", "run", "-o", "frame variable",
                                 "-o", "p *e", "-o", "kill", out])
    text = o + e
    wants = [r"pay_g.* at tinygen_varsize\.goose:19", r"\(Employee \*\) e = 0x",
             r"id = 7", r"name = \(len = 3, data = "]
    missing = [w for w in wants if not re.search(w, text)]
    if missing:
        fail("tinygen variable-size struct debug info under lldb", f"missing {missing}:\n{text}")
        return
    ok("tinygen debug info under lldb: a variable-size struct")

# Optionals (T?, the address a T& would be, null where empty): every kind
# of slot, made, tested, narrowed, rebound and printed (as parameters, results, fields, free
# variables, globals and `return from` values).
PROGRAMS += [
    ("test/tinygen/tinygen_optionals.goose", []),
    ("test/tinygen/tinygen_optionals.goose", ["--debug"]),
]

# The native modules' fixtures (gfx, ui, audio, physics): whole programs that
# call into the packages' C layers. They run headless, as the suite runs
# them; one is skipped where the compiler lacks its package, or for gfx
# where there is no GPU device. Their rejection tests (`// error:`) are not
# programs.
NATIVE_DIRS = ("gfx", "ui", "audio", "physics")
NATIVE = sorted(p for d in NATIVE_DIRS for p in (HERE / d).glob("*.goose")
                if "// error:" not in tc.decode(p.read_bytes()))
PROGRAMS += [(str(p.relative_to(ROOT)), []) for p in NATIVE]

# Programs of the main suite that sweeping found wrong or refused by the backend (G4.2, G4.3): whole
# programs, so that the suite fails on a wrong output. The G4.3 ones are
# a result with relative links built at its receiver, thread_wait with a message, a break in a while's
# condition, qpoll of an image, and a dispatch on a variable-mode enum in a recursive cycle.
# The defer ones cover every exit a defer runs at: fall-through, break, continue, return, a block's
# return, return from, exception::try's finally, and results built before the defers run.
REGRESSIONS = ["test/codegen/forward_adaptation.goose", "test/codegen/bytes_value_equality.goose",
               "test/codegen/reference_element_results.goose", "test/codegen/relative_operands.goose",
               "test/codegen/rendered_pools.goose", "test/threads/queue_producers.goose",
               "test/threads/qlimit_negative.goose", "test/threads/qlimit_zero.goose",
               "test/storage/relative_block_results.goose",
               "test/storage/relative_destination_results.goose",
               "test/storage/alloc_built_in_slot.goose", "test/codegen/relative_receiver_values.goose",
               "test/threads/thread_contained_many.goose", "test/optimizer/bce_while_break_exit.goose",
               "test/threads/qpoll_zero_padded.goose",
               "test/codegen/recursive_dispatch_fixed_variant.goose",
               "test/codegen/defer_exits.goose", "test/codegen/defer_return_from.goose",
               "test/codegen/defer_results.goose", "test/codegen/resizable_struct_returns.goose",
               "test/codegen/limited_capacity_copies.goose",
               "test/codegen/append_resizable_call_to_limited.goose",
               "test/codegen/frame_object_copy_result.goose",
               "test/codegen/frame_object_copy_of_reference.goose",
               "test/codegen/from_bytes_of_view.goose",
               "test/codegen/deep_nested_structs.goose",
               "test/codegen/float_remainder_assign.goose",
               "test/codegen/frame_object_block_value.goose",
               "test/codegen/diverging_arm_value.goose",
               # G3.1's sweep of goose run: the index type checks of a cast and of an image, and
               # the line a relative offset overflow names.
               "test/codegen/index_type_abort.goose", "test/codegen/bound_type_abort.goose",
               "test/typing/index_types.goose", "test/codegen/relative_results.goose",
               "test/codegen/relative_declarations.goose",
               "test/codegen/frame_pool_references.goose",
               # G3.4c1: `self` in a self-relative field of a literal built in a temporary.
               "test/storage/relative_copy_forms.goose",
               # An image local the returns hand back lies at its destination from its declaration.
               "test/codegen/named_image_results.goose"]
# Programs whose header asks for the debug checks (a runtime-debug or GS_DEBUG=1 line), built with
# --debug: thread_contained answers only in a debug build, and the others, also listed above, have
# links the checks verify.
DEBUG_REGRESSIONS = ["test/threads/thread_contained.goose", "test/storage/relative_block_results.goose",
                     "test/storage/relative_destination_results.goose",
                     "test/storage/alloc_built_in_slot.goose",
                     "test/codegen/relative_receiver_values.goose"]
PROGRAMS += [(path, []) for path in REGRESSIONS]
PROGRAMS += [(path, ["--debug"]) for path in DEBUG_REGRESSIONS]
# Fixed values over 4096 bytes lie on a data stack: a local, a
# temporary and a result slot, restored with their scope. Inlined (-O2) they used to pass the native
# stack (a 1 MiB guard refused them), so both inlining levels are whole programs.
LARGE_FIXED = ["test/codegen/large_fixed_values.goose", "test/codegen/large_fixed_exits.goose"]
PROGRAMS += [(path, [level]) for path in LARGE_FIXED for level in ("-O0", "-O2")]
# Programs under a GS_MAX_STACKS limit (their first line), as the main suite builds them: the
# backend's data stack count is checked against it, so these stay within the count the limit allows
# (an image named result lies at its destination, a literal appended is built at the receiver).
STACK_LIMITED = [("test/codegen/append_destination_storage.goose", "-DGS_MAX_STACKS=1"),
                 ("test/codegen/cycle_scratch_locals.goose", "-DGS_MAX_STACKS=4"),
                 ("test/codegen/named_init_inplace.goose", "-DGS_MAX_STACKS=1")]
PROGRAMS += [(path, [flag]) for path, flag in STACK_LIMITED]
# Typed memory in the C (TgFn::TagAt): at -O2 the C compiler may reorder accesses of different C
# types, so an ADT rewritten between variants, the particles loop, nested fields, a global written
# through a reference and reinterpreted bytes must still read back right (check_typed_c too).
TYPED_MEMORY = "test/codegen/typed_memory.goose"
PROGRAMS += [(TYPED_MEMORY, ["-O2"])]
# Byte copies next to or inside their own source (TgFn::CopyDisjoint, tyg_move_dyn) and compares of
# every short length (gs_memeq), whose C check_copy_c reads.
OVERLAPPING_COPIES = "test/codegen/overlapping_copies.goose"
PROGRAMS += [(OVERLAPPING_COPIES, ["-O2"]), (OVERLAPPING_COPIES, [])]
# Pushes counted in locals (TgFn::PoolCache) around calls that read or push the same array, relative
# links null and not; and the range checks a 2 GB reservation still needs, of u16 links (check_rel_c).
POOL_PUSHES = "test/storage/pool_push_locals.goose"
PROGRAMS += [(POOL_PUSHES, ["-O2"]), (POOL_PUSHES, []),
             ("test/storage/reloff_u16.goose", ["-O2"]), ("test/storage/inpool_u16.goose", ["-O2"])]
# A pool's base read once per function, links null-tested and stored through their offsets, varints
# of one byte and more, from a function a global initializer runs too (check_pool_base_c).
POOL_BASES = "test/storage/pool_base_locals.goose"
PROGRAMS += [(POOL_BASES, ["-O2"]), (POOL_BASES, [])]

# Growable arrays counted in a copy of their own (TgFn::PoolCache): locals, globals and reference
# parameters, statements that read them otherwise, a call building a pushed element, aliased
# references; nodes pushed with every field given (check_array_cache_c).
ARRAY_CACHES = "test/storage/array_caches.goose"
PROGRAMS += [(ARRAY_CACHES, ["-O2"]), (ARRAY_CACHES, [])]

# The windowed samples: built whole and not run, since they loop until their
# window closes.
BUILD_ONLY = ["samples/27_gfx_cube.goose", "samples/28_physics_boxes.goose", "samples/29_ui_todo.goose",
              "samples/30_mini_minecraft.goose", "samples/31_mini_doom.goose"]


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


def build(exe, source, out, flags, backend="tinygen"):
    """Builds source with the tinygen backend (or tinygen-c); (ok, the
    compiler's output, functions compiled by tinygen, functions in all)."""
    # The entries' function counts and skips are those of un-inlined code: -O0 unless they ask
    # for a level (the compiler's default is -O2, as the C backend's).
    if not any(f.startswith("-O") for f in flags):
        flags = ["-O0", *flags]
    code, o, e = tc.run_capture([exe, "build", f"--backend={backend}", "-v", *flags, "-o", out,
                                 source])
    m = re.search(r"tinygen: (\d+) of (\d+) function", o + e)
    if code != 0 or not m or not Path(out).exists():
        return False, o + e, 0, 0
    return True, o + e, int(m.group(1)), int(m.group(2))


def whole_build(label, log, done, total, fail):
    """Whether a build's -v log shows the whole program built by tinygen, with no C of the
    program's own compiled; fails it otherwise."""
    if done == 0 or done != total or "tinygen: whole program" not in log:
        fail(f"{label}: {done} of {total} functions through tinygen, whole program expected", log)
        return False
    return True


def check_tinygen_backend(exe, base, ok, fail, say):
    base = (Path(base) / "tinygen").resolve()
    base.mkdir(parents=True, exist_ok=True)
    # A gfx program asking for a window draws off screen instead.
    os.environ["GOOSE_GFX_HEADLESS"] = "1"
    builtin = tc.builtin_modules(exe)
    for i, (rel, flags) in enumerate(PROGRAMS):
        source = ROOT / rel
        label = f"tinygen {source.name}{' ' + ' '.join(flags) if flags else ''}"
        modules = tc.native_imports(tc.decode(source.read_bytes()))
        if any(m not in builtin for m in modules):
            say(f"skip {label} (the compiler was built without {', '.join(modules)})")
            continue
        out = base / f"{source.stem}-{i}{tc.EXE_SUFFIX}"
        good, log, done, total = build(exe, source, out, flags)
        if not good:
            fail(label, log)
            continue
        if not whole_build(label, log, done, total, fail):
            continue
        want, aborts, markers = expected(source)
        # The native fixtures name their data files from the repository's
        # root, where the suite runs them.
        code, got, err = tc.run_capture([out], cwd=ROOT if modules else source.parent)
        if code != 0 and any(tc.native_unavailable(m, err) for m in modules):
            say(f"skip {label} (no device for {', '.join(modules)})")
            continue
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
        ok(f"{label} ({total} functions)")
    for rel in BUILD_ONLY:
        source = ROOT / rel
        label = f"tinygen {source.name} (built, not run)"
        if any(m not in builtin for m in tc.native_imports(tc.decode(source.read_bytes()))):
            say(f"skip {label} (the compiler lacks its packages)")
            continue
        good, log, done, total = build(exe, source, base / f"{source.stem}{tc.EXE_SUFFIX}", [])
        if not good or done != total or "tinygen: whole program" not in log:
            fail(f"{label}: {done} of {total} functions, whole program expected", log)
            continue
        ok(f"{label} ({done} of {total} functions, whole program)")
    check_tinygen_c(exe, base, ok, fail, say, builtin)
    check_typed_c(exe, base, ok, fail)
    check_copy_c(exe, base, ok, fail)
    check_rel_c(exe, base, ok, fail)
    check_pool_base_c(exe, base, ok, fail)
    check_array_cache_c(exe, base, ok, fail)
    check_run(exe, base, ok, fail)
    check_jit(exe, base, ok, fail)
    check_default_inlining(exe, base, ok, fail)
    check_build_defaults(exe, base, ok, fail)
    check_c_path_flags(exe, base, ok, fail)
    check_library(exe, base, ok, fail)
    check_debug(exe, base, ok, fail)
    check_lldb(exe, base, ok, fail, say)
    check_lldb_limited(exe, base, ok, fail)
    check_lldb_adts(exe, base, ok, fail)
    check_lldb_optionals(exe, base, ok, fail)


def tinygen_c_problem(log, debug):
    """What is wrong with the compile of tinygen's C in a tinygen-c build's
    -v log: the first error or warning of the compiler's output (cc.log,
    beside the .tg.c, which the link command compiles), or the wrong flags;
    "" for nothing."""
    cmd = next((line for line in log.splitlines()
                if re.search(r"\.tg\.c\b", line) and " -o " in line), None)
    if cmd is None:
        return "tinygen's C was not compiled"
    want = ["-O0", "-g"] if debug else ["-O2"]
    if any(f not in cmd.split() for f in want):
        return f"compiled without {' '.join(want)}: {cmd}"
    tgc = next(a for a in cmd.split() if a.endswith(".tg.c"))
    out = Path(tgc).parent / "cc.log"
    text = tc.decode(out.read_bytes()) if out.exists() else ""
    first = next((line.strip() for line in text.splitlines()
                  if "error:" in line or "warning:" in line), "")
    if first or re.search(r"building \S+ failed", log):
        return first or "does not compile"
    return ""


def check_tinygen_c(exe, base, ok, fail, say, builtin):
    """--backend=tinygen-c: the same programs, built through tinygen's C
    output and the C compiler (-O2, or -O0 -g with --debug), with the same
    output and the same path; a whole program compiles only its .tg.c. A
    program whose tinygen C does not compile (or warns) fails on a line of
    its own."""
    for i, (rel, flags) in enumerate(PROGRAMS):
        source = ROOT / rel
        label = f"tinygen-c {source.name}{' ' + ' '.join(flags) if flags else ''}"
        modules = tc.native_imports(tc.decode(source.read_bytes()))
        if any(m not in builtin for m in modules):
            say(f"skip {label} (the compiler was built without {', '.join(modules)})")
            continue
        out = base / f"{source.stem}-c{i}{tc.EXE_SUFFIX}"
        good, log, done, total = build(exe, source, out, flags, "tinygen-c")
        problem = tinygen_c_problem(log, "--debug" in flags)
        if problem:
            fail(f"{label}: tinygen's C: {problem}", log)
            continue
        if not good:
            fail(label, log)
            continue
        if not whole_build(label, log, done, total, fail):
            continue
        want, aborts, markers = expected(source)
        code, got, err = tc.run_capture([out], cwd=ROOT if modules else source.parent)
        if code != 0 and any(tc.native_unavailable(m, err) for m in modules):
            say(f"skip {label} (no device for {', '.join(modules)})")
            continue
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
        ok(f"{label} ({total} functions)")
    for rel in BUILD_ONLY:
        source = ROOT / rel
        label = f"tinygen-c {source.name} (built, not run)"
        if any(m not in builtin for m in tc.native_imports(tc.decode(source.read_bytes()))):
            say(f"skip {label} (the compiler lacks its packages)")
            continue
        good, log, done, total = build(exe, source, base / f"{source.stem}-c{tc.EXE_SUFFIX}", [],
                                       "tinygen-c")
        problem = tinygen_c_problem(log, False)
        if problem:
            fail(f"{label}: tinygen's C: {problem}", log)
            continue
        if not good or done != total or "tinygen: whole program" not in log:
            fail(f"{label}: {done} of {total} functions, whole program expected", log)
            continue
        ok(f"{label} ({done} of {total} functions, whole program)")
    # goose run builds the same way, and -o out.c writes tinygen's C alone.
    source = ROOT / "test/tinygen/tinygen_text.goose"
    want, _, _ = expected(source)
    code, got, err = tc.run_capture([Path(exe).resolve(), "run", "--backend", "tinygen-c", source],
                                    cwd=source.parent)
    if code != 0 or got.rstrip("\n") != want.rstrip("\n"):
        fail(f"goose run --backend tinygen-c {source.name} (exit {code})",
             f"{err}\n--- got:\n{got}\n--- want:\n{want}")
        return
    cout = base / "tinygen_text.tg.c"
    code, o, e = tc.run_capture([exe, "build", "--backend=tinygen-c", "-o", cout, source])
    if code != 0 or not cout.exists() or "int main(int argc, char **argv)" not in tc.decode(
            cout.read_bytes()):
        fail("goose build --backend=tinygen-c -o out.c", o + e)
        return
    ok("goose run --backend tinygen-c, and -o out.c writing tinygen's C")


def check_typed_c(exe, base, ok, fail):
    """tinygen's C reads and writes typed memory as its C types: a field along its path of structs
    (`((struct tyg_rN *)p)->f1.f0`), a global through a struct of its own (one member, a struct)
    and a whole struct by assignment, not as bytes."""
    cout = base / "typed_memory.tg.c"
    code, o, e = tc.run_capture([exe, "build", "--backend=tinygen-c", "-O2", "-o", cout,
                                 ROOT / TYPED_MEMORY])
    text = tc.decode(cout.read_bytes()) if code == 0 and cout.exists() else ""
    wraps = re.findall(r"struct (?:TYG_ALIGNED\(\d+\) )?tyg_r(\d+) \{\n\tstruct tyg_r\d+ f0;\n\};", text)
    if not re.search(r"= \(float\)\(\(struct tyg_r\d+ \*\)\(.*\)\)->f\d+\.f\d+;", text) or \
            not any(re.search(rf"\(\(struct tyg_r{w} \*\)\(.*\)\)->f0\.f\d+", text) for w in wraps) or \
            not re.search(r"\(\*\(struct tyg_r\d+ \*\)\(v\d+\)\) = \(\*\(struct tyg_r\d+ \*\)\(v\d+\)\);",
                          text):
        fail("tinygen's C of typed memory: a field path, a global's own struct and a struct "
             "assignment", o + e)
        return
    ok("tinygen's C of typed memory: field paths, globals through structs of their own, struct "
       "assignments")



def check_copy_c(exe, base, ok, fail):
    """tinygen's C copies runs that cannot overlap through gs_tg_memcpy and compares bytes through
    gs_tg_memeq, the runtime's inline gs_memcpy and gs_memeq, never memcmp; a run moved within its
    own array (a limited array set to its own tail) stays a memmove of a run-time length."""
    cout = base / "overlapping_copies.tg.c"
    code, o, e = tc.run_capture([exe, "build", "--backend=tinygen-c", "-O2", "-o", cout,
                                 ROOT / OVERLAPPING_COPIES])
    text = tc.decode(cout.read_bytes()) if code == 0 and cout.exists() else ""
    prog = text.partition("/* ---- the program, written by tinygen ---- */")[2]
    if not re.search(r"\bgs_tg_memcpy\(", prog) or not re.search(r"\bgs_tg_memeq\(", prog) or \
            "memcmp" in prog or not re.search(r"\bmemmove\([^;]*\(size_t\)v\d+\);", prog):
        fail("tinygen's C of byte copies: gs_tg_memcpy, gs_tg_memeq, memmove where runs overlap",
             o + e)
        return
    ok("tinygen's C of byte copies: gs_tg_memcpy and gs_tg_memeq, memmove where runs overlap")


def check_rel_c(exe, base, ok, fail):
    """A relative store's range check (gs_tg_abort_RELOFF) is in tinygen's C only where a root can
    span more than the width holds (TgFn::RelSpan): not for u32 links under the default 2 GB
    reservation, where both ends lie in one, but for them under a 4 GB one, and for u16 links."""
    def checks(source, flags):
        cout = base / f"{Path(source).stem}-rel.tg.c"
        code, o, e = tc.run_capture([exe, "build", "--backend=tinygen-c", "-O2", *flags, "-o", cout,
                                     ROOT / source])
        text = tc.decode(cout.read_bytes()) if code == 0 and cout.exists() else ""
        prog = text.partition("/* ---- the program, written by tinygen ---- */")[2]
        return (len(re.findall(r"\bgs_tg_abort_RELOFF\(", prog)) if prog else -1), o + e
    n32, log32 = checks(POOL_PUSHES, [])
    n32wide, logwide = checks(POOL_PUSHES, ["-DGS_STACK_RESERVE=4294967296"])
    n16, log16 = checks("test/storage/reloff_u16.goose", [])
    if n32 != 0 or n32wide < 3 or n16 < 1:
        fail(f"tinygen's C of relative range checks: {n32} under 2 GB, {n32wide} under 4 GB, "
             f"{n16} of u16", log32 + logwide + log16)
        return
    ok("tinygen's C of relative range checks: none for u32 under 2 GB, kept under 4 GB and for u16")


def check_pool_base_c(exe, base, ok, fail):
    """tinygen's C defines the program's functions static, as the old C generator did, so the C
    compiler inlines one it sees every call of; and a function following a pool's links reads the
    pool's base once, in its entry block (TgFn::PoolBase), not at each link."""
    cout = base / "pool_base_locals.tg.c"
    code, o, e = tc.run_capture([exe, "build", "--backend=tinygen-c", "-O2", "-o", cout,
                                 ROOT / POOL_BASES])
    text = tc.decode(cout.read_bytes()) if code == 0 and cout.exists() else ""
    prog = text.partition("/* ---- the program, written by tinygen ---- */")[2]
    body = re.search(r"\bstatic int64_t sum_g\(int64_t dir[^)]*\) \{\n(.*?)\n\}\n", prog, re.S)
    # The entry block (B1, last) reads the base through the global's address into a local.
    entry = body.group(1).partition("\nB1:;")[2] if body else ""
    based = re.search(r"TYG_LD64\(&(v\d+), \(\(char \*\)&gs_tg_g\d+\)\);.*"
                      r"TYG_LD64\(&(v\d+), \1\);\s*TYG_ST64\(\(\(char \*\)l\d+\), &\2\);", entry, re.S)
    if not based:
        fail("tinygen's C of a pool's base: static functions, the base read in the entry block", o + e)
        return
    ok("tinygen's C of a pool's base: static functions, the base read in the entry block")


def check_array_cache_c(exe, base, ok, fail):
    """In tinygen's C a loop pushing into a local counts in the array's copy and writes the header
    back once, after it (TgFn::PoolCache); a node pushed with every field given takes no zero bytes
    first and its links to references that cannot be null no null test (TgFn::SelfRelInto); and the
    unit of a program without threads says so, for its gs_rf to be a plain static
    (TgSupportBody)."""
    def unit(source, name):
        cout = base / f"{name}.tg.c"
        code, o, e = tc.run_capture([exe, "build", "--backend=tinygen-c", "-O2", "-o", cout,
                                     ROOT / source])
        return (tc.decode(cout.read_bytes()) if code == 0 and cout.exists() else ""), o + e
    text, log = unit(ARRAY_CACHES, "array_caches")
    prog = text.partition("/* ---- the program, written by tinygen ---- */")[2]
    def body(name):
        m = re.search(r"\bstatic [^\n]*\b" + name + r"\([^)]*\) \{\n(.*?)\n\}\n", prog, re.S)
        return m.group(1) if m else ""
    squares, tree = body("squares_g"), body("tree_g")
    hdr = len(re.findall(r"TYG_ST64\(\(v\d+ \+ 8\), ", squares))
    copy = len(re.findall(r"TYG_ST64\(\(\(char \*\)l\d+ \+ 8\), ", squares))
    threads, tlog = unit("test/threads/codegen_threads.goose", "codegen_threads")
    nothreads = "#define GS_TG_NO_THREADS 1" in text and "#define GS_TG_NO_THREADS 1" not in threads
    if (not squares or hdr != 1 or copy < 1 or not tree or "RELNULL" in tree or "memset" in tree
            or not nothreads):
        fail(f"tinygen's C of cached arrays: {hdr} header count stores, {copy} copy stores, "
             f"tree tested or zeroed: {'RELNULL' in tree or 'memset' in tree}, "
             f"threads said: {nothreads}", log + tlog)
        return
    ok("tinygen's C of cached arrays: pushes counted in the copy, nodes built whole and untested")


def check_run(exe, base, ok, fail):

    """goose run --backend=tinygen builds the program in its build cache and
    runs it: the program's output and exit code are the run's, its
    arguments are the ones after --, its resources are its source's
    directory, and a program with threads runs too (a TinyCC run refuses
    one)."""
    exe = Path(exe).resolve()
    for rel in ("test/tinygen/tinygen_text.goose", "test/threads/thread_globals.goose"):
        source = ROOT / rel
        want, _, _ = expected(source)
        code, got, err = tc.run_capture([exe, "run", "--backend=tinygen", source],
                                        cwd=source.parent)
        if code != 0 or got.rstrip("\n") != want.rstrip("\n"):
            fail(f"goose run --backend=tinygen {source.name} (exit {code})",
                 f"{err}\n--- got:\n{got}\n--- want:\n{want}")
            return
    probe = base / "run_probe.goose"
    tc.write_text(probe, "import os;\n\nfn main() {\n    let a = args();\n"
                         "    for i in 1..a.len { print(a[i]); }\n"
                         "    print(resource_dir());\n    exit(a.len as i32 + 2);\n}\n")
    code, got, err = tc.run_capture([exe, "run", "--backend=tinygen", probe, "--", "two words",
                                     "x"], cwd=ROOT)
    want = f"two words\nx\n{probe.parent}/\n"
    if code != 5 or got.replace("\\", "/") != want.replace("\\", "/"):
        fail(f"goose run --backend=tinygen arguments (exit {code})",
             f"{err}\n--- got:\n{got}\n--- want:\n{want}")
        return
    ok("goose run --backend=tinygen: output, arguments, exit code, resources, threads")


# goose run with no --backend runs tinygen's C in TinyCC.
JIT_PROGRAMS = ["samples/01_tour.goose", "test/tinygen/tinygen_text.goose",
                "test/codegen/defer_results.goose", "test/codegen/index_type_abort.goose",
                "test/codegen/relative_results.goose", OVERLAPPING_COPIES]
TINYGEN_JIT = "jit: tinygen's C"


def check_jit(exe, base, ok, fail):
    """Plain goose run (tinygen's C in TinyCC): the blessed output, exit status and stderr markers,
    -v naming the path, at -O2 (inlined code, the default) and -O0."""
    exe = Path(exe).resolve()
    for rel in JIT_PROGRAMS:
        source = ROOT / rel
        want, aborts, markers = expected(source)
        for flags in [[], ["-O2"], ["-O0"]]:
            label = f"goose run {' '.join(flags + ['-v'])} {source.name}"
            code, got, err = tc.run_capture([exe, "run", "-v", *flags, source], cwd=source.parent)
            missing = [mk for mk in markers if mk not in err]
            if (code != 0) != aborts or missing or got.rstrip("\n") != want.rstrip("\n"):
                fail(f"{label} (exit {code})", f"{err}\n--- got:\n{got}\n--- want:\n{want}")
                return
            if TINYGEN_JIT not in err:
                fail(f"{label}: -v should say {TINYGEN_JIT}", err)
                return
    probe = base / "jit_probe.goose"
    tc.write_text(probe, "import os;\n\nfn main() {\n    let a = args();\n"
                         "    for i in 1..a.len { print(a[i]); }\n"
                         "    print(resource_dir());\n    exit(a.len as i32 + 2);\n}\n")
    code, got, err = tc.run_capture([exe, "run", "-v", probe, "--", "two words", "x"], cwd=ROOT)
    want = f"two words\nx\n{probe.parent}/\n"
    if code != 5 or got.replace("\\", "/") != want.replace("\\", "/") or TINYGEN_JIT not in err:
        fail(f"goose run arguments (exit {code})", f"{err}\n--- got:\n{got}\n--- want:\n{want}")
        return
    # A plain run prints nothing of the backend.
    code, got, err = tc.run_capture([exe, "run", ROOT / JIT_PROGRAMS[0]], cwd=ROOT)
    if code != 0 or "tinygen" in err or "jit:" in err:
        fail("goose run without -v: quiet", err)
        return
    ok(f"goose run on tinygen's C: {len(JIT_PROGRAMS)} programs, -O2 "
       "and -O0, arguments, exit code, resources")


# goose build with no --backend: tinygen-c, or tinygen with --debug.
DEFAULT_BUILD_PROGRAMS = ["samples/01_tour.goose", "test/tinygen/tinygen_text.goose",
                          "test/codegen/defer_results.goose"]


def check_build_defaults(exe, base, ok, fail):
    """Plain goose build runs tinygen-c, --debug the native tinygen backend (both -v named, the
    program's output right); -o x.c writes tinygen's C as one unit (G3.4b), which a C compiler builds
    with the runtime object (`cc x.c runtime.o`), or alone with --standalone."""
    exe = Path(exe).resolve()
    work = base / "defaults"
    work.mkdir(parents=True, exist_ok=True)

    def built(source, flags, says, label):
        out = work / f"{source.stem}{tc.EXE_SUFFIX}"
        out.unlink(missing_ok=True)
        code, o, e = tc.run_capture([exe, "build", "-v", *flags, "-o", out, source],
                                    cwd=source.parent)
        log = o + e
        if code != 0 or not out.exists() or not all(t in log for t in says):
            fail(f"goose build {label} {source.name}: -v should say {says}", log)
            return None
        want, aborts, markers = expected(source)
        code, got, err = tc.run_capture([out], cwd=source.parent)
        if (code != 0) != aborts or any(mk not in err for mk in markers) or \
                got.rstrip("\n") != want.rstrip("\n"):
            fail(f"goose build {label} {source.name} (exit {code})",
                 f"{err}\n--- got:\n{got}\n--- want:\n{want}")
            return None
        return log

    for rel in DEFAULT_BUILD_PROGRAMS:
        source = ROOT / rel
        for flags, says in [([], ["build: backend tinygen-c (the default)",
                                  "tinygen: whole program"]),
                            (["--debug"], ["build: backend tinygen (the default)",
                                           "tinygen: whole program"])]:
            if built(source, flags, says, " ".join(flags)) is None:
                return
    # -o x.c: tinygen's C with the runtime's headers in one unit, which the C compiler builds with
    # the runtime object, or with --standalone the whole runtime too, which it builds alone; the
    # verb-less flag form writes the same.
    cc = tc.test_cc()
    runtime = tc.GooseRuntime(exe, work / "runtime")
    for rel in ("samples/01_tour.goose", "test/codegen/codegen_exec.goose",
                "test/threads/thread_globals.goose"):
        source = ROOT / rel
        want, aborts, markers = expected(source)
        for verb, flags in [(["build"], []), (["build"], ["--standalone"]), ([], []),
                            ([], ["--standalone"])]:
            label = f"goose {' '.join(verb + flags)} -o out.c {source.name}"
            out = work / "out.c"
            out.unlink(missing_ok=True)
            code, o, e = tc.run_capture([exe, *verb, *flags, "-o", out, source])
            text = tc.decode(out.read_bytes()) if out.exists() else ""
            if code != 0 or "through tinygen" not in text or "Generated by the Goose" in text:
                fail(f"{label}: tinygen's C expected", o + e)
                return
            if cc is None:
                continue
            prog = work / f"out{tc.EXE_SUFFIX}"
            good, log = cc.compile(out, prog, opt=1, strict_decls=True,
                                   runtime=None if flags else runtime)
            if not good:
                fail(f"{label}: cc out.c{'' if flags else ' runtime.o'}", log)
                return
            code, got, err = tc.run_capture([prog], cwd=source.parent)
            if (code != 0) != aborts or any(mk not in err for mk in markers) or \
                    got.rstrip("\n") != want.rstrip("\n"):
                fail(f"{label} (exit {code})", f"{err}\n--- got:\n{got}\n--- want:\n{want}")
                return
    ok(f"goose build: tinygen-c by default and tinygen with --debug on {len(DEFAULT_BUILD_PROGRAMS)} "
       "programs; -o out.c (build and the flag form, --standalone) built by cc with the runtime "
       "object or alone")


def check_c_path_flags(exe, base, ok, fail):
    """The flags of the C a program used to need (-D, --include, --standalone, export fn): plain goose
    build, --debug and goose run, -v saying so, the program's output and stderr markers right."""
    exe = Path(exe).resolve()
    work = base / "cflags"
    work.mkdir(parents=True, exist_ok=True)

    def each(source, flags, label, want=None, built=""):
        """Builds and runs source each way, the --debug build's -v log holding `built`; False
        after a failure."""
        exp, aborts, markers = expected(source) if want is None else (want, False, [])
        out = work / f"{source.stem}{tc.EXE_SUFFIX}"
        for verb, extra, says in [("build", [], "tinygen: whole program"),
                                  ("build", ["--debug"], "tinygen: whole program"),
                                  ("run", [], TINYGEN_JIT)]:
            name = f"goose {verb} {' '.join(map(str, extra + flags))} {source.name} ({label})"
            out.unlink(missing_ok=True)
            code, o, e = tc.run_capture([exe, verb, "-v", *extra, *flags,
                                         *(["-o", out] if verb == "build" else []), source],
                                        cwd=work)
            if verb == "build":
                if code != 0 or says not in o + e or (extra and built not in o + e):
                    fail(f"{name}: -v should say {says} {built}", o + e)
                    return False
                code, o, e = tc.run_capture([out], cwd=work)
            elif says not in e:
                fail(f"{name}: -v should say {says}", o + e)
                return False
            if (code != 0) != aborts or any(mk not in e for mk in markers) or \
                    o.rstrip("\n") != exp.rstrip("\n"):
                fail(f"{name} (exit {code})", f"{e}\n--- got:\n{o}\n--- want:\n{exp}")
                return False
        return True

    # -D: every define reaches the support unit, GS_STACK_STATS's report among them.
    if not each(ROOT / "test/runtime/stack_stats.goose", ["-DGS_STACK_STATS=1"], "-D"):
        return
    # --include: the externs' trampolines are in a unit that includes the header after the
    # program's C types, so a struct by reference and a static inline function work; the -D given
    # reach the header there.
    if not each(ROOT / "samples/24_call_c.goose", ["--include", ROOT / "samples/call_c.h"],
                "--include"):
        return
    header = work / "scaled.h"
    tc.write_text(header, "#ifndef SCALE\n#error SCALE is a -D of the build\n#endif\n"
                          "static inline int64_t scaled(int64_t x) { return x * SCALE; }\n")
    source = work / "scaled.goose"
    tc.write_text(source, "extern fn scaled(x: i64) -> i64;\n\nfn main() { print(scaled(14)); }\n")
    if not each(source, ["--include", header, "-DSCALE=3"], "static inline, -D", want="42\n"):
        return
    # --standalone: the runtime in tinygen's unit, or for the native object compiled into the build
    # directory instead of the cached object.
    if not each(ROOT / "samples/01_tour.goose", ["--standalone"], "--standalone",
                built="/goose_runtime.c -o "):
        return
    # export fn: the C symbols, large fixed values by value among them (which the C generator's
    # wrapper does not compile), called back through externs.
    if not each(ROOT / "test/codegen/export.goose", [], "export fn"):
        return
    source = work / "export_large.goose"
    tc.write_text(source, "struct Big { a: i64[1000], seed: i64 }\n"
                          "export \"e_take\" fn take(b: Big) -> i64 { return b.a[3] + b.seed; }\n"
                          "export \"e_make\" fn make(seed: i64) -> Big { return Big { seed: seed, .. }; }\n"
                          "extern fn c_take(b: Big) -> i64 = \"e_take\";\n"
                          "extern fn c_make(seed: i64) -> Big = \"e_make\";\n"
                          "fn main() {\n    var b = Big { seed: 5, .. };\n    b.a[3] = 37;\n"
                          "    print(take(b), \" \", c_take(b), \" \", make(4).seed, \" \", c_make(6).seed);\n}\n")
    if not each(source, [], "large fixed export fn", want="42 42 4 6\n"):
        return
    ok("the C path's flags on tinygen: -D, --include (a static inline extern, a -D in the header), "
       "--standalone, export fn (large fixed values too)")


LIBRARY = """struct Pair { a: i64, b: i64 }
var counter: i64 = 40;
export "lib_add" fn add(x: i64, y: i64) -> i64 { counter += 1; return x + y + counter; }
export "lib_swap" fn swap(p: Pair) -> Pair { return Pair { a: p.b, b: p.a }; }
export "lib_name" fn name() -> const u8[:] { return "goose"; }
"""
LIBRARY_HOST = """#include <stdio.h>
#include "lib.h"
int main(int argc, char **argv) {
    if (argc > 1) printf("%lld\\n", (long long)lib_add(1, 2));
    goose_init(argc, argv);
    goose_init(argc, argv);
    printf("%lld\\n", (long long)lib_add(1, 2));
    Pair_g p = { 3, 4 }, q = lib_swap(p);
    printf("%lld %lld\\n", (long long)q.a_g, (long long)q.b_g);
    gs_i64 n;
    const uint8_t *s = lib_name(&n);
    printf("%.*s\\n", (int)n, s);
    return 0;
}
"""


def check_library(exe, base, ok, fail):
    """The flag form with --header (G3.4b): tinygen's C of a library, whose C host includes the
    export header, starts it with goose_init (once, however often called) and calls its exports, a
    struct by value and a slice among them; built with the runtime object or, --standalone, alone;
    an export called before goose_init aborts."""
    exe = Path(exe).resolve()
    cc = tc.test_cc()
    if cc is None:
        return
    work = base / "library"
    work.mkdir(parents=True, exist_ok=True)
    source, host = work / "lib.goose", work / "host.c"
    tc.write_text(source, LIBRARY)
    tc.write_text(host, LIBRARY_HOST)
    runtime = tc.GooseRuntime(exe, work / "runtime")
    for flags in ([], ["--standalone"]):
        label = f"goose {' '.join(flags)} -o lib.c --header lib.h"
        out = work / "lib.c"
        code, o, e = tc.run_capture([exe, "-v", *flags, "-o", out, "--header", work / "lib.h",
                                     source])
        if code != 0 or "tinygen: whole program" not in o + e:
            fail(f"{label}: tinygen's C expected", o + e)
            return
        prog = work / f"host{tc.EXE_SUFFIX}"
        good, log = cc.compile([host, out], prog, opt=1, strict_decls=True,
                               runtime=None if flags else runtime)
        if not good:
            fail(f"{label}: cc host.c lib.c", log)
            return
        code, got, err = tc.run_capture([prog])
        if code != 0 or got.replace("\r", "") != "44\n4 3\ngoose\n":
            fail(f"{label}: the host (exit {code})", f"{err}\n--- got:\n{got}")
            return
        code, got, err = tc.run_capture([prog, "early"])
        if code == 0 or "goose_init must be called before exported functions" not in err:
            fail(f"{label}: an export before goose_init (exit {code})", err)
            return
    ok("the flag form with --header: tinygen's C of a library for a C host, with the runtime object "
       "and --standalone")


def check_default_inlining(exe, base, ok, fail):
    """Without -O, tinygen-c inlines as the C backend does (-O2): a build
    leaves fewer functions than the source has. The native tinygen build is
    the debug build and keeps every function unless -O is given."""
    source = ROOT / "test/tinygen/tinygen_inline.goose"
    counts = {}
    for backend, flags in (("tinygen-c", []), ("tinygen-c", ["-O0"]), ("tinygen", []), ("tinygen", ["-O2"])):
        code, o, e = tc.run_capture([exe, "build", f"--backend={backend}", "-v", *flags, "-o",
                                     base / f"inline_default{tc.EXE_SUFFIX}", source])
        m = re.search(r"tinygen: (\d+) of (\d+) function", o + e)
        if code != 0 or not m or m.group(1) != m.group(2):
            fail(f"{backend} {' '.join(flags) or 'default'}: not whole", o + e)
            return
        counts[(backend, " ".join(flags))] = int(m.group(2))
    if not counts[("tinygen-c", "")] < counts[("tinygen-c", "-O0")]:
        fail("tinygen-c: the default should inline (-O2)", str(counts))
        return
    if not counts[("tinygen", "-O2")] < counts[("tinygen", "")]:
        fail("tinygen: the default should not inline (-O0, the debug build)", str(counts))
        return
    ok("tinygen-c inlines by default (-O2); the native tinygen build does not (-O0)")


def check_debug(exe, base, ok, fail):
    """goose debug builds as goose run --backend=tinygen does and starts the
    program under lldb, with the Goose formatters loaded and the arguments
    after -- set: an ADT shows as its active variant."""
    if not shutil.which("lldb"):
        return
    exe = Path(exe).resolve()
    cmds = base / "debug_cmds.txt"
    # Piped commands run asynchronously unless lldb is told otherwise.
    tc.write_text(cmds, "script lldb.debugger.SetAsync(False)\nb scaled\nrun\n"
                        "frame variable s\nkill\nquit\n")
    source = HERE / "tinygen" / "tinygen_adts.goose"
    code, o, e = tc.run_capture([exe, "debug", source, "--", "an arg"], cwd=ROOT,
                                stdin_path=cmds)
    text = o + e
    wants = [r'run-args\s+"an arg"', r"\(Shape\) s = Shape\.Rect\(w = 3, h = 4\)"]
    missing = [w for w in wants if not re.search(w, text)]
    if missing:
        fail("goose debug", f"missing {missing}:\n{text}")
        return
    ok("goose debug: lldb with the Goose formatters and the program's arguments")


def check_lldb_limited(exe, base, ok, fail):
    """A limited array local, and one in a struct field, as the C half's
    layout under their Goose types: the length and every slot."""
    lldb = shutil.which("lldb")
    if not lldb:
        return
    source = HERE / "tinygen" / "tinygen_limited.goose"
    out = base / f"lldb-limited{tc.EXE_SUFFIX}"
    good, log, _, _ = build(exe, source, out, [])
    if not good:
        fail("tinygen lldb limited build", log)
        return
    code, o, e = tc.run_capture([lldb, "-b", *LLDB_PASS_FAULTS, "-o", "b tinygen_limited.goose:84",
                                 "-o", "b tinygen_limited.goose:89", "-o", "run",
                                 "-o", "frame variable a", "-o", "continue",
                                 "-o", "frame variable n", "-o", "kill", out])
    text = o + e
    wants = [
        r"\(i64\[\.\.8\]\) a = \{\s*len = '\\x05'\s*e = \(\[0\] = 9, \[1\] = 8, .*\[4\] = 5, \[5\] = 0",
        r"\(Name\) n = \{\s*tag = 7\s*text = \(len = '\\x04', e = \"tiny\"\)",
    ]
    missing = [w for w in wants if not re.search(w, text)]
    if missing:
        fail("tinygen limited-array debug info under lldb", f"missing {missing}:\n{text}")
        return
    ok("tinygen debug info under lldb: limited arrays, alone and in a struct")


def check_lldb_adts(exe, base, ok, fail):
    """A fixed-mode ADT as its tag, by variant name, and its payloads;
    a match arm's binder as its variant; a C-like enum's tag."""
    lldb = shutil.which("lldb")
    if not lldb:
        return
    source = HERE / "tinygen" / "tinygen_adts.goose"
    out = base / f"lldb-adts{tc.EXE_SUFFIX}"
    good, log, _, _ = build(exe, source, out, [])
    if not good:
        fail("tinygen lldb ADT build", log)
        return
    code, o, e = tc.run_capture([lldb, "-b", *LLDB_PASS_FAULTS, "-o", "b scaled", "-o", "run",
                                 "-o", "frame variable s",
                                 "-o", "b tinygen_adts.goose:49", "-o", "continue",
                                 "-o", "frame variable r", "-o", "b turn", "-o", "continue",
                                 "-o", "frame variable d", "-o", "kill", out])
    text = o + e
    wants = [
        r"scaled_g\(s=.*at tinygen_adts\.goose:4[67]",
        r"\(Shape\) s = \{\s*tag = Rect\s*u = \{.*Rect = \(w = 3, h = 4\)",
        r"\(Shape\.Rect\) r = \(w = 3, h = 4\)",
        r"\(Dir\) d = \(tag = West\)",
    ]
    missing = [w for w in wants if not re.search(w, text, re.S)]
    if missing:
        fail("tinygen ADT debug info under lldb", f"missing {missing}:\n{text}")
        return
    ok("tinygen debug info under lldb: an ADT, a match binder and a C-like enum")


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
    code, o, e = tc.run_capture([lldb, "-b", *LLDB_PASS_FAULTS, "-o", "b mixed_sum", "-o", "run",
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
    check_lldb_varsize(exe, base, lldb, ok, fail)


def check_lldb_arrays(exe, base, lldb, ok, fail):
    """A slice parameter as its data and length, a fixed array local with
    its elements, by their Goose types."""
    source = HERE / "tinygen" / "tinygen_arrays.goose"
    out = base / f"lldb-arrays{tc.EXE_SUFFIX}"
    good, log, _, _ = build(exe, source, out, [])
    if not good:
        fail("tinygen lldb arrays build", log)
        return
    code, o, e = tc.run_capture([lldb, "-b", *LLDB_PASS_FAULTS, "-o", "b weighted", "-o", "run",
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
    """The static data tinygen holds a copy of, and in a whole program the
    globals it holds, by their Goose names and types."""
    source = HERE / "tinygen" / "tinygen_globals.goose"
    out = base / f"lldb-globals{tc.EXE_SUFFIX}"
    good, log, _, _ = build(exe, source, out, [])
    if not good:
        fail("tinygen lldb globals build", log)
        return
    code, o, e = tc.run_capture([lldb, "-b", *LLDB_PASS_FAULTS, "-o", "b sum_primes", "-o", "run",
                                 "-o", "target variable PRIMES counter seed hist last",
                                 "-o", "kill", out])
    text = o + e
    wants = [r"\(i32\[6\]\) PRIMES = \(\[0\] = 2, \[1\] = 3, \[2\] = 5",
             r"\(i64\) counter = 1005", r"\(i64\) seed = -?\d+",
             r"\(u16\[4\]\) hist = \(\[0\] = \d+", r"\(Vec2\) last = \(x = "]
    missing = [w for w in wants if not re.search(w, text)]
    if missing:
        fail("tinygen global debug info under lldb", f"missing {missing}:\n{text}")
        return
    ok("tinygen debug info under lldb: static data and a whole program's globals")
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
    code, o, e = tc.run_capture([lldb, "-b", *LLDB_PASS_FAULTS, "-o", "b first_last", "-o", "b total", "-o", "run",
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
        args = [lldb, "-b", *LLDB_PASS_FAULTS]
        for c in cmds + ["kill"]:
            args += ["-o", c]
        code, o, e = tc.run_capture([*args, out])
        missing = [w for w in wants if not re.search(w, o + e)]
        if missing:
            fail("tinygen built-array debug info under lldb", f"missing {missing}:\n{o + e}")
            return
    ok("tinygen debug info under lldb: a resizable and a u8[] local built in place")
    check_lldb_freevars(exe, base, lldb, ok, fail)
    check_lldb_whole(exe, base, lldb, ok, fail)


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
    code, o, e = tc.run_capture([lldb, "-b", *LLDB_PASS_FAULTS, "-o", "b bump", "-o", "run", "-o", "frame variable",
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


def check_lldb_whole(exe, base, lldb, ok, fail):
    """A whole program build, whose C main is the tinygen object's: the
    backtrace ends in `main` at fn main()'s Goose line, and fn main()'s
    locals show by their Goose names and types."""
    source = HERE / "tinygen" / "tinygen_structs.goose"
    out = base / f"lldb-whole{tc.EXE_SUFFIX}"
    good, log, _, _ = build(exe, source, out, [])
    if not good or "tinygen: whole program" not in log:
        fail("tinygen lldb whole program build", log)
        return
    code, o, e = tc.run_capture([lldb, "-b", *LLDB_PASS_FAULTS, "-o", "b tinygen_structs.goose:41", "-o", "run",
                                 "-o", "frame variable", "-o", "bt", "-o", "kill", out])
    text = o + e
    wants = [
        r"main_g at tinygen_structs\.goose:41",
        r"\(Vec2\) v = \(x = 4\.5, y = -1",
        r"frame #1: .*`main at tinygen_structs\.goose:39",
    ]
    missing = [w for w in wants if not re.search(w, text)]
    if missing:
        fail("tinygen whole program debug info under lldb", f"missing {missing}:\n{text}")
        return
    ok("tinygen debug info under lldb: a whole program's main and its caller")
    check_lldb_formatters(exe, base, lldb, ok, fail)


FORMATTERS = ROOT / "src" / "lldb" / "goose_lldb.py"


def check_lldb_formatters(exe, base, lldb, ok, fail):
    """With src/lldb/goose_lldb.py imported, values as Goose shows them:
    a slice, a resizable and a limited array as their live elements, text
    as a string, i8 and u8 as numbers, an optional as null or its value,
    an ADT as its active variant. A -v build names the script to import."""
    runs = [
        ("tinygen_arrays.goose", ["b weighted", "run", "frame variable s"],
         [r"\(i64\[:\]\) s = len=5 \{\s*\[0\] = 3\s*\[1\] = 1\s*\[2\] = 4\s*\[3\] = 1\s*"
          r"\[4\] = 5\s*\}"]),
        ("tinygen_strings.goose", ["b first_last", "b total", "run", "frame variable s", "p *s",
                                   "continue", "frame variable v"],
         [r"\(u8\[\] \*\) s = 0x[0-9a-f]+ \"hello\"\n", r"\(u8\[\]\) \"hello\"\n",
          r"\(i64\[>\.\.\]&\) v = len=3 \{\s*\[0\] = 1\s*\[1\] = 2\s*\[2\] = 3\s*\}"]),
        ("tinygen_structs.goose", ["b mixed_sum", "b tinygen_structs.goose:44", "run",
                                   "frame variable r", "continue", "frame variable m"],
         [r"\(Box\) r = \{.*tag = 7\n", r"\(Mixed\) m = \(a = -3, b = 3000,"]),
        ("tinygen_optionals.goose", ["b or_zero", "run", "frame variable o", "continue",
                                     "frame variable o"],
         [r"\(i64\?\) o = 9\n.*\(i64\?\) o = null\n"]),
        ("tinygen_adts.goose", ["b scaled", "b turn", "run", "frame variable s", "continue",
                                "frame variable d"],
         [r"\(Shape\) s = Shape\.Rect\(w = 3, h = 4\)\n", r"\(Dir\) d = Dir\.West\n"]),
        ("tinygen_limited.goose", ["b tinygen_limited.goose:89", "b tinygen_limited.goose:100",
                                   "run", "frame variable a n", "continue", "frame variable s"],
         [r"\(i64\[\.\.8\]\) a = len=2 \{\s*\[0\] = 8\s*\[1\] = 7\s*\}",
          r"\(Name\) n = \{\s*tag = 7\s*text = \"tiny\"\s*\}", r"\(u8\[\.\.8\]\) s = \"Hey!!\""]),
    ]
    for i, (name, cmds, wants) in enumerate(runs):
        out = base / f"lldb-fmt-{i}{tc.EXE_SUFFIX}"
        good, log, _, _ = build(exe, HERE / "tinygen" / name, out, [])
        if not good:
            fail(f"tinygen lldb formatters {name} build", log)
            return
        if f'command script import "{FORMATTERS}"' not in log:
            fail("tinygen -v names the lldb formatters", log)
            return
        args = [lldb, "-b", *LLDB_PASS_FAULTS, "-o", f'command script import "{FORMATTERS}"']
        for c in cmds + ["kill"]:
            args += ["-o", c]
        code, o, e = tc.run_capture([*args, out])
        missing = [w for w in wants if not re.search(w, o + e, re.S)]
        if missing or "Traceback" in o + e:
            fail(f"tinygen lldb formatters on {name}", f"missing {missing}:\n{o + e}")
            return
    ok("tinygen lldb formatters: arrays' elements, text, i8/u8, optionals, ADTs, limited arrays")


def check_lldb_optionals(exe, base, ok, fail):
    """Optionals by their Goose types: a T? parameter and local as the
    address the C half's T * is (0 when null), and what one refers to
    through it, a struct with an optional field."""
    lldb = shutil.which("lldb")
    if not lldb:
        return
    source = HERE / "tinygen" / "tinygen_optionals.goose"
    out = base / f"lldb-optionals{tc.EXE_SUFFIX}"
    good, log, _, _ = build(exe, source, out, [])
    if not good:
        fail("tinygen lldb optionals build", log)
        return
    # or_zero(big), then or_zero(none); then sum's loop.
    code, o, e = tc.run_capture([lldb, "-b", *LLDB_PASS_FAULTS, "-o", "b or_zero", "-o", "run", "-o", "continue",
                                 "-o", "frame variable", "-o", "br del 1",
                                 "-o", "b tinygen_optionals.goose:41", "-o", "continue",
                                 "-o", "frame variable", "-o", "p *n", "-o", "kill", out])
    text = o + e
    wants = [
        r"\(Link\?\) head = 0x[0-9a-f]+",
        r"\(Link\?\) n = 0x[0-9a-f]+\s*\(i64\) s = 0",
        r"\(Link\) \{\s*val = 1\s*next = 0x[0-9a-f]+",
        r"\(i64\?\) o = 0x0+\s",
    ]
    missing = [w for w in wants if not re.search(w, text)]
    if missing:
        fail("tinygen optional debug info under lldb", f"missing {missing}:\n{text}")
        return
    ok("tinygen debug info under lldb: optionals by their Goose types, null and not")


if __name__ == "__main__":
    fails = []
    check_tinygen_backend(sys.argv[1], ROOT / "build" / "gen", lambda m: print("ok", m),
                          lambda m, d="": (fails.append(m), print("FAIL", m, d)), print)
    sys.exit(1 if fails else 0)
