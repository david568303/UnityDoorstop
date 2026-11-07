# IL2CPP compatibility fork

This branch preserves the upstream UnityDoorstop history and is based on
[NeighTools/UnityDoorstop commit 1c3ece1](https://github.com/NeighTools/UnityDoorstop/commit/1c3ece1f1e6c20f4aa74d5402baabf8a0098d282).
The compatibility changes are maintained by David Salas Kalendar, dated
2026-10-28. The original [GNU LGPL 2.1 license](LICENSE) remains in force.

## Behavior

The Windows loader finds the IL2CPP export binder in the loaded x64
`UnityPlayer.dll`, checks its unwind ranges and shared `GetProcAddress`
resolver, and pairs its ordered bindings with public Unity SDK declarations.
Distributed instruction checks verify the native ABI before any alias is
activated. The original Windows export lookup remains the first choice.

Discovery runs once per process from Unity's API-binding path after
`GameAssembly.dll` loads. It reads the mapped PE images and loads Capstone 5
from the integration package at `BepInEx/compatibility/capstone.dll`. Doorstop
retains the validated aliases in memory for native lookups and CoreCLR imports.
It atomically writes `doorstop_api.ini` for the managed bootstrap and records
the result in `BepInEx/cache/compatibility/api-refresh.log`. Each launch
derives fresh aliases from the loaded images, independent of existing INIs.

Example integration configuration, relative to the game directory:

```ini
[Compatibility]
auto_refresh=true
```

No game binaries, precomputed game API maps, game metadata, symbol files, or
symbol-download implementation are included here. Native source changes contain
no build-specific game addresses. A future native API/metadata redesign can
still require changes to the discovery or normalization implementation.

## Build with existing Visual Studio tools

From the repository root in PowerShell on Windows:

```powershell
.\build-msvc.ps1 -Release -CapstoneIncludeDir C:\path\to\capstone\include
```

Prerequisites are an existing Visual Studio installation with the x64 C++ build
tools, Windows SDK, and MASM. The script locates that installation through
`vswhere`, enters its developer environment, generates proxy tail jumps from
upstream `src/windows/proxy/proxylist.txt`, and compiles/links the maintained
sources. It does not install a compiler, SDK, xmake, or other build system.

Outputs are `build/msvc-x64/winhttp.dll` and
`build/msvc-x64/.doorstop_version`. Omit `-Release` to enable verbose native
logging. Generated C, assembly, object files, and the export definition remain
under the ignored `build` directory. This custom build path is Windows x64 only.
The Capstone include directory must contain `capstone/capstone.h` from the
pinned decoder source. Omitting it builds the ordinary export path without
renamed API discovery. The original upstream xmake build files are retained
separately.

The maintained script uses normal static MSVC runtime startup and `/GS` stack
protection. `DOORSTOP_STANDARD_CRT` keeps Doorstop's custom allocation and file
functions separate from the standard runtime's internal functions. This mode
does not require users to install Visual Studio or a redistributable runtime.
Version resources identify the fork; the release build does not need an external
PDB. A `-BuildRoot` argument can select another development output directory.

To rebuild the integration package, use its documented build scripts and point
its native source input at this fork. The package provides Capstone, the
metadata bootstrap, BepInEx, and CoreCLR.

## License and distribution

Modified or added native C/header files carry dated modification
notices. [CHANGES.md](CHANGES.md) lists the changes. Preserve the original license
and copyright notices and provide the corresponding modified source when
redistributing the native binary under the applicable LGPL terms. This fork
does not change the licenses of separately distributed integration dependencies.
