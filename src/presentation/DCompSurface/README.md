# DirectComposition surface receipt

Status: experiment scaffold, 2026-08-29

Question:

> Can DirectPort present and tear down a small retained native incarnation through DirectComposition without creating another standalone renderer executive or treating an HWND swapchain as the public object model?

This experiment is independently implemented from Windows API knowledge. QuickView is a research reference only; no QuickView implementation source is copied here.

Target shape:

```text
D3D11/DXGI device
      ↓
DirectComposition desktop device
      ↓
composition target + visual
      ↓
IDCompositionSurface (premultiplied BGRA)
      ↓ BeginDraw(dirty rect)
IDXGISurface
      ↓
D2D target bitmap
      ↓
content
      ↓ EndDraw / Commit
DWM
```

Required receipt:

1. borderless small surface appears at requested bounds;
2. surroundings remain transparent;
3. first frame is committed before/with visibility without a black/transparent flash;
4. a small dirty rectangle can be updated independently;
5. closing the incarnation destroys only its presentation resources;
6. a second incarnation can be created in the same process afterward;
7. no named IPC is required for the in-process path.

This experiment is deliberately D3D11-backed because DirectComposition accepts the DXGI device directly and the goal is to prove composition/surface lifetime, not choose DirectPort's final universal GPU executive.
