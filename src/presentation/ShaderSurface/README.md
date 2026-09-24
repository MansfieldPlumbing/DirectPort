# ShaderSurface Experiment

`ps2orb-d3d12.cpp` is a donor receipt, not the target implementation.

Useful evidence in the donor:

- HLSL drives an animated orb;
- the vertex shader creates a full-target triangle from `SV_VertexID`;
- a render target can be filled entirely by shader work;
- DWM/Windows presentation can host the result.

Donor scaffolding that must **not** be promoted automatically:

- standalone executable lifetime;
- conventional `WS_OVERLAPPEDWINDOW`;
- its own D3D12 device/queue;
- its own swapchain/message/render loop;
- named shared texture/fence merely because the old proof had external consumers.

Next question:

> Can existing DirectPort create a transient native presentation incarnation and execute the orb shader into it while SMA + DirectPort remain the single resident substrate?
