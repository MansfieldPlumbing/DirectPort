# --- pymultiplexer11.py ---
import directport
import time
import sys
import math

def main():
    """
    A D3D11 multiplexer that discovers all DirectPort producers,
    composites them into a grid, and broadcasts the result.
    """
    print("--- DirectPort D3D11 Multiplexer ---")
    
    try:
        device = directport.DeviceD3D11.create()
    except RuntimeError as e:
        print(f"Fatal: Failed to create D3D11 device: {e}", file=sys.stderr)
        sys.exit(1)

    # --- 1. PRODUCER (OUTPUT) SETUP ---
    MUX_WIDTH, MUX_HEIGHT = 1920, 1080
    window_title = "D3D11 Multiplexer"
    window = device.create_window(1280, 720, f"{window_title} - Initializing...")

    # The texture we render the final grid into (private)
    composite_texture = device.create_texture(MUX_WIDTH, MUX_HEIGHT, directport.DXGI_FORMAT.B8G8R8A8_UNORM)
    
    # The texture we share with the world (will be a copy of the composite)
    shared_out_texture = device.create_texture(MUX_WIDTH, MUX_HEIGHT, directport.DXGI_FORMAT.B8G8R8A8_UNORM)

    # The multiplexer's own producer instance
    producer = device.create_producer("directport_mux_d3d11", shared_out_texture)

    # --- 2. CONSUMER (INPUT) SETUP ---
    connections = {} # Key: PID, Value: {consumer, private_texture}
    last_discovery_time = 0
    
    print("Initialization complete. Starting main loop...")

    try:
        while window.process_events():
            
            # --- DISCOVERY STEP (every 2 seconds) ---
            if time.time() - last_discovery_time > 2.0:
                last_discovery_time = time.time()
                
                # Discover all active producers
                try:
                    discovered_producers = {p.pid: p for p in directport.discover()}
                except Exception as e:
                    print(f"Warning: Discovery failed: {e}", file=sys.stderr)
                    discovered_producers = {}
                    
                current_pids = set(connections.keys())
                discovered_pids = set(discovered_producers.keys())

                # Connect to new producers
                for pid in discovered_pids - current_pids:
                    if pid == producer.pid: continue # Don't connect to ourselves
                    print(f"New producer found with PID: {pid}. Attempting to connect...")
                    try:
                        consumer = device.connect_to_producer(pid)
                        if consumer and consumer.is_alive():
                            # Create a local texture to copy the shared content into
                            shared_tex_info = consumer.get_texture()
                            private_texture = device.create_texture(
                                shared_tex_info.width, shared_tex_info.height, shared_tex_info.format
                            )
                            connections[pid] = {'consumer': consumer, 'private_texture': private_texture}
                            print(f"Successfully connected to PID: {pid}")
                        else:
                            print(f"Failed to connect to PID: {pid}")
                    except Exception as e:
                        print(f"Error connecting to PID {pid}: {e}", file=sys.stderr)

                # Disconnect from lost producers
                for pid in current_pids - discovered_pids:
                    print(f"Producer with PID: {pid} disconnected.")
                    if pid in connections:
                        del connections[pid]
                
                # Also check if any existing connections are no longer alive
                for pid in list(connections.keys()):
                    if not connections[pid]['consumer'].is_alive():
                        print(f"Producer with PID: {pid} is no longer alive.")
                        del connections[pid]
                
                window.set_title(f"{window_title} - Consuming: {len(connections)} | Producing PID: {producer.pid}")

            # --- RENDER LOOP ---

            # STAGE 1: CONSUME
            # Copy new frames from shared textures to our private textures
            for pid, data in connections.items():
                if data['consumer'].wait_for_frame():
                    device.copy_texture(data['consumer'].get_shared_texture(), data['private_texture'])

            # STAGE 2: COMPOSE
            # Clear the main composite texture (by applying a black shader)
            device.apply_shader(output=composite_texture, shader=b'')

            # Calculate grid layout and blit each private texture
            active_consumers = list(connections.values())
            if active_consumers:
                count = len(active_consumers)
                cols = int(math.ceil(math.sqrt(count)))
                rows = (count + cols - 1) // cols
                
                cell_w = MUX_WIDTH // cols
                cell_h = MUX_HEIGHT // rows

                for i, data in enumerate(active_consumers):
                    grid_col = i % cols
                    grid_row = i // cols
                    
                    dest_x = grid_col * cell_w
                    dest_y = grid_row * cell_h
                    
                    device.blit_texture_to_region(
                        source=data['private_texture'],
                        destination=composite_texture,
                        dest_x=dest_x,
                        dest_y=dest_y,
                        dest_width=cell_w,
                        dest_height=cell_h
                    )

            # STAGE 3: PRODUCE
            # Copy our final composite to the texture we are sharing
            device.copy_texture(composite_texture, shared_out_texture)
            producer.signal_frame()

            # STAGE 4: PRESENT
            # Blit the final composite to our local window for preview
            device.blit(composite_texture, window)
            window.present()

    except KeyboardInterrupt:
        print("\nScript interrupted by user.")
    finally:
        print("Shutting down.")
        # The 'connections' dict and other objects will be garbage collected.

if __name__ == "__main__":
    main()