Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Record.Sexe is intentionally one intrinsic script. WASAPI.abi.h is parsed as
# data; the audio path is implemented entirely by this script.

$script:AbiAssembly = $null
$script:AbiModule = $null
$script:AbiDelegateTypes = @{}
$script:Ole32 = [IntPtr]::Zero
$script:Native = @{}
$script:Abi = $null

function Read-AbiHeader {
    param([Parameter(Mandatory)][string]$Path)

    $constants = @{}
    $guids = @{}
    $slots = @{}

    foreach ($line in [IO.File]::ReadLines($Path)) {
        if ($line -match '^\s*#define\s+ABI_CONST_([A-Za-z0-9_]+)\s+([^\s/]+)') {
            $name = $Matches[1]
            $text = $Matches[2].Trim() -replace '[uUlL]+$',''
            [UInt64]$value = if ($text -match '^0[xX]([0-9A-Fa-f]+)$') {
                [Convert]::ToUInt64($Matches[1], 16)
            } else {
                [Convert]::ToUInt64($text, 10)
            }
            $constants[$name] = $value
            continue
        }
        if ($line -match '^\s*#define\s+ABI_GUID_([A-Za-z0-9_]+)\s+"([0-9A-Fa-f-]{36})"') {
            $guids[$Matches[1]] = [Guid]::Parse($Matches[2])
            continue
        }
        if ($line -match '^\s*#define\s+ABI_SLOT_([A-Za-z0-9]+)_([A-Za-z0-9_]+)\s+(\d+)') {
            $slots[($Matches[1] + '.' + $Matches[2])] = [int]$Matches[3]
        }
    }

    foreach ($required in @(
        'CLSCTX_ALL','DEVICE_STATE_ACTIVE','eRender','eCapture','STGM_READ',
        'PKEY_Device_FriendlyName_PID','AUDCLNT_SHAREMODE_SHARED',
        'AUDCLNT_STREAMFLAGS_LOOPBACK','AUDCLNT_BUFFERFLAGS_SILENT'
    )) {
        if (-not $constants.ContainsKey($required)) { throw "ABI constant missing: $required" }
    }
    foreach ($required in @(
        'CLSID_MMDeviceEnumerator','IID_IMMDeviceEnumerator','IID_IAudioClient',
        'IID_IAudioCaptureClient','PKEY_Device_FriendlyName_FMTID'
    )) {
        if (-not $guids.ContainsKey($required)) { throw "ABI GUID missing: $required" }
    }

    return [pscustomobject]@{ Constants = $constants; Guids = $guids; Slots = $slots; Path = $Path }
}

function Initialize-DelegateEmitter {
    if ($null -ne $script:AbiModule) { return }
    $name = [Reflection.AssemblyName]::new('Record.Sexe.DynamicAbi')
    $script:AbiAssembly = [Reflection.Emit.AssemblyBuilder]::DefineDynamicAssembly(
        $name, [Reflection.Emit.AssemblyBuilderAccess]::Run
    )
    $script:AbiModule = $script:AbiAssembly.DefineDynamicModule('Record.Sexe.DynamicAbi')
}

function New-AbiDelegateType {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][Type]$ReturnType,
        [Parameter(Mandatory)][Type[]]$ParameterTypes
    )

    if ($script:AbiDelegateTypes.ContainsKey($Name)) { return $script:AbiDelegateTypes[$Name] }
    Initialize-DelegateEmitter

    $attrs = [Reflection.TypeAttributes]::Class -bor
             [Reflection.TypeAttributes]::Public -bor
             [Reflection.TypeAttributes]::Sealed
    $tb = $script:AbiModule.DefineType($Name, $attrs, [MulticastDelegate])

    $ctor = $tb.DefineConstructor(
        [Reflection.MethodAttributes]::RTSpecialName -bor [Reflection.MethodAttributes]::HideBySig -bor [Reflection.MethodAttributes]::Public,
        [Reflection.CallingConventions]::Standard,
        [Type[]]@([object],[IntPtr])
    )
    $ctor.SetImplementationFlags([Reflection.MethodImplAttributes]::Runtime -bor [Reflection.MethodImplAttributes]::Managed)

    $invoke = $tb.DefineMethod(
        'Invoke',
        [Reflection.MethodAttributes]::Public -bor [Reflection.MethodAttributes]::HideBySig -bor [Reflection.MethodAttributes]::NewSlot -bor [Reflection.MethodAttributes]::Virtual,
        $ReturnType,
        $ParameterTypes
    )
    $invoke.SetImplementationFlags([Reflection.MethodImplAttributes]::Runtime -bor [Reflection.MethodImplAttributes]::Managed)

    $type = $tb.CreateType()
    $script:AbiDelegateTypes[$Name] = $type
    return $type
}

function Initialize-NativeAbi {
    param([Parameter(Mandatory)]$Abi)

    if ($script:Ole32 -ne [IntPtr]::Zero) { return }

    $I = [IntPtr]
    $IRef = [IntPtr].MakeByRefType()
    $U32Ref = [UInt32].MakeByRefType()
    $U64Ref = [UInt64].MakeByRefType()

    $types = @{
        CoInitializeEx = (New-AbiDelegateType -Name 'D_CoInitializeEx' -ReturnType ([int]) -ParameterTypes ([Type[]]@($I,[uint32])))
        CoUninitialize = (New-AbiDelegateType -Name 'D_CoUninitialize' -ReturnType ([void]) -ParameterTypes ([Type[]]@()))
        CoCreateInstance = (New-AbiDelegateType -Name 'D_CoCreateInstance' -ReturnType ([int]) -ParameterTypes ([Type[]]@($I,$I,[uint32],$I,$IRef)))
        PropVariantClear = (New-AbiDelegateType -Name 'D_PropVariantClear' -ReturnType ([int]) -ParameterTypes ([Type[]]@($I)))
        Release = (New-AbiDelegateType -Name 'D_Release' -ReturnType ([uint32]) -ParameterTypes ([Type[]]@($I)))
        EnumAudioEndpoints = (New-AbiDelegateType -Name 'D_EnumAudioEndpoints' -ReturnType ([int]) -ParameterTypes ([Type[]]@($I,[int],[uint32],$IRef)))
        GetDevice = (New-AbiDelegateType -Name 'D_GetDevice' -ReturnType ([int]) -ParameterTypes ([Type[]]@($I,$I,$IRef)))
        GetCount = (New-AbiDelegateType -Name 'D_GetCount' -ReturnType ([int]) -ParameterTypes ([Type[]]@($I,$U32Ref)))
        Item = (New-AbiDelegateType -Name 'D_Item' -ReturnType ([int]) -ParameterTypes ([Type[]]@($I,[uint32],$IRef)))
        Activate = (New-AbiDelegateType -Name 'D_Activate' -ReturnType ([int]) -ParameterTypes ([Type[]]@($I,$I,[uint32],$I,$IRef)))
        OpenPropertyStore = (New-AbiDelegateType -Name 'D_OpenPropertyStore' -ReturnType ([int]) -ParameterTypes ([Type[]]@($I,[uint32],$IRef)))
        GetId = (New-AbiDelegateType -Name 'D_GetId' -ReturnType ([int]) -ParameterTypes ([Type[]]@($I,$IRef)))
        GetValue = (New-AbiDelegateType -Name 'D_GetValue' -ReturnType ([int]) -ParameterTypes ([Type[]]@($I,$I,$I)))
        AudioInitialize = (New-AbiDelegateType -Name 'D_AudioInitialize' -ReturnType ([int]) -ParameterTypes ([Type[]]@($I,[int],[uint32],[long],[long],$I,$I)))
        GetMixFormat = (New-AbiDelegateType -Name 'D_GetMixFormat' -ReturnType ([int]) -ParameterTypes ([Type[]]@($I,$IRef)))
        NoArgHr = (New-AbiDelegateType -Name 'D_NoArgHr' -ReturnType ([int]) -ParameterTypes ([Type[]]@($I)))
        GetService = (New-AbiDelegateType -Name 'D_GetService' -ReturnType ([int]) -ParameterTypes ([Type[]]@($I,$I,$IRef)))
        GetNextPacketSize = (New-AbiDelegateType -Name 'D_GetNextPacketSize' -ReturnType ([int]) -ParameterTypes ([Type[]]@($I,$U32Ref)))
        GetBuffer = (New-AbiDelegateType -Name 'D_GetBuffer' -ReturnType ([int]) -ParameterTypes ([Type[]]@($I,$IRef,$U32Ref,$U32Ref,$U64Ref,$U64Ref)))
        ReleaseBuffer = (New-AbiDelegateType -Name 'D_ReleaseBuffer' -ReturnType ([int]) -ParameterTypes ([Type[]]@($I,[uint32])))
    }

    $script:AbiTypes = $types
    $script:Ole32 = [Runtime.InteropServices.NativeLibrary]::Load('ole32.dll')
    foreach ($name in @('CoInitializeEx','CoUninitialize','CoCreateInstance','PropVariantClear')) {
        $p = [Runtime.InteropServices.NativeLibrary]::GetExport($script:Ole32, $name)
        $script:Native[$name] = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($p, $types[$name])
    }
}

function Get-ComDelegate {
    param(
        [Parameter(Mandatory)][IntPtr]$Object,
        [Parameter(Mandatory)][string]$Slot,
        [Parameter(Mandatory)][Type]$DelegateType
    )
    if ($Object -eq [IntPtr]::Zero) { throw "Null COM pointer for $Slot" }
    if (-not $script:Abi.Slots.ContainsKey($Slot)) { throw "ABI slot missing: $Slot" }
    $vtbl = [Runtime.InteropServices.Marshal]::ReadIntPtr($Object)
    $fn = [Runtime.InteropServices.Marshal]::ReadIntPtr(
        $vtbl,
        [IntPtr]::Size * [int]$script:Abi.Slots[$Slot]
    )
    return [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($fn, $DelegateType)
}

function Assert-HResult {
    param([int]$HResult,[string]$Operation)
    if ($HResult -lt 0) {
        throw [Runtime.InteropServices.COMException]::new(
            ('{0} failed (0x{1:X8})' -f $Operation, [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$HResult), 0)),
            $HResult
        )
    }
}

function New-GuidPointer {
    param([Parameter(Mandatory)][Guid]$Guid)
    $p = [Runtime.InteropServices.Marshal]::AllocHGlobal(16)
    [Runtime.InteropServices.Marshal]::Copy($Guid.ToByteArray(), 0, $p, 16)
    return $p
}

function Release-ComPointer {
    param([IntPtr]$Pointer)
    if ($Pointer -eq [IntPtr]::Zero) { return }
    try {
        $release = Get-ComDelegate $Pointer 'IUnknown.Release' $script:AbiTypes.Release
        [void]$release.Invoke($Pointer)
    } catch { }
}

function Enter-ComApartment {
    $hr = $script:Native.CoInitializeEx.Invoke(
        [IntPtr]::Zero,
        [uint32]$script:Abi.Constants.COINIT_MULTITHREADED
    )
    $hrBits = [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$hr), 0)
    if ($hrBits -eq [uint32]$script:Abi.Constants.RPC_E_CHANGED_MODE) { return $false }
    Assert-HResult $hr 'CoInitializeEx'
    return $true
}

function New-MMDeviceEnumerator {
    $clsid = New-GuidPointer $script:Abi.Guids.CLSID_MMDeviceEnumerator
    $iid = New-GuidPointer $script:Abi.Guids.IID_IMMDeviceEnumerator
    [IntPtr]$enumerator = [IntPtr]::Zero
    try {
        $hr = $script:Native.CoCreateInstance.Invoke(
            $clsid,
            [IntPtr]::Zero,
            [uint32]$script:Abi.Constants.CLSCTX_ALL,
            $iid,
            [ref]$enumerator
        )
        Assert-HResult $hr 'CoCreateInstance(MMDeviceEnumerator)'
        return $enumerator
    }
    finally {
        [Runtime.InteropServices.Marshal]::FreeHGlobal($clsid)
        [Runtime.InteropServices.Marshal]::FreeHGlobal($iid)
    }
}

function Get-DeviceId {
    param([Parameter(Mandatory)][IntPtr]$Device)
    [IntPtr]$idPtr = [IntPtr]::Zero
    $getId = Get-ComDelegate $Device 'IMMDevice.GetId' $script:AbiTypes.GetId
    Assert-HResult ($getId.Invoke($Device, [ref]$idPtr)) 'IMMDevice.GetId'
    try { return [Runtime.InteropServices.Marshal]::PtrToStringUni($idPtr) }
    finally { if ($idPtr -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::FreeCoTaskMem($idPtr) } }
}

function Get-DeviceFriendlyName {
    param([Parameter(Mandatory)][IntPtr]$Device)

    [IntPtr]$store = [IntPtr]::Zero
    $open = Get-ComDelegate $Device 'IMMDevice.OpenPropertyStore' $script:AbiTypes.OpenPropertyStore
    Assert-HResult ($open.Invoke($Device, [uint32]$script:Abi.Constants.STGM_READ, [ref]$store)) 'IMMDevice.OpenPropertyStore'

    $key = [Runtime.InteropServices.Marshal]::AllocHGlobal(20)
    $pv = [Runtime.InteropServices.Marshal]::AllocHGlobal(32)
    try {
        $fmtid = $script:Abi.Guids.PKEY_Device_FriendlyName_FMTID.ToByteArray()
        [Runtime.InteropServices.Marshal]::Copy($fmtid, 0, $key, 16)
        [Runtime.InteropServices.Marshal]::WriteInt32($key, 16, [int]$script:Abi.Constants.PKEY_Device_FriendlyName_PID)
        for ($i = 0; $i -lt 32; $i += 4) { [Runtime.InteropServices.Marshal]::WriteInt32($pv, $i, 0) }

        $getValue = Get-ComDelegate $store 'IPropertyStore.GetValue' $script:AbiTypes.GetValue
        Assert-HResult ($getValue.Invoke($store, $key, $pv)) 'IPropertyStore.GetValue(PKEY_Device_FriendlyName)'
        $vt = [Runtime.InteropServices.Marshal]::ReadInt16($pv, 0)
        $value = [Runtime.InteropServices.Marshal]::ReadIntPtr($pv, 8)
        if ($vt -eq 31 -and $value -ne [IntPtr]::Zero) {
            return [Runtime.InteropServices.Marshal]::PtrToStringUni($value)
        }
        return 'Audio device'
    }
    finally {
        try { [void]$script:Native.PropVariantClear.Invoke($pv) } catch { }
        [Runtime.InteropServices.Marshal]::FreeHGlobal($pv)
        [Runtime.InteropServices.Marshal]::FreeHGlobal($key)
        Release-ComPointer $store
    }
}

function Get-AudioDevices {
    $result = [Collections.Generic.List[object]]::new()
    $enumerator = New-MMDeviceEnumerator
    try {
        foreach ($flow in @(
            [pscustomobject]@{ Value = [int]$script:Abi.Constants.eRender; Prefix = 'System · '; Loopback = $true },
            [pscustomobject]@{ Value = [int]$script:Abi.Constants.eCapture; Prefix = 'Input · '; Loopback = $false }
        )) {
            [IntPtr]$collection = [IntPtr]::Zero
            $enum = Get-ComDelegate $enumerator 'IMMDeviceEnumerator.EnumAudioEndpoints' $script:AbiTypes.EnumAudioEndpoints
            Assert-HResult ($enum.Invoke(
                $enumerator,
                $flow.Value,
                [uint32]$script:Abi.Constants.DEVICE_STATE_ACTIVE,
                [ref]$collection
            )) 'IMMDeviceEnumerator.EnumAudioEndpoints'

            try {
                [uint32]$count = 0
                $getCount = Get-ComDelegate $collection 'IMMDeviceCollection.GetCount' $script:AbiTypes.GetCount
                Assert-HResult ($getCount.Invoke($collection, [ref]$count)) 'IMMDeviceCollection.GetCount'

                $item = Get-ComDelegate $collection 'IMMDeviceCollection.Item' $script:AbiTypes.Item
                for ([uint32]$i = 0; $i -lt $count; $i++) {
                    [IntPtr]$device = [IntPtr]::Zero
                    Assert-HResult ($item.Invoke($collection, $i, [ref]$device)) 'IMMDeviceCollection.Item'
                    try {
                        $result.Add([pscustomobject]@{
                            Id = Get-DeviceId $device
                            Name = $flow.Prefix + (Get-DeviceFriendlyName $device)
                            Loopback = [bool]$flow.Loopback
                        })
                    }
                    finally { Release-ComPointer $device }
                }
            }
            finally { Release-ComPointer $collection }
        }
    }
    finally { Release-ComPointer $enumerator }
    return $result.ToArray()
}

function Get-WaveFormatInfo {
    param([Parameter(Mandatory)][IntPtr]$FormatPointer)

    [uint16]$tag = [uint16][Runtime.InteropServices.Marshal]::ReadInt16($FormatPointer, 0)
    [uint16]$channels = [uint16][Runtime.InteropServices.Marshal]::ReadInt16($FormatPointer, 2)
    [uint32]$rate = [uint32][Runtime.InteropServices.Marshal]::ReadInt32($FormatPointer, 4)
    [uint16]$block = [uint16][Runtime.InteropServices.Marshal]::ReadInt16($FormatPointer, 12)
    [uint16]$bits = [uint16][Runtime.InteropServices.Marshal]::ReadInt16($FormatPointer, 14)
    [uint16]$cb = [uint16][Runtime.InteropServices.Marshal]::ReadInt16($FormatPointer, 16)
    $size = 18 + [int]$cb
    if ($size -lt 18 -or $size -gt 256) { throw "Unexpected WASAPI mix-format size: $size" }

    $bytes = [byte[]]::new($size)
    [Runtime.InteropServices.Marshal]::Copy($FormatPointer, $bytes, 0, $size)

    $kind = 0
    if ($tag -eq [uint16]$script:Abi.Constants.WAVE_FORMAT_PCM) { $kind = 1 }
    elseif ($tag -eq [uint16]$script:Abi.Constants.WAVE_FORMAT_IEEE_FLOAT) { $kind = 3 }
    elseif ($tag -eq [uint16]$script:Abi.Constants.WAVE_FORMAT_EXTENSIBLE -and $size -ge 40) {
        $subtypeData1 = [BitConverter]::ToInt32($bytes, 24)
        if ($subtypeData1 -eq 1) { $kind = 1 }
        elseif ($subtypeData1 -eq 3) { $kind = 3 }
    }

    $sample = if ($kind -eq 3) { 'float' } elseif ($kind -eq 1) { 'PCM' } else { 'native' }
    return [pscustomobject]@{
        Bytes = $bytes
        FormatTag = $tag
        Channels = $channels
        SampleRate = $rate
        BlockAlign = $block
        Bits = $bits
        Kind = $kind
        Description = ('{0:0.#} kHz · {1} ch · {2}-bit {3}' -f ($rate / 1000.0), $channels, $bits, $sample)
    }
}

function Write-WaveHeader {
    param([IO.BinaryWriter]$Writer,[byte[]]$FormatBytes)
    $ascii = [Text.Encoding]::ASCII
    $Writer.Write($ascii.GetBytes('RIFF'))
    $Writer.Write([uint32]0)
    $Writer.Write($ascii.GetBytes('WAVE'))
    $Writer.Write($ascii.GetBytes('fmt '))
    $Writer.Write([uint32]$FormatBytes.Length)
    $Writer.Write($FormatBytes, 0, $FormatBytes.Length)
    if (($FormatBytes.Length -band 1) -ne 0) { $Writer.Write([byte]0) }
    $Writer.Write($ascii.GetBytes('data'))
    $position = $Writer.BaseStream.Position
    $Writer.Write([uint32]0)
    return [long]$position
}

function Complete-WaveHeader {
    param([IO.BinaryWriter]$Writer,[IO.FileStream]$File,[long]$DataSizePosition,[long]$DataBytes)
    if ($DataBytes -gt ([uint32]::MaxValue - 64)) { throw 'Recording exceeded the 4 GiB RIFF/WAV limit.' }
    $end = $File.Length
    $File.Position = 4
    $Writer.Write([uint32]($end - 8))
    $File.Position = $DataSizePosition
    $Writer.Write([uint32]$DataBytes)
    $File.Position = $end
}

function Measure-Peak {
    param([byte[]]$Bytes,$Format)
    if ($Bytes.Length -eq 0 -or $Format.BlockAlign -eq 0) { return [single]0 }

    $frames = [Math]::Max(1, [int]($Bytes.Length / $Format.BlockAlign))
    $strideFrames = [Math]::Max(1, [int]($frames / 160))
    $step = [Math]::Max([int]$Format.BlockAlign, [int]$Format.BlockAlign * $strideFrames)
    [double]$max = 0

    for ($i = 0; $i -lt $Bytes.Length; $i += $step) {
        [double]$v = 0
        if ($Format.Kind -eq 3 -and $Format.Bits -eq 32 -and ($i + 3) -lt $Bytes.Length) {
            $x = [BitConverter]::ToSingle($Bytes, $i)
            if (-not [single]::IsNaN($x)) { $v = [Math]::Abs([double]$x) }
        }
        elseif ($Format.Kind -eq 1 -and $Format.Bits -eq 16 -and ($i + 1) -lt $Bytes.Length) {
            $v = [Math]::Abs([BitConverter]::ToInt16($Bytes, $i) / 32768.0)
        }
        elseif ($Format.Kind -eq 1 -and $Format.Bits -eq 24 -and ($i + 2) -lt $Bytes.Length) {
            $x = [int]$Bytes[$i] -bor ([int]$Bytes[$i+1] -shl 8) -bor ([int]$Bytes[$i+2] -shl 16)
            if ($x -ge 0x800000) { $x -= 0x1000000 }
            $v = [Math]::Abs($x / 8388608.0)
        }
        elseif ($Format.Kind -eq 1 -and $Format.Bits -eq 32 -and ($i + 3) -lt $Bytes.Length) {
            $v = [Math]::Abs([BitConverter]::ToInt32($Bytes, $i) / 2147483648.0)
        }
        if ($v -gt $max) { $max = $v }
    }
    return [single][Math]::Min(1.0, $max)
}

function New-RecorderState {
    return @{
        IsRecording = $false
        Peak = [single]0
        LastError = ''
        CurrentPath = ''
        FormatDescription = ''
        StartedUtc = [DateTime]::MinValue
        DataBytes = [long]0
        Enumerator = [IntPtr]::Zero
        Device = [IntPtr]::Zero
        AudioClient = [IntPtr]::Zero
        CaptureClient = [IntPtr]::Zero
        FormatPointer = [IntPtr]::Zero
        Format = $null
        File = $null
        Writer = $null
        DataSizePosition = [long]0
        Written = [long]0
    }
}

function Stop-WasapiCapture {
    param([Parameter(Mandatory)][hashtable]$Recorder)

    if ($Recorder.AudioClient -ne [IntPtr]::Zero) {
        try {
            $stop = Get-ComDelegate $Recorder.AudioClient 'IAudioClient.Stop' $script:AbiTypes.NoArgHr
            $hr = $stop.Invoke($Recorder.AudioClient)
            if ($hr -lt 0 -and -not $Recorder.LastError) { Assert-HResult $hr 'IAudioClient.Stop' }
        } catch { if (-not $Recorder.LastError) { $Recorder.LastError = $_.Exception.Message } }
    }

    try {
        if ($null -ne $Recorder.Writer -and $null -ne $Recorder.File) {
            $Recorder.Writer.Flush()
            Complete-WaveHeader $Recorder.Writer $Recorder.File $Recorder.DataSizePosition $Recorder.Written
            $Recorder.Writer.Flush()
        }
    } catch { if (-not $Recorder.LastError) { $Recorder.LastError = $_.Exception.Message } }

    if ($null -ne $Recorder.Writer) { try { $Recorder.Writer.Dispose() } catch { } }
    elseif ($null -ne $Recorder.File) { try { $Recorder.File.Dispose() } catch { } }

    Release-ComPointer $Recorder.CaptureClient
    Release-ComPointer $Recorder.AudioClient
    Release-ComPointer $Recorder.Device
    Release-ComPointer $Recorder.Enumerator
    if ($Recorder.FormatPointer -ne [IntPtr]::Zero) {
        [Runtime.InteropServices.Marshal]::FreeCoTaskMem($Recorder.FormatPointer)
    }

    $Recorder.IsRecording = $false
    $Recorder.Peak = [single]0
    $Recorder.Enumerator = [IntPtr]::Zero
    $Recorder.Device = [IntPtr]::Zero
    $Recorder.AudioClient = [IntPtr]::Zero
    $Recorder.CaptureClient = [IntPtr]::Zero
    $Recorder.FormatPointer = [IntPtr]::Zero
    $Recorder.File = $null
    $Recorder.Writer = $null
}

function Start-WasapiCapture {
    param(
        [Parameter(Mandatory)][hashtable]$Recorder,
        [Parameter(Mandatory)][string]$DeviceId,
        [Parameter(Mandatory)][bool]$Loopback,
        [Parameter(Mandatory)][string]$OutputPath
    )

    if ($Recorder.IsRecording) { throw 'Recorder is already running.' }
    $Recorder.LastError = ''
    $Recorder.CurrentPath = $OutputPath
    $Recorder.FormatDescription = ''
    $Recorder.Written = 0
    $Recorder.DataBytes = 0
    $Recorder.Peak = [single]0

    try {
        $Recorder.Enumerator = New-MMDeviceEnumerator

        $id = [Runtime.InteropServices.Marshal]::StringToCoTaskMemUni($DeviceId)
        try {
            [IntPtr]$device = [IntPtr]::Zero
            $getDevice = Get-ComDelegate $Recorder.Enumerator 'IMMDeviceEnumerator.GetDevice' $script:AbiTypes.GetDevice
            Assert-HResult ($getDevice.Invoke($Recorder.Enumerator, $id, [ref]$device)) 'IMMDeviceEnumerator.GetDevice'
            $Recorder.Device = $device
        }
        finally { [Runtime.InteropServices.Marshal]::FreeCoTaskMem($id) }

        $iidAudio = New-GuidPointer $script:Abi.Guids.IID_IAudioClient
        try {
            [IntPtr]$audioClient = [IntPtr]::Zero
            $activate = Get-ComDelegate $Recorder.Device 'IMMDevice.Activate' $script:AbiTypes.Activate
            Assert-HResult ($activate.Invoke(
                $Recorder.Device,
                $iidAudio,
                [uint32]$script:Abi.Constants.CLSCTX_ALL,
                [IntPtr]::Zero,
                [ref]$audioClient
            )) 'IMMDevice.Activate(IAudioClient)'
            $Recorder.AudioClient = $audioClient
        }
        finally { [Runtime.InteropServices.Marshal]::FreeHGlobal($iidAudio) }

        [IntPtr]$formatPointer = [IntPtr]::Zero
        $getMix = Get-ComDelegate $Recorder.AudioClient 'IAudioClient.GetMixFormat' $script:AbiTypes.GetMixFormat
        Assert-HResult ($getMix.Invoke($Recorder.AudioClient, [ref]$formatPointer)) 'IAudioClient.GetMixFormat'
        $Recorder.FormatPointer = $formatPointer
        $Recorder.Format = Get-WaveFormatInfo $formatPointer
        $Recorder.FormatDescription = $Recorder.Format.Description

        [uint32]$flags = if ($Loopback) { [uint32]$script:Abi.Constants.AUDCLNT_STREAMFLAGS_LOOPBACK } else { 0 }
        $initialize = Get-ComDelegate $Recorder.AudioClient 'IAudioClient.Initialize' $script:AbiTypes.AudioInitialize
        Assert-HResult ($initialize.Invoke(
            $Recorder.AudioClient,
            [int]$script:Abi.Constants.AUDCLNT_SHAREMODE_SHARED,
            $flags,
            [long]10000000,
            [long]0,
            $Recorder.FormatPointer,
            [IntPtr]::Zero
        )) 'IAudioClient.Initialize'

        $iidCapture = New-GuidPointer $script:Abi.Guids.IID_IAudioCaptureClient
        try {
            [IntPtr]$captureClient = [IntPtr]::Zero
            $getService = Get-ComDelegate $Recorder.AudioClient 'IAudioClient.GetService' $script:AbiTypes.GetService
            Assert-HResult ($getService.Invoke($Recorder.AudioClient, $iidCapture, [ref]$captureClient)) 'IAudioClient.GetService(IAudioCaptureClient)'
            $Recorder.CaptureClient = $captureClient
        }
        finally { [Runtime.InteropServices.Marshal]::FreeHGlobal($iidCapture) }

        $directory = [IO.Path]::GetDirectoryName($OutputPath)
        if ($directory) { [IO.Directory]::CreateDirectory($directory) | Out-Null }
        $Recorder.File = [IO.FileStream]::new($OutputPath, [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
        $Recorder.Writer = [IO.BinaryWriter]::new($Recorder.File, [Text.Encoding]::ASCII, $true)
        $Recorder.DataSizePosition = Write-WaveHeader $Recorder.Writer $Recorder.Format.Bytes

        $start = Get-ComDelegate $Recorder.AudioClient 'IAudioClient.Start' $script:AbiTypes.NoArgHr
        Assert-HResult ($start.Invoke($Recorder.AudioClient)) 'IAudioClient.Start'
        $Recorder.StartedUtc = [DateTime]::UtcNow
        $Recorder.IsRecording = $true
    }
    catch {
        $Recorder.LastError = $_.Exception.Message
        Stop-WasapiCapture $Recorder
        throw
    }
}

function Read-WasapiPackets {
    param([Parameter(Mandatory)][hashtable]$Recorder)
    if (-not $Recorder.IsRecording) { return }

    $next = Get-ComDelegate $Recorder.CaptureClient 'IAudioCaptureClient.GetNextPacketSize' $script:AbiTypes.GetNextPacketSize
    $getBuffer = Get-ComDelegate $Recorder.CaptureClient 'IAudioCaptureClient.GetBuffer' $script:AbiTypes.GetBuffer
    $release = Get-ComDelegate $Recorder.CaptureClient 'IAudioCaptureClient.ReleaseBuffer' $script:AbiTypes.ReleaseBuffer

    [uint32]$packetFrames = 0
    Assert-HResult ($next.Invoke($Recorder.CaptureClient, [ref]$packetFrames)) 'IAudioCaptureClient.GetNextPacketSize'

    if ($packetFrames -eq 0) {
        $Recorder.Peak = [single]($Recorder.Peak * 0.88)
        return
    }

    while ($packetFrames -ne 0) {
        [IntPtr]$data = [IntPtr]::Zero
        [uint32]$frames = 0
        [uint32]$flags = 0
        [uint64]$devicePosition = 0
        [uint64]$qpcPosition = 0
        Assert-HResult ($getBuffer.Invoke(
            $Recorder.CaptureClient,
            [ref]$data,
            [ref]$frames,
            [ref]$flags,
            [ref]$devicePosition,
            [ref]$qpcPosition
        )) 'IAudioCaptureClient.GetBuffer'

        try {
            $byteCount = [int]($frames * $Recorder.Format.BlockAlign)
            if ($byteCount -gt 0) {
                $buffer = [byte[]]::new($byteCount)
                if (($flags -band [uint32]$script:Abi.Constants.AUDCLNT_BUFFERFLAGS_SILENT) -eq 0) {
                    [Runtime.InteropServices.Marshal]::Copy($data, $buffer, 0, $byteCount)
                }
                $Recorder.Writer.Write($buffer, 0, $buffer.Length)
                $Recorder.Written += $byteCount
                $Recorder.DataBytes = $Recorder.Written
                $p = Measure-Peak $buffer $Recorder.Format
                $Recorder.Peak = [single][Math]::Max([double]$p, [double]$Recorder.Peak * 0.78)
            }
        }
        finally {
            Assert-HResult ($release.Invoke($Recorder.CaptureClient, $frames)) 'IAudioCaptureClient.ReleaseBuffer'
        }

        $packetFrames = 0
        Assert-HResult ($next.Invoke($Recorder.CaptureClient, [ref]$packetFrames)) 'IAudioCaptureClient.GetNextPacketSize'
    }
}

function Start-Record {
    if (-not $IsWindows) { throw 'Record.Sexe requires Windows WASAPI.' }

    $root = $PSScriptRoot
    $manifest = Import-PowerShellDataFile -LiteralPath (Join-Path $root 'Record.psd1')
    $style = Import-PowerShellDataFile -LiteralPath (Join-Path $root ([string]$manifest.Style))
    $script:Abi = Read-AbiHeader (Join-Path $root ([string]$manifest.Abi))
    Initialize-NativeAbi $script:Abi
    $uninitializeCom = Enter-ComApartment

    $gpu = $null
    $recorder = New-RecorderState
    try {
        $gpu = [DirectPort.PowerShell.GpuCanvas2D]::new(
            [int]$manifest.DefaultWidth,
            [int]$manifest.DefaultHeight,
            [string]$manifest.Name
        )

        $devices = @(Get-AudioDevices)
        $music = [Environment]::GetFolderPath([Environment+SpecialFolder]::MyMusic)
        if ([string]::IsNullOrWhiteSpace($music)) { $music = $HOME }
        $outputDirectory = Join-Path $music 'Recordings'
        [IO.Directory]::CreateDirectory($outputDirectory) | Out-Null

        $app = @{
            DeviceIndex = 0
            PrevLeft = $false
            Dirty = $true
            Status = if ($devices.Count) { 'Ready' } else { 'No active Windows audio endpoints found' }
            LastFile = ''
            LastError = ''
            OutputDirectory = $outputDirectory
            LastClock = ''
            LastPeakBucket = -1
        }

        function Get-ClockText {
            if (-not $recorder.IsRecording) { return '00:00:00' }
            $elapsed = [DateTime]::UtcNow - $recorder.StartedUtc
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
            Stop-WasapiCapture $recorder
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
            if ($recorder.IsRecording) { Stop-Capture; return }
            $device = Get-SelectedDevice
            if ($null -eq $device) { return }

            $name = 'Recording {0}.wav' -f (Get-Date -Format 'yyyy-MM-dd HHmmss')
            $path = Join-Path $app.OutputDirectory $name
            try {
                Start-WasapiCapture $recorder ([string]$device.Id) ([bool]$device.Loopback) $path
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

        function Test-Hit([single]$mx,[single]$my,[single]$x,[single]$y,[single]$width,[single]$height) {
            return ($mx -ge $x -and $mx -lt ($x + $width) -and $my -ge $y -and $my -lt ($y + $height))
        }

        while ($true) {
            if ($recorder.IsRecording) {
                try { Read-WasapiPackets $recorder }
                catch {
                    $recorder.LastError = $_.Exception.Message
                    Stop-WasapiCapture $recorder
                    $app.LastError = $recorder.LastError
                    $app.Status = 'Capture stopped with an error'
                    $app.Dirty = $true
                }
            }

            $timeout = if ($recorder.IsRecording) { [uint32]15 } else { [uint32]150 }
            $frame = $gpu.Pump($(if ($app.Dirty) { [uint32]1 } else { $timeout }))
            if (-not $frame.Alive -or $frame.KeyCode -eq 27) { break }

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
                if (Test-Hit $mx $my $pad $sourceY $arrowW $sourceH) { Move-Source -1 }
                elseif (Test-Hit $mx $my ($w - $pad - $arrowW) $sourceY $arrowW $sourceH) { Move-Source 1 }
                elseif (Test-Hit $mx $my $recordX $buttonY $recordW $buttonH) { Toggle-Capture }
                elseif (Test-Hit $mx $my $openX $buttonY $openW $buttonH) { Open-Recordings }
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
            $gpu.DrawText($clockText,$pad + 10,$timerY + 18,$w - ($pad * 2) - 20,54,[uint32]$style.Text,[single]$style.TimerFontSize,[string]$style.Font,$true,1)

            [single]$meterX = $pad + 18
            [single]$meterY = $timerY + $timerH - 24
            [single]$meterW = $w - ($pad * 2) - 36
            [single]$meterH = 6
            $gpu.FillRect($meterX,$meterY,$meterW,$meterH,[uint32]$style.KeyAlt,[single]3)
            if ($recording) {
                [single]$levelW = $meterW * [single][Math]::Min(1.0, [double]$recorder.Peak)
                if ($levelW -gt 1) { $gpu.FillRect($meterX,$meterY,$levelW,$meterH,[uint32]$style.Record,[single]3) }
            }

            $gpu.FillRect($pad,$sourceY,$w - ($pad * 2),$sourceH,[uint32]$style.Key,$radius)
            $gpu.DrawRect($pad,$sourceY,$w - ($pad * 2),$sourceH,[uint32]$style.Border,[single]1,$radius)
            $gpu.DrawText('‹',$pad,$sourceY + 21,$arrowW,48,[uint32]$style.MutedText,[single]28,[string]$style.Font,$true,1)
            $gpu.DrawText('›',$w - $pad - $arrowW,$sourceY + 21,$arrowW,48,[uint32]$style.MutedText,[single]28,[string]$style.Font,$true,1)
            $gpu.DrawText('SOURCE',$pad + $arrowW,$sourceY + 10,$w - ($pad * 2) - ($arrowW * 2),18,[uint32]$style.MutedText,[single]$style.SmallFontSize,[string]$style.Font,$true,1)
            $gpu.DrawText($deviceText,$pad + $arrowW,$sourceY + 31,$w - ($pad * 2) - ($arrowW * 2),44,[uint32]$style.Text,[single]$style.SourceFontSize,[string]$style.Font,$true,1)

            $gpu.FillRect($pad,$statusY,$w - ($pad * 2),$statusH,[uint32]$style.Panel,$radius)
            $statusColor = if ($app.LastError) { [uint32]$style.Record } elseif ($recording) { [uint32]$style.Record } else { [uint32]$style.Accent }
            $gpu.DrawText([string]$app.Status,$pad + 14,$statusY + 10,$w - ($pad * 2) - 28,24,$statusColor,[single]$style.SourceFontSize,[string]$style.Font,$false,0)
            $detail = if ($app.LastError) { [string]$app.LastError } else { $formatText }
            $gpu.DrawText($detail,$pad + 14,$statusY + 34,$w - ($pad * 2) - 28,[Math]::Max([single]18,$statusH - 38),[uint32]$style.MutedText,[single]$style.SmallFontSize,[string]$style.Font,$false,0)

            $gpu.FillRect($openX,$buttonY,$openW,$buttonH,[uint32]$style.Key,$radius)
            $gpu.DrawRect($openX,$buttonY,$openW,$buttonH,[uint32]$style.Border,[single]1,$radius)
            $gpu.DrawText('FOLDER',$openX,$buttonY + 23,$openW,30,[uint32]$style.Text,[single]$style.SmallFontSize,[string]$style.Font,$true,1)

            $recordBg = if ($recording) { [uint32]$style.RecordDark } else { [uint32]$style.Record }
            $recordLabel = if ($recording) { '■  STOP' } else { '●  RECORD' }
            $gpu.FillRect($recordX,$buttonY,$recordW,$buttonH,$recordBg,$radius)
            $gpu.DrawText($recordLabel,$recordX,$buttonY + 20,$recordW,34,[uint32]$style.Text,[single]$style.ButtonFontSize,[string]$style.Font,$true,1)

            [void]$gpu.EndFrame()
            $app.Dirty = $false
        }
    }
    finally {
        try { Stop-WasapiCapture $recorder } catch { }
        if ($null -ne $gpu) { $gpu.Dispose() }
        if ($uninitializeCom) { try { $script:Native.CoUninitialize.Invoke() } catch { } }
        if ($script:Ole32 -ne [IntPtr]::Zero) {
            try { [Runtime.InteropServices.NativeLibrary]::Free($script:Ole32) } catch { }
            $script:Ole32 = [IntPtr]::Zero
        }
    }
}
