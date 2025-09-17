# File: gl_consumer.py

import directport
import time
import sys

def main():
    """A simple OpenGL consumer that finds and displays a stream."""
    print("--- OpenGL Consumer ---")

    try:
        device = directport.gl.Device.create()
    except Exception as e:
        print(f"Fatal: Could not create device. Error: {e}", file=sys.stderr)
        sys.exit(1)

    window = device.create_window(1280, 720, "OpenGL Consumer - Searching...")
    consumer = None

    try:
        while window.process_events():
            if consumer is None or not consumer.is_alive():
                if consumer:
                    print(f"Producer PID {consumer.pid} disconnected. Resuming search...")
                    consumer = None
                
                window.set_title("OpenGL Consumer - Searching...")
                producers = directport.gl.discover()
                
                if producers:
                    target = producers[0]
                    print(f"Found producer: '{target.executable_name}' (PID: {target.pid}, Stream: '{target.stream_name}'). Connecting...")
                    try:
                        consumer = device.connect_to_producer(target.pid)
                        if consumer:
                             print("Successfully connected.")
                        else:
                             print(f"Connection to PID {target.pid} failed. The producer might have closed.")
                             # Sleep to avoid immediately retrying a failing connection
                             time.sleep(2)
                    except Exception as e:
                        print(f"Error connecting to producer: {e}", file=sys.stderr)
                        consumer = None
                        time.sleep(2)
                else:
                    device.clear(window, 0.1, 0.1, 0.4, 1.0) # Blue "searching" screen
                    window.present()
                    time.sleep(1) # Wait before searching again
            
            if consumer:
                window.set_title(f"Connected to PID {consumer.pid}")
                
                # Wait for the producer to signal a new frame. This handles GPU synchronization.
                if consumer.wait_for_frame():
                    # Get the local texture that is now synchronized with the producer's content
                    received_texture = consumer.get_texture()
                    
                    # Blit it to our window
                    device.blit(received_texture, window)
                
                window.present()

    except KeyboardInterrupt:
        print("\nConsumer shutting down.")
    finally:
        print("Exiting.")

if __name__ == "__main__":
    main()