# To do

## Check the tinygen parameter split on Linux and macOS

Commit df40ba0 ("Pass slices and fat references as their members in
tinygen") changed how the tinygen backend passes a slice and a fat
reference. Each is now two parameters, as the C half passes it
(`CodeGen::ParamFields`), instead of one 16-byte record. That fixed the
mixed builds on Windows x64, which passes such a record by reference to a
copy.

It was tested on Windows only. Under SysV and on arm64, the two parameters
go in the same two registers the record did, so nothing should change. But
the signatures, the entry code, the calls, the export wrappers and the
thread thunks all changed, so check it:

- `python test/tinygen_backend.py <goose>` on Linux x86-64, macOS arm64
  and macOS x86-64 (under Rosetta), lldb checks included.
- The full `run_tests.py` on the same three.
- The sweep of the samples and the `codegen/`, `typing/` and `optimizer/`
  fixtures, built with C and with tinygen, plain and with `--debug`: the
  run pairs must still match.
- The run times in `docs/design/tinygen_backend.md`: a whole program now
  passes these values in registers instead of through a copy.
