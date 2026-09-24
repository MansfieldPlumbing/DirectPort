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

def main():
    print("--- Python Visual Consumer ---")

    producers = [p for p in dp.discover() if p.pid != os.getpid()]
    if not producers:
        print("\nNo producers found. Please start a producer application first.")
        sys.exit(1)

    print("\nFound producers:")
    for i, p in enumerate(producers):
        print(f"  [{i}] PID: {p.pid:<6} Name: {p.executable_name:<25} Type: {p.type}")
    
    target_producer = producers[0]
    input_pid = target_producer.pid
    print(f"\n--> Connecting to producer with PID: {input_pid}")

    try:
        # ========================================================================= #
        # --- CORRECTED SINGLE-DEVICE INITIALIZATION ---
        # ========================================================================= #

        # 1. Create a tiny, temporary window. Its only purpose is to provide a D3D11 device.
        size_check_window = dp.Window.create(1, 1, "device_provider")
        if not size_check_window:
            raise RuntimeError("Failed to create a temporary window for device context.")

        # 2. Use the temporary window's device to connect and get the real dimensions.
        input_consumer_for_size = dp.ConsumerD3D11.connect(size_check_window, input_pid)
        if not input_consumer_for_size:
            raise RuntimeError(f"Could not connect to PID {input_pid} to get its dimensions.")
        
        producer_width = input_consumer_for_size.width
        producer_height = input_consumer_for_size.height
        del input_consumer_for_size

        # 3. NOW, create the FINAL window with the correct dimensions.
        window = dp.Window.create(producer_width, producer_height, "Python Visual Consumer")
        if not window:
            raise RuntimeError("Failed to create the main application Window.")

        # 4. Create the final consumer using the FINAL window's device.
        consumer = dp.ConsumerD3D11.connect(window, input_pid)
        if not consumer:
             raise RuntimeError("Failed to create the final consumer.")
        
        # The size_check_window object is kept alive but is not used anymore.
        # This prevents it from being destroyed and sending a premature quit message.

    except Exception as e:
        print(f"\nFATAL ERROR during initialization: {e}")
        traceback.print_exc()
        input("\nPress Enter to exit.")
        sys.exit(1)

    print("\nConsumer connected successfully!")
    print(f"  -> Consuming from PID: {input_pid}")
    print(f"  -> Viewport Size: {producer_width}x{producer_height}")
    
    # --- Main Loop ---
    while window.process_events():
        window.set_title(f"Visual Consumer | Connected to PID: {input_pid}")

        if not consumer.is_alive():
            print("\nProducer has closed. Exiting.")
            break

        # In a single-device consumer, the render loop is simple and robust.
        if consumer.wait_for_frame():
            window.show(consumer.get_texture_ptr())
        else:
            # If waiting fails, clear to a color to indicate a lost signal.
            window.clear((0.0, 0.2, 0.4, 1.0))
        
        window.present()

if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        with open("pyvisualconsumer_error.log", "w") as f:
            f.write(f"A critical error occurred: {e}\n")
            traceback.print_exc(file=f)
        print(f"\nA critical error occurred. See pyvisualconsumer_error.log for details.")
        input("Press Enter to exit.")