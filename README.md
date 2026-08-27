# NeoShared

[![CI](https://github.com/vrifftech/NeoShared/actions/workflows/ci.yml/badge.svg)](https://github.com/vrifftech/NeoShared/actions/workflows/ci.yml)

`NeoShared` contains the reusable C++17 format and wxWidgets infrastructure consumed by the independent Neo tool repositories.

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

## Windows wxWidgets policy

Windows GUI builds use the overlay port in `vcpkg-ports/wxwidgets`, pinned to wxWidgets 3.3.3 and to the exact Lexilla and Scintilla revisions recorded by that release. Install it once for a triplet with:

```powershell
.\scripts\install-wxwidgets.ps1 `
  -VcpkgRoot C:\vcpkg `
  -Triplet x64-windows-static `
  -CleanAfterBuild
```

Each tool's `build.ps1` detects a sibling `neoshared` checkout and automatically passes `neoshared/vcpkg-ports` as `VCPKG_OVERLAY_PORTS`. Direct CMake callers must pass the same path explicitly. Linux CI continues to use the distribution wxGTK package because the Windows list-control repaint defect is specific to wxMSW.

If the Windows dependency build fails, the installer prints the most recent wxWidgets vcpkg logs and saves a complete copy under `.ci-logs/vcpkg-wxwidgets`. NeoShared CI uploads that directory as a failure artifact.

## Public CMake targets

- `neoshared::tabular`: CSV/TSV helpers and generic row filtering.
- `neoshared::json`: JSON parser and writer.
- `neoshared::xml`: XML parser and writer.
- `neoshared::tslpatcher`: generic TSLPatcher/HoloPatcher output helpers, including structure-aware merging into an existing selected installer INI while preserving unrelated sections, comments, and formatting, plus paste-ready INI fragment serialization.
- `wx/NeoPatcherExport.hpp`: the shared **Write to INI** / **Fragment** selection and preview UI used by the patch-exporting applications. Fragment output can be copied to the clipboard or saved as a new INI file without merging into an existing installer INI.
- `neoshared::gff`: GFF reader/writer, JSON/XML conversion, and type helpers.
- `neoshared::tlk`: editable and read-only TLK models, V3.0/V4.0/Dragon Age codecs, StrRef lookup, and text-encoding support.
- `neoshared::gff_app`: GFF document-model behavior that bridges `neoshared::gff` with `neoshared::tlk` for resolved StrRef text.
- `neoshared::wx`: header-only settings, the shared saved-game-directory registry and **Open Game Directory** menu, view state, tabs, theme, status-bar, and common wxWidgets UI helpers.

Consuming repositories should link only the namespaced `neoshared::` targets.

## Build

Linux/macOS:

```sh
./scripts/build.sh --jobs "$(nproc)"
```

Windows:

```powershell
.\scripts\build.ps1 -Parallel ([Environment]::ProcessorCount)
```

## Repository boundary

Only code used by multiple Neo applications belongs here. Application-specific behavior remains with its owner: dialogue graph operations in NeoDLG, KotOR journal operations in NeoJRL, Jade quest operations in NeoQST, texture codecs in NeoTPC, and alignment/LIP behavior in NeoLIP. The shared GFF layer reads and writes classic GFF V3.2 and The Witcher-compatible V3.3 resources, preserves repeated labels used by Jade Empire SAV resources, and exposes occurrence-aware paths such as `Field[#1]` and `Field[#2]`.

JSON and XML are semantic interchange formats rather than byte-preserving substitutes for native files. Their serializers require valid UTF-8; XML also rejects characters forbidden by XML 1.0. Conversions that cannot preserve the native GFF family or version fail explicitly.

Three- and four-component GFF values use the shared `ParseGffVector3Text`, `ParseGffVector4Text`, `FormatGffVector3Text`, and `FormatGffVector4Text` boundary. It requires exactly three or four finite pipe-separated numbers, so every consumer applies the same validation and normalization.

## Shared game-directory contract

All Neo GUIs consume `NeoGameDirectoryMenu.hpp`. The **File > Open Game Directory** submenu is populated from the common `NeoTools` settings store and passes the selected install root to the consuming application's file picker, so each tool opens its own supported-file dialog at that directory. Applications may provide an allowed game-ID list when they only support one game; the submenu and management dialog then show only those installations while continuing to use the common registry. The resolver remains centralized here so tools do not develop incompatible game lists or settings keys.

Format-specific applications can pass `GameDirectoryGameIds` to restrict both the submenu and its management dialog to applicable games. For example, NeoQST passes `{"jade"}`, so it never presents KotOR, Neverwinter Nights, The Witcher, or Dragon Age installations. Unfiltered consumers retain the complete shared list.

Current consumers pass an application-specific open-dialog callback. A source-compatible two-argument overload remains temporarily so independently versioned tool repositories can be rolled forward after `neoshared` without breaking their existing builds.

## Continuous integration

GitHub Actions builds `neoshared` on Ubuntu 24.04 and Windows Server 2025 with Visual Studio 2026. The workflow runs for pushes, pull requests, manual dispatches, and a weekly runner-drift check. Dependabot keeps the referenced GitHub Actions current.
