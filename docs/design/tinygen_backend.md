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
it leaves the set. Both halves pass `gs_sp` as two C functions do (see
Data stacks), so what one half takes does not depend on the other, and
each specialization is built once. `-v` lists what stayed in C and why
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

**Data stacks** (§6.2) are used as the C backend uses them: a tinygen
function that takes `gs_sp` owns `GS(gs_sp + k)` for the indices it counts
up and down as its locals and temporaries take them, and hands a callee in
either half `gs_sp` plus the indices in use. Its stack block is the C half's
thread-local `gs_stks`, which an exported shim (`gs_tg_stks`) returns once
per invocation (gap 6). The stack accounting (`codegen_stacks.h`) counts a
tinygen function as it counts a C one: the backend gives `CodeGen::stackown`
the most indices the body holds at once and `CodeGen::stackcalls` every
call it hands an index to, so `BoundStacks`, `--stacks` and the thread cap
see no difference. See the third stage.

**Packages** need nothing new: their Goose functions are specializations like
any other, their C parts are linked as before, and their extern fns go
through the trampolines.

**Globals** stay in `gs_globals_t` in the C half, initialized by
`gs_init_globals` before `main` as in any build (an initializer calling a
tinygen function calls it from there). tinygen code reaches one through an
accessor the C half exports (`void *gs_tg_g0(void) { return &GS_GL->x_g; }`),
so a worker thread reads its own instance through the thread-local `gs_gl`
without tinygen touching thread-local storage (gap 6). See the second stage.

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
below (no free variables, no `return from`, no pool
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

**Multiple results** (§7.1): the C convention is `CodeGen::SigParams`'s. A
result the backend holds is fixed and not large, so `CollectSpecs` makes
the first one the C return value (`cret == 0`), and every later one is an
out-pointer `T *gs_r<i>` after the declared parameters (and before `gs_sp`).
A call passes the addresses of temporaries; `let a, b = f()` declares the
locals and stores each result into its own, adapted to its type (`Fit`);
`return a, b` evaluates the results in order, writing each later one
through its out-pointer as it is made, as the C does; `return f()` forwards
every result of the call, adapting each. An ordinary value use of such a
call takes the first, a call statement discards them all. Extern fns with
more than one result stay in C.

**Globals**: a fixed global of a type the backend holds (not a pool's,
not `reusable`) is read and written in place. The accessors a body needs
are called once per invocation, in a block the entry block jumps to and
that is filled once the body is done (a body runs on one thread, whose
instance does not change under it); each address is kept in a local. A
`let` of a const type with a compile-time initializer, which the C makes
`static const` data, is static data in the object too: the backend lays out
the bytes of what `StaticInitX` would spell (`tyg_data`, read-only, no
pointers since such initializers hold none, so no `tyg_data_reloc`), reads
it where it lies, and describes it with `tyg_debug_global` under its Goose
name and type (`target variable PRIMES` gives `(i32[6]) PRIMES = ([0] = 2,
...)`). The other globals are described only by the C half, as members of
`gs_globals_main` under their C names (gap 7).

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
| second, so far (+ multiple results, globals) | 1099 of 3811 (29%) | 1 of 172 |
| third, so far (strings and resizable arrays handed in) | 1241 of 3811 (33%) | 53 of 172 |
| third, so far (+ resizable arrays built: locals, results) | 1471 of 3811 (39%) | 92 of 172 |
| third, so far (+ u8[] values, str/format, string equality, shrinking builtins) | 1674 of 3811 (44%) | 167 of 172 |
| third, so far (+ free variables) | 1838 of 3811 (48%) | 172 of 172 |
| third, so far (+ function values) | 2058 of 3811 (54%) | 172 of 172 |
| third, so far (+ globals on dedicated stacks, whole resizables assigned) | 2146 of 3811 (56%) | 172 of 172 |

(The first stage's own write-up counted 394 of 3331 over 284 programs, an
older sweep; the rows above come from the same one.) The step that added
multiple results and globals took fewer functions than the counts promised:
the "multiple results" reason (352) also counted every function whose single
result the C does not return as its value (a `u8[]` string, a resizable
array), and most globals named are strings, resizable arrays or SIMD
vectors. What still sends a function to C, by count (the check order now
reports a result's type before its position): a parameter of a type the
backend does not hold (mostly `u8[]` strings and resizable arrays, 748), a
local of such a type (371), a result of such a type (414), free variables of
nested functions (358 with the calls to such functions), calls through
function values and dispatch (169), `return from` (167), builtins other
than `print` and `len` (108), calls to functions using data stacks (95), and
globals of the types above (51). Goose-in-Goose passes strings and resizable
arrays nearly everywhere, which is stage 3.

## What the third stage adds so far: strings and resizable arrays handed in

A function that receives a string or a resizable array, reads it, and
grows it, compiles through tinygen; one that builds one of its own (a
local, a result, `str`, a literal) stays in C. The representations are the
C backend's (§6.1, C.3), so either half calls the other:

* **types**: a variable array `T[]` / `T[uN]` of fixed elements (a `u8[]`
  string), an image of its length prefix and its elements that a
  `uint8_t *` points at; a resizable `T[>..]` / `T[>..<]` of fixed
  elements, a `gs_rhdr { base, len }` with the elements on a data stack,
  ending at its top. Never as a local or a result: behind a reference, or
  as a by-value parameter. A plain reference to a `T[]` is that pointer; to
  a resizable, a fat reference `gs_rref { hdr, stk }`, which tinygen is
  given as a record of two pointers (C passes its members as two
  parameters, `ParamFields`; the same registers on arm64 and SysV x86-64).
  A by-value `T[]` parameter is the address of the caller's copy; a
  by-value `T[>..]` its header (as a slice's record), then its
  `gs_stack *`, as `SigParams` spells them. Such a function takes `gs_sp`,
  which it does not use; calling one, whose argument must be copied onto a
  stack, stays in C.
* **reading**: `len`, indexing (with the bounds check), slicing, `for`
  over them (by value, by reference, with an index), a view as a slice
  argument (`const u8[:]`), and `print` of any `u8` array or slice as its
  bytes (`gs_out_bytes`), as the C prints one (`SimpleText`).
* **passing on**: a fat reference to another function (an existing one, or
  `&v` of a by-value parameter, its header's address and its stack), a
  `u8[]&` as its pointer.
* **growing**: `v.push(x)` of a fixed element through a fat reference or
  on a by-value parameter: the receiver, then the element (which may grow
  the same array), then the slot at the stack's top, the top and the count
  raised (`EmitPush`); its value is the element's address. No check, as in
  the C: the stack's reservation guards it.

Debug info names them by their Goose types as the C half's structs: a
`T[]` as a record `u8[]` of its `len` (the prefix's type) and `data` (an
array of unknown size), a by-value one as a pointer to it; a resizable as
`i64[>..]` with `base` and `len` (`gs_rhdr`); a fat reference as `i64[>..]&`
with `hdr` and `stk` (a pointer to a `gs_stack` record with its `top`):

```
(lldb) p *s
(u8[])  (len = 5, data = u8[] @ 0x000000769cf7ce84)
(lldb) frame variable v
(i64[>..]&) v = { hdr = 0x000000016fdfe830  stk = 0x0000000100610fa0 }
(lldb) parray 3 v.hdr->base
```

Built so (`goose build --backend=tinygen`, plain and `--debug`), the
Goose-in-Goose compiler compiles itself to the same C, byte for byte, as
the C backend's build of it.

## What the third stage adds next: resizable arrays built

A function builds resizable arrays of fixed elements of its own, and
returns them, with the C backend's conventions, so the halves still call
each other either way:

* **own stacks**: a function with `gs_sp` keeps it in a local and reads the
  thread's stack block once per invocation (beside the globals' accessors,
  in the block the entry jumps to). A resizable local (`var v: i64[>..] =
  ...`, `u8[>..]`, `T[>..<]`) takes the next free index for its block
  (`AllocStk(forlocal)`), and a temporary -- a call's resizable result, a
  by-value argument's copy -- one for its statement, each with the top it
  found saved. Every scope gives its stacks back (top restored) where it
  ends, and a `break`, `continue` or `return` gives back every scope it
  leaves; the right operand of `&&`/`||`, a value arm of an `if` and a
  `while` condition have scopes of their own, since they may not run. A
  local's index is taken before its initializer runs, so what that takes
  lies above it.
* **building**: a local built in place on its own stack, from `[]`, a
  literal (element by element), a call (handed the local's stack as its
  destination), a string literal, or a copy of any array or slice
  (`copy(x)` included); `push` to it; `append` of a call's resizable
  result (built straight at the receiver's top, its count added), of a
  string literal, and of an array, slice or literal (evaluated whole, then
  copied, as in the C).
* **results**: every result has its `SigParams` channel -- the first fixed
  one the C return value (`cret`, not necessarily result 0), other fixed
  ones out-pointers, and a resizable one `gs_stack *gs_dst<i>, int64_t
  *gs_rl<i>`. `return e` builds a resizable result at `gs_dst<i>` (a call
  is handed it; a local's elements are copied, which the C avoids when it
  builds a named result there, an optimization only), writes the count,
  restores the body's stacks and returns. A call passes a temporary's stack
  (or a local's, or the destination it is building at) and the count's
  address; `let a, n = f()` copies a resizable result onto the local's own
  stack.
* **calls**: any callee taking `gs_sp`, in either half, is handed `gs_sp`
  plus the indices in use and recorded for the accounting. A call into the
  recursive cycle the function is in must be made with none in use (§7.8;
  the checker ensures it for the C backend's allocation, which this one
  matches); where it would not be, the function stays in C. A by-value
  resizable argument is copied onto a temporary's stack and passed as its
  header and stack.

What still sends a function to C, by count over the sweep: a parameter of
a type the backend does not hold (467: slices of variable-size elements
such as `Employee[:]`, frame objects such as `Node&`, ADTs, optionals,
pools), free variables (329, and 42 calls), a result type (264: a `u8[]`
string 51, optionals, limited arrays `u8[..8]`, ADTs), builtins (242:
`format` 61, `resize` 40, `str` 22, `default` 22, `pop` 17, `assert` 16,
`abort` 14), a local of a type not held (218: frame objects, limited
arrays, ADTs, `u8[]`), calls through function values and dispatch (183),
`return from` (158), globals (154: mostly strings and resizable arrays),
and operators on types not held (84). Next: `u8[]` values (a prefixed
image on a stack: locals, results, `str`), `format` and `print` of
composites through the C half's render functions, `resize`/`pop`/`clear`,
globals of resizable types (their dedicated stacks), and `default`/
`assert`/`abort`.

## What the third stage adds next: u8[] values, text, shrinking

* **u8[] values** (any `T[]` / `T[uN]` of fixed elements): a local is
  its image's address, the image on a stack of its own (`AllocStk`,
  given back where its scope ends), built from a string literal, a
  slice or any other array (`copy(x)` too), `str(...)`, or a call
  returning one, which is handed the local's stack as its destination.
  A result has the channel `SigParams` gives it, `gs_stack *gs_dst<i>`
  alone: the callee builds the whole image, prefix and elements, at that
  stack's top and leaves the top after it (the C half's own
  element-run twin, `_er`, stays a C function). Anything else is built
  as a resizable's elements behind a reserved prefix, which is then
  patched with the count, checked against the prefix's range with the
  C's `gs_panic` message. A by-value `T[]` argument is an image copied
  onto a statement's stack and passed by its address; a `T[]` where a
  `T[]&` is wanted, its address. Not yet: assigning a new value to a
  `T[]` variable (the C moves the image down its stack).
* **text**: `str(a, b, ...)` and `format(out, a, b, ...)` of scalars
  and of u8 arrays and slices, each argument evaluated and written in
  turn at the builder's top: a scalar through the runtime's exported
  `gs_fmt_i64`/`_u64`/`_f64`/`_f32`/`_bool`, which return the byte
  count, bytes copied. A `str` the checker typed as the slice it is
  passed as is a view of a builder on a statement's stack, as in the C.
  Composite arguments, which the C half renders structurally (its
  render functions), and format overloads still send a function to C.
* **equality** of arrays and slices of integer or bool elements: the
  lengths, then `memcmp` of the bytes (the C's `gs_memeq`).
* **shrinking**: `pop`, `pop_n`, `clear`, and `resize(n)` /
  `resize(n, fill)` of a resizable through a reference or on a local,
  with the C's checks and order: the receiver, the arguments, the
  checks; the top moves with the count, new slots filled through a
  cursor and the top written once. `assert`, `abort(msg)` and `exit`.
  The aborts go through shims naming the C half's codes
  (`gs_tg_abort_POP`, ...).

Coverage after this step is the last table row (0 of 582 runs differ).
What still sends a function to C, by count over the sweep: free variables
(329), calls through function values and dispatch (195), globals (167:
mostly strings and resizable arrays, which live on dedicated stacks),
`return from` (158), operators on types not held (56: optionals, enums,
structs), `print` of composites (33) and format overloads (13), slices of
variable-size elements (`Employee[:]` 32, `City[:]` 24), expression kinds
`match` and `block` (31, 17), `default` (29), ADTs and optionals as
parameters and results, frame objects (`Node&`), limited arrays
(`u8[..8]`). In Goose-in-Goose only five functions remain, all for free
variables of nested functions. Next: globals of resizable and `u8[]`
types (their dedicated stacks), composites rendered through the C half's
render functions given external linkage, `default`, then free variables.

## What the third stage adds next: free variables, function values, globals on stacks

* **free variables** of nested functions (§7.5), in the C's order
  (`SigParams`): after the declared parameters, each as the address of
  the variable in the frame it lives in -- a fixed one's, a `u8[]`'s
  image (`uint8_t *`), a resizable's header then its stack. The nested
  function reads and writes through them as through a by-value `T[]`
  parameter; a call passes what `Call::FreeVarArg` names (a local of
  the caller, or a free variable it was handed itself), so a free
  variable is passed down a chain of nested functions and either half
  calls the other. Pools and frame objects are not held.
* **function values** (§7.6): a call of one splices its checked body
  (`Call::fvbody`) in, as `EmitFvCall` does: the parameters bound in
  order as locals in a scope of their own (so a resizable one's stack
  is given back), the body's value copied, a fixed one. Dispatched
  calls (`Call::dispatch`, over ADTs) stay in C (`call kind: dispatch`).
* **globals of resizable and `u8[]` types**, which live on dedicated
  stacks in the C half's instance (`CodeGen::gstks`): the accessor gives
  the header's address, or the address of the member holding the
  image's, and a second one (`gs_tg_gs<i>`) the stack; both are read
  once per invocation like the others. Read, indexed, iterated, pushed,
  appended, popped, passed on, written in place.
* **assigning a whole resizable** (a local, a global, one behind a
  reference), as `Assign::CgStmt` does: the stack's top back to where
  the elements start, the new ones built there. Not yet: assigning a
  new image to a `T[]` variable.

Free variables are described to the debugger as the C has them, as
pointers under the variables' names (`p *n`); describing the variable
itself needs a location through a pointer, which tinygen lacks (gap 8).

Coverage after this step is the last table row (0 of 582 runs differ).
Goose-in-Goose now goes through tinygen whole: **172 of 172** functions,
and so built, plain and `--debug`, it still compiles itself to the same
C, byte for byte, as the C backend's build. What still sends a function
to C, by count over the sweep: `return from` (197), operators on types
not held (63), free variables of types not held (63: resizables of
variable-size elements such as `Node[>..]`, optionals, ADT variants),
globals of types not held (59: SIMD vectors, ADTs, limited arrays),
`print` of composites (40), slices of variable-size elements
(`Employee[:]` 32, `City[:]` 24), expression kinds `match` and `block`
(31, 17), `default` (29), reference rebinding `.=` (23), and ADTs,
optionals, frame objects and limited arrays as parameters and results.
Dispatched calls are down to 2. Next: `return from` (its channel in the
C half's thread-local `gs_rf`, gap 6), composites rendered through the
C half's render functions, `default`, `match` over fixed-mode enums.

## Testing

`test/tinygen/` holds fixtures the backend covers whole: scalars,
structs, extern calls with C of their own (a sidecar), a division by zero
that must abort with the Goose line, arrays and slices (ending in a failed
bounds check), a slice whose bounds fail, multiple results, globals
(mutable ones, static data, and one whose initializer calls a function),
strings and resizable arrays handed to functions (whose `main`, which
builds them as `u8[]` values, stays in C), and resizable arrays built,
pushed and appended to, returned (alone and beside a fixed result) and
given back on every way out of their scopes, u8[] values built in place
and returned with `str`, `format` and string comparisons, and arrays
popped, resized and cleared (ending in a pop of an empty array),
nested functions reading and writing their free variables of every
kind held, function values of every parameter kind, and globals on
dedicated stacks, read, grown and assigned anew. They
are ordinary fixtures, run by the
main suite through both C backends against `test/expected/`.
`test/tinygen_backend.py` (the "tinygen backend" section of `run_tests.py`)
builds them with `--backend=tinygen`, requires every function to go through
tinygen, builds the structs fixture three more times with chosen functions
left to C (`--tinygen-skip=a,b`, hidden) so that by-value and by-reference
structs cross the two halves in both directions, and the arrays, results,
globals, strings, built-arrays, text, shrink, nested, function-value
and global-stack fixtures twice more so that arrays and slices,
out-pointers, globals, fat references, destination stacks, `gs_sp`,
free variables and dedicated stacks do, builds a few existing
fixtures and a sample that the stage covers in part, and compares every run
with the same expected output. Where `lldb` is installed it runs one program
under it and requires a breakpoint by Goose function name, Goose lines in the
frame and its caller, and a parameter and a local with Goose types; and one
more for a slice parameter's `data` and `len` and a fixed array's elements,
one for a static global by its Goose name, one for a `u8[]` image and
a resizable array's fat reference, header and elements, and one for a
resizable local and a `u8[]` local built in a tinygen function.

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
   lldb and gdb show its elements directly. The same holds for a
   resizable array's header (`base` and `len`) and a `u8[]` image, whose
   `data` is an array of unknown size, so a debugger shows only its
   address. A `u8[]` local is a pointer to that record; lldb shows
   `frame variable *s` as `(len = 3, data = ...)`, but once it has
   shown the pointer itself it reports the record behind it as an
   invalid aggregate (an lldb quirk with a record whose last member has
   no size; a counted array would avoid it too).
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
6. **Thread-locals on Mach-O.** The runtime keeps `gs_gl`, `gs_stks` and
   `gs_rf` in thread-local storage. Globals and data stacks sidestep it (the
   C half's accessors read `gs_gl` and `gs_stks`, once per invocation that
   needs them); reaching `gs_rf` from tinygen code for `return from` would
   need `TYG_SYM_TLS` access on every target, or accessors of its own;
   tinygen's recent work names ELF thread-locals, and Mach-O's TLV access
   should be checked.

7. **Debug info for a member of a data symbol.** `tyg_debug_global` takes a
   symbol, and the variable is at its start. A global that lives in the C
   half's instance block (`gs_globals_main`, or a worker's through
   `gs_gl`) is at an offset in a struct, and with threads at a thread-local
   address: describing it under its Goose name needs a symbol and offset
   (or a DWARF location expression: the TLS block, a load, an offset),
   which tinygen's debug calls cannot express. Until then lldb shows such
   a global as `gs_globals_main.counter_g`.
8. **Debug info for a variable behind a pointer.** A nested function
   reaches its free variables through addresses it is handed (§7.5), and
   `tyg_debug_var`/`tyg_debug_param` take an address that holds the
   value itself (a local, a symbol, a parameter's copy). Describing the
   captured variable under its name needs a location that loads a
   pointer first (DWARF `DW_OP_fbreg`, `DW_OP_deref`; CodeView's
   `S_REGREL32` with an indirection), so lldb shows `n` as a pointer
   and `*n` as the variable, as for the C half's `int64_t *n_g`.

## Plan

1. **This stage**: scalars, structs, references, control flow, calls in both
   directions, extern fns, `print`, debug info; per-function fallback; macOS
   arm64 (tinygen's host target is used, so Linux arm64/x64 should work as
   well, untested).
2. **Fixed arrays and slices**: done so far: `T[k]` values and fields,
   slices of fixed elements as parameters, results and values, indexing
   with bounds checks (`gs_idxfail`, honoring `Index::nobc`), slicing,
   `len`, `for` over arrays and slices, string literals as slices;
   multiple results (out-pointers); globals (C accessors, static data).
   Next: static-capacity `T[..k]`, fixed-mode enums and `match`, optional references and null
   tests, `block`/`break` with values, and `InlineBlock`, so that `-O1` and
   `-O2` builds stay in tinygen.
3. **Data stacks**: done so far: strings and resizable arrays of fixed
   elements as reference and by-value parameters, read, passed on and
   pushed to; `gs_sp`, own stacks and the stack accounting; resizable
   locals and results built in place, `append`, by-value resizable
   arguments; `u8[]` locals and results, `str`/`format` of scalars and
   strings, string equality, `resize`/`pop`/`pop_n`/`clear`; free
   variables of nested functions, function values, globals of resizable
   and `u8[]` types, assigning whole resizables (Goose-in-Goose whole).
   Next: `return from`, rendering composites through the C half's render
   functions (given external linkage), `default`, assigning `u8[]`
   images.
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
