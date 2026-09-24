# src/Scripts/pyheatmap_producer.py

import directport
import numpy as np
import time
import os
import sys
import traceback

def create_heatmap_data(width, height, frame_count):
    """Creates a 2D NumPy array of floats with an animated wave pattern."""
    x = np.linspace(-1, 1, width)
    y = np.linspace(-1, 1, height)
    xv, yv = np.meshgrid(x, y)
    
    dist = np.sqrt(xv**2 + yv**2)
    phase = time.time() * 2.0
    wave = np.sin(dist * 15.0 - phase)
    
    return ((wave + 1.0) / 2.0).astype(np.float32)

def main():
    print("--- DirectPort Heatmap Producer (float32 data) ---")
    try:
        device = directport.DeviceD3D11.create()
    except RuntimeError as e:
        print(f"Fatal: Failed to create D3D11 device: {e}", file=sys.stderr)
        sys.exit(1)

    WIDTH, HEIGHT = 1280, 720
    window = device.create_window(WIDTH, HEIGHT, "Heatmap Producer (Raw float32 data)")

    print("Creating textures...")
    
    # --- FIX: Use a separate private texture for writing and a shared texture for output ---
    # 1. This texture is private to the producer. We will write our NumPy data here.
    private_float_texture = device.create_texture(WIDTH, HEIGHT, directport.DXGI_FORMAT.R32_FLOAT)
    
    # 2. This texture will be given to the producer to be shared with consumers.
    shared_float_texture = device.create_texture(WIDTH, HEIGHT, directport.DXGI_FORMAT.R32_FLOAT)
    
    # Create the producer and give it the texture designated for sharing.
    producer = device.create_producer("heatmap_float_stream", shared_float_texture)
    print("Producer is now broadcasting the shared float texture.")

    # For preview, we still need an RGBA texture to render the colored result into.
    preview_texture = device.create_texture(WIDTH, HEIGHT, directport.DXGI_FORMAT.B8G8R8A8_UNORM)

    # The grayscale preview shader is correct from the previous step.
    grayscale_shader = """
    struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
    Texture2D<float> t : register(t0); SamplerState s : register(s0);
    float4 main(PSInput i) : SV_TARGET {
        float v = t.Sample(s, i.uv).r;
        return float4(v, v, v, 1);
    }
    """

    try:
        frame = 0
        while window.process_events():
            # 1. Generate the raw data in a NumPy array.
            heatmap_data_array = create_heatmap_data(WIDTH, HEIGHT, frame)
            
            # 2. Write the NumPy data to our PRIVATE texture. This is a safe operation.
            directport.numpy.write_texture(device, private_float_texture, heatmap_data_array)
            
            # 3. Perform a simple, stable GPU-to-GPU copy to the SHARED texture.
            device.copy_texture(private_float_texture, shared_float_texture)
            
            # 4. Signal consumers that the SHARED texture now contains a new frame.
            producer.signal_frame()
            
            # 5. Render the local preview using the private data.
            device.apply_shader(preview_texture, grayscale_shader, "main", [private_float_texture])
            device.blit(preview_texture, window)
            window.present()
            
            frame += 1

    except KeyboardInterrupt:
        print("\nProducer stopped.")
    except Exception as e:
        print(f"\nAn unexpected error occurred in the producer loop: {e}", file=sys.stderr)
        traceback.print_exc()
    finally:
        print("Shutting down.")

if __name__ == "__main__":
    main()