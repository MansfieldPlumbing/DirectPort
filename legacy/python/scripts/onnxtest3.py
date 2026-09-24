import directport
import numpy as np
import onnx
from onnx import helper, TensorProto
import os
import sys

def create_simple_add_model(model_path: str, height: int, width: int):
    """
    Creates a simple ONNX model that adds its input to itself.
    This provides a predictable output (input * 2) for verification.
    """
    # Define the input and output tensors for the model
    input_tensor = helper.make_tensor_value_info('input', TensorProto.FLOAT, [1, 1, height, width])
    output_tensor = helper.make_tensor_value_info('output', TensorProto.FLOAT, [1, 1, height, width])

    # Create the 'Add' node
    add_node = helper.make_node(
        'Add',
        inputs=['input', 'input'],  # Input tensor is added to itself
        outputs=['output']
    )

    # Create the graph containing the node
    graph_def = helper.make_graph(
        [add_node],
        'simple-add-model',
        [input_tensor],
        [output_tensor]
    )

    # Create the final model with the necessary opset version
    opset_imports = [helper.make_opsetid("ai.onnx", 16)]
    model_def = helper.make_model(graph_def, producer_name='DirectPortTest', ir_version=8, opset_imports=opset_imports)
    
    # Save the model to the specified path
    onnx.save(model_def, model_path)
    print(f"✅ Created simple ONNX model at '{model_path}'")

def run_directport_test():
    """
    Executes the end-to-end test for the directport.onnx module.
    """
    model_filename = "test_model.onnx"
    height, width = 32, 32

    # 1. SETUP: Create the temporary ONNX model for the test
    create_simple_add_model(model_filename, height, width)

    try:
        # 2. INITIALIZATION: Create the underlying D3D12 device
        print("\n--- Initializing DirectPort ---")
        d3d12 = directport.DeviceD3D12.create()
        print("✅ D3D12 Device created successfully.")
    except Exception as e:
        print(f"❌ ERROR: Failed to create D3D12 device: {e}")
        return

    # 3. DATA PREPARATION: Create CPU data and upload it to a GPU texture
    print("\n--- Preparing GPU Input Data ---")
    # Create a NumPy array where every element is 0.5
    input_data_cpu = np.full((height, width), 0.5, dtype=np.float32)
    
    # Use the directport library to create a GPU texture from the NumPy array
    gpu_texture = d3d12.create_texture(
        width=width,
        height=height,
        format=directport.DXGI_FORMAT.R32_FLOAT, # Match the model's expected data type
        data=input_data_cpu
    )
    print("✅ Created GPU texture and uploaded NumPy data.")

    try:
        # 4. SESSION CREATION: Test the main Session class from your C++ wrapper
        print("\n--- Initializing ONNX Session ---")
        # This calls the C++ constructor, passing in the D3D12 device and model path
        session = directport.onnx.Session(device=d3d12, model_path=model_filename)
        print("✅ ONNX Session created successfully with DirectML provider.")
    except Exception as e:
        print(f"❌ ERROR: Failed to create ONNX Session: {e}")
        return

    try:
        # 5. EXECUTION: Run inference using the zero-copy GPU texture path
        print("\n--- Running Zero-Copy Inference ---")
        # This calls the C++ 'run' method
        result_gpu = session.run(input_texture=gpu_texture)
        print("✅ Inference completed.")
    except Exception as e:
        print(f"❌ ERROR: Inference run failed: {e}")
        return

    # 6. VERIFICATION: Check if the output is correct
    print("\n--- Verifying Results ---")
    expected_shape = (1, 1, height, width)
    print(f"Expected output shape: {expected_shape}")
    print(f"Actual output shape:   {result_gpu.shape}")

    # Assert that the shape of the output NumPy array is what the model defines
    assert result_gpu.shape == expected_shape, "Shape mismatch!"

    # Since the input was 0.5 and the model adds the input to itself, the expected output is 1.0
    expected_value = 1.0
    print(f"Expected output value (everywhere): {expected_value}")
    
    # Check the value of a single element for a quick look
    actual_value = result_gpu[0, 0, 0, 0]
    print(f"Actual value (at corner): {actual_value:.4f}")

    # Assert that all values in the output array are close to the expected value
    assert np.allclose(result_gpu, expected_value), "Value mismatch!"
    print("✅ Verification successful!")
    print("\n🎉 Your directport.onnx module is working correctly! 🎉")


if __name__ == "__main__":
    MODEL_FILE = "test_model.onnx"
    try:
        run_directport_test()
    finally:
        # 7. CLEANUP: Always remove the temporary model file
        if os.path.exists(MODEL_FILE):
            os.remove(MODEL_FILE)
            print(f"\n🧹 Cleaned up '{MODEL_FILE}'.")