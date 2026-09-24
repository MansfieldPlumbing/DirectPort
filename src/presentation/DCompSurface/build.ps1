[CmdletBinding()]
param(
    [string] $OutDir = (Join-Path $PSScriptRoot 'bin')
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$src = Join-Path $PSScriptRoot 'dcomp-surface.cpp'
$out = Join-Path $OutDir 'dcomp-surface.exe'

$cl = Get-Command cl.exe -ErrorAction Ignore
if ($cl) {
    & $cl.Source /nologo /std:c++20 /O2 /EHsc /DUNICODE /D_UNICODE `
        $src `
        "/Fe:$out" `
        /link /SUBSYSTEM:WINDOWS d3d11.lib dxgi.lib d2d1.lib dcomp.lib user32.lib gdi32.lib ole32.lib
    if ($LASTEXITCODE) { throw "cl.exe failed: $LASTEXITCODE" }
    return
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw 'cl.exe is not on PATH and vswhere.exe was not found.'
}

$install = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
if (-not $install) { throw 'A Visual Studio C++ toolchain was not found.' }

$devCmd = Join-Path $install 'Common7\Tools\VsDevCmd.bat'
$command = '"{0}" -no_logo -arch=x64 -host_arch=x64 && cl.exe /nologo /std:c++20 /O2 /EHsc /DUNICODE /D_UNICODE "{1}" /Fe:"{2}" /link /SUBSYSTEM:WINDOWS d3d11.lib dxgi.lib d2d1.lib dcomp.lib user32.lib gdi32.lib ole32.lib' -f $devCmd, $src, $out

& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE) { throw "Visual Studio build failed: $LASTEXITCODE" }

"BUILT $out"
