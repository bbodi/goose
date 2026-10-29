# The backend

Goose compiles every program through tinygen. tinygen is a C99 library,
checked out beside this repository as `../tinygen`. It turns a small IR into
machine code and writes an object file with debug info, or prints the same
IR as C (`TYG_OUTPUT_C`). The backend, `src/tinygen_backend.h`, lowers each
Goose function to that IR.

    goose build prog.goose                            # tinygen's C, built by cc -O2
    goose build --debug prog.goose                    # tinygen's machine code, with GS_DEBUG's checks
    goose build -v prog.goose                         # says which path it took
    goose run prog.goose -- args                      # tinygen's C in TinyCC, in this process
    goose run --backend=tinygen prog.goose -- args    # builds the debug executable, then runs it
    goose debug prog.goose -- args                    # builds it, then starts lldb on it
    goose -o prog.c prog.goose                        # tinygen's C as one file, for a C host

A construct the backend does not compile yet is a compile error that names
it and where it is:

    tinygen backend: <construct> is not supported yet (prog.goose:12)

## Why tinygen

* **The debugger shows Goose.** tinygen's debug info says what the front end
  tells it, so a function in the debugger has its Goose name, its `.goose`
  lines, and its parameters and locals with Goose types.
* **A debug build compiles no C.** tinygen has no optimizer and writes
  machine code in one pass; the link is most of a debug build.
* **One lowering for every output.** The same IR becomes the debug object,
  the optimized build's C (which the C compiler optimizes), the C TinyCC runs
  for `goose run`, and the C file a C host builds.

## How it fits into the compiler

The backend reads the live specializations (`FnSpec`) after typechecking,
optimization and BCE, with the analyses and type services of `CodeGen`
(`src/codegen.h`, `codegen_types.h`, `codegen_program.h`,
`codegen_stacks.h`):

* **The call interface** of each specialization (`CodeGen::sinfo`): its C
  name, whether it takes `gs_sp`, its free variables and `return from`
  targets (`CollectSpecs`).
* **The layouts** (`FixedSize`, `FieldRuns`, `LayoutFields`, `TagStore`):
  Goose's fixed types are packed, so tinygen gets records aligned to 1.
* **The globals' names and static initializers** (`NameGlobals`,
  `StaticInitX`), and the globals a thread program copies (`ThreadGlobals`).
* **The C spelling of the types** (`CT`, `Mangle`), which the extern unit, an
  `--include` header and the export header (`ExportHeader`) are written
  against.
* **The data stack accounting** (`codegen_stacks.h`): the backend reports
  what each body owns and hands its callees (`stackown`, `stackcalls`), and
  `CodeGen::Finish` turns that into the counts `main` starts the runtime
  with, checked against `GS_MAX_STACKS`.
* **The checker's annotations**: `exprtype`, `Call::spec`, `Index::nobc`, and the rest.

The driver (`main.cpp`) constructs `CodeGen` (the analyses), lets
`TinyGenBackend::Choose` build every live specialization, calls
`CodeGen::Finish`, then `WholeProgram`, which adds the globals' storage and
initializers, the exports and the worker thunks, and `EmitMain`. A construct
the backend meets and does not compile throws `TgUnsupported` from
`TgFn::No`; `Attempt` turns it into the compile error. A construct the type
checker rules out is an internal error instead (`TgFn::Unreachable`).

| Part | Does |
|---|---|
| `TgModule` | The type-dependent parts: representations, layouts, debug types, signatures, shims and accessors. |
| `TgFn` | Builds one function: expressions, places, calls, data stacks, control flow, `return from`. |
| `TinyGenBackend` | Builds every function, writes `main`, the globals, the exports and the threads, and the C units' headers. |

## What a build links

**Native (`--backend=tinygen`, `goose build --debug`).** No C of the
program is compiled. The link takes three objects:

| Object | Holds |
|---|---|
| `<stem>.tg.o`, written by tinygen | every function, `main`, `gs_init_globals`, the globals, the `return from` channels, queues and worker thunks |
| the support object (`goose_tgsupport[_checks]-<hash>.o`) | the program-independent shims, the trampolines of `runtime_ext.h`'s static externs, the thread-locals `gs_stks`, `gs_gl` and `gs_rf`, and `gs_tg_start`/`gs_tg_end` |
| the runtime object (`goose_runtime.o`) | as in any build |

The support object depends only on the runtime (`TgSupportBody`,
`TinygenSupportSource` in `main.cpp`) and the `-D` given. The build compiles
it once per runtime version and configuration, and caches it beside the
runtime object, plain or with `GS_DEBUG` for `--debug`.

With `--include`, a native program also has an extern unit
(`TinygenExternSource`), which the build writes to `<stem>.tgx.c` in the
build directory and compiles with the program. It holds the runtime's
headers, the program's C types (`CodeGen::types`), the headers, which are
written against those types, and a `gs_tg_x_` trampoline for each extern fn
the support object does not hold (`ExternTrampolines`). The object then
forwards no extern itself (`ForwardExterns`), so a header's static or inline
function is callable.

With `--standalone`, a native program links a runtime object compiled from
`RuntimeObjectSource` in its build directory, with the `-D` given, instead of
the one cached per C compiler.

**One C unit (tinygen-c, `goose run`, every `-o x.c`).** tinygen's C of a
program is one translation unit. goose hands tinygen the header text through
`tyg_c_header`, which tinygen prints after its prelude (`TinygenUnitHeader`),
and writes defines before and the JIT's cleanup after it (`TinygenUnit`):

| Part | Holds |
|---|---|
| goose's prefix | `_DEFAULT_SOURCE`, the `-D` defines, `GS_SEPARATE_RUNTIME` and `GS_RUNTIME_VERSION` (or, `--standalone`, `GS_NEED_THREADS` for a program with threads), in a run `jit_c.h` and TinyCC's `va_list`, and an `#error` for a `GS_DEBUG` the file was not written for |
| tinygen's prelude | its macros and C headers (a run drops the `#include`s) |
| the header | `runtime.h` (with `--standalone` all of the runtime), with `--include` the program's C types (`CodeGen::types`), `runtime_ext.h`, `TgSupportBody`, the `--include` headers and `ExternTrampolines` |
| tinygen's C | the program's types, data and procedures, with `#line` naming the Goose source, then `main` or a library's `goose_init` |

Every symbol the header declares that tinygen's C names is
`TYG_SYM_DECLARED` (`DeclareHeaderSymbols`): the runtime's `GS_API`
functions, the support body's shims and `gs_tg_start`, and the `gs_tg_x_`
trampolines. tinygen then prints no prototype of its own, which would clash
with the header's typed pointers, and calls them by name with `void *`
arguments, which C converts. The C library's math names stay tinygen's: it
reserves them. An extern call with a struct argument or result (`sl_u8`,
`gs_rref`, a program's struct) cannot pass tinygen's struct to the header's,
so `GenExternCall` calls such a callee through its address in a block of its
own (`tyg_call` turns a call through an address taken in the same block into
a direct one); the ABI makes it the same call. The support body builds the
runtime's threads in a standalone unit only with `GS_NEED_THREADS`, and its
fixed `gs_tg_stkpp` only without it.

`goose build` (tinygen-c) writes the unit to `<stem>.tg.c` in the build
directory and compiles it in the link command, after the runtime object and
the packages' native parts are built: `-O2`, or `-O0 -g` with `--debug`.
`goose build -o out.c` and the flag form `goose -o out.c` write the unit and
build nothing; it builds with the runtime object (`goose --emit-runtime`),
or alone with `--standalone`.

A library (`--header`, the flag form) needs no `main`: `EmitMain` writes
`goose_init`, which starts the program once and runs `main` if there is one,
and each `export fn` wrapper aborts when called before it. The header is
`CodeGen::ExportHeader`'s. An `export fn` whose signature the wrapper cannot
cross is a compile error (`CheckExport`).

Every `-D` goes into the unit or the support object's source
(`DefinesSource`); tinygen's code reads no macro. The stack settings
(`GS_STACK_RESERVE`, `GS_STACK_GAP`, `GS_STACK_BUDGET` and `GS_MAX_STACKS`,
so `--stack-reserve` too) reach the runtime through `main`, which passes them
to `gs_tg_start`; the backend reads them from `CodeGen::stackcfg`.

`-v` names the path:

    build: backend tinygen-c (the default)
    tinygen: 12 of 12 function(s) compiled
    tinygen: whole program: no C compiled for it

## Conventions

These are the calling conventions of spec Appendix C, which the runtime and
the support object share.

**Runtime helpers.** The backend calls what the runtime object exports
(`GS_API`) directly. `runtime.h` defines checked arithmetic, division and the
`as` checks as `static` functions. The backend reaches those through exported
shims (`gs_tg_add_i32`, `gs_tg_div_i64`, `gs_tg_range`, ...), compiled with
the program's defines, so `--debug` means the same everywhere. An abort
passes the Goose file and line.

**Extern fns** (§7.10) go through a trampoline per extern (`gs_tg_x_<name>`),
because the C function may be `static` or a macro. A function the runtime
object exports needs no trampoline (`TgModule::RuntimeApi`). An extern of a
shared library is called through a pointer in its trampoline,
`gs_tgdl_<symbol>`, which `gs_tg_dlbind` fills with the runtime's
`gs_dl_bind` (the libraries and spellings from `CodeGen::DlLibs`); `main`
calls it once the runtime has started, before the globals (`DylibBinding`,
`EmitMain`).

**Parameters.** A slice is two parameters, its data and length; a fat
reference is its header and stack; a pool reference is its four members
(`CodeGen::ParamFields`, `TgModule::SplitParam`). Passing a slice as one
16-byte record instead works under SysV and on arm64, which put it in the
same two registers, but not on Windows x64, which passes it by reference to a
copy.

**Results.** The first small fixed result is the return value. Later fixed
results are out-pointers. A resizable or `u8[]` result is built at a
destination stack the caller passes.

**Data stacks** (§6.2). A function that takes `gs_sp` owns `GS(gs_sp + k)` for
the indices it uses, and hands callees `gs_sp` plus the indices in use. Values
over 4096 bytes live on a data stack too, outside recursive cycles. The
backend reports its counts to the stack accounting, which `--stacks`, the
thread cap and `GS_MAX_STACKS` read. An array appended from a call or
construct yielding a `T[..]` is built on the receiver's stack and compacted
there (`AppendSrc`), and a `T[]` or `T[..]` local that the returns hand back
(guaranteed NRVO, spec 7.3) is declared on the destination stack (`nrvo`,
`NrvoBind`), in a function and in its inlined form. A resizable or frame
object named result still has a stack of its own and is copied out at the
return.

**Globals and the stack block.** Each function reads the stack block and the
addresses of its globals once, on entry. How it reads them depends on threads:

* *Without threads* (`TgModule::fixedaddrs`), each address is fixed at load.
  The function loads it from a data pointer (`gs_tg_stkpp`, `gs_tg_g<i>`).
* *With threads*, each thread has its own instance. The function calls an
  accessor (`gs_tg_stks`, `gs_tg_g<i>`) that reads the thread-local.

`UsesThreads` scans for thread and queue builtins before any body is built;
`Compile` stops the build if that scan missed a thread use.

**`return from`** (§7.9). The return site writes the target's channels, sets
`gs_rf`, and returns. After every call that can propagate, the caller checks
`gs_rf`.

## What the backend holds

* scalars, fixed structs, fixed arrays `T[k]`, slices, and plain references;
* `T[]` images and resizable arrays on data stacks, passed by fat reference;
* limited arrays `T[..k]`, variable-size structs, frame objects;
* fixed-mode and variable-mode ADTs, `match`, and case-function dispatch;
* optionals, relative references and pools;
* free variables of nested functions, and function values, spliced at the
  call;
* `print`, `str` and `format` of composites, through `RenderInto`, or through
  a render procedure (`gs_tg_render_<T>`) for a recursive type;
* elementwise operators on arrays, threads and queues, `defer`;
* inlined calls: the optimizer's `NK_InlineBlock` is built by
  `TgFn::GenInline`, the argument bindings in the caller's scope, the body in
  a scope of its own, and a `return` of the inlined function, like a `break`
  out of a `block`, a store of its value and a jump to the block's end.

About 190 `No` sites remain, which no known program reaches; each is the
compile error above when one does.

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
* **`RenderInto`** renders a value structurally.

### Typed memory in the C

tinygen's C prints an access as bytes (a `memcpy`), which the C compiler must
assume may alias anything, unless the address is tagged with the C type it is
accessed as, along the path of records and arrays it was reached through
(`tyg_at_type`, `tyg_at_field`, `tyg_at_elem`): `((struct tyg_r0 *)p)->f1.f0`
for `p.vel.x`. C's aliasing rules then let it reorder accesses of different
types, and two paths alias only where one's outermost type contains the
other's. So a place is tagged only where its bytes hold one Goose type,
written as that type or untagged. `P::typed` marks such places, and `P::a`
carries the path: variables (not a large value on a data stack), what a
reference refers to, the elements of slices and resizable arrays (each a path
of its own), and, from a typed base, fields (`FieldPlace`: field k of the
struct's record, pads not counted) and inline array elements (`ConstElem` for
a constant index; a variable one starts a path at the element). A global is
reached through a record of its own (`GlobalP`, `TgModule::WrapAT`), as the
old C generator's `GS_GL->G.x` was: a loop's stores into an array of `float3`
structs then cannot touch a `float3` global's reads, so they leave the loop,
while a write through a reference to the global still aliases them. `TagAt`
tags a scalar load or store, and a whole fixed value's copy (both sides, so
the C prints a struct assignment); the leaves of elementwise arithmetic walk
the same path (`ElemAt`). Untagged stay: ADT payloads (another variant's
fields hold the same bytes; payload places are made by offset, untagged, and
a whole ADT is a record whose variants overlap, a union, which C lets alias),
a struct literal's field stores, varint and relative-reference slots, headers
of resizables, images, frame objects, `tyg_move` and `tyg_zero`. Untagged
accesses may alias every type, so a typed read of bytes an untagged store
wrote stays in order. What remains open is memory a data stack or a pool
reuses for another type in the same function; C's own allocators share that.

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
* Each global is a symbol under its Goose name.

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
-v` prints the line, with the script's path. The compiler looks for the
script in `lldb/` beside its executable, and then in the source tree it was
built from.

A null optional of a struct shows as `NULL`. For a null pointer to a
record, lldb prints its own nil marker and does not ask the formatter.

## Building tinygen into goose

`CMakeLists.txt` takes a `TINYGEN_DIR` cache path, by default `../tinygen`,
which must hold `src/tinygen.c` and `include/tinygen.h`; CMake compiles
tinygen as a static library, and stops with an error where it finds none.

## Inlining defaults

`goose build` (tinygen-c) inlines at `-O2` by default. The native build
(`--backend=tinygen`, `goose build --debug`, `goose debug`) is the debug
build and defaults to `-O0`, which keeps every Goose function a function in
the debugger. `goose run` defaults to `-O2`. Any `-O` given wins.

## `goose run`

Plain `goose run` (and `goose file.goose`) runs tinygen's C in TinyCC
(`docs/design/jit_backend.md`). The module is created with `TYG_OUTPUT_C`, as
for tinygen-c, and `CText` returns its text in memory with the appended
`main`. TinyCC compiles the one unit (`TinygenUnit` with `jit`, above): the
prelude's `#include`s of the C headers give way to `jit_c.h` and TinyCC's
`va_list`, because the JIT has no C headers; the unit names `gs_rt_start`
without the runtime version, since the runtime is the one built into Goose;
`GS_TG_TLS` is empty because the JIT runs no threads; and `gs_jit_cleanup`
follows the program. tinygen's prelude has TinyCC fallbacks of its own for
traps, byte swaps, atomics and `asm` labels. The runtime's `gs_*` functions
bind through the `GS_JIT_SYMBOL` table. Exit status, arguments, resource root
and abort statuses come from the same hosted runtime (`test/jit_runtime.py`).
`-v` prints `jit: tinygen's C, compiled by TinyCC`. With `--jit -o out.c`,
the unit for a C compiler goes to `out.c` and the TinyCC one runs. Threads
are refused: TinyCC cannot place thread-local storage in an in-memory run.

tinygen's C reads and writes memory through `memcpy`, which a C compiler
inlines and TinyCC calls, so it runs several times slower under TinyCC than
built by a C compiler.

`goose run --backend=tinygen` builds the executable in the program's build
directory, as `goose build` would, and runs it as a child process. The
program shares Goose's standard streams, gets the arguments after `--`, and
its exit code becomes the run's. Build messages go to stderr. A program finds
its resources next to its executable. A run through TinyCC uses the entry
source's directory instead. To match it, Goose sets `GOOSE_RESOURCE_DIR` to
that directory, and `gs_os_resource_dir` prefers the variable when it is set.
Unlike a TinyCC run, this path also runs programs with threads.

Two things of tinygen's C output are worked around in the backend:

* C requires `main(int, char **)`, and a tinygen pointer prints as `void *`.
  So in C output `EmitMain` defines `gs_tg_main`, and `CText` appends a
  `main` that calls it.
* tinygen declares a base debug type by its name, so `_Bool` became
  `typedef _Bool _Bool;`. In C output the `bool` base type is named `bool`,
  which tinygen leaves to the C type.

## Testing

* `test/tinygen/` holds fixtures for each area: scalars, structs, arrays,
  strings, text, ADTs, optionals, limited arrays, variable-size structs,
  `return from`, elementwise operators, and more. They are ordinary fixtures
  too, run by `run_tests.py` against `test/expected/`.
* `test/tinygen_backend.py` is the "tinygen backend" section of
  `run_tests.py`. It builds each program with `--backend=tinygen`, checks
  that the build is whole (every function through tinygen, no C of the
  program compiled) and compares the output with the expected output; then
  again with `--backend=tinygen-c`, the `--debug` ones at `-O0 -g`, failing
  a program whose tinygen C does not compile or warns on a line of its own.
  It also checks `goose run`, the defaults of `goose build`, `-o x.c` built
  by cc with the runtime object and alone, the flags (`-D`, `--include`,
  `--standalone`, `export fn`) and a library with `--header`.
* Where lldb is installed, it checks breakpoints by Goose name, Goose lines,
  and parameters, locals and globals with Goose types. With the formatters
  imported, it checks arrays' elements, text, `i8` and `u8`, optionals,
  ADTs and limited arrays as Goose values.
* `goose_in_goose` (`docs/testing.md`) builds its stage 1 from tinygen's C
  and checks that stages 2 and 3, the Goose-written compiler's own C, agree.
* On an arm64 Mac, an x86-64 compiler (`-DCMAKE_OSX_ARCHITECTURES=x86_64`)
  runs under Rosetta and passes `-arch x86_64` to cc. The Command Line Tools'
  `cc`, `git` and `lldb` are arm64 only. So `GOOSE_CC` must name a wrapper
  that runs cc natively (`arch -arm64 /usr/bin/cc "$@"`).
* On Windows x64, three lldb checks fail (see "What tinygen lacks"). lldb
  needs Python's `python3.dll` on `PATH` to start.

## What tinygen lacks

1. **In-memory loading.** tinygen cannot map and relocate its own code in
   the running process. So `goose run --backend=tinygen` builds and links
   an executable, which costs a link step that a TinyCC run does not have.
2. **Counted arrays in debug info.** A slice's or a resizable's elements need
   an array type whose count is another member. A field after a
   variable-size field needs a member location expression. tinygen has
   neither, so the DWARF has `data` and `len` separately. lldb shows the
   elements only through the formatters (see "lldb formatters").
3. **Goose values in the debugger without a script.** lldb reads every
   language through clang's types, so without the formatters `i8` and `u8`
   show as characters and an ADT as all its payloads.
4. **Register variables lldb can read in CodeView.** On Windows, lldb cannot
   show an `f64` local that tinygen keeps in an XMM register ("unable to
   convert register kind=4 reg_num=110"). A one-byte value in a register
   shows wrong: a `Dir` parameter holding `West` in `rbx` shows as `North`.
   An optional shows under its pointer type (`Link *`), not its Goose name
   (`Link?`). The values are right; three of the lldb checks fail on
   Windows for this.
5. **Direct thread-local access on Mach-O.** Programs with threads read
   their stack block and globals through accessor calls.
6. **A multiway branch.** A `match` is a chain of compares, where a C
   compiler makes a jump table.
7. **Vector code.** `simd` functions (§7.12) are their baseline version,
   with no per-instruction-set versions, and elementwise float arithmetic
   is member by member.

## Next steps

* Faster loop code in the native build.
* Windows: CodeView register numbers lldb understands (see "What tinygen
  lacks").
* An in-memory `goose run --backend=tinygen`, once tinygen can load code
  in memory, if its faster start is worth losing the debugger.
* Direct globals for programs with threads, through a context pointer passed
  beside `gs_sp`.

## Decisions

* **Appendix C's conventions stay.** The runtime and the support object are
  written against them, and a measurement found that a convention of our own
  would gain under 1% on every benchmark.
* **One lowering, two outputs.** tinygen's C is the optimized build and the
  portable file; its object is the debug build. Both come from the same IR,
  so they cannot disagree on a program's meaning.
* **`goose run --backend=tinygen` builds an executable.** It reuses the
  build path whole, and a debugger can attach to it.
* **What the backend does not support is a compile error.** There is no
  other code generator to fall back to; the error names the construct and
  its line.
