import directport
import time
import sys

# If the script is not in the same directory as the .pyd, uncomment the next line
# sys.path.append(r'B:\DirectPort')

def run():
    """
    True zero-copy Python consumer.
    This script discovers and renders shared textures without any
    external Python graphics libraries. All rendering is handled
    by the C++ directport module.
    """
    print("--- Python Zero-Copy Consumer ---")
    
    try:
        # --- THIS IS THE CORRECTED LINE ---
        # Initialize the Consumer with a window title, width, and height.
        # This single object now manages the window, the D3D device, and discovery.
        consumer = directport.Consumer("DirectPort Zero-Copy Consumer (Python Controlled)", 1280, 720)
        
        print("Consumer and Renderer initialized successfully.")
    except Exception as e:
        print(f"Fatal error during initialization: {e}")
        return

    active_producers = {}
    last_discovery_time = 0

    # Main Application Loop
    while True:
        # Let the C++ backend handle window messages (closing, moving, etc.)
        if not consumer.process_events():
            print("Window closed. Exiting.")
            break
        
        # Discover new producers every 2 seconds
        current_time = time.time()
        if current_time - last_discovery_time > 2.0:
            last_discovery_time = current_time
            print("Searching for producers...")
            discovered = consumer.discover()
            
            # Connect to any newly discovered producers
            for info in discovered:
                if info.pid not in active_producers:
                    print(f"Found new producer: {info.name} (PID: {info.pid}, Type: {info.type})")
                    producer = consumer.connect(info.pid)
                    if producer:
                        print(f"Successfully connected to PID {info.pid}")
                        active_producers[info.pid] = producer
                    else:
                        print(f"Failed to connect to PID {info.pid}")

        # Check for disconnected producers and prepare a list for rendering
        producers_to_render = []
        disconnected_pids = []
        for pid, producer in active_producers.items():
            if producer.wait_for_frame():
                producers_to_render.append(producer)
            else:
                print(f"Producer disconnected: PID {pid}")
                disconnected_pids.append(pid)
        
        # Remove dead producers
        for pid in disconnected_pids:
            del active_producers[pid]

        # Tell the C++ backend to render the frames from the active producers
        consumer.render_frame(producers_to_render)

        # Tell the C++ backend to present the final image to the screen
        consumer.present()

        # Sleep to yield CPU time, targeting ~60fps
        time.sleep(0.016)

    print("Shutdown complete.")

if __name__ == "__main__":
    run()