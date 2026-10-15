# NeoShared

[![CI](https://github.com/vrifftech/NeoShared/actions/workflows/ci.yml/badge.svg)](https://github.com/vrifftech/NeoShared/actions/workflows/ci.yml)

`NeoShared` contains the reusable and wxWidgets infrastructure consumed by the independent Neo tool repositories.

## Checkout layout

Clone `neoshared` beside each tool repository:

```text
workspace/
  neoshared/
  Neo2DA/
  NeoGFF/
  ...
```

Each tool resolves `../neoshared` automatically. For another checkout layout, set the `NEOSHARED_ROOT` environment variable or CMake cache variable:

```sh
cmake -S . -B build -DNEOSHARED_ROOT=/path/to/neoshared
```
