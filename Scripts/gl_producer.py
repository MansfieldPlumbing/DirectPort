import directport
import numpy as np
import time
import os
import sys

def create_gradient_texture(width, height):
    """Creates a colorful NumPy array to serve as the texture source."""
    x = np.linspace(0, 255, width, dtype=np.uint8)
    y = np.linspace(0, 255, height, dtype=np.uint8)
    xv, yv = np.meshgrid(x, y)
    
    r = xv
    g = yv
    b = np.full((height, width), 128, dtype=np.uint8)
    a = np.full((height, width), 255, dtype=np.uint8)
    
    # directport.gl expects BGRA format for B8G8R8A8_UNORM
    return np.stack([b, g, r, a], axis=-1)

def main():
    """A simple OpenGL producer that creates and shares a static texture."""
    print("--- OpenGL Producer ---")
    
    try:
        # The OpenGL device internally creates a D3D11 device for interop
        device = directport.gl.Device.create()
    except Exception as e:
        print(f"Fatal: Could not create device. Error: {e}", file=sys.stderr)
        sys.exit(1)

    WIDTH, HEIGHT = 1024, 1024
    producer_pid = os.getpid()
    stream_name = "gl_python_producer_test"
    
    window = device.create_window(WIDTH, HEIGHT, f"OpenGL Producer (PID: {producer_pid})")
    
    # 1. Create texture data on the CPU
    cpu_texture_data = create_gradient_texture(WIDTH, HEIGHT)
    
    # 2. Create a GPU texture with the initial data
    gpu_texture = device.create_texture(WIDTH, HEIGHT, directport.gl.DXGI_FORMAT.B8G8R8A8_UNORM, cpu_texture_data)
    
    # 3. Create a producer to share this texture
    # The C++ backend handles creating the actual shared resource.
    producer = device.create_producer(stream_name, gpu_texture)
    print(f"Broadcasting stream '{stream_name}' from PID {producer_pid}")

    try:
        while window.process_events():
            # For this simple test, we don't change the texture content.
            # We just signal that it's available and display it locally.
            
            # Blit the texture to our local window for a preview
            device.blit(gpu_texture, window)
            
            # Present the rendered frame to the screen
            window.present()
            
            # Signal to any consumers that a frame is ready.
            # This copies the content to the shared texture and signals the GPU fence.
            producer.signal_frame()
            
            # A small sleep to prevent maxing out the CPU
            time.sleep(1/60) 
            
    except KeyboardInterrupt:
        print("\nProducer shutting down.")
    finally:
        print("Exiting.")

if __name__ == "__main__":
    main()