#!/usr/bin/env python3
"""Static regressions for the shared browser-file and compatibility bridges."""

from __future__ import annotations

import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PRE_JS = ROOT / "wasm" / "neo-pre.js"
COMPAT_CPP = ROOT / "src" / "wasm_dialog_compat.cpp"
COMPAT_HPP = ROOT / "include" / "neoshared" / "wasm_dialog_compat.h"
BROWSER_CPP = ROOT / "wx" / "NeoBrowserFiles.cpp"
BROWSER_HPP = ROOT / "wx" / "NeoBrowserFiles.hpp"
WX_UI = ROOT / "wx" / "NeoWxUi.hpp"

pre_js = PRE_JS.read_text(encoding="utf-8")
compat_cpp = COMPAT_CPP.read_text(encoding="utf-8")
compat_hpp = COMPAT_HPP.read_text(encoding="utf-8")
browser_cpp = BROWSER_CPP.read_text(encoding="utf-8")
browser_hpp = BROWSER_HPP.read_text(encoding="utf-8")
wx_ui = WX_UI.read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


# Canonical importer invariants.
require("FS.writeFile(path, bytes, { canOwn: true });" in pre_js,
        "legacy imports must transfer their typed-array storage to MEMFS")
require("registerBrowserWritablePath(record.path, record.name" in pre_js,
        "imported files must be registered for browser write-back")
require("legacyImportPathSessions" in pre_js,
        "import paths must resolve to explicit import sessions")
require("Close or release an imported document" in pre_js,
        "active import budget exhaustion must reject instead of evicting")
require("'maxLegacyImportAgeMs', 0" in pre_js,
        "legacy import expiry must be disabled by default")
require("pruneLegacyImportSessions" not in pre_js,
        "active imports must not be silently pruned")
require("browserWritableTombstones" in pre_js and "originalRename" in pre_js,
        "atomic unlink/rename save sequences must preserve write-back bindings")
require("chooseAndImportDirectory" in pre_js,
        "compatibility directory selection must use the canonical bridge")
require("Do not copy\n          // the existing host directory into MEMFS." in pre_js,
        "output-directory selection must not import existing host contents")
require("maxLegacyImportFiles" in pre_js and "maxLegacyImportFileBytes" in pre_js,
        "legacy imports need count and per-file limits")
require("maxBrowserWritableOutputs" in pre_js and "maxBrowserWritableDirectories" in pre_js,
        "writable destination registries must be bounded")
require("maxBrowserWritebackBytes" in pre_js,
        "whole-file compatibility write-back must have a byte limit")
require("queued, or in-flight host write/download" in pre_js,
        "releasing a document must flush pending saves before reclaiming MEMFS")
require("browserWritablePublishes" in pre_js and "enqueueBrowserWritablePublish" in pre_js,
        "write-back operations must be serialized per virtual path")

# Compatibility bridge must contain no independent storage/copy subsystem.
require("Module.neoToolsBrowserFiles" in compat_cpp,
        "compatibility dialogs must delegate to NeoBrowserFiles")
for forbidden in (
    "files: new Map()",
    "directories: []",
    "FS.writeFile(",
    "file.arrayBuffer()",
    "FS.readFile(",
    "const state = {",
):
    require(forbidden not in compat_cpp,
            f"compatibility bridge still contains independent I/O operation: {forbidden}")
require("chooseAndImportFiles" in compat_cpp and "chooseAndImportDirectory" in compat_cpp,
        "compatibility pickers must use shared import entry points")
require("neo_wasm_io_init();\n    const state" not in compat_cpp,
        "EM_JS bodies must not call a C symbol by its unprefixed JavaScript name")
require("ReleasePath" in compat_hpp and "ReleaseDirectory" in compat_hpp,
        "compatibility callers need explicit cleanup APIs")
require("publishOnDestroy_" not in compat_cpp and "publishOnDestroy_" not in compat_hpp,
        "save dialogs must not publish before the caller writes the file")

# C++ completion ownership and dead-window cleanup.
require("class BrowserImportLease" in browser_hpp and "requestOpenFilesOwned" in browser_hpp,
        "new callers need a move-only import lease API")
require("releaseOnFailure.empty() ? result.paths : releaseOnFailure" in browser_cpp,
        "throwing callbacks must release undelivered imports")
require("releaseImportedFiles(const std::vector<std::filesystem::path>& paths) noexcept" in browser_cpp,
        "RAII import cleanup must not throw")
require("PendingOpenFilesDelivery" in browser_cpp,
        "queued wx completions must release abandoned imports")
require("if (found == callbacks.end())" in browser_cpp and
        "neobrowser::releaseImportedFiles(paths);" in browser_cpp,
        "orphaned JS completions must release imported sessions")
require(wx_ui.count("neobrowser::releaseImportedFiles(result.paths);") >= 4,
        "dead parents and error paths must release imported sessions")


def extract_em_js_bodies(source: str) -> list[tuple[bool, str]]:
    bodies: list[tuple[bool, str]] = []
    pattern = re.compile(r"EM_(ASYNC_)?JS\s*\(")
    for match in pattern.finditer(source):
        async_body = bool(match.group(1))
        body_start = source.find(", {", match.end())
        require(body_start >= 0, "unable to locate EM_JS body")
        open_brace = body_start + 2
        depth = 0
        state = "code"
        escaped = False
        index = open_brace
        while index < len(source):
            ch = source[index]
            nxt = source[index + 1] if index + 1 < len(source) else ""
            if state == "line_comment":
                if ch == "\n": state = "code"
            elif state == "block_comment":
                if ch == "*" and nxt == "/":
                    state = "code"
                    index += 1
            elif state in {"single", "double", "template"}:
                if escaped:
                    escaped = False
                elif ch == "\\":
                    escaped = True
                elif (state == "single" and ch == "'") or \
                     (state == "double" and ch == '"') or \
                     (state == "template" and ch == "`"):
                    state = "code"
            else:
                if ch == "/" and nxt == "/":
                    state = "line_comment"
                    index += 1
                elif ch == "/" and nxt == "*":
                    state = "block_comment"
                    index += 1
                elif ch == "'": state = "single"
                elif ch == '"': state = "double"
                elif ch == "`": state = "template"
                elif ch == "{": depth += 1
                elif ch == "}":
                    depth -= 1
                    if depth == 0:
                        bodies.append((async_body, source[open_brace + 1:index]))
                        break
            index += 1
        else:
            raise AssertionError("unterminated EM_JS body")
    return bodies


bodies = extract_em_js_bodies(compat_cpp)
require(len(bodies) >= 8, "expected all compatibility bridge EM_JS bodies")
with tempfile.TemporaryDirectory(prefix="neoshared-em-js-") as directory:
    script = Path(directory) / "compat-bodies.mjs"
    pieces = []
    for index, (is_async, body) in enumerate(bodies):
        prefix = "async " if is_async else ""
        pieces.append(f"{prefix}function emBody{index}() {{\n{body}\n}}\n")
    script.write_text("\n".join(pieces), encoding="utf-8")
    subprocess.run(["node", "--check", str(script)], check=True)

print("NeoShared static WebAssembly regressions: PASS")
