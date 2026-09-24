param(
    [string] $AssemblyPath = (Join-Path $PSScriptRoot 'bin\DirectPort.PowerShell.dll'),
    [uint32] $Width = 1280,
    [uint32] $Height = 720,
    [int] $Frames = 300
)

$ErrorActionPreference = 'Stop'
[Reflection.Assembly]::LoadFrom($AssemblyPath) | Out-Null

$hlsl = @'
cbuffer Constants : register(b0) {
    float2 resolution;
    float time;
    float padding;
};

struct PixelInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

PixelInput VSMain(uint id : SV_VertexID) {
    PixelInput output;
    float2 uv = float2((id << 1) & 2, id & 2);
    output.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}

float4 PSMain(PixelInput input) : SV_TARGET {
    float2 p = input.uv - 0.5;
    p.x *= resolution.x / resolution.y;
    float ring = exp(-abs(length(p) - 0.22) * 90.0);
    float3 color = 0.5 + 0.5 * cos(time + float3(0.0, 2.1, 4.2));
    return float4(color * ring, 1.0);
}
'@

$suffix = "${PID}_pwshshader"
$producer = $null

try {
    $producer = [DirectPort.PowerShell.FullscreenShaderProducer]::new(
        $Width,
        $Height,
        [DirectPort.PowerShell.PixelFormat]::Bgra8,
        "Global\D3D12_Texture_$suffix",
        "Global\D3D12_Fence_$suffix",
        $hlsl,
        'VSMain',
        'PSMain',
        16)

    "DEVICE=0x$($producer.DevicePointer.ToInt64().ToString('X')) RESOURCE=0x$($producer.ResourcePointer.ToInt64().ToString('X'))"
    "TEX=0x$($producer.SharedTextureHandle.ToInt64().ToString('X')) FENCE=0x$($producer.SharedFenceHandle.ToInt64().ToString('X'))"

    $clock = [Diagnostics.Stopwatch]::StartNew()
    $submitted = 0
    $dropped = 0
    for ($frame = 1; $frame -le $Frames; $frame++) {
        [single[]] $constants = @([single] $Width, [single] $Height, [single] $clock.Elapsed.TotalSeconds, [single] 0.0)
        if ($producer.TryDrawFloats($constants, [uint64] $frame)) {
            $submitted++
            if ($submitted -eq 1) { 'READY' }
        }
        else {
            $dropped++
        }
    }
    $clock.Stop()
    'SUBMITTED={0} DROPPED={1} BUILD={2:N3}ms FENCE={3}' -f $submitted, $dropped, ($clock.Elapsed.TotalMilliseconds / $Frames), $producer.CompletedValue
}
finally {
    if ($null -ne $producer) { $producer.Dispose() }
    'CLOSED'
}
