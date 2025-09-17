Of course. Here is a comprehensive GitHub README file for your DirectPort library, formatted in Markdown.

---

# DirectPort

**A high-performance C++ library with Python bindings for zero-copy GPU texture and data sharing across processes and graphics APIs (DirectX 11, DirectX 12, OpenGL).**

![Language](https://img.shields.io/badge/Language-C%2B%2B%20%26%20Python-blue.svg)![Platform](https://img.shields.io/badge/Platform-Windows-0078D6.svg)![License](https://img.shields.io/badge/License-MIT-green.svg)![Graphics APIs](https://img.shields.io/badge/APIs-D3D11%20%7C%20D3D12%20%7C%20OpenGL-orange.svg)

DirectPort is a developer-focused toolkit engineered for advanced, low-latency GPU communication. It enables applications to share textures directly from GPU memory, eliminating the performance-intensive step of copying data through the CPU. It provides a unified interface over multiple graphics APIs and includes powerful Python bindings for rapid development and integration with ML and scientific computing libraries.

## What is DirectPort?

DirectPort establishes a robust producer-consumer framework for sharing GPU resources on Windows. One or more applications can act as **Producers**, creating and updating textures on the GPU. Other applications can act as **Consumers**, discovering and reading these textures in real-time with minimal overhead.

This is achieved using a "zero-copy" approach, where different processes can access the same surface in GPU memory, synchronized with native GPU fences for maximum performance.

## Who is this for?

*   **Live Broadcast & Streaming Developers:** Build real-time video mixers, switchers, and effects pipelines.
*   **AI/ML Engineers:** Run GPU-accelerated ONNX models directly on live video streams for object detection, style transfer, or real-time analysis without ever leaving the GPU.
*   **Creative Coders & VJ Artists:** Mix and composite visuals from multiple applications and sources in real-time.
*   **Data Scientists & Researchers:** Visualize massive NumPy datasets on the GPU without the bottleneck of CPU-GPU data transfers.
*   **Game & Engine Developers:** Create plugins and tools that can share render targets or video feeds between separate applications.

## Core Features

DirectPort is structured into several powerful, interoperable modules:

*   **🖥️ Multi-API Graphics Core (D3D11/D3D12/OpenGL):**
    *   Create windows and render targets using DirectX 11, DirectX 12, or modern OpenGL.
    *   Share textures seamlessly between processes, even if they are using different graphics APIs.
    *   A powerful `discover()` function automatically finds all running DirectPort producers on the system.
    *   Apply custom HLSL or GLSL shaders to textures on the GPU.

*   **🚀 GPU-Accelerated Machine Learning (ONNX Runtime):**
    *   Load ONNX models and run inference directly on shared GPU textures using the DirectML execution provider.
    *   Enables true zero-copy AI pipelines: video frame -> GPU -> AI model -> result, with no CPU round-trip.

*   **🔬 High-Performance NumPy Integration:**
    *   `directport.numpy.write_texture()`: Upload a NumPy array's contents directly into a D3D11 texture.
    *   `directport.numpy.read_texture()`: Download a D3D11 texture's contents directly into a new NumPy array.
    *   Massively accelerates visualization and GPU-based processing of CPU-generated data.

*   **📷 High-Performance Camera Input:**
    *   A dedicated `DirectPortCamera` class provides a high-performance, low-latency video capture source using Windows Media Foundation.
    *   Frames can be delivered as NumPy arrays for CPU processing (e.g., with OpenCV) or rendered directly to a GPU texture.

## Getting Started

### Prerequisites

1.  **Windows 10/11:** The library uses modern Windows graphics features.
2.  **Visual Studio 2022:** With the "Desktop development with C++" workload.
3.  **CMake:** Version 3.15 or higher.
4.  **vcpkg:** The C++ package manager. DirectPort uses `vcpkg` to manage dependencies like `pybind11`, `glew`, and `wil`.

### Installation

1.  **Install Dependencies with vcpkg:**
    ```bash
    # Clone vcpkg if you haven't already
    git clone https://github.com/microsoft/vcpkg
    ./vcpkg/bootstrap-vcpkg.bat

    # Install DirectPort's dependencies
    ./vcpkg/vcpkg install glew pybind11 wil onnxruntime-gpu --triplet x64-windows
    ```

2.  **Configure and Build with CMake:**
    ```bash
    # Create a build directory
    mkdir build
    cd build

    # Configure the project, pointing to your vcpkg installation
    cmake .. -DCMAKE_TOOLCHAIN_FILE=[path-to-vcpkg]/scripts/buildsystems/vcpkg.cmake

    # Build the project (e.g., in Release mode)
    cmake --build . --config Release
    ```

3.  **Locate the Module:** The compiled Python module (`directport.pyd`) will be in the `build\Release` (or `build\Debug`) directory. The example Python scripts can be run from the project root and will automatically find this module.

## Quickstart Examples

The `src/Scripts` directory contains a wealth of examples. To run them, simply navigate to the project's root directory and execute the script.

### 1. D3D12 Shader Producer & D3D11 Consumer

This demonstrates the core cross-API texture sharing capability.

-   **Start the Producer:** Run one of the C++ example producers.
    ```bash
    # Open a terminal in the project root
    ./build/Release/DirectPortShaderProducerD3D12.exe
    ```
-   **Start the Consumer:** In a separate terminal, run the Python D3D11 consumer.
    ```bash
    python src/Scripts/pyconsumer11.py
    ```
    The Python window will automatically discover and display the stream from the C++ D3D12 application.

### 2. Live Camera Feed to NumPy

Capture your webcam feed directly into a NumPy array for analysis.

```bash
python src/Scripts/numpytest.py
```

### 3. Multiplexing (Compositing) Streams

Start multiple producers (e.g., `DirectPortProducerD3D11.exe` and `DirectPortCamera.exe`). Then run the multiplexer to see them composited into a single new stream.

```bash
# Start producers in separate terminals...
./build/Release/DirectPortProducerD3D11.exe
./Binaries/DirectPortCamera.exe

# Start the multiplexer in a third terminal
python src/Scripts/pymultiplexer12.py
```

### 4. Zero-Copy ONNX Inference

Run a simple "add 1.0" filter on a GPU texture using ONNX Runtime.

```bash
python src/Scripts/onnxtest.py
```

## API Overview

### D3D11 / D3D12

The core API is nearly identical for both DirectX versions.

```python
import directport

# 1. Create a device
device = directport.DeviceD3D11.create() # or DeviceD3D12

# 2. Create a window and a texture
window = device.create_window(1280, 720, "My Window")
texture = device.create_texture(1280, 720, directport.DXGI_FORMAT.B8G8R8A8_UNORM)

# 3. Create a producer to share the texture
producer = device.create_producer("my_stream_name", texture)

# 4. Main loop
while window.process_events():
    # Render something into `texture` using apply_shader...
    device.apply_shader(output=texture, shader=b"...")
    
    # Signal that a new frame is ready for consumers
    producer.signal_frame()
    
    # Show the result in our local window
    device.blit(texture, window)
    window.present()
```

### OpenGL

The OpenGL module provides a unified API that internally manages interop with DirectX for sharing.

```python
import directport.gl as dp_gl

# 1. Create an OpenGL device
device = dp_gl.Device.create()
window = device.create_window(800, 600, "OpenGL Producer")

# 2. Create a texture and a producer
texture = device.create_texture(800, 600, dp_gl.DXGI_FORMAT.R8G8B8A8_UNORM)
producer = device.create_producer("my_gl_stream", texture)

# 3. Main loop
while window.process_events():
    # Make the window's context current
    window.make_current()
    
    # Render to texture using apply_shader...
    device.apply_shader(output=texture, glsl_fragment_shader="...")
    
    # Signal and present
    producer.signal_frame()
    device.blit(texture, window)
    window.present()
```

---


---
LICENSE
---

MIT. Use it. Build on it. 

-Mr. Mansfield
