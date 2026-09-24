#include "DirectPortNumpy.h"
#include <pybind11/pybind11.h>

namespace py = pybind11;

void bind_numpy(py::module_& m) {
    auto numpy_module = m.def_submodule("numpy", "The fiefdom for NumPy-GPU interoperability.");

    numpy_module.def("read_texture",
        &DirectPort::Numpy::read_texture,
        py::arg("device"), py::arg("texture"),
        "Reads a GPU texture to a NumPy array without copying in Python."
    );

    numpy_module.def("write_texture",
        &DirectPort::Numpy::write_texture,
        py::arg("device"), py::arg("texture"), py::arg("numpy_array"),
        "Writes a NumPy array's contents to a GPU texture."
    );
}