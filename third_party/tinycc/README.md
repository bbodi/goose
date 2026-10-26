# TinyCC

This directory holds the sources of the Tiny C Compiler that
`cmake/libtcc.cmake` builds into the compiler as its in-process C backend
(`docs/design/jit_backend.md`), copied from
[TinyCC/tinycc](https://github.com/TinyCC/tinycc) at commit
`0fb54300b56512754221d80adda85ddb9815bceb` (2026-09-04, `release_0_9_26-2610`).
It contains the `.c`, `.h` and `.def` files of the repository root,
`include/`, `lib/`, `win32/include/` and `win32/lib/`, together with
`VERSION`, `COPYING`, `RELICENSING` and upstream's `README`. Its tests,
examples, documentation and build scripts are left out; the CMake file
replaces its configure and makefile.

The backend is not optional: every Goose compiler is built with it.

License: LGPL 2.1 (`COPYING`), with the relicensing notes in `RELICENSING`.

Local changes:

- `arm64-gen.c`: use Apple's natural stack size and alignment for fixed
  floating-point arguments, including homogeneous float aggregates. This
  fixes a trailing float being passed at offset 16 instead of 12 after a
  spilled three-float struct. Variadic arguments retain eight-byte slots.
  `test/packages/pkg_float_abi.goose` checks calls into native C through the
  in-process backend as well as native builds.
- `x86_64-gen.c`: on SysV, a struct with a field off its natural alignment
  (a packed struct, such as every Goose fixed struct) is passed and returned
  in memory, as the psABI and clang do. TinyCC passed `{int32_t; double}`
  packed in two integer registers, so a JIT call into native C read garbage.
  `test/tinygen/tinygen_extern.goose`, run through the in-process backend,
  checks it. TinyCC still classifies a struct as one class, not per
  eightbyte: `{double; int64_t}` goes in two integer registers where clang
  uses an SSE and an integer register.

To update, copy the same files from a newer TinyCC commit and record that
commit here. Preserve the local changes until they are included upstream.
