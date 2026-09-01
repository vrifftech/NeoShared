#!/usr/bin/env python3
"""Verify that NeoWasm.cmake preserves path-bearing linker arguments with spaces."""

from __future__ import annotations

import argparse
import pathlib
import shlex
import subprocess
import tempfile


def cmake_path(path: pathlib.Path) -> str:
    return path.resolve().as_posix().replace('"', '\\"')


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--neoshared-root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1],
    )
    args = parser.parse_args()
    shared = args.neoshared_root.resolve()
    module = shared / "cmake" / "NeoWasm.cmake"
    if not module.is_file():
        raise SystemExit(f"NeoWasm.cmake not found: {module}")

    with tempfile.TemporaryDirectory(prefix="neo wasm cmake path test ") as temporary:
        root = pathlib.Path(temporary)
        source = root / "source tree"
        build = root / "build tree"
        wx_source = source / "fake wx source"
        assets = source / "preload assets"
        wx_js = wx_source / "build" / "wasm" / "wx.js"
        wx_dom_js = wx_source / "build" / "wasm" / "wx-dom.js"
        icon = source / "test icon.svg"
        dummy = source / "dummy source.cpp"
        wx_js.parent.mkdir(parents=True)
        assets.mkdir(parents=True)
        wx_js.write_text("// wx test\n", encoding="utf-8")
        wx_dom_js.write_text("// wx-dom test\n", encoding="utf-8")
        icon.write_text("<svg xmlns='http://www.w3.org/2000/svg'/>\n", encoding="utf-8")
        dummy.write_text("int main() { return 0; }\n", encoding="utf-8")
        (assets / "payload.txt").write_text("payload\n", encoding="utf-8")

        (source / "CMakeLists.txt").write_text(
            f"""cmake_minimum_required(VERSION 3.16)
project(NeoWasmSpacePathProbe LANGUAGES CXX)
set(EMSCRIPTEN TRUE)
include(\"{cmake_path(module)}\")
add_executable(wasm_path_probe \"{cmake_path(dummy)}\")
neo_configure_wasm_target(wasm_path_probe
    NAME \"Path Probe\"
    SLUG \"path-probe\"
    ICON \"{cmake_path(icon)}\"
    VERSION \"1.0\"
    PRELOAD \"{cmake_path(assets)}@/assets\")
""",
            encoding="utf-8",
        )

        configured = subprocess.run(
            ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
             f"-DNEO_WX_WASM_SOURCE:PATH={wx_source}"],
            text=True,
            capture_output=True,
        )
        if configured.returncode != 0:
            raise SystemExit("CMake configuration failed:\n" + configured.stdout + configured.stderr)
        commands = subprocess.run(
            ["ninja", "-C", str(build), "-t", "commands", "wasm_path_probe"],
            check=True,
            text=True,
            capture_output=True,
        ).stdout.splitlines()
        link_line = next((line for line in commands if "--pre-js" in line), "")
        if not link_line:
            raise SystemExit("Generated Ninja commands did not contain the WebAssembly link command")
        tokens = shlex.split(link_line)

        expected_pairs = [
            ("--pre-js", str((shared / "wasm" / "neo-pre.js").resolve())),
            ("--pre-js", str(wx_js.resolve())),
            ("--pre-js", str(wx_dom_js.resolve())),
            ("--shell-file", str((build / "wasm-generated" / "wasm_path_probe" / "path-probe-shell.html").resolve())),
            ("--preload-file", f"{assets.resolve()}@/assets"),
        ]
        cursor = 0
        for option, value in expected_pairs:
            try:
                index = tokens.index(option, cursor)
            except ValueError as error:
                raise SystemExit(f"Missing generated argument {option!r} after token {cursor}: {tokens}") from error
            if index + 1 >= len(tokens) or tokens[index + 1] != value:
                actual = tokens[index + 1] if index + 1 < len(tokens) else "<missing>"
                raise SystemExit(
                    f"Generated {option} path was split or changed:\nexpected: {value}\nactual:   {actual}\ncommand:  {link_line}"
                )
            cursor = index + 2

    print("NeoWasm path quoting test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
