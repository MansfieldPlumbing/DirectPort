<div align="center">
  <img src="docs/directport.png" width="220" alt="DirectPort Logo" style="border-radius: 8px;" />
  <h1>DirectPort</h1>
  <p><strong>Hardware-Fenced Inter-Process GPU Memory Conduit for Windows</strong></p>
  <p><em>Archived Predecessor Architecture // Retained for NT Handle &amp; Hardware Fence Benchmark Reference</em></p>
</div>

---

> [!NOTE]
> **ARCHIVE STATUS & SUCCESSOR ARCHITECTURE**  
> DirectPort is an archived milestone. It is retained to document the hardware-synchronized NT handle primitives and Direct2D/DirectComposition SMA bindings developed during this research phase.
> * **Successor for Memory Pipelines**: **[dpx](https://github.com/MansfieldPlumbing/DPX)** — Push-based, lock-free SPSC dataflow inference runtime with zero-copy UMA memory transit.
> * **Successor for Native Windows Runtime**: **[QuickPS](https://github.com/MansfieldPlumbing/QuickPS)** — Zero-dependency Windows runtime with in-memory `[Reflection.Emit]` COM vtables and WASAPI audio capture.

---

## 1. Architectural Scope & Retrospective

DirectPort solved a fundamental Windows graphics bottleneck: **sharing GPU memory between distinct operating system processes without CPU staging or host RAM loopbacks**.

By proxying DirectX 12 hardware fences through Windows NT named handles (`CreateSharedHandle` / `OpenSharedHandleByName`), producer processes signal GPU completion directly to consumer command queues. Consumers unblock at physical PCIe crossbar latency (**~170 ns**) rather than waiting for Windows thread scheduler quantum boundaries (1–15 ms).

### Plain Concession: The Frame & Video Paradigm

DirectPort was conceptualized around **textures, video frames, framebuffers, and camera streams**. It approached GPU IPC through the lens of video delivery (DirectX 11/12 textures, Media Foundation sources, display composition) rather than general tensor computation DAGs or push-based token dataflow. 

While highly effective for multi-process video multiplexing and real-time display compositing, the frame-based abstraction introduces unnecessary boundaries when applied to continuous machine learning workloads. Those lessons directly prompted the shift toward lock-free, double-buffered node isolation in **dpx**.

### Quarantining the Python Prototype

Early versions of DirectPort included Python bindings (`pybind11`), ONNX Runtime DirectML integrations, and NumPy buffer bridges. 

Tethering a sub-microsecond GPU IPC conduit to Python's Global Interpreter Lock (GIL) and runtime overhead proved to be an architectural mismatch. All Python bindings and demonstration scripts have been strictly quarantined under [`legacy/python/`](legacy/python/). The core systems engineering lives in native C++ and PowerShell SMA.

---

## 2. Repository Layout

```text
DirectPort/
├── docs/
│   └── directport.png            # DirectPort hardware die & VRAM badge
├── src/
│   ├── sdk/                      # Clean, minimal C-API transport layer
│   │   ├── directport.h          # Public C API header & format definitions
│   │   ├── directportd3d12.cpp   # D3D12 resource creation, fences, NT handle resolver
│   │   └── directportd3d11.cpp   # D3D11 compatibility layer & texture sharing
│   ├── sma/                      # DirectPortSMA: Native PowerShell / SMA engine
│   │   ├── DirectPort.Canvas2D.Native.cpp  # Direct2D / DirectWrite canvas rendering
│   │   ├── DirectPort.Console.Native.cpp   # High-throughput console presentation
│   │   ├── DirectPort.PowerShell.cpp       # Native unmanaged SMA bridge
│   │   ├── DirectPort.Shader.Native.cpp    # In-memory HLSL shader compilation
│   │   └── SMA.cpp                         # Native SMA runspace hosting
│   ├── ipc/                      # Windows Shell & Context Menu IPC
│   │   ├── menudump.cpp          # Windows Explorer context menu interception
│   │   ├── menudump.def          # Shell extension export definition
│   │   ├── ipc.cpp               # Out-of-process menu graph transport
│   │   └── shell-reg.ps1         # Shell extension registration script
│   ├── presentation/             # DirectComposition & Shader Surfaces
│   │   ├── DCompSurface/         # DirectComposition visual tree integration
│   │   └── ShaderSurface/        # Standalone D3D12 shader surfaces (ps2orb)
│   └── apps/                     # Native Applications
│       └── RecordKit/            # Low-latency WASAPI float-PCM audio recorder
└── legacy/                       # Historical prototypes
    ├── DirectPort/               # Original 2025 C++ implementation
    ├── Examples/                 # Native C++ examples
    └── python/                   # Quarantined Python bindings & scripts
```

---

## 3. The Core C-API (`src/sdk`)

The production SDK layer provides a minimal, dependency-free C API for inter-process GPU memory sharing:

```c
#include "directport.h"

// 1. Initialize subsystem (once per process)
dp12_init();

// 2. Producer: Create shared D3D12 resource with NT handle names
DP_HANDLE port = dp12_create_shared_resource(
    1920, 1080, DP_FORMAT_VIDEO, /*is_system_ram=*/false,
    L"DirectPort_SharedTexture", L"DirectPort_SharedFence"
);

// 3. Signal completion on GPU command queue
dp12_signal_fence(port, frame_counter++);

// 4. Consumer: Open by NT name and queue asynchronous GPU hardware wait
DP_HANDLE consumer = dp12_open_shared_resource(
    L"DirectPort_SharedTexture", L"DirectPort_SharedFence"
);
dp12_queue_wait(consumer, pCommandQueue, completed_value);
// GPU unblocks at ~170ns PCIe crossbar latency. Zero CPU intervention.
```

---

## 4. Lineage: Evolution into QuickPS

The native PowerShell bindings in [`src/sma/`](src/sma/) and the WASAPI audio capture in [`src/apps/RecordKit/`](src/apps/RecordKit/) represent the key evolutionary bridge to **[QuickPS](https://github.com/MansfieldPlumbing/QuickPS)**:

1. **Phase 1 (DirectPort C++)**: Proved GPU VRAM sharing via NT handles and DX12 fences.
2. **Phase 2 (DirectPortSMA)**: Integrated D3D12, Direct2D, and WASAPI audio into PowerShell via unmanaged C++ DLL shims.
3. **Phase 3 (QuickPS)**: Eliminated the C++ compilation step entirely by using `[Reflection.Emit]` to synthesize COM vtables and Win32 message pumps dynamically in-memory.

---

## License

MIT License. See [LICENSE](LICENSE) for details.
