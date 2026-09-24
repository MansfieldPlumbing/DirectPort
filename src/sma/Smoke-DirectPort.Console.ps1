[CmdletBinding()]
param(
    [string] $AssemblyPath = (Join-Path $PSScriptRoot 'bin\DirectPort.PowerShell.dll'),
    [string] $AtlasPng = $env:DIRECTPORT_ATLAS_PNG,
    [string] $MetricsJson = $env:DIRECTPORT_ATLAS_METRICS,
    [uint32] $Width = 1280,
    [uint32] $Height = 720,
    [uint32] $Columns = 143,
    [uint32] $Rows = 51,
    [double] $Seconds = 10,
    [switch] $InProcessView,
    [switch] $LaunchConsumer,
    [string] $ConsumerPath
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($AtlasPng) -or
    [string]::IsNullOrWhiteSpace($MetricsJson) -or
    -not (Test-Path -LiteralPath $AtlasPng) -or
    -not (Test-Path -LiteralPath $MetricsJson)) {
    throw 'Pass -AtlasPng and -MetricsJson, or set DIRECTPORT_ATLAS_PNG and DIRECTPORT_ATLAS_METRICS.'
}
if ($LaunchConsumer -and
    ([string]::IsNullOrWhiteSpace($ConsumerPath) -or -not (Test-Path -LiteralPath $ConsumerPath))) {
    throw 'Pass a valid -ConsumerPath when using -LaunchConsumer.'
}
[Reflection.Assembly]::LoadFrom($AssemblyPath) | Out-Null

$suffix = "${PID}_pwshconsole"
$textureName = "Global\D3D12_Texture_$suffix"
$fenceName = "Global\D3D12_Fence_$suffix"
$manifestName = "D3D12_Producer_Manifest_$PID"
$console = $null
$consumer = $null

try {
    $console = [DirectPort.PowerShell.GpuConsole]::new(
        $Width, $Height, ($Columns * $Rows),
        $textureName, $fenceName, $AtlasPng, $MetricsJson)
    $console.EnableManifest($manifestName)
    if ($InProcessView) { $console.Show('DirectPort PowerShell Console', $Width, $Height) }

    [uint32] $blank = ([uint32]7 -shl 16) -bor [uint32][char]' '
    [uint32[]] $cells = [uint32[]]::new($Columns * $Rows)
    [Array]::Fill($cells, $blank)
    $banner = 'POWERSHELL -> UINT32 CELLS -> D3D12/MSDF -> SHARED TEXTURE'
    for ($x = 0; $x -lt [Math]::Min($Columns, $banner.Length); $x++) {
        $fg = [uint32](9 + ($x % 7))
        $cells[2 * $Columns + $x] = ($fg -shl 16) -bor [uint32][char]$banner[$x]
    }
    for ($y = 5; $y -lt ($Rows - 2); $y++) {
        for ($x = 2; $x -lt ($Columns - 2); $x++) {
            $cp = if ((($x + $y) % 11) -eq 0) { [uint32]0x2588 } else { [uint32][char]([char](48 + (($x + $y) % 10))) }
            $fg = [uint32](($x + $y) -band 15)
            $bg = [uint32](($x / 9 + $y / 5) -band 15)
            $cells[$y * $Columns + $x] = ($bg -shl 24) -bor ($fg -shl 16) -bor $cp
        }
    }

    if ($LaunchConsumer) { $consumer = Start-Process -FilePath $ConsumerPath -PassThru }
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $buildTicks = [long]0
    [uint64] $value = 0
    $submitted = 0
    $lastAt = -1
    [uint32] $underAt = $blank
    while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
        if ($InProcessView -and -not $console.Pump(0).Alive) { break }
        $at = 10 * $Columns + ([int]($clock.Elapsed.TotalMilliseconds / 40) % $Columns)
        if ($lastAt -ge 0) { $cells[$lastAt] = $underAt }
        $underAt = $cells[$at]
        $cells[$at] = ([uint32]15 -shl 16) -bor [uint32][char]'@'
        $lastAt = $at
        $value++
        $before = [Diagnostics.Stopwatch]::GetTimestamp()
        $ok = $console.TryPresent($cells, $Columns, $Rows, [single]$clock.Elapsed.TotalSeconds, $value)
        $buildTicks += [Diagnostics.Stopwatch]::GetTimestamp() - $before
        if ($ok) {
            $submitted++
            if ($submitted -eq 1) { 'READY' }
        }
        [Threading.Thread]::Sleep(8)
    }
    $elapsed = $clock.Elapsed.TotalSeconds
    $clock.Stop()
    'FPS={0:N0} BUILD={1:N3}ms DROPPED={2}' -f ($submitted / $elapsed), (($buildTicks * 1000.0 / [Diagnostics.Stopwatch]::Frequency) / [Math]::Max(1, $submitted)), $console.DroppedFrames

    # Static mode: publish once, then perform no cadence work.
    $value++
    $null = $console.TryPresent($cells, $Columns, $Rows, 0, $value)
    [Threading.Thread]::Sleep(500)
    'IDLE'
}
finally {
    if ($null -ne $consumer -and -not $consumer.HasExited) {
        $null = $consumer.CloseMainWindow()
        if (-not $consumer.WaitForExit(2000)) { Stop-Process -Id $consumer.Id -Force }
    }
    if ($null -ne $console) { $console.Dispose() }
    'CLOSED'
}
