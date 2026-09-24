#include "DirectPortCamera.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>

namespace py = pybind11;

py::array frame_to_numpy(const FrameData& frame) {
    if (frame.data.empty() || frame.width <= 0 || frame.height <= 0) {
        return py::array();
    }
    py::array_t<uint8_t> result({(py::ssize_t)frame.height, (py::ssize_t)frame.width, (py::ssize_t)frame.channels});
    std::memcpy(result.mutable_data(), frame.data.data(), frame.data.size());
    return result;
}

void bind_camera(py::module_& m) {
    // Note: No submodule, binding directly to 'directport' as before
    
    // The handle to the window, opaque to Python
    py::class_<Window, std::shared_ptr<Window>>(m, "DPWindowHandle");

    py::class_<DirectPortCamera>(m, "DirectPortCamera")
        .def(py::init<>())
        // Original methods
        .def("init", &DirectPortCamera::init, "Initializes the camera source.")
        .def("start_capture", &DirectPortCamera::startCapture, "Starts the capture process.")
        .def("stop_capture", &DirectPortCamera::stopCapture, "Stops the capture process.")
        .def("is_running", &DirectPortCamera::isRunning, "Checks if the camera is capturing.")
        .def("get_frame", [](DirectPortCamera& cam) {
            return frame_to_numpy(cam.getFrame());
        }, "Retrieves a frame as a NumPy array (for OpenCV).")

        // New windowing methods
        .def("create_window", &DirectPortCamera::create_window, py::arg("width"), py::arg("height"), py::arg("title"),
            "Creates a new DirectX window for rendering.")
        .def("process_events", &DirectPortCamera::process_events, py::arg("window"),
            "Processes window messages. Returns false if the window was closed.")
        .def("render_frame_to_window", &DirectPortCamera::render_frame_to_window, py::arg("window"),
            "Gets the latest camera frame and renders it to the specified window.")
        .def("present", &DirectPortCamera::present, py::arg("window"),
            "Presents the rendered frame to the screen.");
}