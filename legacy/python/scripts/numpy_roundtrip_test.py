# Save this file as numpy_roundtrip_test.py

import numpy as np
import sys
import os

def run_numpy_roundtrip_test():
    """
    Tests the full write->read cycle for the directport.numpy submodule.
    1. Creates a NumPy array with a predictable pattern.
    2. Writes it to a new GPU texture.
    3. Reads the data back from the GPU texture into a new array.
    4. Compares the original and read-back arrays to ensure they are identical.
    """
    try:
        import directport
    except ImportError:
        print("❌ Fatal Error: Could not import the 'directport' module.")
        print(f"   Ensure 'directport...pyd' is in the same folder as this script: {os.getcwd()}")
        input("   Press Enter to exit.")
        return

    device = None
    try:
        # 1. Initialize the DirectPort D3D11 device
        # The numpy functions currently require a D3D11 device context.
        print("Initializing D3D11 device...")
        device = directport.DeviceD3D11.create()

        # 2. Create a test NumPy array with a recognizable gradient pattern
        width, height = 256, 256
        print(f"Creating a {width}x{height} NumPy array with color gradients...")
        
        # Create a blank 4-channel (BGRA) array
        original_np = np.zeros((height, width, 4), dtype=np.uint8)
        
        # Create linear ramps for the x and y axes
        x_ramp = np.linspace(0, 255, width, dtype=np.uint8)
        y_ramp = np.linspace(0, 255, height, dtype=np.uint8)
        
        # Use meshgrid to create 2D gradient fields
        xv, yv = np.meshgrid(x_ramp, y_ramp)
        
        # Assign patterns to channels
        original_np[:, :, 0] = 50              # Blue channel is a constant medium blue
        original_np[:, :, 1] = yv               # Green channel is a vertical gradient
        original_np[:, :, 2] = xv               # Red channel is a horizontal gradient
        original_np[:, :, 3] = 255              # Alpha channel is fully opaque

        # 3. Create a matching GPU texture to be the target
        print("Creating a matching GPU texture...")
        gpu_texture = device.create_texture(width, height, directport.DXGI_FORMAT.B8G8R8A8_UNORM)

        # 4. === WRITE ===
        # Upload the NumPy data to the GPU texture.
        print("STEP 1: Writing NumPy array TO GPU texture...")
        directport.numpy.write_texture(device, gpu_texture, original_np)
        print("✅ Write operation complete.")

        # 5. === READ ===
        # Download the data from the GPU texture into a new NumPy array.
        print("STEP 2: Reading GPU texture BACK to a new NumPy array...")
        readback_np = directport.numpy.read_texture(device, gpu_texture)
        print("✅ Read operation complete.")

        # 6. === VERIFY ===
        print("\n--- Verification ---")
        if readback_np is None or readback_np.size == 0:
            raise RuntimeError("Verification failed: The readback array is empty.")

        print(f"Original array shape: {original_np.shape}, dtype: {original_np.dtype}")
        print(f"Readback array shape: {readback_np.shape}, dtype: {readback_np.dtype}")

        # Use NumPy's robust array comparison function.
        are_equal = np.array_equal(original_np, readback_np)

        if are_equal:
            print("\n✅ SUCCESS! The original and readback arrays are identical.")
            print("   The NumPy <-> GPU data roundtrip was successful.")
        else:
            print("\n❌ FAILED! The arrays are different.")
            diff = np.abs(original_np.astype(int) - readback_np.astype(int))
            print(f"   Maximum difference between pixels: {np.max(diff)}")

    except Exception as e:
        print(f"\n--- An Error Occurred ---")
        print(f"ERROR: {e}")
    
    finally:
        print("\nTest finished.")
        input("Press Enter to exit.")

if __name__ == "__main__":
    # Add the current directory to the path to find the .pyd module
    sys.path.append(os.getcwd())
    run_numpy_roundtrip_test()