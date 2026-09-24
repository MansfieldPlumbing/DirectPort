# Save this file as DPCamera.py in your project root

import sys
import os
import time

def find_module():
    """Finds the 'directport.pyd' module."""
    # --- FIX: Add '.' to search the current directory first ---
    possible_paths = ['.', 'build/Debug', 'build/Release']
    
    for path in possible_paths:
        build_dir = os.path.join(os.getcwd(), path)
        if os.path.exists(build_dir):
            for file in os.listdir(build_dir):
                if file.startswith("directport") and file.endswith(".pyd"):
                    print(f"✅ Found module in: {build_dir}")
                    # Add the directory to sys.path if it's not the current one
                    if path != '.':
                        sys.path.append(build_dir)
                    return True
    print("❌ Error: Could not find 'directport.pyd'.")
    return False

def main():
    if not find_module():
        input("Press Enter to exit.") # Add a pause so the error can be read
        return

    import directport

    cam = None
    try:
        # 1. Initialize the camera source
        print("Initializing camera...")
        cam = directport.DirectPortCamera()
        cam.init()
        print("Camera initialized successfully.")

        # 2. Create a DirectPort-style window using the camera module
        print("Creating DirectPort window...")
        window = cam.create_window(1280, 720, "DirectPort Camera Feed")

        # 3. Start capturing frames
        cam.start_capture()
        print("🚀 Capture started. Displaying feed in DirectPort window...")
        print("   Close the window to exit.")

        # 4. Main Render Loop
        while cam.process_events(window):
            # Render the latest frame directly to the window's back buffer
            cam.render_frame_to_window(window)

            # Present the back buffer to the screen
            cam.present(window)

    except Exception as e:
        print(f"\n--- An Error Occurred ---")
        print(f"ERROR: {e}")

    finally:
        print("\nCleaning up resources...")
        if cam is not None and cam.is_running():
            cam.stop_capture()
            print("Capture stopped.")
        print("Exiting.")
        input("Press Enter to exit.") # Add a final pause


if __name__ == "__main__":
    main()