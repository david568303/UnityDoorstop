param([switch]$Release)
$ErrorActionPreference = 'Stop'
$sourceRoot = $PSScriptRoot
$buildRoot = Join-Path $sourceRoot 'build/msvc-x64'
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Existing Visual Studio C++ build tools were not found. No compiler will be installed.' }
Import-Module (Join-Path $vsRoot 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

# Assembly tail jumps preserve all native arguments and the return address.
$functions = @(Get-Content -LiteralPath (Join-Path $sourceRoot 'src/windows/proxy/proxylist.txt') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$c = [Collections.Generic.List[string]]::new()
$asm = [Collections.Generic.List[string]]::new()
$def = [Collections.Generic.List[string]]::new()
$c.Add('#include <windows.h>')
$asm.Add('option casemap:none')
$def.Add('EXPORTS')
foreach ($fn in $functions) {
    if ($fn -notmatch '^[A-Za-z_][A-Za-z_0-9]*$') { throw 'Invalid upstream proxy function name.' }
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
$c | Set-Content -LiteralPath (Join-Path $buildRoot 'proxy.c') -Encoding ascii
$asm | Set-Content -LiteralPath (Join-Path $buildRoot 'proxy.asm') -Encoding ascii
$def | Set-Content -LiteralPath (Join-Path $buildRoot 'doorstop.def') -Encoding ascii

Push-Location $buildRoot
try {
    & ml64.exe /nologo /c /Fo'proxy_asm.obj' 'proxy.asm'
    if ($LASTEXITCODE -ne 0) { throw 'Proxy assembly build failed.' }
    $sources = @('src/windows/*.c', 'src/*.c', 'src/config/*.c', 'src/util/*.c', 'src/runtimes/*.c') | ForEach-Object {
        Get-ChildItem -Path (Join-Path $sourceRoot $_) -File | ForEach-Object { $_.FullName }
    }
    $compileArgs = @('/nologo','/c','/O1','/Ob2','/GS-','/MT','/GL-','/DUNICODE','/D_UNICODE','/W3')
    if (-not $Release) { $compileArgs += '/DVERBOSE=1' }
    & cl.exe @compileArgs @sources 'proxy.c'
    if ($LASTEXITCODE -ne 0) { throw 'Doorstop compilation failed.' }
    $objects = @($sources | ForEach-Object { Join-Path $buildRoot ([IO.Path]::GetFileNameWithoutExtension($_) + '.obj') })
    $objects += @((Join-Path $buildRoot 'proxy.obj'), (Join-Path $buildRoot 'proxy_asm.obj'))
    & link.exe /nologo /dll /nodefaultlib /entry:DllEntry /dynamicbase /out:winhttp.dll /def:doorstop.def @objects kernel32.lib shell32.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Doorstop linking failed.' }
    Set-Content -LiteralPath '.doorstop_version' -Value '4.6.0-binary-api-compatibility' -Encoding ascii
} finally { Pop-Location }
Write-Host "Built x64 Doorstop with $vsRoot"
Write-Host (Join-Path $buildRoot 'winhttp.dll')
