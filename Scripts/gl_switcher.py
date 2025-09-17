# File: gl_switcher.py

import directport
import time
import os
import sys

def main():
    """
    Connects to all available producers and re-broadcasts one of them,
    switching between sources every few seconds.
    """
    print("--- OpenGL Switcher (Multiplexer) ---")
    
    try:
        device = directport.gl.Device.create()
    except Exception as e:
        print(f"Fatal: Could not create device. Error: {e}", file=sys.stderr)
        sys.exit(1)

    WIDTH, HEIGHT = 1280, 720
    my_pid = os.getpid()
    
    window = device.create_window(WIDTH, HEIGHT, f"GL Switcher (PID: {my_pid}) - Initializing...")
    
    output_texture = device.create_texture(WIDTH, HEIGHT, directport.gl.DXGI_FORMAT.B8G8R8A8_UNORM)
    producer = device.create_producer(f"gl_python_switcher_{my_pid}", output_texture)
    print(f"Broadcasting output stream from PID: {my_pid}")

    connections = {} 
    last_discovery_time = 0
    last_switch_time = 0
    active_source_index = 0

    try:
        while window.process_events():
            now = time.time()
            
            if now - last_discovery_time > 2.0:
                last_discovery_time = now
                discovered = {p.pid: p for p in directport.gl.discover() if p.pid != my_pid}
                current_pids = set(connections.keys())
                discovered_pids = set(discovered.keys())

                for pid in discovered_pids - current_pids:
                    try:
                        print(f"Attempting to connect to new producer: {pid} ({discovered[pid].stream_name})...")
                        consumer = device.connect_to_producer(pid)
                        if consumer:
                            connections[pid] = {'consumer': consumer}
                            print(f"Connected to new producer: {pid}")
                    except Exception as e:
                        print(f"Failed to connect to {pid}: {e}", file=sys.stderr)
                
                for pid in list(connections.keys()):
                    if not connections[pid]['consumer'].is_alive():
                        print(f"Producer {pid} disconnected.")
                        del connections[pid]
            
            active_connections = list(connections.values())
            
            if not active_connections:
                window.set_title(f"GL Switcher (PID: {my_pid}) - Searching...")
                device.clear(window, 0.4, 0.1, 0.1, 1.0)
            else:
                if now - last_switch_time > 5.0:
                    last_switch_time = now
                    active_source_index = (active_source_index + 1) % len(active_connections)
                    print(f"Switching to source {active_source_index + 1}/{len(active_connections)}")

                source_consumer = active_connections[active_source_index]['consumer']
                window.set_title(f"GL Switcher | Showing PID: {source_consumer.pid}")

                if source_consumer.wait_for_frame():
                    received_texture = source_consumer.get_texture()
                    # Resize by blitting, not a direct copy
                    device.blit_texture_to_region(source=received_texture, destination=output_texture,
                                                  dest_x=0, dest_y=0, dest_width=WIDTH, dest_height=HEIGHT)
                
                producer.signal_frame()
                device.blit(output_texture, window)

            window.present()

    except KeyboardInterrupt:
        print("\nSwitcher shutting down.")
    finally:
        print("Exiting.")

if __name__ == "__main__":
    main()