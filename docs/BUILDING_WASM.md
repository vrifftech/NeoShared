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

The shared browser bridge imports host files into Emscripten's virtual
filesystem and exposes individual outputs through a prominent, persistent
**Download <filename>** action above the editor. It does not enter a native
Save File picker from a wxWidgets event.

For exporters that explicitly opt in, package-aware **Write to INI** uses the
browser File System Access directory API:

1. the user grants read/write access to one installer folder;
2. existing INIs are listed by exact package-relative path;
3. the selected INI, same-name payloads, and `info.rtf` are copied into a fresh
   virtual preflight workspace;
4. the existing audited C++ merge and payload logic runs against that workspace;
5. the browser rechecks the host files for concurrent changes;
6. unchanged files are reused, payloads are written before the INI, and the INI
   is committed last.

The folder remains on the user's computer and is not uploaded. Browsers without
writable directory access, and exporters that have not adopted the package
transaction, remain fail-closed in **Fragment** mode. Fragment still creates no
payloads, omits `[Settings]`, and never merges into an existing INI.

Installed-game-directory scanning and unrestricted directory-wide operations
remain unavailable. Individual browser downloads continue through the visible
Download action above the editor.

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


## CMake wxWidgets discovery

Emscripten configures CMake to search headers and libraries only inside the SDK
sysroot. The pinned wxWidgets-WASM build is intentionally external to that
sysroot. CMake's stock `FindwxWidgets` first obtains flags from `wx-config`, then
validates the reported headers and `-l` libraries with CMake search commands.
Without a compatibility wrapper, valid files in `<deps-root>/wxwidgets-wasm-build`
are rejected as out-of-sysroot and the application reports that wxWidgets was
not found.

`cmake/wasm/FindwxWidgets.cmake` delegates to CMake's stock module while
temporarily allowing the explicit external paths reported by `wx-config`. It
restores the Emscripten root-search policy immediately afterward, so unrelated
package discovery remains sysroot-isolated. `scripts/build-wasm-app.sh` also
prints the selected wx configuration, compile flags, and libraries before CMake
configuration to make future discovery failures diagnosable from one CI log.

Do not work around this by copying wxWidgets into the Emscripten SDK sysroot or
by changing root-search modes globally in every application repository. Publish
and pin the corrected `neoshared` revision instead.

## Native validation boundary

The committed repositories do not add browser smoke-test fixtures or test-only
resources. The first hosted Pages run is the authoritative Emscripten compile
and link check. After it succeeds, manually open the deployed site and exercise
Open, Save/download, clipboard, modal dialogs, large-document interaction, and
Fragment output in a supported browser.

## Browser recent files

Recent-file menus retain imported virtual files only for the current page session.
Reloading the page clears the list because browsers do not expose reusable host paths,
and shared code does not silently persist user file contents.
