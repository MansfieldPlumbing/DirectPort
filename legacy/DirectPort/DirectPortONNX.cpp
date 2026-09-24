#include "DirectPortONNX.h"
#include <onnxruntime_cxx_api.h>
#include "dml_provider_factory.h"
#include <DirectML.h>
#include "d3dx12.h"
#include <vector>
#include <numeric>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "DirectML.lib")

namespace DirectPortONNX {

Session::Session(std::shared_ptr<DPMirror::DeviceD3D12> device, const std::string& model_path)
    : dp_device(device),
      ort_env(ORT_LOGGING_LEVEL_VERBOSE, "DirectPortONNX"),
      ort_session(nullptr),
      ort_memory_info("DML", OrtAllocatorType::OrtDeviceAllocator, 0, OrtMemType::OrtMemTypeDefault)
{
    if (!dp_device || !dp_device->pImpl || !dp_device->pImpl->device || !dp_device->pImpl->commandQueue) {
        throw std::runtime_error("ONNX Session: Provided D3D12 device is invalid or null.");
    }

    Ort::SessionOptions session_options;

    session_options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    session_options.DisableMemPattern();

    Microsoft::WRL::ComPtr<IDMLDevice> dml_device;
    HRESULT hr = DMLCreateDevice(dp_device->pImpl->device.Get(), DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(&dml_device));
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create IDMLDevice from D3D12Device.");
    }

    const OrtApi& ort_api = Ort::GetApi();
    const OrtDmlApi* ort_dml_api;
    ort_api.GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void**>(&ort_dml_api));
    if (!ort_dml_api) {
        throw std::runtime_error("Failed to get ONNX Runtime DirectML API. Ensure the required provider DLL is available.");
    }
    
    Ort::ThrowOnError(ort_dml_api->SessionOptionsAppendExecutionProvider_DML1(session_options, dml_device.Get(), dp_device->pImpl->commandQueue.Get()));
    
    session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    std::wstring w_model_path(model_path.begin(), model_path.end());
    ort_session = Ort::Session(ort_env, w_model_path.c_str(), session_options);

    size_t num_input_nodes = ort_session.GetInputCount();
    if (num_input_nodes != 1) {
        throw std::runtime_error("ONNX Session: Model must have exactly one input.");
    }

    Ort::AllocatedStringPtr input_name_ptr = ort_session.GetInputNameAllocated(0, ort_allocator);
    input_name = _strdup(input_name_ptr.get());

    Ort::AllocatedStringPtr output_name_ptr = ort_session.GetOutputNameAllocated(0, ort_allocator);
    output_name = _strdup(output_name_ptr.get());

    Ort::TypeInfo input_type_info = ort_session.GetInputTypeInfo(0);
    auto input_tensor_info = input_type_info.GetTensorTypeAndShapeInfo();
    input_tensor_type = input_tensor_info.GetElementType();
    input_shape = input_tensor_info.GetShape();
    
    input_tensor_size = 1;
    for (int64_t dim : input_shape) {
        if (dim < 1) {
             throw std::runtime_error("ONNX Session: Model input shape must be fully defined (no dynamic axes).");
        }
        input_tensor_size *= dim;
    }
    size_t element_size = 0;
    if (input_tensor_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) element_size = sizeof(float);
    else if(input_tensor_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8) element_size = sizeof(uint8_t);
    else throw std::runtime_error("ONNX Session: Unsupported model input data type.");
    
    size_t input_tensor_byte_size = input_tensor_size * element_size;

    auto heap_props = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto buffer_desc = CD3DX12_RESOURCE_DESC::Buffer(input_tensor_byte_size);

    auto d3d12_device_ptr = dp_device->pImpl->device.Get();
    hr = d3d12_device_ptr->CreateCommittedResource(
        &heap_props,
        D3D12_HEAP_FLAG_NONE,
        &buffer_desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&tensor_buffer)
    );
    if (FAILED(hr)) { throw std::runtime_error("ONNX Session: Failed to create intermediate tensor buffer on GPU."); }
    
    hr = d3d12_device_ptr->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copy_allocator));
    if (FAILED(hr)) { throw std::runtime_error("ONNX Session: Failed to create copy command allocator."); }

    hr = d3d12_device_ptr->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copy_allocator.Get(), nullptr, IID_PPV_ARGS(&copy_command_list));
    if (FAILED(hr)) { throw std::runtime_error("ONNX Session: Failed to create copy command list."); }
    copy_command_list->Close();

    hr = d3d12_device_ptr->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy_fence));
    if (FAILED(hr)) { throw std::runtime_error("ONNX Session: Failed to create copy fence."); }
    copy_fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!copy_fence_event) { throw std::runtime_error("ONNX Session: Failed to create copy fence event."); }
}

Session::~Session() {
    WaitForGpu();

    if (copy_fence_event) {
        CloseHandle(copy_fence_event);
        copy_fence_event = nullptr;
    }
    
    free(input_name);
    input_name = nullptr;
    
    free(output_name);
    output_name = nullptr;
}

py::array Session::run(std::shared_ptr<DPMirror::Texture> input_texture) {
    auto d3d12_device = dp_device->pImpl->device.Get();

    copy_allocator->Reset();
    copy_command_list->Reset(copy_allocator.Get(), nullptr);

    auto src_resource = input_texture->pImpl->d3d12Resource.Get();

    auto barrier_copy_src = CD3DX12_RESOURCE_BARRIER::Transition(src_resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy_command_list->ResourceBarrier(1, &barrier_copy_src);

    auto barrier_copy_dst = CD3DX12_RESOURCE_BARRIER::Transition(tensor_buffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    copy_command_list->ResourceBarrier(1, &barrier_copy_dst);
    
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 total_bytes = 0;
    auto desc = src_resource->GetDesc();
    d3d12_device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &num_rows, &row_size, &total_bytes);

    CD3DX12_TEXTURE_COPY_LOCATION src_loc(src_resource, 0);
    CD3DX12_TEXTURE_COPY_LOCATION dst_loc(tensor_buffer.Get(), layout);

    copy_command_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
    
    auto barrier_common_src = CD3DX12_RESOURCE_BARRIER::Transition(src_resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    copy_command_list->ResourceBarrier(1, &barrier_common_src);
    auto barrier_common_dst = CD3DX12_RESOURCE_BARRIER::Transition(tensor_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    copy_command_list->ResourceBarrier(1, &barrier_common_dst);

    copy_command_list->Close();
    ID3D12CommandList* ppCommandLists[] = { copy_command_list.Get() };
    dp_device->pImpl->commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
    
    WaitForGpu();

    const char* input_names[] = { input_name };
    const char* output_names[] = { output_name };

    size_t element_size = 0;
    if (input_tensor_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) element_size = sizeof(float);
    else if(input_tensor_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8) element_size = sizeof(uint8_t);
    size_t input_tensor_byte_size = input_tensor_size * element_size;

    auto input_tensor = Ort::Value::CreateTensor(ort_memory_info, reinterpret_cast<void*>(tensor_buffer->GetGPUVirtualAddress()), input_tensor_byte_size, input_shape.data(), input_shape.size(), input_tensor_type);

    auto output_tensors = ort_session.Run(Ort::RunOptions{nullptr}, input_names, &input_tensor, 1, output_names, 1);
    
    auto& output_tensor = output_tensors[0];
    auto output_type_info = output_tensor.GetTensorTypeAndShapeInfo();
    auto output_shape = output_type_info.GetShape();
    void* output_data = output_tensor.GetTensorMutableData<void>();
    ONNXTensorElementDataType output_type = output_type_info.GetElementType();

    py::dtype dt;
    if (output_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        dt = py::dtype::of<float>();
    } else if (output_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8) {
        dt = py::dtype::of<uint8_t>();
    } else {
        throw std::runtime_error("Unsupported output tensor data type.");
    }
    
    py::array result_array(dt, output_shape);
    
    size_t total_elements = 1;
    for(long long dim : output_shape) {
        total_elements *= dim;
    }
    size_t output_byte_size = total_elements * dt.itemsize();

    std::memcpy(result_array.mutable_data(), output_data, output_byte_size);
    
    return result_array;
}

void Session::WaitForGpu() {
    const UINT64 fence_to_wait = copy_fence_value;
    dp_device->pImpl->commandQueue->Signal(copy_fence.Get(), fence_to_wait);
    copy_fence_value++;

    if (copy_fence->GetCompletedValue() < fence_to_wait) {
        copy_fence->SetEventOnCompletion(fence_to_wait, copy_fence_event);
        WaitForSingleObject(copy_fence_event, INFINITE);
    }
}

}