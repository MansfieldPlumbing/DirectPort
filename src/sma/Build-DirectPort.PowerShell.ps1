[CmdletBinding()]
param(
    [string] $DirectPortSource,
    [string] $DirectPortInclude,
    [string] $VcVars,
    [string] $DotnetPackVersion
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot
if (-not $DirectPortSource) { $DirectPortSource = Join-Path $repoRoot 'directportd3d12.cpp' }
if (-not $DirectPortInclude) { $DirectPortInclude = $repoRoot }
$out = Join-Path $PSScriptRoot 'bin'
$work = Join-Path $PSScriptRoot 'obj'
New-Item -ItemType Directory -Force -Path $out, $work | Out-Null

if (-not $VcVars) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer vswhere.exe was not found.' }
    $vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsRoot) { throw 'A Visual Studio installation with the C++ x64 tools was not found.' }
    $VcVars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
}

$refPackRoot = Join-Path $env:ProgramFiles 'dotnet\packs\Microsoft.NETCore.App.Ref'
if (-not $DotnetPackVersion) {
    $DotnetPackVersion = Get-ChildItem -LiteralPath $refPackRoot -Directory |
        Where-Object Name -Like '11.*' |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1 -ExpandProperty Name
    if (-not $DotnetPackVersion) { throw '.NET 11 reference packs were not found.' }
}

# Import the VS environment once, then invoke cl/link directly so quoting is
# deterministic even when the repo lives under a path containing spaces.
& cmd.exe /d /s /c "`"$VcVars`" >nul && set" | ForEach-Object {
    $at = $_.IndexOf('=')
    if ($at -gt 0) { [Environment]::SetEnvironmentVariable($_.Substring(0, $at), $_.Substring($at + 1), 'Process') }
}

$ref = Join-Path $refPackRoot "$DotnetPackVersion\ref\net11.0"
$hostNative = Join-Path $env:ProgramFiles "dotnet\packs\Microsoft.NETCore.App.Host.win-x64\$DotnetPackVersion\runtimes\win-x64\native"
foreach ($required in @($DirectPortSource, $DirectPortInclude, $ref, $hostNative)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing build input: $required" }
}

$cl = 'cl.exe'
& $cl /nologo /std:c++20 /O2 /EHsc /MD /c $DirectPortSource "/Fo:$work\directport-native.obj"
if ($LASTEXITCODE) { throw "DirectPort native compile failed: $LASTEXITCODE" }
& $cl /nologo /std:c++20 /O2 /EHsc /MD /c "$PSScriptRoot\DirectPort.Shader.Native.cpp" "/Fo:$work\directport-shader-native.obj"
if ($LASTEXITCODE) { throw "Shader native compile failed: $LASTEXITCODE" }
& $cl /nologo /std:c++20 /O2 /EHsc /MD /c "$PSScriptRoot\DirectPort.Console.Native.cpp" "/Fo:$work\directport-console-native.obj"
if ($LASTEXITCODE) { throw "Console native compile failed: $LASTEXITCODE" }
& $cl /nologo /std:c++20 /O2 /EHsc /MD /c "$PSScriptRoot\DirectPort.Canvas2D.Native.cpp" "/Fo:$work\directport-canvas2d-native.obj"
if ($LASTEXITCODE) { throw "Canvas2D native compile failed: $LASTEXITCODE" }

& $cl /nologo /std:c++20 /O2 /EHa /MD /clr:netcore /c "$PSScriptRoot\DirectPort.PowerShell.cpp" `
    "/I$DirectPortInclude" "/I$PSScriptRoot" "/AI$ref" `
    "/FU$ref\System.Runtime.dll" "/FU$ref\System.Runtime.InteropServices.dll" `
    "/Fo:$work\directport-managed.obj"
if ($LASTEXITCODE) { throw "Managed bridge compile failed: $LASTEXITCODE" }

& link.exe /nologo /DLL /MACHINE:X64 "/OUT:$out\DirectPort.PowerShell.dll" `
    "$work\directport-managed.obj" "$work\directport-native.obj" `
    "$work\directport-shader-native.obj" "$work\directport-console-native.obj" `
    "$work\directport-canvas2d-native.obj" `
    "/LIBPATH:$hostNative" ijwhost.lib msvcrt.lib vcruntime.lib ucrt.lib `
    kernel32.lib user32.lib gdi32.lib advapi32.lib ole32.lib windowscodecs.lib Synchronization.lib `
    d3d12.lib d3d11.lib d2d1.lib dwrite.lib d3dcompiler.lib dxgi.lib
if ($LASTEXITCODE) { throw "DirectPort.PowerShell link failed: $LASTEXITCODE" }

# C++/CLI on CoreCLR resolves this native bootstrap beside the mixed assembly.
Copy-Item -LiteralPath (Join-Path $hostNative 'ijwhost.dll') -Destination (Join-Path $out 'ijwhost.dll') -Force

"BUILT $out\DirectPort.PowerShell.dll"
