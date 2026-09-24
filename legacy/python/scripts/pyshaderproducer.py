import sys
import os
import time
import struct
import traceback
from tkinter import Tk, filedialog

# --- CRITICAL: Set the Current Working Directory ---
# This ensures that dependent DLLs (like d3dcompiler_47.dll) are found
# when the script is run from a different directory.
try:
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)
    print(f"Changed working directory to: {script_dir}")
except Exception:
    # This might fail in some bundled environments, but is robust for standard execution.
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
STREAM_NAME = "py_shader_producer_stream"
PRODUCER_PID = os.getpid()

def pack_constants(width, height, current_time):
    """Packs data into a bytes object matching the HLSL ConstantBuffer struct."""
    # cbuffer Constants { float4 bar_rect; float2 resolution; float time; float pad; }
    return struct.pack('4f 2f f f', 0.0, 0.0, 0.0, 0.0, float(width), float(height), float(current_time), 0.0)

def main():
    print("--- Python Shader Producer ---")
    print(f"Producer running with PID: {PRODUCER_PID}. Close the window to exit.")

    window = dp.Window.create(WINDOW_WIDTH, WINDOW_HEIGHT, "Python Shader Producer (D3D11)")
    if not window:
        raise RuntimeError("Failed to create the application Window.")

    shader_producer = dp.ShaderProducerD3D11.create(WINDOW_WIDTH, WINDOW_HEIGHT, STREAM_NAME)
    if not shader_producer:
        raise RuntimeError("Failed to create the D3D11 Shader Producer.")

    # This consumer is for the local preview window only
    preview_consumer = dp.ConsumerD3D11.connect(window, PRODUCER_PID)

    # --- BEHAVIOR FIX: Add a clear shader and start with no shader loaded ---
    clear_shader_hlsl = "float4 main(float4 p:SV_POSITION):SV_TARGET{return float4(0.0, 0.2, 0.4, 1.0);}"
    error_hlsl = "float4 main(float4 p:SV_POSITION):SV_TARGET{return float4(1,0,0,1);}"
    
    current_shader_hlsl = None
    current_shader_cso = None
    loaded_shader_name = "No Shader Loaded"
    # --- END OF FIX ---

    print("\n--- Controls (Window must be focused) ---")
    print(" L: Load a new shader file (.hlsl or .cso)")
    print(" R: Reset to clear blue screen")
    print("------------------------------------------\n")

    last_l_state, last_r_state = False, False

    # --- TIME FIX: Initialize start time for correct animation ---
    start_time = time.time()

    while window.process_events():
        l_pressed = (win32api.GetAsyncKeyState(ord('L')) & 0x8000) != 0
        r_pressed = (win32api.GetAsyncKeyState(ord('R')) & 0x8000) != 0

        if l_pressed and not last_l_state:
            root = Tk()
            root.withdraw() # Hide the empty Tkinter window
            filepath = filedialog.askopenfilename(
                title="Select a Producer Shader File",
                filetypes=(("Shader Files", "*.hlsl *.cso"), ("All files", "*.*"))
            )
            if filepath:
                try:
                    with open(filepath, 'rb') as f:
                        shader_bytes = f.read()
                    if filepath.lower().endswith('.cso'):
                        current_shader_cso = shader_bytes
                        current_shader_hlsl = None
                    else:
                        current_shader_hlsl = shader_bytes.decode('utf-8')
                        current_shader_cso = None
                    loaded_shader_name = os.path.basename(filepath)
                    print(f"Loaded shader: {loaded_shader_name}")
                except Exception as e:
                    print(f"Error loading shader file: {e}")
            root.destroy()
        
        # --- BEHAVIOR FIX: 'R' key now resets to the clear screen state ---
        if r_pressed and not last_r_state:
            current_shader_hlsl = None
            current_shader_cso = None
            loaded_shader_name = "No Shader Loaded"
            print("Reset to clear screen.")

        last_l_state, last_r_state = l_pressed, r_pressed

        window.set_title(f"Shader Producer (D3D11) - Rendering: {loaded_shader_name}")

        # --- TIME FIX: Calculate elapsed time instead of absolute time ---
        elapsed_time = time.time() - start_time
        constants = pack_constants(WINDOW_WIDTH, WINDOW_HEIGHT, elapsed_time)
        
        try:
            # --- BEHAVIOR FIX: Render logic now handles the "no shader" state ---
            if current_shader_cso:
                shader_producer.render_cso(current_shader_cso, constants)
            elif current_shader_hlsl:
                shader_producer.render_shader(current_shader_hlsl, constants)
            else:
                # No shader is loaded, so render the simple clear color shader
                shader_producer.render_shader(clear_shader_hlsl)

            shader_producer.signal_frame()
            
        except Exception as e:
            # If a loaded shader fails to compile/run, show an error state
            print(f"\n--- SHADER ERROR ---\n{e}\n--------------------\n")
            current_shader_hlsl = error_hlsl
            current_shader_cso = None
            loaded_shader_name = "ERROR! (See console for details)"

        # The preview window logic remains the same
        if preview_consumer and preview_consumer.wait_for_frame():
            window.show(preview_consumer.get_texture_ptr())
        else:
            # This clear is for the window itself, in case the consumer fails
            window.clear((0.1, 0.0, 0.0, 1.0))

        window.present()

if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        # Log any fatal script errors for easier debugging
        with open("pyshaderproducer_error.log", "w") as f:
            f.write(f"A critical error occurred: {e}\n")
            traceback.print_exc(file=f)