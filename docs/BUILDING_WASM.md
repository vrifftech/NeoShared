# NeoTools WebAssembly build infrastructure

`neoshared` owns the common browser toolchain and packaging implementation used
by the independent application repositories.

## Pinned dependencies

- Emscripten SDK `4.0.2`
- `PCBJam/wxWidgets` wasm-port commit `bca69b9fddc88adec57b05e6809467ef9f5158c8`

The application and `neoshared` remain sibling repositories. wxWidgets-WASM and
Emscripten are external build dependencies stored in a separate dependency
cache; they are not vendored into any application repository.

## Shared commands

```bash
bash scripts/setup-wasm.sh --deps-root ../.neo-wasm-deps
bash scripts/build-wx-wasm.sh --deps-root ../.neo-wasm-deps --jobs 4
```

Applications call `scripts/build-wasm-app.sh` through their own thin
`scripts/build-wasm.sh` wrapper.

## Deliberate choices

- Existing C++/wxWidgets UI, not a web UI rewrite.
- Single-threaded WASM so GitHub Pages needs no special security headers.
- In-link Asyncify for modal dialogs and clipboard operations.
- Emscripten JavaScript exceptions rather than the fork's more complex
  wasm-EH post-link Binaryen pipeline.
- Browser-local wxConfig persistence through the port's localStorage backend; IDBFS is also mounted for browser-owned files.
- Relative static assets suitable for project Pages URLs.
- No direct installed-game discovery or unrestricted native process launch.

The first native Pages workflow run is the authoritative Emscripten/wxWidgets
compiler and browser-link validation. Desktop builds remain independent and
unchanged.

## Browser filesystem boundary

The selected wxWidgets port can import files into Emscripten's virtual
filesystem and download individual saved files. It does not provide an atomic,
writable installer-directory transaction. The browser preview therefore:

- keeps patcher **Fragment** preview/copy/download available;
- disables package-aware **Write to INI**, which must update the selected INI
  and all companion payloads together;
- disables installed-game-directory scanning;
- disables directory-wide export and extraction, including NeoERF's
  multi-resource extraction command.

These are deliberate fail-closed boundaries, not silent partial exports. A
future ZIP-based package workspace or browser File System Access integration
can add those workflows without weakening the existing desktop semantics.

## Autoconf wrapper permissions

GNU Autoconf executes its `AUTOM4TE`, `SHELL`, and `CONFIG_SHELL` helpers as
programs. Repository ZIPs and Windows-originated commits may lose Unix
executable mode bits even when the script contents are correct. The shared
wxWidgets-WASM build therefore installs private mode-`0755` copies under:

```text
<deps-root>/autoconf-wrappers/
```

Do not add tool-specific `chmod` steps to individual application workflows.
Publish the corrected `neoshared` revision and point each application's
`NEOSHARED_REF` at that revision.

## Native validation boundary

The committed repositories do not add browser smoke-test fixtures or test-only
resources. The first hosted Pages run is the authoritative Emscripten compile
and link check. After it succeeds, manually open the deployed site and exercise
Open, Save/download, clipboard, modal dialogs, large-document interaction, and
Fragment output in a supported browser.
