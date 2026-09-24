import onnxruntime as ort
import numpy as np

# Define the path to your ONNX model
model_path = "inswapper_128.onnx"

try:
    # Create an inference session
    session = ort.InferenceSession(model_path, providers=ort.get_available_providers())
    print("ONNX Runtime session created successfully.")

    # Get model input details
    input_details = session.get_inputs()
    for i, input_meta in enumerate(input_details):
        print(f"Input {i}: Name={input_meta.name}, Shape={input_meta.shape}, Type={input_meta.type}")

    # Get model output details
    output_details = session.get_outputs()
    for i, output_meta in enumerate(output_details):
        print(f"Output {i}: Name={output_meta.name}, Shape={output_meta.shape}, Type={output_meta.type}")

    # --- Create dummy input data for BOTH inputs ---
    
    # Get the names of the input nodes
    target_input_name = session.get_inputs()[0].name
    source_input_name = session.get_inputs()[1].name

    # Create dummy data for the 'target' input (shape [1, 3, 128, 128])
    dummy_target_input = np.random.randn(1, 3, 128, 128).astype(np.float32)

    # Create dummy data for the 'source' input (shape [1, 512])
    dummy_source_input = np.random.randn(1, 512).astype(np.float32)

    # --- Run inference ---
    # Create the input feed dictionary with both inputs
    input_feed = {
        target_input_name: dummy_target_input,
        source_input_name: dummy_source_input
    }

    outputs = session.run(None, input_feed)
    print("\nInference executed successfully.")
    print(f"Number of outputs: {len(outputs)}")
    for i, output in enumerate(outputs):
        print(f"Output {i} shape: {output.shape}")


except Exception as e:
    print(f"An error occurred: {e}")