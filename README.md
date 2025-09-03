DirectPort
by MansfieldPlumbing

"I don't make the models. I fix the damned pipes."

---
PROJECT SUMMARY
---

DirectPort is a self-contained, first-principles C++ primitive for zero-copy, synchronized GPU IPC on Windows. It establishes a direct, real-time VRAM conduit between previously siloed applications, eliminating the GPU-CPU-GPU bottleneck entirely.

This is not a framework. It is a fundamental building block. It is the plumbing.


---
THE PROBLEM: THE LEAKY PIPE
---

Modern AI pipelines are leaking performance. Every time you move a tensor from one GPU process to another, you are likely pouring it down the slow drain of the PCIe bus. The conventional workflow (GPU -> CPU -> IPC -> CPU -> GPU) is a fundamental bottleneck that makes true real-time, multi-process AI systems fragile and inefficient. This is the performance trap that limits the scale and ambition of our work.


---
THE SOLUTION: NEW PLUMBING
---

DirectPort fixes the pipes. It uses two core OS-level features:

1. NT HANDLES: To share a direct, protected pointer to a GPU resource (a texture or buffer) between processes. The data never leaves VRAM.

2. GPU FENCES (D3D11/D3D12): To provide robust, low-level synchronization, ensuring a consuming process only reads a resource after the producer has finished with it. No tearing, no race conditions.

The result is a simple, robust, high-pressure VRAM conduit between processes.


---
THE PROOF: BATTLE-TESTED EXAMPLES
---

This repository contains the full source for the C++ primitive and several examples that prove its power and flexibility:

ProducerD3D11.cpp / ProducerD3D12.cpp:
Standalone producers that create and share a real-time texture using different graphics APIs.

SuperConsumer64.cpp:
A robust C++ consumer that discovers and renders streams from any number of producers simultaneously with near-zero CPU overhead. This is the proof of life for the C++ core.

pybind_wrapper.cpp / directport.py:
A complete pybind11 wrapper that exposes the C++ primitive to Python, turning it into a simple, high-performance module.

knownperfectgroundtruthperfectswap.py:
A Python script demonstrating how to use the DirectPort consumer to view a shared texture, bridging the C++/Python gap.


---
IMPLICATIONS FOR AI/ML RESEARCH
---

This primitive is more than just a way to share video frames. It is a generic data bus for GPU-resident tensors. It unlocks a new, superior architectural pattern for AI systems:

1. GPU-NATIVE MICROSERVICES: Decouple your pipeline. Run a Whisper model in one process, a CLIP model in another, and a depth estimation model in a third. Each publishes its output tensor directly to the GPU bus.

2. REAL-TIME MULTIMODAL FUSION: A central C++ hub can consume all these disparate tensors, synchronize them, and fuse them into a single multimodal tensor in a compute shader. The data never leaves the GPU.

3. EFFICIENT DISTILLATION & TRAINING: The final fused tensor can be published and consumed by a simple Python training script. The trainer doesn't care about the complex upstream pipeline; it just receives a perfectly packaged, zero-latency stream of training data. It just works.


---
THE CHALLENGE
---

The C++ core is done. The Python bindings are done. The proof is in this repository.

I am not a library maintainer. I am a plumber. I built the pipes because my system needed them. Now I am open-sourcing the blueprints and the tools.

Let somebody else make the Rust shit. Let somebody else make the C# shit.

The problem is solved. Go build.


---
LICENSE
---

MIT. Use it. Build on it. 

-Mr. Mansfield
