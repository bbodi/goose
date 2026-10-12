# The tinygen backend

`goose build --backend=tinygen` builds a debug build without compiling the
program's C. tinygen is a C99 library, checked out beside this repository as
`../tinygen`. It turns a small IR into machine code and writes an object file
with debug info. The backend lowers each Goose function to that IR.

    goose build --backend=tinygen prog.goose          # debug build, -O0
    goose build --backend=tinygen --debug prog.goose  # with GS_DEBUG's checks
    goose build --backend=tinygen -v prog.goose       # says what went to C, and why
    goose run --backend=tinygen prog.goose -- args    # builds, then runs it

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

**Mixed.** The C backend writes its C as usual, and the C compiler compiles it
at `-O0 -g`. That C holds `main`, the globals, any function tinygen did not
take, and the shims the object calls. `CodeGen` gives every function external
linkage, so the two objects resolve each other's calls.

A build takes the mixed path when a function or a global initializer falls
back to C. It also takes it for `--include`, `--standalone`, a library, an `export fn` that
`ExportOk` rejects, or any `-D` other than `GS_DEBUG`. `--stack-reserve` adds
such a `-D`, so it forces the mixed path too. `--tinygen-mixed` (hidden)
forces it for comparisons. `-v` names the path and the reason:

    tinygen: whole program: no C compiled for it
    tinygen: mixed: the C half compiled by the C compiler (-DGS_STACK_RESERVE=2147483648ull)

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

## Building tinygen into goose

`CMakeLists.txt` takes a `TINYGEN_DIR` cache path, by default `../tinygen`.
When that path holds `src/tinygen.c` and `include/tinygen.h`, CMake compiles
tinygen as a static library and defines `GOOSE_HAVE_TINYGEN=1`. Without it,
the compiler has the C backend only, and `--backend=tinygen` says so.

`--backend=tinygen` makes `-O0` the default, so no function is inlined away
in the debugger. `-O1` and `-O2` still work.

## `goose run`

`goose run --backend=tinygen` builds the executable in the program's build
directory, as `goose build` would, and runs it as a child process. The
program shares Goose's standard streams, gets the arguments after `--`, and
its exit code becomes the run's. Build messages go to stderr.

A program finds its resources next to its executable. A run through TinyCC
uses the entry source's directory instead. To match it, Goose sets
`GOOSE_RESOURCE_DIR` to that directory, and `gs_os_resource_dir` prefers the
variable when it is set. Unlike a TinyCC run, this path also runs programs
with threads.

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
* Where lldb is installed, it checks breakpoints by Goose name, Goose lines,
  and parameters, locals and globals with Goose types.
* goose_in_goose built through tinygen compiles itself to C that is byte for
  byte the C backend's.
* On an arm64 Mac, an x86-64 compiler (`-DCMAKE_OSX_ARCHITECTURES=x86_64`)
  runs under Rosetta and passes `-arch x86_64` to cc. The Command Line Tools'
  `cc`, `git` and `lldb` are arm64 only. So `GOOSE_CC` must name a wrapper
  that runs cc natively (`arch -arm64 /usr/bin/cc "$@"`).

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
| sum | 142 | 630 | 34 |
| calc | 267 | 694 | 108 |
| sexp | 731 | 1987 | 335 |
| lru | 515 | 950 | 138 |
| scene | 145 | 1158 | 38 |
| blur | 131 | 2659 | 14 |
| particles | 363 | (aborts, see below) | 141 |

tinygen beats `cc -O0` on every benchmark. It is 2 to 10 times slower than
`cc -O2`, most of all in tight loops such as blur's. particles' `--debug`
build aborts on a lossy `as` conversion, which its checks are meant to
catch.

## What tinygen lacks

1. **In-memory loading.** tinygen cannot map and relocate its own code in
   the running process. So `goose run --backend=tinygen` builds and links
   an executable, which costs a link step that a TinyCC run does not have.
2. **Counted arrays in debug info.** A slice's or a resizable's elements need
   an array type whose count is another member. A field after a
   variable-size field needs a member location expression. tinygen has
   neither, so the debugger shows `data` and `len` separately.
3. **lldb shows `i8` and `u8` as characters.** lldb reads every language
   through clang's types. Goose would need lldb formatters shipped with the
   compiler.
4. **Packed records on Windows x64.** On arm64 and on SysV x86-64 (checked on
   macOS under Rosetta), tinygen passes packed records as clang does: a
   misaligned field sends the record to memory. tinygen's `test/abi.c` and
   the `tinygen_abi` fixture check this. Windows x64 is unchecked.
5. **Direct thread-local access on Mach-O.** Programs with threads read
   their stack block and globals through accessor calls.
6. **Debug info for a member of a symbol.** In a mixed build, the C half
   holds the globals inside one struct. lldb then shows a global as
   `gs_globals_main.counter_g`.
7. **A multiway branch.** A `match` is a chain of compares, where the C
   compiler makes a jump table.

## Next steps

* Faster loop code, which is where the gap to `cc -O2` is.
* Linux and Windows: run tinygen's ABI test and this suite there.
* An in-memory `goose run --backend=tinygen`, once tinygen can load code
  in memory, if its faster start is worth losing the debugger.
* Let `--stack-reserve` take the whole-program path.
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
