#include "DirectPort.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>

namespace py = pybind11;
using namespace DirectPort;

static std::string wstring_to_string(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

void bind_prime(py::module_& m) {
    m.doc() = "The ancestral library for high-performance GPU operations in Python.";

    py::enum_<DXGI_FORMAT>(m, "DXGI_FORMAT", "")
        .value("B8G8R8A8_UNORM", DXGI_FORMAT_B8G8R8A8_UNORM, "")
        .value("R32G32B32A32_FLOAT", DXGI_FORMAT_R32G32B32A32_FLOAT, "")
        .value("R16G16B16A16_FLOAT", DXGI_FORMAT_R16G16B16A16_FLOAT, "")
        .value("R10G10B10A2_UNORM", DXGI_FORMAT_R10G10B10A2_UNORM, "")
        .value("R8G8B8A8_UNORM", DXGI_FORMAT_R8G8B8A8_UNORM, "")
        .value("R32_FLOAT", DXGI_FORMAT_R32_FLOAT, "")
        .value("R16_FLOAT", DXGI_FORMAT_R16_FLOAT, "")
        .value("R8_UNORM", DXGI_FORMAT_R8_UNORM, "")
        .export_values();

    py::class_<ProducerInfo>(m, "ProducerInfo", "")
        .def_readonly("pid", &ProducerInfo::pid, "")
        .def_property_readonly("executable_name", [](const ProducerInfo &p) { return wstring_to_string(p.executable_name); }, "")
        .def_property_readonly("stream_name", [](const ProducerInfo &p) { return wstring_to_string(p.stream_name); }, "")
        .def_property_readonly("type", [](const ProducerInfo &p) { return wstring_to_string(p.type); }, "");
    
    m.def("discover", &discover, "", py::call_guard<py::gil_scoped_release>());

    py::class_<Texture, std::shared_ptr<Texture>>(m, "Texture", "")
        .def_property_readonly("width", &Texture::get_width, "")
        .def_property_readonly("height", &Texture::get_height, "")
        .def_property_readonly("format", &Texture::get_format, "")
        .def("get_d3d11_texture_ptr", &Texture::get_d3d11_texture_ptr, "")
        .def("get_d3d11_srv_ptr", &Texture::get_d3d11_srv_ptr, "")
        .def("get_d3d11_rtv_ptr", &Texture::get_d3d11_rtv_ptr, "")
        .def("get_d3d12_resource_ptr", &Texture::get_d3d12_resource_ptr, "");

    py::class_<Consumer, std::shared_ptr<Consumer>>(m, "Consumer", "")
        .def("wait_for_frame", &Consumer::wait_for_frame, "", py::call_guard<py::gil_scoped_release>())
        .def("is_alive", &Consumer::is_alive, "", py::call_guard<py::gil_scoped_release>())
        .def("get_texture", &Consumer::get_texture, "")
        .def("get_shared_texture", &Consumer::get_shared_texture, "")
        .def_property_readonly("pid", &Consumer::get_pid, "");

    py::class_<Producer, std::shared_ptr<Producer>>(m, "Producer", "")
        .def("signal_frame", &Producer::signal_frame, "", py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("pid", &Producer::get_pid, "");
        
    py::class_<Window, std::shared_ptr<Window>>(m, "Window", "")
        .def("process_events", &Window::process_events, "", py::call_guard<py::gil_scoped_release>())
        .def("present", &Window::present, py::arg("vsync") = true, "", py::call_guard<py::gil_scoped_release>())
        .def("set_title", &Window::set_title, py::arg("title"), "", py::call_guard<py::gil_scoped_release>())
        .def("get_width", &Window::get_width, "")
        .def("get_height", &Window::get_height, "");

    auto create_texture_d3d11 = [](DeviceD3D11& self, uint32_t w, uint32_t h, DXGI_FORMAT f, py::object data) {
        if (data.is_none()) {
            return self.create_texture(w, h, f, nullptr, 0);
        }
        py::buffer_info info = py::buffer(data).request();
        return self.create_texture(w, h, f, info.ptr, info.size);
    };

    auto create_texture_d3d12 = [](DeviceD3D12& self, uint32_t w, uint32_t h, DXGI_FORMAT f, py::object data) {
        if (data.is_none()) {
            return self.create_texture(w, h, f, nullptr, 0);
        }
        py::buffer_info info = py::buffer(data).request();
        return self.create_texture(w, h, f, info.ptr, info.size);
    };

    auto apply_shader_lambda_d3d11 = [](DeviceD3D11& self, std::shared_ptr<Texture> output, const py::object& shader, const std::string& entry_point, const py::list& inputs, const py::bytes& constants) {
        std::vector<uint8_t> shader_bytes;
        if (py::isinstance<py::str>(shader)) {
            std::string hlsl = shader.cast<std::string>();
            shader_bytes.assign(hlsl.begin(), hlsl.end());
        } else if (py::isinstance<py::bytes>(shader)) {
            std::string cso = shader.cast<std::string>();
            shader_bytes.assign(cso.begin(), cso.end());
        } else if (!shader.is_none()) {
                throw py::type_error("Shader must be bytes (CSO/HLSL) or str (HLSL source).");
        }
        std::vector<std::shared_ptr<Texture>> cpp_inputs;
        for(const auto& item : inputs) { 
            cpp_inputs.push_back(item.cast<std::shared_ptr<Texture>>()); 
        }
        std::string_view const_sv(constants);
        self.apply_shader(output, shader_bytes, entry_point, cpp_inputs, {const_sv.begin(), const_sv.end()});
    };

    auto apply_shader_lambda_d3d12 = [](DeviceD3D12& self, std::shared_ptr<Texture> output, const py::object& shader, const std::string& entry_point, const py::list& inputs, const py::bytes& constants) {
        std::vector<uint8_t> shader_bytes;
        if (py::isinstance<py::str>(shader)) {
            std::string hlsl = shader.cast<std::string>();
            shader_bytes.assign(hlsl.begin(), hlsl.end());
        } else if (py::isinstance<py::bytes>(shader)) {
            std::string cso = shader.cast<std::string>();
            shader_bytes.assign(cso.begin(), cso.end());
        } else if (!shader.is_none()) {
                throw py::type_error("Shader must be bytes (CSO/HLSL) or str (HLSL source).");
        }
        std::vector<std::shared_ptr<Texture>> cpp_inputs;
        for(const auto& item : inputs) { 
            cpp_inputs.push_back(item.cast<std::shared_ptr<Texture>>()); 
        }
        std::string_view const_sv(constants);
        self.apply_shader(output, shader_bytes, entry_point, cpp_inputs, {const_sv.begin(), const_sv.end()});
    };

    py::class_<DeviceD3D11, std::shared_ptr<DeviceD3D11>>(m, "DeviceD3D11", "")
        .def_static("create", &DeviceD3D11::create, "")
        .def("create_texture", create_texture_d3d11, py::arg("width"), py::arg("height"), py::arg("format"), py::arg("data") = py::none(), "")
        .def("create_producer", &DeviceD3D11::create_producer, py::arg("stream_name"), py::arg("texture"), "")
        .def("connect_to_producer", &DeviceD3D11::connect_to_producer, py::arg("pid"), "")
        .def("create_window", &DeviceD3D11::create_window, py::arg("width"), py::arg("height"), py::arg("title"), "")
        .def("resize_window", &DeviceD3D11::resize_window, py::arg("window"), "")
        .def("apply_shader", apply_shader_lambda_d3d11, py::arg("output"), py::arg("shader"), py::arg("entry_point") = "PSMain", py::arg("inputs") = py::list(), py::arg("constants") = py::bytes(""), 
        "", py::call_guard<py::gil_scoped_release>())
        .def("copy_texture", &DeviceD3D11::copy_texture, py::arg("source"), py::arg("destination"), "", py::call_guard<py::gil_scoped_release>())
        .def("blit", &DeviceD3D11::blit, py::arg("source"), py::arg("destination"), "", py::call_guard<py::gil_scoped_release>())
        .def("clear", &DeviceD3D11::clear, py::arg("window"), py::arg("r"), py::arg("g"), py::arg("b"), py::arg("a"), "", py::call_guard<py::gil_scoped_release>())
        .def("blit_texture_to_region", &DeviceD3D11::blit_texture_to_region, py::arg("source"), py::arg("destination"),
             py::arg("dest_x"), py::arg("dest_y"), py::arg("dest_width"), py::arg("dest_height"),
             "", py::call_guard<py::gil_scoped_release>())
        .def("get_d3d11_device", [](DeviceD3D11& self) { return reinterpret_cast<uintptr_t>(self.get_d3d11_device()); })
        .def("get_d3d11_context", [](DeviceD3D11& self) { return reinterpret_cast<uintptr_t>(self.get_d3d11_context()); });
    
    py::class_<DeviceD3D12, std::shared_ptr<DeviceD3D12>>(m, "DeviceD3D12", "")
        .def_static("create", &DeviceD3D12::create, "")
        .def("create_texture", create_texture_d3d12, py::arg("width"), py::arg("height"), py::arg("format"), py::arg("data") = py::none(), "")
        .def("create_producer", &DeviceD3D12::create_producer, py::arg("stream_name"), py::arg("texture"), "")
        .def("connect_to_producer", &DeviceD3D12::connect_to_producer, py::arg("pid"), "")
        .def("create_window", &DeviceD3D12::create_window, py::arg("width"), py::arg("height"), py::arg("title"), "")
        .def("resize_window", &DeviceD3D12::resize_window, py::arg("window"), "")
        .def("apply_shader", apply_shader_lambda_d3d12, py::arg("output"), py::arg("shader"), py::arg("entry_point") = "PSMain", py::arg("inputs") = py::list(), py::arg("constants") = py::bytes(""), 
        "", py::call_guard<py::gil_scoped_release>())
        .def("copy_texture", &DeviceD3D12::copy_texture, py::arg("source"), py::arg("destination"), "", py::call_guard<py::gil_scoped_release>())
        .def("blit", &DeviceD3D12::blit, py::arg("source"), py::arg("destination"), "", py::call_guard<py::gil_scoped_release>())
        .def("clear", &DeviceD3D12::clear, py::arg("window"), py::arg("r"), py::arg("g"), py::arg("b"), py::arg("a"), "", py::call_guard<py::gil_scoped_release>())
        .def("blit_texture_to_region", &DeviceD3D12::blit_texture_to_region, py::arg("source"), py::arg("destination"),
             py::arg("dest_x"), py::arg("dest_y"), py::arg("dest_width"), py::arg("dest_height"),
             "", py::call_guard<py::gil_scoped_release>());
}