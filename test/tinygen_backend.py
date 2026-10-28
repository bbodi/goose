"""The tinygen backend: `goose build --backend=tinygen` against the
expected output of the C backend.

Each program in PROGRAMS is built and run; its output must be the blessed
one the C backend is held to. Tinygen must compile at least one function of
each program, and every function of those marked whole (the test/tinygen/
fixtures among them). Some fixtures are also built with --tinygen-skip,
which leaves the named functions to C, so that calls cross between the two
halves in both directions. The WHOLE_PROGRAM programs must take the whole
program path, which compiles no C of the program's own; with
--tinygen-mixed or --tinygen-skip they must take the mixed path. The gfx,
ui, audio and physics fixtures run headless; the windowed samples
(BUILD_ONLY) are only built. All of them are built and run again with
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

# (source, whether every function must go through tinygen, extra build flags)
PROGRAMS = [
    ("test/tinygen/tinygen_scalars.goose", True, []),
    ("test/tinygen/tinygen_scalars.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_structs.goose", True, []),
    ("test/tinygen/tinygen_structs.goose", False, ["--tinygen-skip=main"]),
    ("test/tinygen/tinygen_structs.goose", False, ["--tinygen-skip=add,area,mixed_sum,swap"]),
    ("test/tinygen/tinygen_structs.goose", False, ["--tinygen-skip=scale,dot,grow,mixed"]),
    # Packed structs by value across the C calling convention, both ways: C returns to
    # tinygen, tinygen passes to C and back, C calls tinygen past the registers.
    ("test/tinygen/tinygen_abi.goose", True, []),
    ("test/tinygen/tinygen_abi.goose", False, ["--tinygen-skip=main"]),
    ("test/tinygen/tinygen_abi.goose", False,
     ["--tinygen-skip=mk_b1,mk_b3,mk_f2,mk_i8m,mk_u8f,mk_fu8,mk_if12,mk_f3,mk_df12,mk_u8i64,"
      "mk_i64u8,mk_di,mk_id,mk_d2,mk_odd16,mk_s17,mk_s24,mk_s33,mk_arr,"
      "mk_nest,mk_nest8,ret_i8,ret_u16"]),
    ("test/tinygen/tinygen_abi.goose", False,
     ["--tinygen-skip=h_b1,h_b3,h_f2,h_i8m,h_u8f,h_fu8,h_if12,h_f3,h_df12,h_u8i64,h_i64u8,"
      "h_di,h_id,h_d2,h_odd16,h_s17,h_s24,h_s33,h_arr,h_nest,h_nest8,p_b3,"
      "p_i8m,p_u8f,p_fu8,p_if12,p_f3,p_df12,p_u8i64,p_di,p_id,p_odd16,p_s17,"
      "p_s33,p_nest,p_nest8,small,many_int,many_sse,many_mix,many_ret,"
      "many_ret2"]),
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
    ("test/tinygen/tinygen_strings.goose", True, []),
    ("test/tinygen/tinygen_strings.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_strings.goose", False, ["--tinygen-skip=main"]),
    ("test/tinygen/tinygen_strings.goose", False,
     ["--tinygen-skip=count,first_last,total,sum_view,addto,own"]),
    ("test/tinygen/tinygen_strings.goose", False, ["--tinygen-skip=walk,shout,far"]),
    ("test/tinygen/tinygen_build.goose", True, []),
    ("test/tinygen/tinygen_build.goose", True, ["--debug"]),
    # The stack settings reach the support object at run time, so they keep a whole build.
    ("test/tinygen/tinygen_build.goose", True, ["--stack-reserve", "64M"]),
    ("test/tinygen/tinygen_build.goose", False, ["--tinygen-skip=main"]),
    ("test/tinygen/tinygen_build.goose", False,
     ["--tinygen-skip=squares,word,evens,label,split,points"]),
    ("test/tinygen/tinygen_build.goose", False, ["--tinygen-skip=sum,own_sum,scan,nested"]),
    ("test/tinygen/tinygen_text.goose", True, []),
    ("test/tinygen/tinygen_text.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_text.goose", True, ["--tinygen-mixed"]),
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
    # An --include header reads a struct its extern takes by reference, while every Goose
    # function, the extern's callers among them, is tinygen's: whole, the header in the extern unit.
    ("samples/24_call_c.goose", True, ["--include", str(ROOT / "samples" / "call_c.h")]),
    ("test/tinygen/tinygen_render.goose", True, []),
    ("test/tinygen/tinygen_render.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_render.goose", False, ["--tinygen-skip=main"]),
    ("test/tinygen/tinygen_render.goose", False, ["--tinygen-skip=show,label,points,link"]),
    ("test/tinygen/tinygen_control.goose", True, []),
    ("test/tinygen/tinygen_control.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_control.goose", False, ["--tinygen-skip=main"]),
    ("test/tinygen/tinygen_control.goose", False,
     ["--tinygen-skip=classify,small,side,valued,first_over,larger"]),
    ("test/tinygen/tinygen_elemwise.goose", True, []),
    ("test/tinygen/tinygen_elemwise.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_elemwise.goose", False, ["--tinygen-skip=bump,bumpa,make"]),
    ("test/codegen/loop_invariant_divisors.goose", False, []),
    ("test/codegen/cnames.goose", False, []),
    ("test/codegen/wider_slot_operations.goose", False, []),
    ("samples/21_raytrace.goose", False, []),
    # Inlined code (-O2, the default; -O0 inlines nothing): a body spliced into its caller, its
    # returns leaving the block. Whole programs, so a fallback to C fails.
    ("samples/21_raytrace.goose", True, ["-O2"]),
    ("test/tinygen/tinygen_inline.goose", True, ["-O2"]),
    ("test/tinygen/tinygen_inline.goose", True, ["-O1"]),
    ("test/tinygen/tinygen_inline.goose", True, ["-O0"]),
    ("test/tinygen/tinygen_inline.goose", True, ["-O2", "--debug"]),
    ("test/codegen/recursive_loop_walk.goose", True, ["-O2"]),
    ("test/codegen/exit_value_destination.goose", True, ["-O2"]),
    ("test/codegen/exit_value_destination.goose", True, ["-O0"]),
    ("samples/20_life.goose", True, []),
]

# The programs tinygen builds whole with no C half: main in the object,
# the rest from the support object, no C of the program compiled.
WHOLE_PROGRAM = {
    "tinygen_scalars", "tinygen_structs", "tinygen_divzero", "tinygen_arrays",
    "tinygen_slicefail", "tinygen_results", "tinygen_build", "tinygen_text",
    "tinygen_shrink", "tinygen_nested", "tinygen_funvals", "20_life",
    "loop_invariant_divisors", "tinygen_globals", "tinygen_gstacks", "tinygen_retfrom",
    "tinygen_render", "tinygen_strings", "tinygen_control", "tinygen_elemwise",
    "tinygen_abi",
}

# ---- ADTs: fixed-mode values, variants, match and case-function dispatch,
# whole and with the cases, the scrutinees' owners and main left to C in
# turn; and a sample of variable-mode ADTs.
PROGRAMS += [
    ("test/tinygen/tinygen_adts.goose", True, []),
    ("test/tinygen/tinygen_adts.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_adts.goose", False, ["--tinygen-skip=main"]),
    ("test/tinygen/tinygen_adts.goose", False,
     ["--tinygen-skip=area,scale_area,name,tokval,widen,dirnum"]),
    ("test/tinygen/tinygen_adts.goose", False,
     ["--tinygen-skip=scaled,by_ref,grow,first_tok,turn,circle"]),
    ("samples/05_shapes.goose", False, []),
]
WHOLE_PROGRAM |= {"tinygen_adts", "cnames", "tinygen_inline", "recursive_loop_walk"}
WHOLE_PROGRAM |= {"wider_slot_operations", "05_shapes"}


# Limited arrays T[..k]: the fixture whole, plain and --debug, then with
# the functions taking, returning and growing them left to C in turn, so
# that by-value and by-reference limited arrays (and structs holding one)
# cross the two halves both ways.
PROGRAMS += [
    ("test/tinygen/tinygen_limited.goose", True, []),
    ("test/tinygen/tinygen_limited.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_limited.goose", False, ["--tinygen-skip=main"]),
    ("test/tinygen/tinygen_limited.goose", False, ["--tinygen-skip=grow,halves,bag"]),
    ("test/tinygen/tinygen_limited.goose", False,
     ["--tinygen-skip=digits,sum,shrink,named,card"]),
    # A return nested in another return's T[] value as it is built.
    ("test/codegen/exit_value_destination.goose", True, []),
]
WHOLE_PROGRAM |= {"tinygen_limited", "exit_value_destination"}

# Variable-size structs (tinygen_varsize): built in place, read behind
# their variable-size fields, passed and returned by value and by
# reference, crossing the halves both ways.
PROGRAMS += [
    ("test/tinygen/tinygen_varsize.goose", True, []),
    ("test/tinygen/tinygen_varsize.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_varsize.goose", False, ["--tinygen-skip=main"]),
    ("test/tinygen/tinygen_varsize.goose", False, ["--tinygen-skip=hire,pay,pick,with_count"]),
    ("test/tinygen/tinygen_varsize.goose", False, ["--tinygen-skip=raise,describe,initials"]),
]
WHOLE_PROGRAM |= {"tinygen_varsize"}



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
# of slot, made, tested, narrowed, rebound and printed, whole, and with
# chosen functions left to C so that optionals cross the two halves both
# ways (as parameters, results, fields, free variables, globals and
# `return from` values).
PROGRAMS += [
    ("test/tinygen/tinygen_optionals.goose", True, []),
    ("test/tinygen/tinygen_optionals.goose", True, ["--debug"]),
    ("test/tinygen/tinygen_optionals.goose", False, ["--tinygen-skip=main"]),
    ("test/tinygen/tinygen_optionals.goose", False,
     ["--tinygen-skip=first_big,bump,count,shift,pick,grow,search,label,see"]),
    ("test/tinygen/tinygen_optionals.goose", False,
     ["--tinygen-skip=or_zero,sum,longer,same,total,walk,look,find,positive,counted"]),
]
WHOLE_PROGRAM |= {"tinygen_optionals"}
WHOLE_PROGRAM |= {"tinygen_extern", "21_raytrace"}   # libm and package externs called directly
WHOLE_PROGRAM |= {"24_call_c"}   # the --include header's externs through the extern unit

# The native modules' fixtures (gfx, ui, audio, physics): whole programs that
# call into the packages' C layers. They run headless, as the suite runs
# them; one is skipped where the compiler lacks its package, or for gfx
# where there is no GPU device. Their rejection tests (`// error:`) are not
# programs.
NATIVE_DIRS = ("gfx", "ui", "audio", "physics")
NATIVE = sorted(p for d in NATIVE_DIRS for p in (HERE / d).glob("*.goose")
                if "// error:" not in tc.decode(p.read_bytes()))
PROGRAMS += [(str(p.relative_to(ROOT)), True, []) for p in NATIVE]
WHOLE_PROGRAM |= {p.stem for p in NATIVE}

# Programs of the main suite that sweeping found wrong or refused by the backend (G4.2, G4.3): whole
# programs, so that the suite fails on a wrong output and on any function left to C. The G4.3 ones are
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
               "test/codegen/frame_pool_references.goose"]
# Programs whose header asks for the debug checks (a runtime-debug or GS_DEBUG=1 line), built with
# --debug: thread_contained answers only in a debug build, and the others, also listed above, have
# links the checks verify.
DEBUG_REGRESSIONS = ["test/threads/thread_contained.goose", "test/storage/relative_block_results.goose",
                     "test/storage/relative_destination_results.goose",
                     "test/storage/alloc_built_in_slot.goose",
                     "test/codegen/relative_receiver_values.goose"]
PROGRAMS += [(path, True, []) for path in REGRESSIONS]
PROGRAMS += [(path, True, ["--debug"]) for path in DEBUG_REGRESSIONS]
WHOLE_PROGRAM |= {Path(path).stem for path in REGRESSIONS + DEBUG_REGRESSIONS}
# Fixed values above the C generator's 4096-byte limit lie on a data stack, as there: a local, a
# temporary and a result slot, restored with their scope. Inlined (-O2) they used to pass the native
# stack (a 1 MiB guard refused them), so both inlining levels are whole programs.
LARGE_FIXED = ["test/codegen/large_fixed_values.goose", "test/codegen/large_fixed_exits.goose"]
PROGRAMS += [(path, True, [level]) for path in LARGE_FIXED for level in ("-O0", "-O2")]
WHOLE_PROGRAM |= {Path(path).stem for path in LARGE_FIXED}
# Programs the backend does not yet build whole: each leaves the named functions to the C generator
# (the reference semantics), so the suite proves the fallback gives the blessed output today. An entry
# moves to REGRESSIONS once its refusal is lifted, and each comment names the refusal it reaches.
GAPS = [
]
PROGRAMS += [(path, False, []) for path in GAPS]
# Results with relative links written by the callee into its caller's slot (and a return from's channel)
# across the two halves: the callee, or the caller, left to C.
PROGRAMS += [("test/storage/relative_destination_results.goose", False,
              ["--tinygen-skip=distant,make"]),
             ("test/codegen/relative_receiver_values.goose", False,
              ["--tinygen-skip=choose,make_pair"])]

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


def check_tinygen_backend(exe, base, ok, fail, say):
    base = (Path(base) / "tinygen").resolve()
    base.mkdir(parents=True, exist_ok=True)
    probe = base / "probe.goose"
    tc.write_text(probe, "fn main() { print(1); }\n")
    code, o, e = tc.run_capture([exe, "build", "--backend=tinygen", "--dry-run", probe])
    if "built without tinygen" in o + e:
        say("skip tinygen backend (the compiler was built without tinygen)")
        return
    # A gfx program asking for a window draws off screen instead.
    os.environ["GOOSE_GFX_HEADLESS"] = "1"
    builtin = tc.builtin_modules(exe)
    for i, (rel, whole, flags) in enumerate(PROGRAMS):
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
        if done == 0 or (whole and done != total):
            fail(f"{label}: {done} of {total} functions through tinygen", log)
            continue
        # The path the build took: whole for the WHOLE_PROGRAM fixtures
        # (unless asked for the mixed one or told to leave functions to
        # C), with no compile of the program's C, else mixed.
        alone = (source.stem in WHOLE_PROGRAM and
                 not any(f.startswith("--tinygen-") for f in flags))
        cfile = f"{source.stem}.c"
        compiled = [line for line in log.splitlines() if cfile in line and " -o " in line]
        if alone != ("tinygen: whole program" in log) or (alone and compiled):
            fail(f"{label}: expected the {'whole program' if alone else 'mixed'} path", log)
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
        ok(f"{label} ({done} of {total} functions{', whole program' if alone else ''})")
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
    check_run(exe, base, ok, fail)
    check_jit(exe, base, ok, fail)
    check_default_inlining(exe, base, ok, fail)
    check_build_defaults(exe, base, ok, fail)
    check_c_path_flags(exe, base, ok, fail)
    check_debug(exe, base, ok, fail)
    check_lldb(exe, base, ok, fail, say)
    check_lldb_limited(exe, base, ok, fail)
    check_lldb_adts(exe, base, ok, fail)
    check_lldb_optionals(exe, base, ok, fail)


def tinygen_c_problem(log, debug):
    """What is wrong with the compile of tinygen's C in a tinygen-c build's
    -v log: the first error or warning of the compiler's output (tgc.log,
    beside the .tg.c), or the wrong flags; "" for nothing."""
    cmd = next((line for line in log.splitlines() if ".tg.c -o " in line), None)
    if cmd is None:
        return "tinygen's C was not compiled"
    want = ["-O0", "-g"] if debug else ["-O2"]
    if any(f not in cmd.split() for f in want):
        return f"compiled without {' '.join(want)}: {cmd}"
    tgc = next(a for a in cmd.split() if a.endswith(".tg.c"))
    out = Path(tgc).parent / "tgc.log"
    text = tc.decode(out.read_bytes()) if out.exists() else ""
    first = next((line.strip() for line in text.splitlines()
                  if "error:" in line or "warning:" in line), "")
    if first or "compiling tinygen's C failed" in log:
        return first or "does not compile"
    return ""


def check_tinygen_c(exe, base, ok, fail, say, builtin):
    """--backend=tinygen-c: the same programs, built through tinygen's C
    output and the C compiler (-O2, or -O0 -g with --debug), with the same
    output and the same path; a whole program compiles only its .tg.c. A
    program whose tinygen C does not compile (or warns) fails on a line of
    its own."""
    for i, (rel, whole, flags) in enumerate(PROGRAMS):
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
        if done == 0 or (whole and done != total):
            fail(f"{label}: {done} of {total} functions through tinygen", log)
            continue
        alone = (source.stem in WHOLE_PROGRAM and
                 not any(f.startswith("--tinygen-") for f in flags))
        cfile = f"{source.stem}.c"
        compiled = [line for line in log.splitlines() if cfile in line and " -o " in line]
        if alone != ("tinygen: whole program" in log) or (alone and compiled):
            fail(f"{label}: expected the {'whole program' if alone else 'mixed'} path", log)
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
        ok(f"{label} ({done} of {total} functions{', whole program' if alone else ''})")
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


# goose run with no --backend runs tinygen's C in TinyCC (G3.1); --backend=c the C generator's.
JIT_PROGRAMS = ["samples/01_tour.goose", "test/tinygen/tinygen_text.goose",
                "test/codegen/defer_results.goose", "test/codegen/index_type_abort.goose",
                "test/codegen/relative_results.goose"]
TINYGEN_JIT = "jit: tinygen's C"
C_JIT = "jit: the C generator's C"


def check_jit(exe, base, ok, fail):
    """Plain goose run (tinygen's C in TinyCC) and goose run --backend=c (the
    C generator's): the blessed output, exit status and stderr markers on
    both, -v naming the path, and -O2 (inlined code, the default) and -O0 on tinygen's C."""
    exe = Path(exe).resolve()
    for rel in JIT_PROGRAMS:
        source = ROOT / rel
        want, aborts, markers = expected(source)
        for flags, says in [([], TINYGEN_JIT), (["-O2"], TINYGEN_JIT), (["-O0"], TINYGEN_JIT),
                            (["--backend=c"], None)]:
            label = f"goose run {' '.join(flags + ['-v'])} {source.name}"
            code, got, err = tc.run_capture([exe, "run", "-v", *flags, source], cwd=source.parent)
            missing = [mk for mk in markers if mk not in err]
            if (code != 0) != aborts or missing or got.rstrip("\n") != want.rstrip("\n"):
                fail(f"{label} (exit {code})", f"{err}\n--- got:\n{got}\n--- want:\n{want}")
                return
            if says is None and "jit:" in err or says is not None and says not in err:
                fail(f"{label}: -v should say {says or 'nothing of tinygen'}", err)
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
    ok(f"goose run on tinygen's C and with --backend=c: {len(JIT_PROGRAMS)} programs, -O2 "
       "and -O0, arguments, exit code, resources")


# goose build with no --backend (G3.2): tinygen-c, or tinygen with --debug; the C generator's C
# for the whole program when tinygen cannot take it.
DEFAULT_BUILD_PROGRAMS = ["samples/01_tour.goose", "test/tinygen/tinygen_text.goose",
                          "test/codegen/defer_results.goose"]
DEFAULT_C_PATH = "build: the C generator's C, compiled by the C compiler"


def check_build_defaults(exe, base, ok, fail):
    """Plain goose build runs tinygen-c, --debug the native tinygen backend (both -v named, the
    program's output right); -o x.c writes tinygen's C or, for --standalone, the generator's."""
    exe = Path(exe).resolve()
    work = base / "defaults"
    work.mkdir(parents=True, exist_ok=True)

    def built(source, flags, says, label):
        out = work / f"{source.stem}{tc.EXE_SUFFIX}"
        out.unlink(missing_ok=True)
        code, o, e = tc.run_capture([exe, "build", "-v", *flags, "-o", out, source],
                                    cwd=source.parent)
        log = o + e
        if code != 0 or not out.exists() or not all(t in log for t in says) or \
                (DEFAULT_C_PATH in log) != any(DEFAULT_C_PATH in t for t in says):
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
    source = ROOT / DEFAULT_BUILD_PROGRAMS[0]
    # -o x.c: tinygen's C for a program it takes, the generator's C else (--standalone: one file
    # with the runtime, which tinygen's C cannot be).
    for flags, fallback in [([], False), (["--standalone"], True)]:
        out = work / "out.c"
        out.unlink(missing_ok=True)
        code, o, e = tc.run_capture([exe, "build", *flags, "-o", out, source])
        text = tc.decode(out.read_bytes()) if out.exists() else ""
        if code != 0 or ("Generated by the Goose compiler" in text) != fallback or len(text) < 100:
            fail(f"goose build {' '.join(flags)} -o out.c (fallback: {fallback})", o + e)
            return
    ok(f"goose build: tinygen-c by default and tinygen with --debug on {len(DEFAULT_BUILD_PROGRAMS)} "
       "programs, -o out.c (the C generator's with --standalone)")


def check_c_path_flags(exe, base, ok, fail):
    """The flags that took a program to the C generator's C (G3.3) keep it on tinygen: plain goose
    build, --debug and goose run, -v saying so, the program's output and stderr markers right."""
    exe = Path(exe).resolve()
    work = base / "cflags"
    work.mkdir(parents=True, exist_ok=True)

    def each(source, flags, label, want=None, built=""):
        """Builds and runs source each way, a build's -v log holding `built`; False after a
        failure."""
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
                if code != 0 or says not in o + e or DEFAULT_C_PATH in o + e or built not in o + e:
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
    # --standalone: the runtime compiled into the build directory instead of the cached object.
    if not each(ROOT / "samples/01_tour.goose", ["--standalone"], "--standalone",
                built="/goose_runtime.c -o "):
        return
    ok("the C path's flags on tinygen: -D, --include (a static inline extern, a -D in the header), "
       "--standalone")


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
