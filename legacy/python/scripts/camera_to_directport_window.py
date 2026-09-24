# Save as threaded_camera_viewer.py
import sys
import os
import time
import threading
import queue

def find_and_import_directport():
    """Finds the directport .pyd module in the current directory and imports it."""
    for file in os.listdir('.'):
        if file.startswith("directport") and file.endswith(".pyd"):
            print(f"✅ Found module: {file}")
            import directport
            return directport
    print("❌ Error: Could not find the 'directport' module in the current directory.")
    return None

# --- Global Threading Objects ---
# A thread-safe queue to hold the latest frame from the camera.
# maxsize=1 ensures it only ever stores the most recent frame, preventing lag.
frame_queue = queue.Queue(maxsize=1)
# An event to cleanly signal the camera thread to stop.
stop_event = threading.Event()

def camera_worker():
    """
    This function runs on a separate thread. It creates, initializes,
    runs, and shuts down the camera object entirely within this thread,
    ensuring all COM operations are on the same thread.
    """
    cam = None
    try:
        print("📷 Camera thread started. Initializing camera...")
        directport = find_and_import_directport()
        if not directport:
            raise ImportError("DirectPort module not found in camera thread.")
            
        cam = directport.DirectPortCamera()
        # init() correctly creates the COM apartment for this thread.
        cam.init()
        cam.start_capture()
        print("📷 Camera initialized and running.")
        
        while not stop_event.is_set():
            frame = cam.get_frame()
            if frame is not None and frame.size > 0:
                try:
                    # Non-blocking put: if the main thread is slow, we just replace the old frame.
                    frame_queue.get_nowait() 
                except queue.Empty:
                    pass # Queue was empty, which is fine.
                frame_queue.put(frame)
            else:
                # Avoid busy-looping if the camera fails for a moment
                time.sleep(0.01)
    
    except Exception as e:
        print(f"--- ERROR in Camera Thread ---: {e}")

    finally:
        if cam:
            print("📷 Camera thread shutting down...")
            # shutdown() correctly cleans up the COM apartment on this thread.
            cam.shutdown()
        print("📷 Camera thread finished.")

def main():
    """
    Main function to run the camera -> DirectPort window test.
    The main thread handles the window and rendering.
    The background thread handles the camera.
    """
    directport = find_and_import_directport()
    if not directport:
        return

    device = None
    camera_thread = None

    try:
        # --- Start the Camera Worker Thread ---
        camera_thread = threading.Thread(target=camera_worker)
        camera_thread.start()

        # --- Get First Frame to Determine Dimensions ---
        print("MAIN: Waiting for first frame from camera thread...")
        try:
            initial_frame = frame_queue.get(timeout=5)
        except queue.Empty:
            raise RuntimeError("Camera failed to produce a frame within 5 seconds. Is it connected and working?")
            
        height, width, _ = initial_frame.shape
        print(f"MAIN: Detected camera resolution: {width}x{height}")

        # --- Initialize DirectPort Graphics and Window on the Main Thread ---
        print("MAIN: Initializing Direct3D device and window...")
        device = directport.DeviceD3D11.create()
        win = device.create_window(width, height, "DirectPort Camera Feed")
        gpu_texture = device.create_texture(width, height, directport.DXGI_FORMAT.B8G8R8A8_UNORM)

        # --- Main Render Loop ---
        print("🚀 Capture started. Displaying in DirectPort window...")
        
        last_time = time.time()
        frame_count = 0
        latest_frame = initial_frame # Start with the frame we already have.

        while win.process_events():
            try:
                # Get the newest frame from the queue without blocking.
                latest_frame = frame_queue.get_nowait()
            except queue.Empty:
                # No new frame was ready. We'll just re-render the last one.
                # This is crucial for keeping the window responsive.
                pass

            # Upload the NumPy array data directly to the GPU texture.
            directport.numpy.write_texture(device, gpu_texture, latest_frame)
            
            # Blit (render) the GPU texture to the window
            device.blit(gpu_texture, win)
            
            win.present()

            # FPS calculation
            frame_count += 1
            current_time = time.time()
            elapsed = current_time - last_time
            if elapsed >= 1.0:
                fps = frame_count / elapsed
                win.set_title(f"DirectPort Camera Feed | FPS: {fps:.2f}")
                frame_count = 0
                last_time = current_time

    except Exception as e:
        print(f"\n--- An Error Occurred in Main Thread ---")
        print(f"ERROR: {e}")
    finally:
        # --- Cleanup Resources ---
        print("\nMAIN: Cleaning up resources...")
        stop_event.set() # Signal the camera thread to stop
        
        if camera_thread is not None:
            print("MAIN: Waiting for camera thread to join...")
            camera_thread.join(timeout=2) # Wait for the thread to finish cleanly
        
        print("MAIN: Window closed. Exiting.")
        input("Press Enter to exit.")

if __name__ == "__main__":
    main()