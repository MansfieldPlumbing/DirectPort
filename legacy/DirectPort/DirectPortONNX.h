#pragma once

#include <string>
#include <vector>
#include <memory>
#include <stdexcept>

#include <pybind11/numpy.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <onnxruntime_cxx_api.h>

namespace py = pybind11;

namespace DirectPortONNX {

    namespace DPMirror { 
        struct Texture_Impl {
            Microsoft::WRL::ComPtr<ID3D12Resource> d3d12Resource;
            UINT32 width = 0;
            UINT32 height = 0;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            bool is_d3d11 = false;
            bool is_d3d12 = false;
        };
        class Texture {
        public:
            std::unique_ptr<Texture_Impl> pImpl;
        };
        struct DeviceD3D12_Impl {
            Microsoft::WRL::ComPtr<ID3D12Device> device;
            Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue;
            Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
            Microsoft::WRL::ComPtr<ID3D12Fence> fence;
            HANDLE fenceEvent;
            UINT64 fenceValue = 1;
            UINT64 frameFenceValues[2] = {};
            LUID adapterLuid;
        };
        class DeviceD3D12 {
        public:
            std::unique_ptr<DeviceD3D12_Impl> pImpl;
        };
    }

    class Session {
    public:
        Session(std::shared_ptr<DPMirror::DeviceD3D12> device, const std::string& model_path);
        ~Session();

        py::array run(std::shared_ptr<DPMirror::Texture> input_texture);

    private:
        void WaitForGpu();

        std::shared_ptr<DPMirror::DeviceD3D12> dp_device;
        ::Ort::Env ort_env;
        ::Ort::Session ort_session;
        ::Ort::MemoryInfo ort_memory_info;
        ::Ort::AllocatorWithDefaultOptions ort_allocator;
        
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> copy_allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> copy_command_list;
        Microsoft::WRL::ComPtr<ID3D12Fence> copy_fence;
        HANDLE copy_fence_event;
        UINT64 copy_fence_value = 1;
        
        Microsoft::WRL::ComPtr<ID3D12Resource> tensor_buffer;
        std::vector<int64_t> input_shape;
        size_t input_tensor_size = 0;
        ONNXTensorElementDataType input_tensor_type;
        
        char* input_name = nullptr;
        char* output_name = nullptr;
    };
}