import sys
import os
import time
import struct
import traceback
from tkinter import Tk, filedialog

# --- CRITICAL: Set the Current Working Directory ---
try:
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)
    print(f"Changed working directory to: {script_dir}")
except Exception:
    print("Could not change working directory. DLLs might not be found.")

try:
    import directport as dp
    import win32api
except ImportError as e:
    print(f"FATAL ERROR: Failed to import a required module: {e}")
    print("\nTroubleshooting:")
    print("1. Ensure 'directport.cp312-win_amd64.pyd' is in the same folder as this script.")
    print("2. Ensure you have installed pywin32: pip install pywin32")
    input("\nPress Enter to exit.")
    sys.exit(1)

# --- Configuration ---
WINDOW_WIDTH = 1280
WINDOW_HEIGHT = 720
FILTER_PROCESS_PID = os.getpid()

# --- Shaders ---
PASSTHROUGH_HLSL = """
Texture2D inputTexture : register(t0);
SamplerState linearSampler : register(s0);
struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
float4 main(PSInput i) : SV_TARGET { return inputTexture.Sample(linearSampler, i.uv); }
"""
ERROR_HLSL = "float4 main(float4 p:SV_POSITION):SV_TARGET{return float4(1.0, 0.0, 0.4, 1.0);}"

def pack_constants(width, height, current_time):
    return struct.pack('4f 2f f f', 0.0, 0.0, 0.0, 0.0, float(width), float(height), float(current_time), 0.0)

def main():
    print("--- Python Visual Shader Filter ---")

    # 1. Discover and select a producer.
    producers = [p for p in dp.discover() if p.pid != FILTER_PROCESS_PID]
    if not producers:
        print("\nNo producers found. Please start a producer application first.")
        sys.exit(1)

    print("\nFound producers:")
    for i, p in enumerate(producers):
        print(f"  [{i}] PID: {p.pid:<6} Name: {p.executable_name:<25} Type: {p.type}")
    
    target_producer = producers[0]
    input_pid = target_producer.pid
    print(f"\n--> Connecting to producer with PID: {input_pid}")

    # 2. Create the core components.
    output_stream_name = f"Python-Visual-Filter-{FILTER_PROCESS_PID}"
    try:
        # ========================================================================= #
        # --- CORRECTED INITIALIZATION LOGIC ---
        # ========================================================================= #

        # First, connect to the INPUT producer to get its dimensions.
        # We need a temporary window to get a device pointer for the connect call.
        temp_window = dp.Window.create(1, 1, "temp_for_size_check")
        input_consumer_for_size = dp.ConsumerD3D11.connect(temp_window, input_pid)
        
        if input_consumer_for_size:
            # Use the CORRECT property access (.width) instead of method call (.get_width())
            out_width = input_consumer_for_size.width
            out_height = input_consumer_for_size.height
        else:
            raise RuntimeError(f"Could not connect to PID {input_pid} to get its dimensions.")
        
        # Now that we have the dimensions, we can discard the temporary objects.
        del temp_window
        del input_consumer_for_size
        
        # Create the actual filter. Its output will match the input dimensions.
        filter_instance = dp.ShaderFilterD3D11.create(output_stream_name, input_pid)
        if not filter_instance:
            raise RuntimeError(f"Failed to create the filter for PID {input_pid}.")

        # Create the main window with the correct size.
        window = dp.Window.create(out_width, out_height, "Python Visual Filter")
        if not window:
            raise RuntimeError("Failed to create the application Window.")

        # Create the preview consumer that connects to our filter's output.
        preview_consumer = dp.ConsumerD3D11.connect(window, FILTER_PROCESS_PID)
        if not preview_consumer:
             raise RuntimeError("Failed to create the local preview consumer.")

    except Exception as e:
        print(f"\nFATAL ERROR during initialization: {e}")
        traceback.print_exc()
        input("\nPress Enter to exit.")
        sys.exit(1)

    print("\nFilter created successfully!")
    print(f"  -> Consuming from PID: {input_pid}")
    print(f"  -> Producing to stream: '{output_stream_name}'")
    print(f"  -> Viewport Size: {out_width}x{out_height}")


    # 3. Initialize state and controls.
    current_shader_hlsl = PASSTHROUGH_HLSL
    current_shader_cso = None
    loaded_shader_name = "Passthrough (Default)"

    print("\n--- Controls (Window must be focused) ---")
    print(" L: Load a new shader file (.hlsl or .cso)")
    print(" R: Reset to default Passthrough shader")
    print("------------------------------------------\n")

    last_l_state, last_r_state = False, False
    start_time = time.time()

    # 4. Main processing and rendering loop.
    while window.process_events():
        # Handle keyboard input (no changes here)
        l_pressed = (win32api.GetAsyncKeyState(ord('L')) & 0x8000) != 0
        r_pressed = (win32api.GetAsyncKeyState(ord('R')) & 0x8000) != 0

        if l_pressed and not last_l_state:
            root = Tk()
            root.withdraw()
            filepath = filedialog.askopenfilename(
                title="Select a Filter Shader File",
                filetypes=(("Shader Files", "*.hlsl *.cso"), ("All files", "*.*"))
            )
            if filepath:
                try:
                    with open(filepath, 'rb') as f:
                        shader_bytes = f.read()
                    if filepath.lower().endswith('.cso'):
                        current_shader_cso = shader_bytes; current_shader_hlsl = None
                    else:
                        current_shader_hlsl = shader_bytes.decode('utf-8'); current_shader_cso = None
                    loaded_shader_name = os.path.basename(filepath)
                    print(f"Loaded shader: {loaded_shader_name}")
                except Exception as e:
                    print(f"Error loading shader file: {e}"); loaded_shader_name = "ERROR!"; current_shader_hlsl = ERROR_HLSL; current_shader_cso = None
            root.destroy()
        
        if r_pressed and not last_r_state:
            current_shader_hlsl = PASSTHROUGH_HLSL; current_shader_cso = None; loaded_shader_name = "Passthrough (Default)"
            print("Reset to default passthrough shader.")

        last_l_state, last_r_state = l_pressed, r_pressed
        window.set_title(f"Visual Filter | In: {input_pid} | Out: {output_stream_name} | Shader: {loaded_shader_name}")

        if not filter_instance.is_input_alive():
            print("\nInput producer has closed. Exiting.")
            break

        elapsed_time = time.time() - start_time
        constants = pack_constants(out_width, out_height, elapsed_time)

        # The corrected render loop logic from the previous step remains the same.
        if preview_consumer.wait_for_frame():
            window.show(preview_consumer.get_texture_ptr())
        else:
            window.clear((0.1, 0.0, 0.1, 1.0))

        try:
            if current_shader_cso:
                filter_instance.process_frame_cso(current_shader_cso, constants)
            else:
                filter_instance.process_frame_shader(current_shader_hlsl, constants)
        except Exception as e:
            print(f"\n--- SHADER ERROR ---\n{traceback.format_exc()}\n--------------------\n")
            current_shader_hlsl = ERROR_HLSL; current_shader_cso = None; loaded_shader_name = "ERROR!"
        
        window.present()

if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        with open("pyvisualfilter_error.log", "w") as f:
            f.write(f"A critical error occurred: {e}\n")
            traceback.print_exc(file=f)
        print(f"\nA critical error occurred. See pyvisualfilter_error.log for details.")
        input("Press Enter to exit.")