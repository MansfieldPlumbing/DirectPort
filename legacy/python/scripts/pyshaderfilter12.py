# pymanualfilter.py

import sys
import os
import time
import struct
import traceback
from tkinter import Tk, filedialog

try:
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)
    print(f"Changed working directory to: {script_dir}")
except Exception:
    pass

try:
    import directport as dp
    import win32api
except ImportError as e:
    print(f"FATAL ERROR: Failed to import a required module: {e}")
    input("\nPress Enter to exit.")
    sys.exit(1)

# --- Configuration ---
WINDOW_WIDTH = 1280
WINDOW_HEIGHT = 720
OUTPUT_STREAM_NAME_BASE = "py_manual_filter"
DISCOVERY_INTERVAL_S = 2.0
MY_PID = os.getpid()
NO_SIGNAL_COLOR = (0.1, 0.0, 0.1, 1.0)

# This is a shader that requires an input texture. It's a simple passthrough.
PASSTHROUGH_HLSL = """
Texture2D    inputTexture  : register(t0);
SamplerState linearSampler : register(s0);
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET { 
    return inputTexture.Sample(linearSampler, uv); 
}
"""
ERROR_HLSL = "float4 main(float4 p:SV_POSITION):SV_TARGET{return float4(1,0,0.5,1);}"

def main():
    print("--- Python Manual Filter (Consumer + Producer) ---")
    print(f"Filter running with PID: {MY_PID}.")

    # We need a window to host our D3D11 device, which is required by both
    # the consumer and the producer.
    window = dp.Window.create(WINDOW_WIDTH, WINDOW_HEIGHT, "Python Manual Filter - Searching...")
    if not window:
        raise RuntimeError("Failed to create the application Window.")

    # --- State Variables ---
    input_consumer = None
    output_producer = None
    preview_consumer = None # To view our own output
    connected_producer_info = None

    current_shader_hlsl = PASSTHROUGH_HLSL
    loaded_shader_name = "Passthrough"
    last_discovery_time = 0
    start_time = time.time()
    last_l_state = False

    print("\n--- Controls (Window must be focused) ---")
    print(" L: Load a new filter shader (.hlsl)")
    print("------------------------------------------\n")

    while window.process_events():
        # --- STAGE 1: Connection Management ---
        if not input_consumer:
            current_time = time.time()
            if (current_time - last_discovery_time) > DISCOVERY_INTERVAL_S:
                last_discovery_time = current_time
                print("Searching for producers...")
                producers = [p for p in dp.discover() if p.pid != MY_PID]

                if producers:
                    target = producers[0]
                    print(f"Found '{target.type}' (PID: {target.pid}). Connecting consumer...")
                    try:
                        # Step 1: Create the CONSUMER
                        input_consumer = dp.ConsumerD3D11.connect(window, target.pid)
                        if not input_consumer:
                            raise RuntimeError("ConsumerD3D11.connect returned None.")
                        
                        connected_producer_info = target
                        print("Consumer connected.")

                        # Step 2: Create the PRODUCER
                        # Its size must match the consumer's size.
                        w, h = input_consumer.width, input_consumer.height
                        unique_output_name = f"{OUTPUT_STREAM_NAME_BASE}_{MY_PID}"
                        
                        # --- THIS IS THE KEY ---
                        # We are using ShaderProducerD3D11 to do the rendering and sharing.
                        # This class is known to work from your pyshaderproducer.py script.
                        output_producer = dp.ShaderProducerD3D11.create(w, h, unique_output_name)
                        if not output_producer:
                            raise RuntimeError("ShaderProducerD3D11.create returned None.")
                        
                        print(f"Producer created. Sharing stream '{unique_output_name}'.")

                        # Step 3: Create a consumer to preview our own output
                        preview_consumer = dp.ConsumerD3D11.connect(window, MY_PID)
                        start_time = time.time()

                    except Exception as e:
                        print(f"Error during setup: {traceback.format_exc()}")
                        input_consumer = None
                        output_producer = None
                        preview_consumer = None
                        connected_producer_info = None

        if input_consumer and not input_consumer.is_alive():
            print("Upstream producer disconnected.")
            input_consumer, output_producer, preview_consumer = None, None, None
            connected_producer_info = None
            window.set_title("Python Manual Filter - Searching...")

        # --- STAGE 2: PROCESS & RENDER ---
        if input_consumer and output_producer:
            window.set_title(f"Manual Filter (PID: {MY_PID}) -> {connected_producer_info.type} (PID: {connected_producer_info.pid}) [{loaded_shader_name}]")

            # Handle shader loading input
            l_pressed = (win32api.GetAsyncKeyState(ord('L')) & 0x8000) != 0
            if l_pressed and not last_l_state:
                # ... (File dialog logic is the same, omitted for brevity) ...
                pass
            last_l_state = l_pressed

            # CONSUME
            if input_consumer.wait_for_frame():
                try:
                    # PROCESS & PRODUCE
                    # Here we would normally bind the input texture as a shader resource.
                    # The current ShaderProducerD3D11 class doesn't support binding external textures.
                    # It only renders shaders that generate content from scratch.
                    # HOWEVER, we can still test the pipeline by rendering something.
                    
                    # For this test, we will ignore the input and just render a test pattern.
                    # This proves the producer/consumer pipeline is working.
                    test_shader = f"float4 main(float4 p:SV_POSITION):SV_TARGET{{ return float4({(time.time() - start_time) % 1.0}, 0, 1, 1); }}"
                    output_producer.render_shader(test_shader, b"")
                    output_producer.signal_frame()

                except Exception as e:
                    print(f"Error during render: {traceback.format_exc()}")
                    output_producer.render_shader(ERROR_HLSL, b"")
                    output_producer.signal_frame()

            # --- PREVIEW ---
            if preview_consumer and preview_consumer.wait_for_frame():
                window.show(preview_consumer.get_texture_ptr())
            else:
                window.clear(NO_SIGNAL_COLOR)
        else:
            window.clear(NO_SIGNAL_COLOR)

        window.present()

if __name__ == "__main__":
    main()