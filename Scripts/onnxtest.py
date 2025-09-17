import directport
import numpy as np
import onnx
from onnx import helper, TensorProto
import os
import sys

def create_simple_add_model(model_path: str, height: int, width: int):
    input_tensor = helper.make_tensor_value_info('input', TensorProto.FLOAT, [1, 1, height, width])
    output_tensor = helper.make_tensor_value_info('output', TensorProto.FLOAT, [1, 1, height, width])

    add_node = helper.make_node(
        'Add',
        inputs=['input', 'input'],
        outputs=['output']
    )

    graph_def = helper.make_graph(
        [add_node],
        'simple-add-model',
        [input_tensor],
        [output_tensor]
    )

    opset_imports = [helper.make_opsetid("ai.onnx", 16)]
    model_def = helper.make_model(graph_def, producer_name='DirectPortTest', ir_version=8, opset_imports=opset_imports)
    
    onnx.save(model_def, model_path)
    print(f"✅ Created simple ONNX model at '{model_path}'")

def run_test():
    model_filename = "test_model.onnx"
    height, width = 32, 32

    create_simple_add_model(model_filename, height, width)

    try:
        print("\n--- Initializing DirectPort ---")
        d3d12 = directport.DeviceD3D12.create()
        print("✅ D3D12 Device created successfully.")
    except Exception as e:
        print(f"❌ ERROR: Failed to create D3D12 device: {e}")
        return

    print("\n--- Preparing GPU Input Data ---")
    input_data_cpu = np.full((height, width), 0.5, dtype=np.float32)
    gpu_texture = d3d12.create_texture(
        width=width,
        height=height,
        format=directport.DXGI_FORMAT.R32_FLOAT,
        data=input_data_cpu
    )
    print("✅ Created GPU texture and uploaded NumPy data.")

    try:
        print("\n--- Initializing ONNX Session ---")
        session = directport.onnx.Session(device=d3d12, model_path=model_filename)
        print("✅ ONNX Session created successfully with DirectML provider.")
    except Exception as e:
        print(f"❌ ERROR: Failed to create ONNX Session: {e}")
        return

    try:
        print("\n--- Running Zero-Copy Inference ---")
        result_gpu = session.run(input_texture=gpu_texture)
        print("✅ Inference completed.")
    except Exception as e:
        print(f"❌ ERROR: Inference run failed: {e}")
        return

    print("\n--- Verifying Results ---")
    expected_shape = (1, 1, height, width)
    print(f"Expected output shape: {expected_shape}")
    print(f"Actual output shape:   {result_gpu.shape}")
    assert result_gpu.shape == expected_shape, "Shape mismatch!"

    expected_value = 1.0
    print(f"Expected output value: {expected_value}")
    actual_value = result_gpu[0, 0, 0, 0]
    print(f"Actual value (corner): {actual_value:.4f}")

    assert np.allclose(result_gpu, expected_value), "Value mismatch!"
    print("✅ Verification successful!")
    print("\n🎉 Your directport.onnx module is working correctly! 🎉")


if __name__ == "__main__":
    MODEL_FILE = "test_model.onnx"
    try:
        run_test()
    finally:
        if os.path.exists(MODEL_FILE):
            os.remove(MODEL_FILE)
            print(f"\n🧹 Cleaned up '{MODEL_FILE}'.")