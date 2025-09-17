import sys
import os
import cv2
import numpy as np
import time

def find_module():
    """
    Finds the 'directport.pyd' module in common build directories.
    This makes the script runnable from the project root without extra setup.
    """
    # List of possible relative paths to the compiled module
    # This covers both Debug and Release builds.
    possible_paths = [
        'build/Debug',
        'build/Release',
        'build',
        'src/build/Debug',
        'src/build/Release',
        'src/build'
    ]
    
    # Find the first valid path
    for path in possible_paths:
        # Construct the full path to the .pyd file
        # The exact name depends on the Python version and platform
        # but we can search for the prefix 'directport'.
        build_dir = os.path.join(os.getcwd(), path)
        if os.path.exists(build_dir):
            for file in os.listdir(build_dir):
                if file.startswith("directport") and file.endswith(".pyd"):
                    print(f"✅ Found module in: {build_dir}")
                    sys.path.append(build_dir)
                    return True
    
    print("❌ Error: Could not find the 'directport.pyd' module.")
    print("   Please ensure you have compiled the project successfully.")
    print("   Searched in the following relative directories:", possible_paths)
    return False

def main():
    """Main function to run the camera test."""
    
    # Step 1: Find and import the C++ module
    if not find_module():
        return

    try:
        import directport
    except ImportError as e:
        print(f"Fatal error during import: {e}")
        return

    cam = None  # Initialize to None for the finally block
    try:
        # Step 2: Initialize the Camera
        print("Initializing camera...")
        cam = directport.DirectPortCamera()
        cam.init() # This calls createD3D12Device() and createSourceReader()
        print("Camera initialized successfully.")

        # Step 3: Start the capture loop
        cam.start_capture()
        print("🚀 Capture started. Displaying feed...")
        print("   Press 'q' in the video window to exit.")

        last_time = time.time()
        frame_count = 0
        fps = 0

        while cam.is_running():
            # Step 4: Get a frame from our C++ library
            frame = cam.get_frame()

            # Step 5: Check if the frame is valid
            if frame is not None and frame.size > 0:
                # Calculate FPS
                frame_count += 1
                current_time = time.time()
                elapsed = current_time - last_time
                if elapsed >= 1.0:
                    fps = frame_count / elapsed
                    frame_count = 0
                    last_time = current_time

                # Add FPS text to the frame using OpenCV
                fps_text = f"FPS: {fps:.2f}"
                cv2.putText(frame, fps_text, (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)
                
                # Display the resulting frame
                cv2.imshow('DirectPort Camera Feed', frame)
            else:
                print("Received an empty frame. Camera may be disconnected.")
                time.sleep(0.5)

            # Wait for 1ms and check if the 'q' key was pressed
            if cv2.waitKey(1) & 0xFF == ord('q'):
                print("'q' pressed, exiting loop.")
                break

    except Exception as e:
        # This will catch C++ exceptions forwarded by Pybind11
        print(f"\n--- An Error Occurred ---")
        print(f"ERROR: {e}")
        print("--------------------------")

    finally:
        # Step 6: Cleanup Resources
        # This block executes whether an error occurred or not.
        print("\nCleaning up resources...")
        if cam is not None and cam.is_running():
            cam.stop_capture()
            print("Capture stopped.")
        
        cv2.destroyAllWindows()
        print("OpenCV windows closed. Exiting.")


if __name__ == "__main__":
    main()