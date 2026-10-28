# IL2CPP compatibility fork

This branch preserves the upstream UnityDoorstop history and is based on
[NeighTools/UnityDoorstop commit 1c3ece1](https://github.com/NeighTools/UnityDoorstop/commit/1c3ece1f1e6c20f4aa74d5402baabf8a0098d282).
The compatibility changes are maintained by David Salas Kalendar, dated
2026-10-28. The original [GNU LGPL 2.1 license](LICENSE) remains in force.

## Behavior

The Windows loader can resolve renamed IL2CPP exports using a canonical API map
produced from the paired `UnityPlayer.dll` and `GameAssembly.dll` binaries. It
validates the map format, discovery method, PE image identity, export name, and
export RVA before using an alias. It applies those aliases to Doorstop's native
lookups and CoreCLR imports. The original Windows export lookup remains the
first choice.

When the map is absent or stale and refresh is enabled, Doorstop starts the
configured Python helper from Unity's API-binding path, after GameAssembly has
loaded. It never starts that helper from `DllEntry`. The helper process has a
90-second wait limit. The helper and its dependencies are supplied by the
separate compatibility integration package, not by this native repository.

Example integration configuration, relative to the game directory:

```ini
[Compatibility]
auto_refresh=true
helper_script=BepInEx\compatibility\tools\prepare_api_map.py
python_executable=BepInEx\compatibility\python\python.exe
```

No game binaries, precomputed game API maps, game metadata, symbol files, or
symbol-download implementation are included here. Native source changes contain
no build-specific game addresses. A future native API/metadata redesign can
still require changes to the discovery or normalization implementation.

## Build with existing Visual Studio tools

From the repository root in PowerShell on Windows:

```powershell
.\build-msvc.ps1 -Release
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
The original upstream xmake build files are retained separately.

To rebuild the integration package, use its documented build scripts and point
its native source input at this fork. The native DLL alone does not supply the
API discovery helper, metadata bootstrap, BepInEx, Python, or CoreCLR.

## License and distribution

The six modified or added native C/header files carry dated modification
notices. [CHANGES.md](CHANGES.md) lists the changes. Preserve the original license
and copyright notices and provide the corresponding modified source when
redistributing the native binary under the applicable LGPL terms. This fork
does not change the licenses of separately distributed integration dependencies.
