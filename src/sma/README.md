# DirectPort.PowerShell

This prototype exposes DirectPort/D3D12 resources as managed .NET 11 objects for PowerShell 7.7. The native assembly is built once; scripts retain and mutate the object graph at runtime without compiling C# or rasterizing cells on the CPU.

## GPU console path

```text
PowerShell uint32 cells
    -> one copy into a free persistently mapped UPLOAD slot
    -> StructuredBuffer<uint> SRV
    -> fullscreen MSDF shader
    -> GPU-local BGRA shared texture
    -> timeline fence
```

The packed cell ABI is:

```text
bits  0..15  Unicode codepoint
bits 16..23  foreground palette index
bits 24..31  background palette index
```

`GpuConsole.TryPresent()` never CPU-waits. Three frame slots are used; if the next slot is still in flight, it returns `false` and the caller retains only its newest semantic state. The optional DirectPort manifest supports the existing D3D11/D3D12 example consumers. `GpuConsole.Show()` creates an in-process D3D12 flip-swapchain viewer with mouse, wheel, key, and resize state returned to PowerShell.

## Build

Requirements:

- Windows x64
- Visual Studio C++ tools
- .NET 11 SDK/reference packs
- PowerShell 7.7 for the examples

```powershell
pwsh ./powershell/Build-DirectPort.PowerShell.ps1
```

The build discovers Visual Studio with `vswhere`, compiles optimized native and C++/CLI objects, and writes `DirectPort.PowerShell.dll` plus `ijwhost.dll` to `powershell/bin/`.

## Run the canvas

Set the atlas paths if your MSDF assets live elsewhere:

```powershell
pwsh ./powershell/DEBUG-CANVAS.r6.ps1 `
  -AtlasPng C:\glyphs\cascadia-code-atlas.png `
  -MetricsJson C:\glyphs\cascadia-code-metrics.json
```

The canvas includes Touch, Pan, 16 Colors, Glyphs, Grid, and Noise modes. Click the GPU-drawn toolbar or press `1` through `6`; use `+` and `-` to change cell scale and `Esc` to close. Static modes publish on demand and go idle. Animated modes run on cadence.

For a compact producer/consumer smoke test:

```powershell
pwsh ./powershell/Smoke-DirectPort.Console.ps1 -InProcessView
```

Expected output is intentionally small:

```text
READY
FPS=... BUILD=...ms DROPPED=...
IDLE
CLOSED
```

## Managed objects

- `Device`: DirectPort D3D12 lifecycle and adapter identity.
- `SharedResource`: low-level shared resource/fence wrapper. CPU-row-major textures are adapter-dependent; prefer GPU-optimal resources on discrete adapters.
- `FullscreenShaderProducer`: PowerShell-supplied fullscreen HLSL node with a shared output texture.
- `GpuConsole`: packed-cell/MSDF renderer, shared texture/fence producer, optional manifest, and optional in-process viewer.

PowerShell owns names, manifests, cadence, graph topology, and publication policy. The native layer owns device objects, memory placement, command recording, and fence signaling.

## Cross-device semantics

DirectPort identity is logical; native handles are local capabilities. A texture, tensor, audio buffer, or control stream keeps the same object ID, type, layout, version, and dependency edges when rebound to another backend, while D3D12 handles, Vulkan file descriptors, Android hardware buffers, and Binder objects remain on the device that created them.

A disconnected remote incarnation is reported as unavailable, or as an immutable pending command against an expected owner epoch. It is never presented as mutated remote memory. Reconnection must explicitly rebind, replay, or reject pending work after comparing epochs and versions. This preserves the object graph's semantics and causality without claiming that disconnected physical memory is shared.
