# Building neoshared on macOS

`neoshared` is compiled and its existing tests are run on native Apple Silicon and Intel runners.

| Architecture | GitHub runner |
|---|---|
| `arm64` | `macos-15` |
| `x86_64` | `macos-15-intel` |

## Prerequisites

```bash
xcode-select --install
brew install cmake ninja
```

## Build and test

```bash
cd neoshared
bash ./scripts/build-macos.sh --clean
```

Explicit example:

```bash
bash ./scripts/build-macos.sh \
  --arch "$(uname -m)" \
  --deployment-target 15.0 \
  --jobs 4 \
  --clean
```

Cross-architecture builds are rejected; use the native architecture of the Mac or runner.

Application repositories use `neoshared/scripts/build-macos-app.sh` and `neoshared/scripts/package-macos-app.sh` for consistent configuration, portable dependency fixup, architecture validation, ad-hoc signing, and ZIP creation.
