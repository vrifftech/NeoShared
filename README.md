# NeoShared

[![CI](https://github.com/vrifftech/NeoShared/actions/workflows/ci.yml/badge.svg)](https://github.com/vrifftech/NeoShared/actions/workflows/ci.yml)

`NeoShared` contains the reusable and wxWidgets infrastructure consumed by the independent Neo tool repositories.

Support is offered in the OpenKotOR Discord.

[![OpenKotOR Discord](https://discordapp.com/api/guilds/739590575359262792/widget.png?style=banner2)](https://discord.gg/OpenKotOR)

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
