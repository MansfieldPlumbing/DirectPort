import directport
import time
import sys

def main():
    """
    A Python script to discover, connect to, and display a texture stream
    from a DirectPort producer, now with robust window resizing.
    """
    print("Initializing DirectPort D3D11 device...")
    try:
        device = directport.DeviceD3D11.create()
    except RuntimeError as e:
        print(f"Fatal: Failed to create D3D11 device: {e}", file=sys.stderr)
        print("Please ensure a compatible GPU and the latest DirectX runtime are installed.", file=sys.stderr)
        sys.exit(1)

    window = device.create_window(1280, 720, "DirectPort Python Consumer - Searching...")

    consumer = None
    local_texture = None
    shared_texture = None
    
    # Track the window's last known size
    last_width, last_height = window.get_width(), window.get_height()

    try:
        while window.process_events():
            # --- PRIMITIVE-BASED RESIZE LOGIC ---
            current_width, current_height = window.get_width(), window.get_height()
            if current_width != last_width or current_height != last_height:
                if current_width > 0 and current_height > 0:
                    print(f"Window resized to {current_width}x{current_height}. Recreating swap chain...")
                    # Command the C++ engine to perform the resize primitive
                    device.resize_window(window)
                    last_width, last_height = current_width, current_height
                else: 
                    # Window was minimized, sleep to avoid busy-waiting
                    time.sleep(0.1)
                    continue
            # --- END OF RESIZE LOGIC ---

            if consumer is None or not consumer.is_alive():
                if consumer:
                    print(f"Producer PID {consumer.pid} has disconnected. Resuming search...")
                    consumer = None
                    window.set_title("DirectPort Python Consumer - Searching...")

                producers = directport.discover()
                if producers:
                    target = producers[0]
                    print(f"Found producer: '{target.executable_name}' (PID: {target.pid})")
                    try:
                        consumer = device.connect_to_producer(target.pid)
                        if consumer:
                            print(f"Successfully connected to PID {consumer.pid}.")
                            window.set_title(f"Connected to {target.executable_name} (PID: {consumer.pid})")
                            local_texture = consumer.get_texture()
                            shared_texture = consumer.get_shared_texture()
                        else:
                            print(f"Failed to connect to PID {target.pid}.")
                    except Exception as e:
                        print(f"An error occurred while trying to connect: {e}", file=sys.stderr)
                        consumer = None
                else:
                    # Clear to a default blue color while searching
                    device.clear(window, 0.1, 0.1, 0.4, 1.0)
                    window.present()
                    time.sleep(1)

            if consumer and local_texture:
                if consumer.wait_for_frame():
                    device.copy_texture(shared_texture, local_texture)
                    device.blit(local_texture, window)
                    window.present()
                else:
                    time.sleep(0.001)

    except KeyboardInterrupt:
        print("\nScript interrupted by user.")
    except Exception as e:
        print(f"\nAn unexpected error occurred: {e}", file=sys.stderr)
    finally:
        print("Shutting down.")

if __name__ == "__main__":
    main()