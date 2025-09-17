import directport
import time
import sys

def main():
    """
    A Python script that uses a D3D12 device to discover, connect to,
    and display a texture stream from any DirectPort producer, now with robust window resizing.
    """
    print("Initializing DirectPort D3D12 device...")
    try:
        device = directport.DeviceD3D12.create()
    except RuntimeError as e:
        print(f"Fatal: Failed to create D3D12 device: {e}", file=sys.stderr)
        print("Please ensure a compatible GPU and the latest DirectX runtime are installed.", file=sys.stderr)
        sys.exit(1)

    window = device.create_window(1280, 720, "DirectPort Python D3D12 Consumer - Searching...")

    consumer = None
    
    last_width, last_height = window.get_width(), window.get_height()

    try:
        while window.process_events():
            current_width, current_height = window.get_width(), window.get_height()
            if current_width != last_width or current_height != last_height:
                if current_width > 0 and current_height > 0:
                    print(f"Window resized to {current_width}x{current_height}. Recreating swap chain...")
                    device.resize_window(window)
                    last_width, last_height = current_width, current_height
                else:
                    time.sleep(0.1)
                    continue

            if consumer is None or not consumer.is_alive():
                if consumer:
                    print(f"Producer PID {consumer.pid} has disconnected. Resuming search...")
                    consumer = None
                    window.set_title("DirectPort Python D3D12 Consumer - Searching...")

                producers = directport.discover()
                if producers:
                    target = producers[0]
                    print(f"Found producer: '{target.executable_name}' (PID: {target.pid})")
                    try:
                        consumer = device.connect_to_producer(target.pid)
                        if consumer:
                            print(f"Successfully connected to PID {consumer.pid}.")
                            window.set_title(f"Connected to {target.executable_name} (PID: {consumer.pid})")
                        else:
                            print(f"Failed to connect to PID {target.pid}.")
                    except Exception as e:
                        print(f"An error occurred while trying to connect: {e}", file=sys.stderr)
                        consumer = None
                else:
                    device.clear(window, 0.1, 0.1, 0.4, 1.0)
                    window.present()
                    time.sleep(1)

            if consumer:
                if consumer.wait_for_frame():
                    shared_texture = consumer.get_shared_texture()
                    local_texture = consumer.get_texture()
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