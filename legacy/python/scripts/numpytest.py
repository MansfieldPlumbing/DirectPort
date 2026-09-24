import numpy as np
import time
import sys
import os

def run_camera_to_numpy_test():
    """
    Initializes a camera, captures a frame, converts it to a NumPy array,
    and prints numerical information about the texture to the console.
    """
    # --- The find_module() function is GONE. We just import directly. ---
    try:
        import directport
    except ImportError:
        print("❌ Fatal Error: Could not import the 'directport' module.")
        print(f"   Ensure 'directport...pyd' is in the same folder as this script: {os.getcwd()}")
        # Add a pause so the user can read the error in case the window closes
        input("   Press Enter to exit.")
        return

    cam = None # Ensure cam is defined for the finally block
    try:
        # 1. Initialize the DirectPortCamera
        print("Initializing camera...")
        cam = directport.DirectPortCamera()
        cam.init()
        cam.start_capture()

        if not cam.is_running():
            print("Error: Camera failed to start.")
            return

        # 2. Capture a frame with a retry loop to give the camera time to warm up.
        print("Capturing a frame (will retry for up to 2 seconds)...")
        frame_numpy = None
        for _ in range(100): # Try up to 100 times
            frame_numpy = cam.get_frame()
            if frame_numpy is not None and frame_numpy.size > 0:
                print("✅ Frame captured successfully!")
                break # Exit the loop once we have a frame
            time.sleep(0.02) # Wait 20ms between attempts
        
        if frame_numpy is None or frame_numpy.size == 0:
            print("❌ Error: Failed to capture a frame from the camera after multiple attempts.")
            return

        # 3. Perform Numerical Analysis on the NumPy Array
        print("\n--- Texture Numerical Analysis ---")
        print(f"Array data type: {frame_numpy.dtype}")
        print(f"Array dimensions: {frame_numpy.shape}")

        if frame_numpy.ndim == 3 and frame_numpy.shape[2] >= 3:
            channel_names = ["Blue", "Green", "Red", "Alpha"]
            for i in range(frame_numpy.shape[2]):
                channel_data = frame_numpy[:, :, i]
                mean = np.mean(channel_data)
                std_dev = np.std(channel_data)
                min_val = np.min(channel_data)
                max_val = np.max(channel_data)
                
                channel_name = channel_names[i] if i < len(channel_names) else f"Channel {i}"
                
                print(f"\n--- {channel_name} ---")
                print(f"  Mean: {mean:.2f}")
                print(f"  Std Dev: {std_dev:.2f}")
                print(f"  Min/Max: {min_val} / {max_val}")
        else:
            print("\nFrame is not in a standard HxWxC format, cannot perform channel analysis.")

        print("\n--- Top-Left 5x5 Pixel Values (First Channel) ---")
        print(frame_numpy[:5, :5, 0])


    except Exception as e:
        print(f"An error occurred: {e}")

    finally:
        if cam is not None and cam.is_running():
            print("\nStopping camera capture.")
            cam.stop_capture()

if __name__ == "__main__":
    run_camera_to_numpy_test()