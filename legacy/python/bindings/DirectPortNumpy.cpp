// src/DirectPort/DirectPortNumpy.cpp

#include "DirectPortNumpy.h"
#include <stdexcept>
#include <vector>
#include <map>

namespace py = pybind11;
using namespace Microsoft::WRL;

namespace DirectPort::Numpy {

struct unmapper {
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Resource> resource;
    uint32_t subresource;
    unmapper(const ComPtr<ID3D11DeviceContext>& ctx, const ComPtr<ID3D11Resource>& res, uint32_t sub)
        : context(ctx), resource(res), subresource(sub) {}
    ~unmapper() {
        if (context && resource) {
            context->Unmap(resource.Get(), subresource);
        }
    }
};


void write_texture(
    std::shared_ptr<DirectPort::DeviceD3D11> device,
    std::shared_ptr<DirectPort::Texture> texture,
    const py::array& array) {

    if (!device) {
        throw std::invalid_argument("Device cannot be null.");
    }
    if (!texture || !texture->get_d3d11_texture_ptr()) {
        throw std::invalid_argument("Invalid D3D11 texture provided.");
    }
    if (!array || !array.request().ptr) {
        throw std::invalid_argument("Invalid or empty NumPy array provided.");
    }
    
    auto* pDevice = device->get_d3d11_device();
    auto* pContext = device->get_d3d11_context();
    auto* pTexture = reinterpret_cast<ID3D11Texture2D*>(texture->get_d3d11_texture_ptr());

    if (!pDevice || !pContext || !pTexture) {
        throw std::runtime_error("Failed to retrieve valid D3D11 pointers via mirror structures.");
    }

    py::buffer_info info = array.request();
    D3D11_TEXTURE2D_DESC desc_target;
    pTexture->GetDesc(&desc_target);

    if (info.ndim < 2 || info.ndim > 3) {
        throw std::invalid_argument("NumPy array must be 2D (HxW) or 3D (HxWxC).");
    }
    if (static_cast<UINT>(info.shape[0]) != desc_target.Height || static_cast<UINT>(info.shape[1]) != desc_target.Width) {
        throw std::invalid_argument("NumPy array dimensions do not match the target texture.");
    }

    D3D11_TEXTURE2D_DESC desc_staging = desc_target;
    desc_staging.Usage = D3D11_USAGE_STAGING;
    desc_staging.BindFlags = 0;
    desc_staging.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc_staging.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> stagingTexture;
    HRESULT hr = pDevice->CreateTexture2D(&desc_staging, nullptr, &stagingTexture);
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create staging texture for writing. HRESULT: " + std::to_string(hr));
    }

    D3D11_MAPPED_SUBRESOURCE mapped_resource;
    hr = pContext->Map(stagingTexture.Get(), 0, D3D11_MAP_WRITE, 0, &mapped_resource);
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to map staging texture for writing. HRESULT: " + std::to_string(hr));
    }

    unmapper u(pContext, stagingTexture.Get(), 0);

    const auto* src_data = static_cast<const uint8_t*>(info.ptr);
    auto* dst_data = static_cast<uint8_t*>(mapped_resource.pData);
    
    const size_t src_row_pitch = info.strides[0];
    const size_t dst_row_pitch = mapped_resource.RowPitch;
    const size_t bytes_to_copy_per_row = static_cast<size_t>(info.shape[1]) * info.strides[1];

    if (src_row_pitch == dst_row_pitch) {
        memcpy(dst_data, src_data, src_row_pitch * info.shape[0]);
    } else {
        for (size_t y = 0; y < static_cast<size_t>(info.shape[0]); ++y) {
            memcpy(dst_data + y * dst_row_pitch, src_data + y * src_row_pitch, bytes_to_copy_per_row);
        }
    }

    pContext->CopyResource(pTexture, stagingTexture.Get());
}

py::array read_texture(
    std::shared_ptr<DirectPort::DeviceD3D11> device,
    std::shared_ptr<DirectPort::Texture> texture)
{
    if (!device) {
        throw std::invalid_argument("Device cannot be null.");
    }
    if (!texture || !texture->get_d3d11_texture_ptr()) {
        throw std::invalid_argument("Invalid D3D11 texture provided.");
    }

    auto* pDevice = device->get_d3d11_device();
    auto* pContext = device->get_d3d11_context();
    auto* pTexture = reinterpret_cast<ID3D11Texture2D*>(texture->get_d3d11_texture_ptr());

    if (!pDevice || !pContext || !pTexture) {
        throw std::runtime_error("Failed to retrieve valid D3D11 pointers via mirror structures.");
    }

    D3D11_TEXTURE2D_DESC desc;
    pTexture->GetDesc(&desc);

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    
    ComPtr<ID3D11Texture2D> stagingTexture;
    HRESULT hr = pDevice->CreateTexture2D(&desc, nullptr, &stagingTexture);
    if (FAILED(hr)) {
        throw std::runtime_error("Numpy: Failed to create staging texture.");
    }

    pContext->CopyResource(stagingTexture.Get(), pTexture);

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    hr = pContext->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mappedResource);
    if (FAILED(hr)) {
        throw std::runtime_error("Numpy: Failed to map staging texture.");
    }
    
    unmapper u(pContext, stagingTexture.Get(), 0);

    std::vector<py::ssize_t> shape;
    py::dtype dtype("uint8"); 
    size_t bytesPerPixel = 4;

    switch (desc.Format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            shape = {(py::ssize_t)desc.Height, (py::ssize_t)desc.Width, 4};
            dtype = py::dtype("uint8");
            bytesPerPixel = 4;
            break;
        case DXGI_FORMAT_R32_FLOAT:
            shape = {(py::ssize_t)desc.Height, (py::ssize_t)desc.Width};
            dtype = py::dtype("float32");
            bytesPerPixel = 4;
            break;
        default:
            throw std::runtime_error("Numpy: Unsupported texture format for readback.");
    }

    py::array result(dtype, shape);
    auto buf = result.request();
    auto* pDest = static_cast<uint8_t*>(buf.ptr);
    const auto* pSrc = static_cast<const uint8_t*>(mappedResource.pData);

    const size_t bytes_to_copy_per_row = desc.Width * bytesPerPixel;
    const size_t src_row_pitch = mappedResource.RowPitch;
    const size_t dst_row_pitch = buf.strides[0];

    for (UINT y = 0; y < desc.Height; ++y) {
        memcpy(pDest + y * dst_row_pitch, pSrc + y * src_row_pitch, bytes_to_copy_per_row);
    }
    
    return result;
}

}