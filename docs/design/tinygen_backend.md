# The tinygen backend

`goose build --backend=tinygen` builds a debug build without compiling the
program's C. tinygen is a C99 library, checked out beside this repository as
`../tinygen`. It turns a small IR into machine code and writes an object file
with debug info. The backend lowers each Goose function to that IR.

    goose build --backend=tinygen prog.goose          # debug build, -O2
    goose build --backend=tinygen --debug prog.goose  # with GS_DEBUG's checks
    goose build --backend=tinygen -v prog.goose       # says what went to C, and why
    goose run --backend=tinygen prog.goose -- args    # builds, then runs it
    goose debug prog.goose -- args                    # builds, then starts lldb on it

## Why

The C backend's debug build has two problems.

* **The debugger shows C.** lldb steps through `main_g` and `t5 = fib_g(t4)`
  in a generated `.c` file, with C temporaries and C type names. `#line`
  directives would fix the line numbers, but not the names or the types.
* **The C compiler is the slow part of a build.** Every build parses and
  lowers the program's C, including the inline half of the runtime.

tinygen solves both. Its debug info says what the front end tells it, so a
function in the debugger has its Goose name, its `.goose` lines, and its
parameters and locals with Goose types. It also has no optimizer and writes
machine code in one pass.

## How it fits into the compiler

The backend reads what the C backend reads: the live specializations
(`FnSpec`) after typechecking, optimization and BCE. It reuses `CodeGen`'s
decisions instead of making its own:

* **The call interface** of each specialization (`CodeGen::sinfo`,
  `SigParams`): its C name, `gs_sp`, free variables and result channels.
* **The layouts** (`StructLayout`, `FixedSize`): Goose's fixed types are
  packed, so tinygen gets records aligned to 1.
* **The checker's annotations**: `exprtype`, `Call::spec`, `Index::nobc`, the
  loop shapes, and the rest.

So a tinygen function and a C function call each other through the platform's
C calling convention, as two C functions would.

`CodeGen` knows the backend through one interface, `CodeGen::SpecBackend`.
After the call interfaces exist, `Choose` builds every specialization it can.
A function that meets something unsupported throws `TgUnsupported`. tinygen
then drops it (`tyg_proc_abort`), and the C backend compiles it instead.
`-v` prints each such function and the first reason.

The code is `src/tinygen_backend.h`, compiled only with `GOOSE_HAVE_TINYGEN`:

| Part | Does |
|---|---|
| `TgModule` | The type-dependent parts: representations, layouts, debug types, signatures, shims and accessors. |
| `TgFn` | Builds one function: expressions, places, calls, data stacks, control flow, `return from`. |
| `TinyGenBackend` | Chooses the functions, decides between the two build paths, and writes `main`, the globals and the threads of a whole program. |

## The two build paths

**Whole program.** When tinygen compiles every live function, no C of the
program is compiled. The link takes three objects:

| Object | Holds |
|---|---|
| `<stem>.tg.o`, written by tinygen | every function, `main`, `gs_init_globals`, the globals, the `return from` channels, queues and worker thunks |
| the support object (`goose_tgsupport[_checks]-<hash>.o`) | the program-independent shims, the trampolines of `runtime_ext.h`'s static externs, the thread-locals `gs_stks`, `gs_gl` and `gs_rf`, and `gs_tg_start`/`gs_tg_end` |
| the runtime object (`goose_runtime.o`) | as in any build |

The support object depends only on the runtime (`TgSupportBody`,
`TinygenSupportSource` in `main.cpp`). The build compiles it once per runtime
version and configuration, and caches it beside the runtime object. It comes
in two variants: plain, and with `GS_DEBUG` for `--debug`.

**Extern unit.** With `--include`, a whole program also has an extern unit
(`TinygenExternSource`), which the build writes to `<stem>.tgx.c` in the
build directory and compiles with the program, and which a `goose run`
compiles as a third TinyCC unit. It holds the runtime's headers, the
program's C types as the C generator declares them (`CodeGen::types`), the
headers, which are written against those types, and a `gs_tg_x_` trampoline
for each extern fn the support object does not hold (`ExternTrampolines`).
The object then forwards no extern itself (`ForwardExterns`), so a header's
static or inline function is callable. The `-D` given go into the unit as
into the support object. `goose build -o x.c` with `--include` still writes
the C generator's C, since tinygen's C alone lacks the header's functions.

**`--standalone`.** A whole program built with `--standalone` links a runtime
object compiled from `RuntimeObjectSource` in its build directory, with the
`-D` given, instead of the one cached per C compiler. `goose build -o x.c
--standalone` still writes the C generator's single file: tinygen's C
declares the runtime's functions with tinygen's signatures (`void *` for the
runtime's typed pointers), which conflict with the runtime's own in one
translation unit, the reason `goose run` uses two TinyCC units.

**Mixed.** The C backend writes its C as usual, and the C compiler compiles it
at `-O0 -g`. That C holds `main`, the globals, any function tinygen did not
take, and the shims the object calls. `CodeGen` gives every function external
linkage, so the two objects resolve each other's calls.

A build takes the mixed path when a function or a global initializer falls
back to C. It also takes it for a library or an `export fn` that `ExportOk`
rejects, and for `--include` or `--standalone` with `-o x.c`. A `-D` goes into the support object's source, and
`GS_DEBUG` picks its variant. The stack
settings (`GS_STACK_RESERVE`, `GS_STACK_GAP`, `GS_STACK_BUDGET` and
`GS_MAX_STACKS`, so `--stack-reserve` too) keep a whole build: `main` passes
them to `gs_tg_start`, and the backend reads them from `CodeGen::stackcfg`.
`--tinygen-mixed` (hidden) forces the mixed path for comparisons. `-v` names
the path and the reason:

    tinygen: whole program: no C compiled for it
    tinygen: mixed: the C half compiled by the C compiler (export fns)

## Conventions shared with the C backend

**Runtime helpers.** The backend calls what the runtime object exports
(`GS_API`) directly. `runtime.h` defines checked arithmetic, division and the
`as` checks as `static` functions. The backend reaches those through exported
shims (`gs_tg_add_i32`, `gs_tg_div_i64`, `gs_tg_range`, ...). The shims are
compiled with the program's defines, so `--debug` means the same in both
halves. An abort passes the Goose file and line.

**Extern fns** (§7.10) go through a trampoline per extern (`gs_tg_x_<name>`),
because the C function may be `static` or a macro. A function the runtime
object exports needs no trampoline (`TgModule::RuntimeApi`).

**Parameters** follow `SigParams`. A slice is two parameters, its data and
length; a fat reference is its header and stack; a pool reference is its four
members (`CodeGen::ParamFields`, `TgModule::SplitParam`). Passing a slice as
one 16-byte record instead works under SysV and on arm64, which put it in the
same two registers, but not on Windows x64, which passes it by reference to a
copy.

**Results** follow `SigParams`. The first fixed result is the C return value.
Later fixed results are out-pointers. A resizable or `u8[]` result is built at
a destination stack the caller passes.

**Data stacks** (§6.2). A function that takes `gs_sp` owns `GS(gs_sp + k)` for
the indices it uses, and hands callees `gs_sp` plus the indices in use. The
backend reports its counts to the stack accounting (`CodeGen::stackown`,
`stackcalls`), so `--stacks` and the thread cap see no difference.

**Globals and the stack block.** Each function reads the stack block and the
addresses of its globals once, on entry. How it reads them depends on threads:

* *Without threads* (`TgModule::fixedaddrs`), each address is fixed at load.
  The function loads it from a data pointer (`gs_tg_stkpp`, `gs_tg_g<i>`).
* *With threads*, each thread has its own instance. The function calls an
  accessor (`gs_tg_stks`, `gs_tg_g<i>`) that reads the thread-local.

The C backend sets `CodeGen::usesthreads` only while it generates bodies. So
`UsesThreads` scans for thread and queue builtins before any body is built.
`Shims` stops the build if that scan missed a thread use.

**`return from`** (§7.9) works as `CodeGen::GenFromReturn` does. The return
site writes the target's channels, sets `gs_rf`, and returns. After every call
that can propagate, the caller checks `gs_rf`. One return can pass through
frames of both halves.

## What the backend holds

Every representation is the C backend's, so either half reads the other's
values:

* scalars, fixed structs, fixed arrays `T[k]`, slices, and plain references;
* `T[]` images and resizable arrays on data stacks, passed by fat reference;
* limited arrays `T[..k]`, variable-size structs, frame objects;
* fixed-mode and variable-mode ADTs, `match`, and case-function dispatch;
* optionals, relative references and pools;
* free variables of nested functions, and function values (spliced, as
  `CodeGen::EmitFvCall` does);
* `print`, `str` and `format` of composites, through `RenderInto`, or through
  a render procedure (`gs_tg_render_<T>`) for a recursive type;
* elementwise operators on arrays, threads and queues.

A few corners still fall back to C. `-v` names them. Examples are some
literal and fill kinds, unusual casts, and thread programs with free
variables.

Over the sweep, which covers the samples and the `codegen/`, `typing/` and
`optimizer/` fixtures, every function goes through tinygen: 3964 of 3964, in
295 programs, all of them whole.

### The main entry points in `TgFn`

* **`BuildInto(n, d, l)`** builds a value at a destination (`InPlace`): a
  `T[]` image, a resizable, a frame object, or a frame object's bytes.
  `CallAt` hands the destination to a call. `CopyFrom` copies an existing
  value. `BuildTemp` builds on a temporary stack of the statement.
* **`Ctl`** is the shared skeleton of `if`, `block`, `loop` and `match`
  values. `CtlBegin` tries to build in place. `CtlArm` builds each arm.
  `CtlTarget` makes a loop's or block's break target.
* **`FieldPlace`** finds a field in any representation. A variable-size
  struct uses a cursor past each variable-size field.
* **`RenderInto`** renders a value as `CodeGen::RenderLoc` does.

## Debug info

tinygen writes DWARF on macOS and Linux, and CodeView on Windows. Each
function is described under its Goose name, with its C name as the linkage
name. It has a line entry per statement and per loop head, and its parameters
and locals sit in lexical scopes.

* Scalars are typedefs named `i64`, `u8`, `f64`, `bool`.
* Structs, ADTs and variants are records with Goose field names. An ADT has
  an enum `tag` and a union `u` of its payloads.
* A slice is a record of `data` and `len`. A resizable array is a record of
  `base` and `len`.
* An optional is a typedef under its Goose name over a pointer.
* Free variables appear as the variables, through their addresses
  (`tyg_debug_var_ref`).
* In a whole program, each global is a symbol under its Goose name.

```
(lldb) b mixed_sum
Breakpoint 1: where = prog`mixed_sum_g + 40 at tinygen_structs.goose:27
(lldb) bt
  * frame #0: prog`mixed_sum_g(m=...) at tinygen_structs.goose:27
    frame #1: prog`main_g at tinygen_structs.goose:50
(lldb) frame variable r d
(Shape.Rect) r = (w = 3, h = 4)
(Dir) d = (tag = West)
```

### lldb formatters

DWARF cannot say that a slice's `data` holds `len` elements, and lldb reads
every language through clang's types, so `u8` shows as a character. Goose
ships lldb formatters in Python, `src/lldb/goose_lldb.py`, that show the
values as Goose does:

| Value | Without | With the formatters |
|---|---|---|
| `T[:]`, `T[]`, `T[>..]`, `T[>..]&`, `T[..k]` | `{ data, len }`, `{ len, data }`, `{ base, len }`, `{ hdr, stk }`, `{ len, e = all k slots }` | `len=3 { [0] = .. [2] = .. }`, the live elements only |
| text: the same arrays of `u8` | as above | `"hello"`, or its bytes where it is not UTF-8 |
| `i8`, `u8`, alone, in fields and arrays | `'\xfd'`, `"hi\0"` | `-3`, `([0] = 104, [1] = 105, [2] = 0)` |
| `T?` | `0x000000016fdfe600`, `0x0000000000000000` | what it refers to, or `null` |
| an ADT | `{ tag = Rect, u = { every payload } }` | `Shape.Rect(w = 3, h = 4)`, expanding to its fields |

```
(lldb) command script import /path/to/goose/src/lldb/goose_lldb.py
(lldb) frame variable
(i64[:]) s = len=3 {
  [0] = 3
  [1] = 1
  [2] = 4
}
(u8[] *) name = 0x0000007000000000 "hello"
(i64?) o = 9
(Shape) sh = Shape.Rect(w = 3, h = 4)
```

They match types by their Goose names (`[:]$`, `\?$`, and so on), so they
apply to every instantiation. An ADT is matched by its shape instead, a
first member `tag` whose enum is named `<ADT>.tag`. They live in the type
category `goose`, which `type category disable goose` turns off.

**Loading.** lldb does not load the script by itself. It could come from a
dSYM's `Contents/Resources/Python/<prog>.py`, but Apple's lldb defaults
`target.load-script-from-symbol-file` to `trusted`. It loads such a script
only from a dSYM with a trusted code signature, and for any other it only
prints the command that would load it. A build would then also pay for
`dsymutil` and a bundle beside the program, and the user would still have
to type the command or change the setting. So the build writes no dSYM.
`goose debug prog.goose -- args` builds as `goose run --backend=tinygen`
does, then starts lldb with the script imported and the program's arguments
set. With lldb started some other way, the user imports the script once per
session, or adds the import to `~/.lldbinit`. `goose build --backend=tinygen
-v` prints the line, with the script's path. The compiler looks for the script in `lldb/` beside its
executable, and then in the source tree it was built from.

A null optional of a struct shows as `NULL`. For a null pointer to a
record, lldb prints its own nil marker and does not ask the formatter.

## Building tinygen into goose

`CMakeLists.txt` takes a `TINYGEN_DIR` cache path, by default `../tinygen`.
When that path holds `src/tinygen.c` and `include/tinygen.h`, CMake compiles
tinygen as a static library and defines `GOOSE_HAVE_TINYGEN=1`. Without it,
the compiler has the C backend only, and `--backend=tinygen` says so.

`--backend=tinygen` inlines as the C backend does, `-O2` by default; `-O0`
keeps every Goose function a function in the debugger, and `goose debug`
defaults to it. The optimizer's `NK_InlineBlock` is built by `TgFn::GenInline`:
the argument bindings in the caller's scope, the body in a scope of its own,
and a `return` of the inlined function, like a `break` out of a `block`, a
store of its value and a jump to the block's end. A function whose native
frame, with the large fixed values of its inlined calls, passes 1 MiB is left
to C (the C backend keeps those on a data stack).

## `goose run`

Plain `goose run` (and `goose file.goose`) runs tinygen's C in TinyCC
(`docs/design/jit_backend.md`). The module is created with `TYG_OUTPUT_C`, as
for tinygen-c, and `CText` returns its text in memory with the appended
`main`. TinyCC compiles two translation units into one in-memory program:

* the support unit, `TinygenSupportSource(checks, true)`: the `-D` defines,
  `jit_c.h`, `runtime.h`, the packed prelude, `runtime_ext.h`, `TgSupportBody`
  (shims, trampolines, `gs_tg_start`) and `gs_jit_cleanup`. It names
  `gs_rt_start` without the runtime version, since the runtime is the one
  built into Goose. `GS_TG_TLS` is empty there because the JIT runs no
  threads;
* tinygen's C (`TinygenJitUnit`). Its prelude's `#include`s of the C headers
  give way to `jit_c.h` and TinyCC's `va_list`, because the JIT has no C
  headers. tinygen's prelude has TinyCC fallbacks of its own for traps, byte
  swaps, atomics and `asm` labels.

They are two units because tinygen's C declares the runtime's functions with
its own signatures, which conflict with `runtime.h`'s. Both bind the
runtime's `gs_*` functions through the `GS_JIT_SYMBOL` table, as the C
generator's program does. Exit status, arguments, resource root and abort
statuses come from the same hosted runtime (`test/jit_runtime.py`).

The run takes this path only if tinygen takes the program whole, by the
`goose build` rules: every function compiled and a plain `main`. Every `-D`
goes into the support unit, and an `--include` header into the extern unit. Two more classes take
the old path for now. The first is a program calling an extern of a shared
library: the C generator binds it through the runtime's loader, whereas
tinygen's C calls it by name, which only a link resolves. The second is a
run with `-DGS_MAX_STACKS`, because tinygen counts more data stacks than the
C generator for some programs (`append_destination_storage`). Otherwise the whole
program runs on the C generator's C, built by a second `CodeGen` without
tinygen; a mixed program never goes into one TinyCC unit. `-v` prints
`jit: tinygen's C` or `jit: the C generator's C ... (why)`. Inlining defaults
to `-O2`, as for the C backend, and `-O0` is selectable. `--backend=c`
asks for the C generator's path. With `--jit -o out.c`, the C generator's C still goes to
`out.c` and tinygen's C runs. Threads are refused on both paths.

tinygen's C is about 5 to 15 times slower than the C generator's under
TinyCC (samples 07, 09, 10, 21, 22). It reads and writes memory through
`memcpy`, which a C compiler inlines and TinyCC calls.

`goose run --backend=tinygen` builds the executable in the program's build
directory, as `goose build` would, and runs it as a child process. The
program shares Goose's standard streams, gets the arguments after `--`, and
its exit code becomes the run's. Build messages go to stderr.

A program finds its resources next to its executable. A run through TinyCC
uses the entry source's directory instead. To match it, Goose sets
`GOOSE_RESOURCE_DIR` to that directory, and `gs_os_resource_dir` prefers the
variable when it is set. Unlike a TinyCC run, this path also runs programs
with threads.

## Optimized builds: `--backend=tinygen-c`

`--backend=tinygen-c` builds as `--backend=tinygen` does, whole or mixed by
the same rules, but the module is created with `TYG_OUTPUT_C`. `Write` then
writes `<stem>.tg.c`, tinygen's C for the whole object, and the C compiler
builds it into `<stem>.tg.o` with the c backend's flags: `-O2`, or `-O0 -g`
with `--debug`. The link is the tinygen one. A mixed build's C half is
compiled with the same flags. Goose's own inlining is at `-O2` by default, as
for the C backend. `goose build --backend=tinygen-c -o out.c`
writes tinygen's C and builds nothing; a mixed program refuses it.

Two things of tinygen's C output are worked around here:

* C requires `main(int, char **)`, and a tinygen pointer prints as `void *`.
  So in C output `EmitMain` defines `gs_tg_main`, and `Write` appends a
  `main` that calls it.
* tinygen declares a base debug type by its name, so `_Bool` became
  `typedef _Bool _Bool;`. In C output the `bool` base type is named `bool`,
  which tinygen leaves to the C type.

## Defaults of `goose build`

With a compiler built with tinygen, `goose build` without `--backend` means
`--backend=tinygen-c` (the optimized build), and `goose build --debug` means
`--backend=tinygen` (the fast debug build, GS_DEBUG checks as before). `-o x.c`
selects tinygen-c, also with `--debug`. A program that tinygen cannot take
whole builds through the C generator's C, all of it, as the JIT of `goose run`
does: a library or `export fn` (`WholeProgram`), `--include` or `--standalone`
with `-o x.c`, or any
function left to C. A `-D` keeps the program on tinygen: it goes into the support
object's source (`DefinesSource`), which a define other than GS_DEBUG and the ones
main hands the runtime as it starts (GS_STACK_*, GS_MAX_STACKS) makes an object of
its own in the cache.
`-v` prints `build: backend tinygen-c (the default)` and, for a fallback,
`build: the C generator's C, compiled by the C compiler, for the whole program
(tinygen cannot take it whole: why)`. There is no mixed build unless
`--tinygen-mixed` or an explicit `--backend` asks for one. `-o x.c` of a
fallback program writes the C generator's C to `x.c`. `--backend=c` is the old
path; a compiler without tinygen only has it.

## Testing

* `test/tinygen/` holds fixtures for each area: scalars, structs, arrays,
  strings, text, ADTs, optionals, limited arrays, variable-size structs,
  `return from`, elementwise operators, and more. They are ordinary fixtures
  too, run through the C backends against `test/expected/`.
* `test/tinygen_backend.py` is the "tinygen backend" section of
  `run_tests.py`. It builds each fixture through tinygen and compares the
  output with the expected output. It builds many fixtures again with
  `--tinygen-skip=f,g`, which leaves chosen functions to C. Calls then cross
  the two halves in both directions.
* It checks that the `WHOLE_PROGRAM` fixtures compile no C, and that
  `--tinygen-mixed` gives the same output.
* It builds every program again with `--backend=tinygen-c`, the `--debug`
  ones at `-O0 -g`, and fails a program whose tinygen C does not compile
  or warns on a line of its own.
* Where lldb is installed, it checks breakpoints by Goose name, Goose lines,
  and parameters, locals and globals with Goose types. With the formatters
  imported, it checks arrays' elements, text, `i8` and `u8`, optionals,
  ADTs and limited arrays as Goose values.
* goose_in_goose built through tinygen compiles itself to C that is byte for
  byte the C backend's.
* On an arm64 Mac, an x86-64 compiler (`-DCMAKE_OSX_ARCHITECTURES=x86_64`)
  runs under Rosetta and passes `-arch x86_64` to cc. The Command Line Tools'
  `cc`, `git` and `lldb` are arm64 only. So `GOOSE_CC` must name a wrapper
  that runs cc natively (`arch -arm64 /usr/bin/cc "$@"`).
* On Linux x86-64 (GCC 15, lldb 21), the full suite passes, lldb checks
  included.
* On Windows x64 (MSVC 19.44, clang-cl and lldb 23), the full suite passes
  but for three lldb checks (see "What tinygen lacks"). That compiler was
  built without the audio, gfx, physics and ui packages, so their fixtures
  were skipped. lldb needs Python's `python3.dll` on `PATH` to start.

## Status

These figures come from macOS arm64, Apple clang 21, and a Release build of
the compiler, with a warm cache. Each is the median of five runs.

| `goose build` of goose_in_goose (172 functions) | ms |
|---|---|
| C backend, default (`cc -O2`) | 1557 |
| C backend, `--debug` (`cc -O0 -g`) | 474 |
| `--backend=tinygen` (whole program) | 101 |

Of tinygen's 101 ms, the compiler takes 76 ms (`--dry-run`) and the link
takes the rest.

Run time of `bench/goose` programs at medium size, in milliseconds:

| Program | tinygen | C `--debug` (`cc -O0`) | C default (`cc -O2`) |
|---|---|---|---|
| sum | 75 | 626 | 34 |
| push | 54 | 311 | 36 |
| calc | 186 | 694 | 108 |
| sexp | 595 | 2011 | 333 |
| lru | 336 | 945 | 136 |
| graph | 402 | 897 | 384 |
| scene | 106 | 1158 | 38 |
| blur | 92 | 2678 | 14 |
| particles | 274 | 797 | 141 |

tinygen beats `cc -O0` on every benchmark, by 2 to 29 times. It is 1.05 to 7
times slower than `cc -O2`, most of all in tight loops such as blur's.

### Windows x64

These come from an Intel Core i7-8700, MSVC 19.44 and clang-cl 23, and a
Release build of the compiler made by MSVC. Builds are the median of five
runs, run times the best of three. Every build printed the same output, and
tinygen compiled every program whole.

| `goose build` of goose_in_goose | ms |
|---|---|
| C backend, default, MSVC (`/O2`) | 4360 |
| C backend, default, clang-cl (`-O2`) | 4219 |
| C backend, `--debug`, MSVC | 778 |
| C backend, `--debug`, clang-cl | 1267 |
| `--backend=tinygen`, linked by MSVC | 492 |
| `--backend=tinygen`, linked by clang-cl | 569 |

The compiler takes about 400 ms of tinygen's build (`--dry-run`), the same
with either backend: on this machine the front end, not tinygen, is most of
it.

| Program | tinygen | MSVC `--debug` | clang-cl `--debug` | MSVC `/O2` | clang-cl `-O2` |
|---|---|---|---|---|---|
| sum | 434 | 923 | 912 | 72 | 85 |
| push | 475 | 630 | 523 | 124 | 77 |
| calc | 553 | 1091 | 1149 | 208 | 188 |
| sexp | 1606 | 3261 | 3282 | 702 | 599 |
| lru | 1306 | 1914 | 1892 | 888 | 851 |
| graph | 1269 | 1459 | 1476 | 766 | 905 |
| scene | 425 | 2923 | 2956 | 128 | 121 |
| blur | 408 | 4205 | 5808 | 212 | 35 |
| particles | 712 | 1675 | 1607 | 519 | 458 |

tinygen beats both compilers' debug builds on every benchmark, by 1.1 to 14
times. It is 1.4 to 12 times slower than their optimized builds. The gap is
wider than in the macOS figures, which come from another machine. Nothing
in it is particular to Windows: sum's inner loop calls nothing and touches
no thread-local, but reloads the array's base and length from the stack
on every iteration.

## What tinygen lacks

1. **In-memory loading.** tinygen cannot map and relocate its own code in
   the running process. So `goose run --backend=tinygen` builds and links
   an executable, which costs a link step that a TinyCC run does not have.
2. **Counted arrays in debug info.** A slice's or a resizable's elements need
   an array type whose count is another member. A field after a
   variable-size field needs a member location expression. tinygen has
   neither, so the DWARF has `data` and `len` separately. lldb shows the
   elements only through the formatters (see "lldb formatters"). Another
   debugger, or lldb without them, shows the records.
3. **Goose values in the debugger without a script.** lldb reads every
   language through clang's types, so without the formatters `i8` and `u8`
   show as characters and an ADT as all its payloads. lldb does not load
   the formatters by itself (see "Loading").
4. **Register variables lldb can read in CodeView.** On Windows, lldb cannot
   show an `f64` local that tinygen keeps in an XMM register ("unable to
   convert register kind=4 reg_num=110"). A one-byte value in a register
   shows wrong: a `Dir` parameter holding `West` in `rbx` shows as `North`.
   An optional shows under its pointer type (`Link *`), not its Goose name
   (`Link?`). The values are right; three of the lldb checks fail on
   Windows for this. (Packed records are fine there: the `tinygen_abi`
   fixture passes whole and mixed, as on arm64 and SysV x86-64.)
5. **Direct thread-local access on Mach-O.** Programs with threads read
   their stack block and globals through accessor calls.
6. **Debug info for a member of a symbol.** In a mixed build, the C half
   holds the globals inside one struct. lldb then shows a global as
   `gs_globals_main.counter_g`.
7. **A multiway branch.** A `match` is a chain of compares, where the C
   compiler makes a jump table.

## Next steps

* Faster loop code, which is where the gap to `cc -O2` is.
* Windows: CodeView register numbers lldb understands (see "What tinygen
  lacks").
* Linux and macOS: check the split slice and fat reference parameters
  (`docs/todo.md`).
* An in-memory `goose run --backend=tinygen`, once tinygen can load code
  in memory, if its faster start is worth losing the debugger.
* Direct globals for programs with threads, through a context pointer passed
  beside `gs_sp`.

## Decisions

* **Fallback is per function.** The C file is generated anyway, so a
  per-function fallback is as safe as a per-program one. It let coverage
  grow one construct at a time.
* **The C backend's conventions stay.** Mixed builds depend on them. A
  measurement found that a convention of our own would gain under 1% on
  every benchmark.
* **`goose run` builds an executable.** It reuses the build path whole, and
  a debugger can attach to it. In-memory loading would skip the link, but
  a debugger could not see the code.
