// DirectPortGLWrapper.cpp
#include "DirectPortGL.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>
#include <vector>
#include <string_view>

namespace py = pybind11;
using namespace DirectPortGL;

static std::string wstring_to_string(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

enum DUMMY_DXGI_FORMAT_PY {
    PY_DXGI_FORMAT_UNKNOWN = 0,
    PY_DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    PY_DXGI_FORMAT_B8G8R8A8_UNORM = 87,
    PY_DXGI_FORMAT_R32_FLOAT = 41,
};

void bind_gl(py::module_& m) {
    auto gl_module = m.def_submodule("gl", "Unified, modern OpenGL interop with DirectX.");

    py::enum_<DUMMY_DXGI_FORMAT_PY>(gl_module, "DXGI_FORMAT")
        .value("R8G8B8A8_UNORM", PY_DXGI_FORMAT_R8G8B8A8_UNORM)
        .value("B8G8R8A8_UNORM", PY_DXGI_FORMAT_B8G8R8A8_UNORM)
        .value("R32_FLOAT", PY_DXGI_FORMAT_R32_FLOAT)
        .export_values();
    
    py::class_<ProducerInfo>(gl_module, "ProducerInfo", "Information about a running producer.")
        .def_readonly("pid", &ProducerInfo::pid)
        .def_property_readonly("executable_name", [](const ProducerInfo &p) { return wstring_to_string(p.executable_name); })
        .def_property_readonly("stream_name", [](const ProducerInfo &p) { return wstring_to_string(p.stream_name); })
        .def_property_readonly("type", [](const ProducerInfo &p) { return wstring_to_string(p.type); });
    
    gl_module.def("discover", &discover, "Discover running producers (D3D11, D3D12, OpenGL).", py::call_guard<py::gil_scoped_release>());

    py::class_<TextureGL, std::shared_ptr<TextureGL>>(gl_module, "Texture", "An OpenGL Texture object.")
        .def_property_readonly("width", &TextureGL::get_width)
        .def_property_readonly("height", &TextureGL::get_height)
        .def_property_readonly("id", &TextureGL::get_gl_texture_id, "The native OpenGL texture ID.");

    py::class_<ConsumerGL, std::shared_ptr<ConsumerGL>>(gl_module, "Consumer", "Connects to a producer to receive textures.")
        .def("wait_for_frame", &ConsumerGL::wait_for_frame, py::arg("timeout_ms") = 1000, "Waits for a new frame from the producer.", py::call_guard<py::gil_scoped_release>())
        .def("is_alive", &ConsumerGL::is_alive, "Checks if the producer process is still running.", py::call_guard<py::gil_scoped_release>())
        .def("get_texture", &ConsumerGL::get_texture, "Gets the local texture containing the latest frame.")
        .def_property_readonly("pid", &ConsumerGL::get_pid);

    py::class_<ProducerGL, std::shared_ptr<ProducerGL>>(gl_module, "Producer", "Produces textures to be shared with consumers.")
        .def("signal_frame", &ProducerGL::signal_frame, "Signals that a new frame is ready.", py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("pid", &ProducerGL::get_pid);
        
    py::class_<WindowGL, std::shared_ptr<WindowGL>>(gl_module, "Window", "An application window with an OpenGL context.")
        .def("process_events", &WindowGL::process_events, py::call_guard<py::gil_scoped_release>())
        .def("present", &WindowGL::present, py::call_guard<py::gil_scoped_release>())
        .def("set_title", &WindowGL::set_title, py::arg("title"))
        .def("get_width", &WindowGL::get_width)
        .def("get_height", &WindowGL::get_height)
        .def("make_current", &WindowGL::make_current, "Makes this window's OpenGL context current.");
    
    auto create_texture_gl = [](DeviceGL& self, uint32_t w, uint32_t h, DUMMY_DXGI_FORMAT_PY f, py::object data) {
        if (data.is_none()) {
            return self.create_texture(w, h, static_cast<DXGI_FORMAT>(f), nullptr);
        }
        py::buffer_info info = py::buffer(data).request();
        if (info.ndim != 3 || (uint32_t)info.shape[0] != h || (uint32_t)info.shape[1] != w) {
            throw std::runtime_error("NumPy array shape does not match texture dimensions (height, width, channels).");
        }
        return self.create_texture(w, h, static_cast<DXGI_FORMAT>(f), info.ptr);
    };

    auto apply_shader_gl = [](DeviceGL& self, std::shared_ptr<TextureGL> output, const std::string& glsl, const py::list& inputs, const py::bytes& constants) {
        std::vector<std::shared_ptr<TextureGL>> cpp_inputs;
        for(const auto& item : inputs) { 
            cpp_inputs.push_back(item.cast<std::shared_ptr<TextureGL>>()); 
        }
        std::string_view const_sv(constants);
        self.apply_shader(output, glsl, cpp_inputs, {const_sv.begin(), const_sv.end()});
    };

    py::class_<DeviceGL, std::shared_ptr<DeviceGL>>(gl_module, "Device", "The main interface for creating and managing OpenGL resources.")
        .def_static("create", &DeviceGL::create, "Creates the unified OpenGL device context.")
        .def("create_texture", create_texture_gl, py::arg("width"), py::arg("height"), py::arg("format"), py::arg("data") = py::none())
        .def("create_window", &DeviceGL::create_window, py::arg("width"), py::arg("height"), py::arg("title"))
        .def("create_producer", &DeviceGL::create_producer, py::arg("stream_name"), py::arg("texture"), py::call_guard<py::gil_scoped_release>())
        .def("connect_to_producer", &DeviceGL::connect_to_producer, py::arg("pid"), py::call_guard<py::gil_scoped_release>())
        .def("blit", &DeviceGL::blit, py::arg("source"), py::arg("destination"), py::call_guard<py::gil_scoped_release>())
        .def("clear", &DeviceGL::clear, py::arg("window"), py::arg("r"), py::arg("g"), py::arg("b"), py::arg("a"), py::call_guard<py::gil_scoped_release>())
        .def("apply_shader", apply_shader_gl, py::arg("output"), py::arg("glsl_fragment_shader"), py::arg("inputs") = py::list(), py::arg("constants") = py::bytes(""), py::call_guard<py::gil_scoped_release>())
        .def("copy_texture", &DeviceGL::copy_texture, py::arg("source"), py::arg("destination"), py::call_guard<py::gil_scoped_release>())
        .def("blit_texture_to_region", &DeviceGL::blit_texture_to_region, py::arg("source"), py::arg("destination"), py::arg("dest_x"), py::arg("dest_y"), py::arg("dest_width"), py::arg("dest_height"), py::call_guard<py::gil_scoped_release>());
}