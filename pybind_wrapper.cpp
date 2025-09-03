#include "DirectPort.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <vector>

std::string wstring_to_string(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

std::wstring string_to_wstring(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

namespace py = pybind11;
using namespace DirectPort;

PYBIND11_MODULE(directport, m) {
    m.doc() = "Python module for zero-copy GPU texture sharing with self-contained rendering.";

    py::class_<ProducerInfo>(m, "ProducerInfo")
        .def_readonly("pid", &ProducerInfo::pid)
        .def_property_readonly("name", [](const ProducerInfo &p) { return wstring_to_string(p.name); })
        .def_property_readonly("type", [](const ProducerInfo &p) { return wstring_to_string(p.type); });

    // --- THIS IS THE CORRECTED BLOCK ---
    // We now explicitly bind all the methods that Python needs to call.
    py::class_<Producer, std::shared_ptr<Producer>>(m, "Producer")
        .def("wait_for_frame", &Producer::wait_for_frame, 
             py::call_guard<py::gil_scoped_release>(), "Waits for a new frame. Returns False if producer disconnected.")
        .def("get_texture_ptr", &Producer::get_texture_ptr, 
             py::call_guard<py::gil_scoped_release>(), "Returns the ID3D11ShaderResourceView* as an integer pointer.")
        .def_property_readonly("width", &Producer::get_width)
        .def_property_readonly("height", &Producer::get_height)
        .def_property_readonly("pid", &Producer::get_pid);
    // --- END CORRECTION ---

    py::class_<Consumer>(m, "Consumer")
        .def(py::init([](const std::string& title, int width, int height) {
            return std::make_unique<Consumer>(string_to_wstring(title), width, height);
        }), py::arg("title"), py::arg("width"), py::arg("height"))
        
        .def("process_events", &Consumer::process_events, py::call_guard<py::gil_scoped_release>())
        .def("render_frame", &Consumer::render_frame, py::arg("producers"), py::call_guard<py::gil_scoped_release>())
        .def("present", &Consumer::present, py::call_guard<py::gil_scoped_release>())
        
        .def("discover", &Consumer::discover, py::call_guard<py::gil_scoped_release>())
        .def("connect", &Consumer::connect, py::arg("pid"), py::call_guard<py::gil_scoped_release>());
}