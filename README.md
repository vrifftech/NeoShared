# neoshared

[![CI](https://github.com/vrifftech/neoshared/actions/workflows/ci.yml/badge.svg)](https://github.com/vrifftech/neoshared/actions/workflows/ci.yml)

`neoshared` contains the reusable C++17 format and wxWidgets infrastructure consumed by the independent Neo tool repositories.

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

## Public CMake targets

- `neoshared::tabular`: CSV/TSV helpers and generic row filtering.
- `neoshared::json`: JSON parser and writer.
- `neoshared::xml`: XML parser and writer.
- `neoshared::tslpatcher`: generic TSLPatcher/HoloPatcher output helpers.
- `neoshared::gff`: GFF reader/writer, JSON/XML conversion, and type helpers.
- `neoshared::tlk`: editable and read-only TLK models, V3.0/V4.0/Dragon Age codecs, StrRef lookup, and text-encoding support.
- `neoshared::gff_app`: GFF document-model behavior that bridges `neoshared::gff` with `neoshared::tlk` for resolved StrRef text.
- `neoshared::wx`: header-only settings, the shared saved-game-directory registry and **Open Game Directory** menu, view state, tabs, theme, status-bar, and common wxWidgets UI helpers.

Consuming repositories should link only the namespaced `neoshared::` targets.

## Build and test

Linux/macOS:

```sh
./scripts/build.sh --tests ON --jobs "$(nproc)"
ctest --test-dir build --output-on-failure
```

Windows:

```powershell
.\scripts\build.ps1 -Tests ON -Parallel ([Environment]::ProcessorCount)
ctest --test-dir build -C Release --output-on-failure
```


## Continuous integration

GitHub Actions builds and tests `neoshared` on Ubuntu 24.04 and Windows Server 2025 with Visual Studio 2026. The workflow runs for pushes, pull requests, manual dispatches, and a weekly runner-drift check. Dependabot keeps the referenced GitHub Actions current.
