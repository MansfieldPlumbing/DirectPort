Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Start-Record {
    $root = $PSScriptRoot
    $manifest = Import-PowerShellDataFile -LiteralPath (Join-Path $root 'Record.psd1')
    $style = Import-PowerShellDataFile -LiteralPath (Join-Path $root ([string]$manifest.Style))

    if (-not ('Sexe.Record.AudioRecorder' -as [type])) {
        Add-Type -Path (Join-Path $root 'WasapiRecorder.cs')
    }

    $gpu = [DirectPort.PowerShell.GpuCanvas2D]::new(
        [int]$manifest.DefaultWidth,
        [int]$manifest.DefaultHeight,
        [string]$manifest.Name
    )

    $recorder = [Sexe.Record.AudioRecorder]::new()
    $devices = @([Sexe.Record.AudioRecorder]::GetDevices())

    $music = [Environment]::GetFolderPath([Environment+SpecialFolder]::MyMusic)
    if ([string]::IsNullOrWhiteSpace($music)) { $music = $HOME }
    $outputDirectory = Join-Path $music 'Recordings'
    [IO.Directory]::CreateDirectory($outputDirectory) | Out-Null

    $app = @{
        DeviceIndex     = 0
        PrevLeft       = $false
        Dirty          = $true
        Status         = if ($devices.Count) { 'Ready' } else { 'No active Windows audio endpoints found' }
        LastFile       = ''
        LastError      = ''
        OutputDirectory= $outputDirectory
        LastClock      = ''
        LastPeakBucket = -1
    }

    function Get-ClockText {
        if (-not $recorder.IsRecording) { return '00:00:00' }
        $elapsed = $recorder.Elapsed
        $hours = [int][Math]::Floor($elapsed.TotalHours)
        return '{0:00}:{1:00}:{2:00}' -f $hours, $elapsed.Minutes, $elapsed.Seconds
    }

    function Get-SelectedDevice {
        if ($devices.Count -eq 0) { return $null }
        if ($app.DeviceIndex -lt 0) { $app.DeviceIndex = $devices.Count - 1 }
        if ($app.DeviceIndex -ge $devices.Count) { $app.DeviceIndex = 0 }
        return $devices[$app.DeviceIndex]
    }

    function Move-Source([int]$delta) {
        if ($recorder.IsRecording -or $devices.Count -eq 0) { return }
        $app.DeviceIndex = ($app.DeviceIndex + $delta) % $devices.Count
        if ($app.DeviceIndex -lt 0) { $app.DeviceIndex += $devices.Count }
        $app.Status = 'Ready'
        $app.LastError = ''
        $app.Dirty = $true
    }

    function Stop-Capture {
        if (-not $recorder.IsRecording) { return }
        $path = $recorder.CurrentPath
        $recorder.Stop()
        if ($recorder.LastError) {
            $app.LastError = $recorder.LastError
            $app.Status = 'Capture stopped with an error'
        } else {
            $app.LastFile = $path
            $app.Status = 'Saved · ' + [IO.Path]::GetFileName($path)
        }
        $app.Dirty = $true
    }

    function Toggle-Capture {
        if ($recorder.IsRecording) {
            Stop-Capture
            return
        }

        $device = Get-SelectedDevice
        if ($null -eq $device) { return }

        $name = 'Recording {0}.wav' -f (Get-Date -Format 'yyyy-MM-dd HHmmss')
        $path = Join-Path $app.OutputDirectory $name
        try {
            $recorder.Start([string]$device.Id, [bool]$device.Loopback, $path)
            $app.Status = 'Recording'
            $app.LastError = ''
            $app.LastFile = ''
        }
        catch {
            $app.LastError = $_.Exception.Message
            $app.Status = 'Could not start capture'
        }
        $app.Dirty = $true
    }

    function Open-Recordings {
        $psi = [Diagnostics.ProcessStartInfo]::new()
        $psi.FileName = $app.OutputDirectory
        $psi.UseShellExecute = $true
        [void][Diagnostics.Process]::Start($psi)
    }

    function Test-Hit([single]$mx, [single]$my, [single]$x, [single]$y, [single]$width, [single]$height) {
        return ($mx -ge $x -and $mx -lt ($x + $width) -and $my -ge $y -and $my -lt ($y + $height))
    }

    try {
        while ($true) {
            $timeout = if ($recorder.IsRecording) { [uint32]50 } else { [uint32]150 }
            $frame = $gpu.Pump($(if ($app.Dirty) { [uint32]1 } else { $timeout }))
            if (-not $frame.Alive) { break }
            if ($frame.KeyCode -eq 27) { break }

            if ($frame.KeyCode -eq 32) { Toggle-Capture }
            if ($frame.KeyCode -eq 37) { Move-Source -1 }
            if ($frame.KeyCode -eq 39) { Move-Source 1 }

            [single]$w = $frame.Width
            [single]$h = $frame.Height
            [single]$pad = $style.Padding
            [single]$gap = $style.Gap
            [single]$radius = $style.CornerRadius

            [single]$timerY = $pad
            [single]$timerH = 116
            [single]$sourceY = $timerY + $timerH + $gap
            [single]$sourceH = 88
            [single]$statusY = $sourceY + $sourceH + $gap
            [single]$buttonH = 70
            [single]$buttonY = $h - $pad - $buttonH
            [single]$statusH = [Math]::Max([single]52, $buttonY - $gap - $statusY)

            [single]$arrowW = 52
            [single]$recordW = [Math]::Min([single]230, $w - ($pad * 2) - 118 - $gap)
            [single]$openW = $w - ($pad * 2) - $recordW - $gap
            [single]$recordX = $w - $pad - $recordW
            [single]$openX = $pad

            if ($frame.LeftDown -and -not $app.PrevLeft) {
                [single]$mx = $frame.MouseX
                [single]$my = $frame.MouseY
                if (Test-Hit $mx $my $pad $sourceY $arrowW $sourceH) {
                    Move-Source -1
                }
                elseif (Test-Hit $mx $my ($w - $pad - $arrowW) $sourceY $arrowW $sourceH) {
                    Move-Source 1
                }
                elseif (Test-Hit $mx $my $recordX $buttonY $recordW $buttonH) {
                    Toggle-Capture
                }
                elseif (Test-Hit $mx $my $openX $buttonY $openW $buttonH) {
                    Open-Recordings
                }
            }
            $app.PrevLeft = $frame.LeftDown

            if ($frame.ResizeSerial -ne 0) { $app.Dirty = $true }

            if ($recorder.IsRecording) {
                $clock = Get-ClockText
                $bucket = [int]([Math]::Min(1.0, [double]$recorder.Peak) * 30)
                if ($clock -ne $app.LastClock -or $bucket -ne $app.LastPeakBucket) {
                    $app.LastClock = $clock
                    $app.LastPeakBucket = $bucket
                    $app.Dirty = $true
                }
            }

            if (-not $app.Dirty) { continue }
            if (-not $gpu.BeginFrame([uint32]$style.Background)) { continue }

            $recording = [bool]$recorder.IsRecording
            $clockText = Get-ClockText
            $device = Get-SelectedDevice
            $deviceText = if ($null -ne $device) { [string]$device.Name } else { 'No audio source' }
            $formatText = if ($recorder.FormatDescription) { $recorder.FormatDescription } else { 'WAV · native device mix format' }

            $gpu.FillRect($pad, $timerY, $w - ($pad * 2), $timerH, [uint32]$style.Panel, $radius)
            $gpu.DrawText(
                $clockText,
                $pad + 10,
                $timerY + 18,
                $w - ($pad * 2) - 20,
                54,
                [uint32]$style.Text,
                [single]$style.TimerFontSize,
                [string]$style.Font,
                $true,
                1
            )

            [single]$meterX = $pad + 18
            [single]$meterY = $timerY + $timerH - 24
            [single]$meterW = $w - ($pad * 2) - 36
            [single]$meterH = 6
            $gpu.FillRect($meterX, $meterY, $meterW, $meterH, [uint32]$style.KeyAlt, [single]3)
            if ($recording) {
                [single]$levelW = $meterW * [single][Math]::Min(1.0, [double]$recorder.Peak)
                if ($levelW -gt 1) { $gpu.FillRect($meterX, $meterY, $levelW, $meterH, [uint32]$style.Record, [single]3) }
            }

            $gpu.FillRect($pad, $sourceY, $w - ($pad * 2), $sourceH, [uint32]$style.Key, $radius)
            $gpu.DrawRect($pad, $sourceY, $w - ($pad * 2), $sourceH, [uint32]$style.Border, [single]1, $radius)
            $gpu.DrawText('‹', $pad, $sourceY + 21, $arrowW, 48, [uint32]$style.MutedText, [single]28, [string]$style.Font, $true, 1)
            $gpu.DrawText('›', $w - $pad - $arrowW, $sourceY + 21, $arrowW, 48, [uint32]$style.MutedText, [single]28, [string]$style.Font, $true, 1)
            $gpu.DrawText('SOURCE', $pad + $arrowW, $sourceY + 10, $w - ($pad * 2) - ($arrowW * 2), 18, [uint32]$style.MutedText, [single]$style.SmallFontSize, [string]$style.Font, $true, 1)
            $gpu.DrawText($deviceText, $pad + $arrowW, $sourceY + 31, $w - ($pad * 2) - ($arrowW * 2), 44, [uint32]$style.Text, [single]$style.SourceFontSize, [string]$style.Font, $true, 1)

            $gpu.FillRect($pad, $statusY, $w - ($pad * 2), $statusH, [uint32]$style.Panel, $radius)
            $statusColor = if ($app.LastError) { [uint32]$style.Record } elseif ($recording) { [uint32]$style.Record } else { [uint32]$style.Accent }
            $gpu.DrawText([string]$app.Status, $pad + 14, $statusY + 10, $w - ($pad * 2) - 28, 24, $statusColor, [single]$style.SourceFontSize, [string]$style.Font, $false, 0)
            $detail = if ($app.LastError) { [string]$app.LastError } else { $formatText }
            $gpu.DrawText($detail, $pad + 14, $statusY + 34, $w - ($pad * 2) - 28, [Math]::Max([single]18, $statusH - 38), [uint32]$style.MutedText, [single]$style.SmallFontSize, [string]$style.Font, $false, 0)

            $gpu.FillRect($openX, $buttonY, $openW, $buttonH, [uint32]$style.Key, $radius)
            $gpu.DrawRect($openX, $buttonY, $openW, $buttonH, [uint32]$style.Border, [single]1, $radius)
            $gpu.DrawText('FOLDER', $openX, $buttonY + 23, $openW, 30, [uint32]$style.Text, [single]$style.SmallFontSize, [string]$style.Font, $true, 1)

            $recordBg = if ($recording) { [uint32]$style.RecordDark } else { [uint32]$style.Record }
            $recordLabel = if ($recording) { '■  STOP' } else { '●  RECORD' }
            $gpu.FillRect($recordX, $buttonY, $recordW, $buttonH, $recordBg, $radius)
            $gpu.DrawText($recordLabel, $recordX, $buttonY + 20, $recordW, 34, [uint32]$style.Text, [single]$style.ButtonFontSize, [string]$style.Font, $true, 1)

            [void]$gpu.EndFrame()
            $app.Dirty = $false
        }
    }
    finally {
        try { $recorder.Stop() } catch { }
        $recorder.Dispose()
        $gpu.Dispose()
    }
}

Export-ModuleMember -Function Start-Record
