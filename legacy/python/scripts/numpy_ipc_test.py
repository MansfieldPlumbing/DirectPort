# src/Scripts/numpy_ipc_test.py

import directport
import numpy as np
import time
import os
import sys
import traceback

def create_gradient_array(width, height, frame_count):
    """Creates a NumPy array with a moving diagonal gradient."""
    # Create arrays for x and y coordinates
    x = np.linspace(0, 1, width)
    y = np.linspace(0, 1, height)
    xv, yv = np.meshgrid(x, y)

    # Animate the gradient by adding a time-based offset
    phase = np.sin(frame_count * 0.05) * 0.5 + 0.5
    
    # Calculate the gradient value
    gradient = (xv + yv + phase) / 3.0
    
    # Create the 4-channel image
    r = (255 * gradient).astype(np.uint8)
    g = (255 * (1.0 - gradient)).astype(np.uint8)
    
    b_value = (128 * np.sin(frame_count * 0.1) + 127).astype(np.uint8)
    b = np.full((height, width), b_value, dtype=np.uint8)
    
    a = np.full((height, width), 255, dtype=np.uint8)
    
    # Stack the channels to create the final (H, W, 4) array
    return np.stack([r, g, b, a], axis=-1)

def main():
    """
    A self-contained test to verify pickle-free IPC of NumPy arrays
    using the DirectPort library.
    """
    print("--- DirectPort NumPy IPC Test ---")
    
    try:
        device = directport.DeviceD3D11.create()
    except RuntimeError as e:
        print(f"Fatal: Failed to create D3D11 device: {e}", file=sys.stderr)
        sys.exit(1)
        
    WIDTH, HEIGHT = 1280, 720
    producer_pid = os.getpid()
    
    # --- FIX: Create a window. This is likely required to initialize DXGI for sharing. ---
    # We don't need to show it or process its events for this specific test.
    _ = device.create_window(WIDTH, HEIGHT, "NumPy IPC Test")
    print("Created a window to ensure full DXGI initialization.")
    
    # --- 1. PRODUCER SIDE ---
    print(f"\n[Producer PID: {producer_pid}] Setting up...", flush=True)
    
    # Create the source NumPy array
    source_array = create_gradient_array(WIDTH, HEIGHT, 0)
    
    # Create a GPU texture that matches the NumPy array's properties
    gpu_texture = device.create_texture(WIDTH, HEIGHT, directport.DXGI_FORMAT.R8G8B8A8_UNORM)
    
    # Upload the initial NumPy data to the GPU texture
    print("[Producer] Writing initial NumPy array to GPU texture...", flush=True)
    directport.numpy.write_texture(device, gpu_texture, source_array)
    
    # Create a producer to share this texture
    producer = device.create_producer("numpy_ipc_test_stream", gpu_texture)
    print("[Producer] Producer is now broadcasting the texture.", flush=True)

    # --- 2. CONSUMER SIDE ---
    print("\n[Consumer] Setting up...", flush=True)
    
    # Connect to the producer (in this test, it's our own process)
    consumer = device.connect_to_producer(producer_pid)
    if not consumer:
        print("[Consumer] FATAL: Failed to connect to our own producer.", file=sys.stderr)
        sys.exit(1)
    
    # Get the consumer's local texture, which will receive the copied data
    local_texture = consumer.get_texture()
    print("[Consumer] Successfully connected and received local texture.", flush=True)
    
    # --- 3. VERIFICATION LOOP ---
    print("\n--- Starting Verification Loop (100 frames) ---", flush=True)
    try:
        for i in range(100):
            # PRODUCER: Update and send a new frame
            source_array = create_gradient_array(WIDTH, HEIGHT, i)
            directport.numpy.write_texture(device, gpu_texture, source_array)
            producer.signal_frame()
            
            # CONSUMER: Wait for and receive the frame
            if not consumer.wait_for_frame():
                print(f"Frame {i}: Consumer failed to wait for frame.", file=sys.stderr)
                break
                
            # Copy from the shared texture to our local one
            device.copy_texture(consumer.get_shared_texture(), local_texture)
            
            # Read the result from the GPU back into a new NumPy array
            result_array = directport.numpy.read_texture(device, local_texture)
            
            # VERIFY
            if np.array_equal(source_array, result_array):
                print(f"Frame {i:03d}: OK - Data integrity verified.", flush=True)
            else:
                print(f"Frame {i:03d}: FAIL - Data mismatch detected!", file=sys.stderr)
                diff = np.sum(source_array.astype(np.int16) - result_array.astype(np.int16))
                print(f"  -> Sum of differences: {diff}", file=sys.stderr)
                break
                
            time.sleep(0.01) # Small delay to make output readable

    except KeyboardInterrupt:
        print("\nTest interrupted by user.")
    except Exception as e:
        print(f"\nAn unexpected error occurred: {e}", file=sys.stderr)
        traceback.print_exc()
    finally:
        print("\n--- Test Complete ---")

if __name__ == "__main__":
    main()