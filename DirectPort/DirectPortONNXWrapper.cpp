#include "DirectPortONNX.h" 
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "DirectPort.h" 

namespace py = pybind11;

void bind_onnx(py::module_& m) {
    auto onnx_module = m.def_submodule("onnx", "The fiefdom for direct ONNX Runtime integration.");

    py::class_<DirectPortONNX::Session, std::shared_ptr<DirectPortONNX::Session>>(onnx_module, "Session")
        .def(py::init([](py::object device_obj, const std::string& model_path) {
            auto device_ptr_real = device_obj.cast<std::shared_ptr<DirectPort::DeviceD3D12>>();
            
            auto device_ptr_mirrored = std::reinterpret_pointer_cast<DirectPortONNX::DPMirror::DeviceD3D12>(device_ptr_real);

            return std::make_shared<DirectPortONNX::Session>(device_ptr_mirrored, model_path);
        }), 
             py::arg("device"), py::arg("model_path"),
             py::call_guard<py::gil_scoped_release>(),
             "Initializes an ONNX Runtime session with the DirectML provider using a shared D3D12 device.")

        .def("run", &DirectPortONNX::Session::run, py::arg("input_texture"),
             py::call_guard<py::gil_scoped_release>(),
             "Runs inference on a GPU texture with zero-copy, returning the result as a NumPy array.");
}