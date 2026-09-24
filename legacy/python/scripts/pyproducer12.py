import directport
import time
import sys
import struct
import keyboard
from tkinter import Tk, filedialog

# --- Application State ---
# We use a dictionary to hold state that the keyboard hook can modify.
app_state = {
    "shader_bytes": None,
    "shader_path": "None",
    "shader_dirty": True  # Flag to indicate a new shader has been loaded
}

def open_shader_file():
    """Opens a file dialog and loads the content of a shader file into app_state."""
    print("\nOpening file dialog...")
    # Hide the root Tkinter window
    root = Tk()
    root.withdraw()
    
    # Define file types for the dialog
    filetypes = (
        ('Shader Files', '*.hlsl *.cso'),
        ('All files', '*.*')
    )
    
    # Open the dialog
    filepath = filedialog.askopenfilename(
        title='Open Pixel Shader File',
        filetypes=filetypes
    )

    if not filepath:
        print("No file selected.")
        return

    try:
        with open(filepath, 'rb') as f:
            app_state["shader_bytes"] = f.read()
            app_state["shader_path"] = filepath
            app_state["shader_dirty"] = True # Signal to the main loop to update
            print(f"Successfully loaded shader: {filepath}")
    except Exception as e:
        print(f"Error loading shader file: {e}", file=sys.stderr)
        app_state["shader_bytes"] = None
        app_state["shader_path"] = "Error"
        app_state["shader_dirty"] = True


def main():
    """
    A D3D12 Python producer that renders a dynamically loaded shader 
    and shares it via DirectPort.
    """
    print("--- DirectPort D3D12 Shader Producer ---")
    print("Press [SPACE] to open a file dialog and load a pixel shader.")
    
    # Set up the keyboard hook. This runs in the background.
    # Note: On some systems, this may require administrator privileges.
    keyboard.on_press_key("space", lambda _: open_shader_file())

    try:
        device = directport.DeviceD3D12.create()
    except RuntimeError as e:
        print(f"Fatal: Failed to create D3D12 device: {e}", file=sys.stderr)
        sys.exit(1)

    RENDER_WIDTH, RENDER_HEIGHT = 1280, 720
    window = device.create_window(RENDER_WIDTH, RENDER_HEIGHT, "D3D12 Shader Producer - No Shader Loaded")
    
    # This is the texture we will render our shader into.
    shader_texture = device.create_texture(RENDER_WIDTH, RENDER_HEIGHT, directport.DXGI_FORMAT.B8G8R8A8_UNORM)

    # Create the producer to share the texture with consumers.
    producer = device.create_producer("directport_py_shader_d3d12", shader_texture)

    print("Device and producer initialized successfully. Starting render loop...")
    start_time = time.time()
    
    try:
        active_shader_bytes = None
        while window.process_events():
            
            # Check if the keyboard hook has loaded a new shader
            if app_state["shader_dirty"]:
                active_shader_bytes = app_state["shader_bytes"]
                window.set_title(f"D3D12 Shader Producer - Loaded: {app_state['shader_path']}")
                app_state["shader_dirty"] = False

            current_time = time.time() - start_time
            
            # The C++ examples use a struct with 8 floats:
            # float bar_rect[4]; float resolution[2]; float time; float pad;
            constants = struct.pack(
                '8f',  # Pack 8 floating-point numbers
                0.0, 0.0, 0.0, 0.0,  # bar_rect (unused)
                float(RENDER_WIDTH), float(RENDER_HEIGHT), # resolution
                current_time,        # time
                0.0                  # padding
            )
            
            # --- Render Pass ---
            # Apply the loaded shader to our texture.
            # If no shader is loaded, the C++ backend renders black by default.
            device.apply_shader(
                output=shader_texture, 
                shader=active_shader_bytes or b'', 
                entry_point="main", 
                inputs=[], 
                constants=constants
            )

            # Signal to consumers that a new frame is ready on the GPU.
            producer.signal_frame()
            
            # Blit the result to our local window for preview.
            device.blit(shader_texture, window)
            
            window.present()

    except KeyboardInterrupt:
        print("\nScript interrupted by user.")
    finally:
        print("Shutting down.")


if __name__ == "__main__":
    main()