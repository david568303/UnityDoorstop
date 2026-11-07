# Standard MSVC startup, stack protection and version resources, 2026-11-07.
# GNU LGPL 2.1; see LICENSE. Existing compiler tools are used without installation.
param([switch]$Release, [string]$BuildRoot, [string]$CapstoneIncludeDir)
$SourceRoot = $PSScriptRoot
if (-not $BuildRoot) { $BuildRoot = Join-Path $SourceRoot 'build/msvc-x64' }
$ErrorActionPreference = 'Stop'
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Existing Visual Studio C++ tools not found; no compiler will be installed.' }
Import-Module (Join-Path $vsRoot 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

# Preserve the maintained x64 proxy's argument-transparent assembly tail jumps.
$functions = @(Get-Content -LiteralPath (Join-Path $SourceRoot 'src/windows/proxy/proxylist.txt') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$c = [Collections.Generic.List[string]]::new()
$asm = [Collections.Generic.List[string]]::new()
$def = [Collections.Generic.List[string]]::new()
$c.Add('#include <windows.h>')
$asm.Add('option casemap:none')
$def.Add('EXPORTS')
foreach ($fn in $functions) {
    if ($fn -notmatch '^[A-Za-z_][A-Za-z_0-9]*$') { throw 'Invalid upstream proxy name.' }
    $c.Add("FARPROC proxy_$fn;")
    $asm.Add("EXTERN proxy_${fn}:QWORD")
    $def.Add("$fn=exp_$fn")
}
$c.Add('void load_functions(void *dll) {')
foreach ($fn in $functions) { $c.Add("proxy_$fn = GetProcAddress((HMODULE)dll, `"$fn`" );") }
$c.Add('}')
$asm.Add('.code')
foreach ($fn in $functions) {
    $asm.Add("exp_$fn PROC")
    $asm.Add("jmp QWORD PTR [proxy_$fn]")
    $asm.Add("exp_$fn ENDP")
}
$asm.Add('END')
$c | Set-Content -LiteralPath (Join-Path $BuildRoot 'proxy.c') -Encoding ascii
$asm | Set-Content -LiteralPath (Join-Path $BuildRoot 'proxy.asm') -Encoding ascii
$def | Set-Content -LiteralPath (Join-Path $BuildRoot 'doorstop.def') -Encoding ascii

# The maintained MSVC path omitted upstream's version resource. Restore it using
# the upstream template, with truthful identification of the modified project.
$info = Get-Content -LiteralPath (Join-Path $SourceRoot 'info.lua') -Raw
$values = @{ NAME='Doorstop'; DESCRIPTION='UnityDoorstop compatibility loader for CarX Drift Racing Online 2'; ORGANIZATION='NeighTools / CarX-Modding'; RELEASE='-CarX-compatibility' }
foreach ($field in @('MAJOR','MINOR','PATCH')) {
    if ($info -notmatch ($field.ToLowerInvariant() + '\s*=\s*(\d+)')) { throw "Upstream version is missing $field" }
    $values[$field] = $Matches[1]
}
$resource = Get-Content -LiteralPath (Join-Path $SourceRoot 'src/windows/build_tools/info.rc.in') -Raw
foreach ($field in $values.Keys) { $resource = $resource.Replace(('${' + $field + '}'), [string]$values[$field]) }
if ($resource -match '\$\{') { throw 'An upstream resource field was not expanded.' }
$resource | Set-Content -LiteralPath (Join-Path $BuildRoot 'info.rc') -Encoding ascii

Push-Location $BuildRoot
try {
    & ml64.exe /nologo /c /Fo'proxy_asm.obj' 'proxy.asm'
    if ($LASTEXITCODE -ne 0) { throw 'Proxy assembly build failed.' }
    & rc.exe /nologo /fo'info.res' 'info.rc'
    if ($LASTEXITCODE -ne 0) { throw 'Version resource compilation failed.' }
    $sources = @('src/windows/*.c','src/*.c','src/config/*.c','src/util/*.c','src/runtimes/*.c') | ForEach-Object {
        Get-ChildItem -Path (Join-Path $SourceRoot $_) -File | ForEach-Object { $_.FullName }
    }
    # Preserve loader code generation; use standard static CRT startup and /GS.
    # Custom heap/file routines are namespaced, keeping allocation ownership unchanged.
    $compileArgs = @('/nologo','/c','/O1','/Ob2','/GS','/MT','/GL-','/DUNICODE','/D_UNICODE','/DDOORSTOP_STANDARD_CRT','/W3')
    if ($CapstoneIncludeDir) {
        $CapstoneIncludeDir = (Resolve-Path -LiteralPath $CapstoneIncludeDir).Path
        if (-not (Test-Path -LiteralPath (Join-Path $CapstoneIncludeDir 'capstone/capstone.h') -PathType Leaf)) {
            throw 'CapstoneIncludeDir must contain the pinned Capstone 5 C headers.'
        }
        $compileArgs += @('/DDOORSTOP_NATIVE_API', ('/I' + $CapstoneIncludeDir))
    }
    if (-not $Release) { $compileArgs += '/DVERBOSE=1' }
    & cl.exe @compileArgs @sources 'proxy.c'
    if ($LASTEXITCODE -ne 0) { throw 'Native source compilation failed.' }
    $objects = @($sources | ForEach-Object { Join-Path $BuildRoot ([IO.Path]::GetFileNameWithoutExtension($_) + '.obj') })
    $objects += @((Join-Path $BuildRoot 'proxy.obj'),(Join-Path $BuildRoot 'proxy_asm.obj'))
    # /Brepro enables reproducible PE provenance; normal Windows mitigations and
    # an image checksum remain enabled. No external PDB or signing key is needed.
    & link.exe /nologo /dll /dynamicbase /highentropyva /nxcompat /Brepro /release /out:winhttp.dll /def:doorstop.def /opt:ref /opt:icf @objects info.res kernel32.lib shell32.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Native DLL linking failed.' }
    Set-Content -LiteralPath '.doorstop_version' -Value '4.6.0-binary-api-compatibility' -Encoding ascii
    $built = Join-Path $BuildRoot 'winhttp.dll'
    [ordered]@{sourceRoot=$SourceRoot; visualStudio=$vsRoot; path=$built; sha256=(Get-FileHash -LiteralPath $built -Algorithm SHA256).Hash.ToLowerInvariant(); bytes=(Get-Item -LiteralPath $built).Length} | ConvertTo-Json | Set-Content -LiteralPath 'build-result.json' -Encoding utf8
} finally { Pop-Location }

Write-Host "Built x64 Doorstop with $vsRoot"
Write-Host (Join-Path $BuildRoot 'winhttp.dll')
