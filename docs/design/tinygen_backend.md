# The tinygen backend

A second native backend for debug builds: the compiler hands the functions it
can to tinygen (a C99 library, checked out beside this repository as
`../tinygen`), which writes them straight to an
object file with debug info, and the rest of the program goes through the C
backend as before. This document records why, how it fits into the compiler,
what the first stage covers, and the plan from here.

    goose build --backend=tinygen prog.goose        # debug build, -O0
    goose build --backend=tinygen --debug prog.goose  # with GS_DEBUG's checks
    goose build --backend=tinygen -v prog.goose     # also says what stayed in C

## Why

A debug build today is the C backend's C compiled at `-O0 -g`. Two things
are wrong with that for debugging Goose:

* **The debugger shows C.** The generated C carries no `#line` directives,
  so lldb, gdb and Visual Studio step through `main_g`, `t5 = fib_g(t4)` and
  `gs_add_i64(...)` in a 50 000-line `.c` file under `GOOSE_HOME`, with
  temporaries for locals and C type names (`int64_t`, `Vec2_g`). `#line`
  alone would fix the lines but not the names, the types, or the dozens of
  `tN` temporaries per statement, and it would make the C's own debugging
  (which compiler developers need) worse.
* **The C compiler is the slow part of a build.** A program's C repeats the
  inline half of the runtime and every type's helpers; a C compiler parses,
  type-checks and lowers all of it on every build, even at `-O0`.

tinygen addresses both: it is a code generator built for debug builds (no
optimizer, one pass to machine code, Mach-O/ELF/COFF with DWARF/CodeView),
and its debug info is whatever the front end says it is. A function compiled
through it is, in the debugger, a Goose function: its Goose name, its lines
in the `.goose` file, its parameters and locals under their Goose names with
Goose types (`i64`, `f64`, `Box`, `Box *` for a `Box&`).

## Where it plugs in

The backend consumes exactly what the C backend consumes: the live
specializations (`FnSpec`) after typechecking, optimization and BCE, with
their cloned, annotated bodies (`docs/implementation.md` §1). Nothing is
re-derived. In particular it reuses, from `CodeGen`:

* the **call interface** of every specialization (`CodeGen::sinfo`,
  `SigParams`: its C name, whether it takes `gs_sp`, its free variables, which
  result is the C return value, the `return from` channels), so that a C
  function and a tinygen function call each other through the platform's C
  calling convention exactly as two C functions would;
* the **layouts** (`StructLayout`, `FixedSize`, `Cls`, `IsLargeFixed`): Goose
  fixed types are packed, and tinygen is given the same records, aligned to 1,
  that `#pragma pack(1)` gives the C half;
* the checker's annotations on each node: `exprtype`, `OperandT`'s reference
  transparency, the `as` target types (`AsCast::totype`), the resolved callee
  of each call (`Call::spec`), the loop shapes (`ForLoop::iterkind`).

The code is `src/tinygen_backend.h`, compiled in only when tinygen is found
(`GOOSE_HAVE_TINYGEN`). `CodeGen` knows of it through one small interface,
`CodeGen::SpecBackend`, which chooses among the live specializations once
their call interfaces are known (`Choose`).

## Two halves, one program

Decision: **the C backend still runs for every program**, and tinygen takes
over function bodies one at a time. The build links three objects:

| Object | Holds |
|---|---|
| the program's C (`<stem>.c`), compiled `-O0 -g` | `main`, `gs_program_init`, the globals and their initializers, the inline half of the runtime (`runtime.h`, `runtime_ext.h`), every specialization tinygen did not take, the shims and extern trampolines below |
| `<stem>.tg.o`, written by tinygen | every specialization tinygen took |
| the runtime object (`goose_runtime.o`) | as for any build (`build.h`) |

With a backend present, `CodeGen` gives every specialization external linkage
(`FnLinkage`: no `static`, and no `GS_INLINE`, since an inline definition with
external linkage defines no symbol in C), declares the taken ones with a
prototype, and emits no body for them. Names are the C backend's (`fib_g`), so
the symbol table, the linkers and `--header` see no difference.

**Fallback granularity.** The obvious safe choice is the whole program: a
program using anything tinygen does not support goes through C. With the C
half always present, the per-function choice costs nothing more and covers
far more code in the first stages, so that is what this does: a
specialization whose interface or body holds anything unsupported anywhere is
left to C whole (no partial bodies). `Choose` builds each candidate straight
into the module; one that meets something unsupported is abandoned
(`tyg_proc_abort`), the shims and trampolines it asked for are taken back, and
it leaves the set. A body may call a specialization that takes `gs_sp` only
if that one is tinygen's too (it then never uses the stacks it is handed, and
is passed 0): where one that left was called so by a body already built, the
module is built again from scratch without it, until nothing called leaves
(rare: such callees mostly use data stacks and leave on the first pass).
`-v` lists what stayed in C and why
(`tinygen: main_g (prog.goose:13) left to C: builtin push (line 17)`).

**The runtime.** Everything the runtime object exports (`GS_API`: printing,
`gs_divfail`, `gs_ovf`, the OS layer) is called directly. What `runtime.h`
defines as `static` in the program's own C — checked arithmetic, division and
modulo, the `as` checks, `gs_f2iwrap` — is reached through exported shims the
backend appends to the C (`gs_tg_add_i32`, `gs_tg_div_i64`, `gs_tg_range`,
...). They are compiled with the program's defines, so `GS_DEBUG` (`--debug`)
means the same in both halves: unsigned arithmetic and release-build signed
arithmetic are inline wrapping instructions, a debug build's signed
arithmetic and every division go through the shim, and the abort names the
Goose file and line, which the object passes as a string of its own.

**Extern fns** (§7.10) are called through a trampoline per extern
(`gs_tg_x_<cname>`) that the C half defines, because the C function may be a
`static` one (an `--include` header's, `runtime_ext.h`'s) or a macro. The
trampoline adds a call; a later stage can call plain library symbols
directly. The externs called count as used (`CodeGen::usedexterns`), so the
native parts of the packages they come from are built and linked as for C
(`NativePackagesUsed`).

**Data stacks** (§6.2) stay entirely in C for now: a tinygen function builds
nothing on a stack, and calls no C function that takes `gs_sp`. The stack
accounting (`codegen_stacks.h`) counts what C functions use; a tinygen
function counts as owning none.

**Packages** need nothing new: their Goose functions are specializations like
any other, their C parts are linked as before, and their extern fns go
through the trampolines.

**Globals** stay in `gs_globals_t` in the C half; a function naming one is
left to C in the first stage.

## Building tinygen into goose

tinygen is one translation unit. `CMakeLists.txt` takes a `TINYGEN_DIR`
cache path, by default `../tinygen` beside this checkout, and when
`src/tinygen.c` and `include/tinygen.h` are there compiles it as a static
library (`goose_tinygen`, C99, warnings off as for the other third-party
code) linked into the compiler, with `GOOSE_HAVE_TINYGEN=1`. Without it the
compiler builds as before and `--backend=tinygen` says it was left out. A
later step can vendor tinygen under `third_party/` like TinyCC, or fetch it
at a pinned tag like the blessed packages, once its API settles.

## What the first stage supports

Specializations whose interface is plain: parameters and a result of the types
below (no free variables, no `return from`, no multiple results, no pool
parameters, no large fixed values), and whose bodies use only:

* **types**: every integer type but `varint`, `f32`, `f64`, `bool`, plain
  references `T&` to those or to fixed structs, fixed structs of all of these
  (nested, by value, by reference, as parameters and results);
* **expressions**: literals; locals and parameters; field access, through
  references too; arithmetic, comparison, bitwise and shift operators at every
  width, with §6.2's wrapping, checks and Euclidean `%`; float `%` (libm);
  `&&`, `||`, `!`, `~`, unary `-`; `as` and `as!` between the scalar types
  with §6.3's checks; struct literals; `if` as a value; calls to
  specializations (in either half) and to extern fns; `print` of scalars and
  string literals; a string literal as an extern's `u8[:]` argument;
* **statements**: `let`/`var`, assignment and compound assignment (to
  locals, fields, and through references), `++`/`--`, `if`, `while`,
  `loop`, `for` over a range or a count, `break`, `continue`, `return`.

The debug info: one compile unit per object; each procedure under its Goose
name (`tyg_debug_proc`, the C name as its linkage name), a line entry per
statement and per loop head (`tyg_loc`), parameters (`tyg_debug_param`)
and locals in lexical scopes (`tyg_debug_var`, `tyg_scope_begin`/`end`),
with Goose types: scalars as typedefs named `i64`, `u8`, `f64`, `bool` over
C base types (lldb renames a base type after the C type of its size, but
keeps a typedef's name), structs as records with their Goose field names,
references as pointers.

```
(lldb) b mixed_sum
Breakpoint 1: where = prog`mixed_sum_g + 40 at tinygen_structs.goose:27
(lldb) run
* thread #1, stop reason = breakpoint 1.1
    frame #0: prog`mixed_sum_g(m=(a = '\xfd', b = 3000, c = 0.25, d = true, e = 65535)) at tinygen_structs.goose:27
-> 27   fn mixed_sum(m: Mixed) -> f64 {
(lldb) bt
  * frame #0: prog`mixed_sum_g(m=...) at tinygen_structs.goose:27
    frame #1: prog`main_g at tinygen_structs.goose:50
    frame #2: prog`main(argc=1, argv=...) at tinygen_structs.c:1218:5
```

`--backend=tinygen` makes `-O0` the default (no inlining: every Goose
function stays a function in the debugger; `-O1`/`-O2` still work, and
functions with inlined bodies then go to C) and compiles the C half `-O0 -g`
(`BuildOptions::debug`) without defining `GS_DEBUG`; `--debug` adds that.
It works with `goose build`; `goose run` keeps the in-process TinyCC path.

## What the second stage adds: fixed arrays and slices

* **types**: fixed arrays `T[k]` (k > 0) of any type above, and slices
  `T[:]` of fixed elements, as locals, fields, elements, parameters and
  results. For the ABI a `T[k]` is the C half's `struct { T e[k]; }`, packed:
  a record holding one `tyg_type_array`; a slice is `{ data, len }`, a pointer
  and an `int64_t` (C.3).
* **expressions**: indexing, as a value and as a place, with GS_IDX's bounds
  check calling `gs_idxfail` (the runtime object's, so the message is the C
  backend's) unless BCE elided it (`Index::nobc`) or the index is a constant
  within a fixed array; slicing `a[lo..hi]` (`^k` from the end) with
  `GS_E_SLICE`'s check through a shim; `.len`; array literals `[a, b, c]`
  and `[v; k]`; a whole array where a slice is wanted (§3.10), viewed where it
  lies; a string literal as a `u8[:]`. Slices of fixed elements also go to extern fns
  (an array, a part of one), not only string literals.
* **statements**: `for x in a`, `for &x in a` and `for x, i in a` over a
  fixed array or a slice. As in the C, the length is read again at the top
  of every iteration and the elements where the path leads then; an
  iterated expression that is not a check-free path runs once.

The debug info gives a fixed array as `tyg_dt_array` (lldb: `(i64[5]) a =
([0] = 3, [1] = 1, ...)`) and a slice as a record named by its Goose type
with its `data` pointer and `len` (`(i64[:]) s = { data = 0x... len = 5 }`;
`parray 5 s.data` shows the elements). A debugger cannot show the elements
by themselves: that needs an array type whose count is the `len` member,
which tinygen lacks (gap 3).

Not yet: static-capacity limited arrays `T[..k]` (a length beside the slots,
built by copy from other arrays, §4.2), slices of variable-size elements,
slices of string literals and `u8[]` values, element-wise operators on
arrays, array comparisons, and the leading-check hoisting the C backend does
(an optimization only).

Coverage, measured over the samples and the `codegen/`, `typing/` and
`optimizer/` fixtures (291 programs; every one built with `goose build -O0`
and with `--backend=tinygen`, each with and without `--debug`, and all 582
pairs print the same and exit alike), counting the specializations the
backend compiles:

| Stage | Specializations through tinygen | `test/goose_in_goose/` |
|---|---|---|
| first (scalars, structs, control flow) | 542 of 3811 (14%) | 0 of 172 |
| second, so far (fixed arrays, slices) | 952 of 3811 (25%) | 1 of 172 |

(The first stage's own write-up counted 394 of 3331 over 284 programs, an
older sweep; both rows above come from the same one.) What still sends a
function to C, by count: a parameter or result of a type the backend does not
hold (mostly `u8[]` strings and resizable arrays, 870), a local of such a type
(357), multiple results (352), free variables of nested functions (329), a
global (179), calls through function values and dispatch (169), `return
from` (158), and calls to functions using data stacks (87). Goose-in-Goose
passes strings and resizable arrays nearly everywhere, which is stage 3.

## Testing

`test/tinygen/` holds fixtures the backend covers whole: scalars,
structs, extern calls with C of their own (a sidecar), a division by zero
that must abort with the Goose line, arrays and slices (ending in a failed
bounds check), and a slice whose bounds fail. They are ordinary fixtures, run by the
main suite through both C backends against `test/expected/`.
`test/tinygen_backend.py` (the "tinygen backend" section of `run_tests.py`)
builds them with `--backend=tinygen`, requires every function to go through
tinygen, builds the structs fixture three more times with chosen functions
left to C (`--tinygen-skip=a,b`, hidden) so that by-value and by-reference
structs cross the two halves in both directions, and the arrays fixture twice
more so that arrays and slices do, builds a few existing
fixtures and a sample that the stage covers in part, and compares every run
with the same expected output. Where `lldb` is installed it runs one program
under it and requires a breakpoint by Goose function name, Goose lines in the
frame and its caller, and a parameter and a local with Goose types; and one
more for a slice parameter's `data` and `len` and a fixed array's elements.

## What tinygen lacks for Goose

Recorded here rather than changed in tinygen, which other work is changing:

1. ~~No way to abandon a procedure.~~ tinygen now has `tyg_proc_abort`,
   which `Choose` uses; the throwaway module per candidate is gone.
2. **No in-memory loading.** `goose run` runs programs in-process; tinygen
   writes an object (`tyg_module_emit` gives its bytes) but has no loader
   that maps and relocates code and resolves symbols in the running process,
   as TinyCC's `tcc_relocate` does. Until it has, `goose run
   --backend=tinygen` would have to build an executable and start it.
3. **Debug types for Goose arrays.** A Goose slice is `{ data, len }` and a
   resizable array a header `{ base, len }` with its elements on a data
   stack; a debugger shows them well only with an array type whose count is
   another member (DWARF `DW_AT_count` as a location expression, CodeView's
   equivalent through natvis). tinygen's `tyg_dt_array` takes a constant
   count or none, so the backend describes a slice as a plain record of
   `data` and `len` (see the second stage); a `tyg_dt_array` whose count is
   a member of the enclosing record (or a location expression) would let
   lldb and gdb show its elements directly.
4. **The source language, and lldb's formatters.** tinygen now names the
   unit's language (`tyg_module_language`); the backend says C11, as the C
   half's unit does. Tried under lldb (Xcode's, macOS arm64) with C99, C11,
   C++, Rust, D and Pascal: no difference at all. lldb reads every one of
   them through clang's type system, which turns any 8-bit integer base type
   into a character type whatever its name or encoding, so an `i8` or `u8`
   still shows as `'\xfd'` although the backend gives them `DW_ATE_signed` /
   `DW_ATE_unsigned` (gdb goes by the encoding and prints numbers); and the
   `objc` formatter category, enabled for every language, matches Carbon's
   `Rect` and `Point` by name (`Point` prints as `(v=2, h=1)`, a `Rect`
   without `top`/`left` fields as a summary error). Neither is fixable in
   the DWARF tinygen can write: the way out is formatters shipped with the
   compiler (`type format add -f d i8 u8`, a `goose` category overriding
   those names), or a namespace for Goose's record types, which tinygen's
   debug types cannot express (no `DW_TAG_namespace`). Backtraces still
   print the linkage name (`fib_g`).
5. **Packed records on x86-64.** The records here are aligned to 1 with
   fields at packed offsets. On arm64 (checked here, by-value structs of 12,
   16 and 33 bytes across both halves) tinygen classifies them as clang does.
   On SysV x86-64, clang passes a struct with a misaligned field in memory;
   whether tinygen's classifier does the same for an align-1 record is
   unchecked, and the Windows x64 rules differ again. This needs a test on
   those targets before the backend is enabled there.
6. **Thread-locals on Mach-O.** The runtime keeps `gs_gl` and `gs_rf` in
   thread-local storage. Reaching them from tinygen code (a later stage:
   globals, `return from`) needs `TYG_SYM_TLS` access on every target;
   tinygen's recent work names ELF thread-locals, and Mach-O's TLV access
   should be checked.

## Plan

1. **This stage**: scalars, structs, references, control flow, calls in both
   directions, extern fns, `print`, debug info; per-function fallback; macOS
   arm64 (tinygen's host target is used, so Linux arm64/x64 should work as
   well, untested).
2. **Fixed arrays and slices**: done so far: `T[k]` values and fields,
   slices of fixed elements as parameters, results and values, indexing
   with bounds checks (`gs_idxfail`, honoring `Index::nobc`), slicing,
   `len`, `for` over arrays and slices, string literals as slices. Next, in
   order of what the coverage count says they unlock: multiple results
   (out-pointers, 352 functions), globals through an exported accessor for
   `gs_globals_t` (or `gs_globals_main` made external, 179), static-capacity
   `T[..k]`, fixed-mode enums and `match`, optional references and null
   tests, `block`/`break` with values, and `InlineBlock`, so that `-O1` and
   `-O2` builds stay in tinygen.
3. **Data stacks**: resizable and variable-size values, `gs_sp` and the stack
   accounting (`CodeGen` exposing `NoteStackCall`/`stackown` to the second
   backend), `push`/`append`/`str`/`format`, rendering composites through the
   C half's render functions (given external linkage), free variables of
   nested functions, function values, `return from`.
4. **Driver and platforms**: `goose run --backend=tinygen` (build and start,
   or an in-memory loader, see above); Linux and Windows (CodeView) in CI; the
   x86-64 ABI checks; lldb/gdb formatters for Goose arrays shipped with the
   compiler. `#line` in the C half for what remains in C.
5. **Whole program**: once nearly everything goes through tinygen, the C half
   shrinks to `main` and the runtime's inline helpers, which can then move
   into the runtime object behind exported names, and a debug build needs no
   C compiler for the program at all — which is where the build-time win is.
   Measure compile time against `cc -O0` then.

## Decisions

* Per-function fallback over whole-program: the C half exists anyway, so it
  is no less safe, and it lets the first stages cover real programs in part.
* `goose build` only: an in-process run needs a loader tinygen lacks.
* `-O0` by default with the backend: debug builds are its purpose; the
  backend does not handle inlined bodies yet.
* Shims for runtime.h's static helpers instead of reimplementing them: the
  checks' semantics and messages stay in one place, and follow `GS_DEBUG`.
* Trampolines for every extern fn: correct for static and macro C functions;
  a direct call for plain library symbols is an optimization for later.
* tinygen's host target only (`tyg_host_target`): cross builds are not part
  of `goose build` today.
