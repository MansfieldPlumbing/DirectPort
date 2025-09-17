// src/DirectPort/Manifest.cpp
// THIS IS THE ASSEMBLY POINT FOR THE PYTHON MODULE (THE KINGDOM).
// IT CALLS EACH WRAPPER TO BIND THE FUNCTIONALITY OF ITS FIEFDOM.

#include <pybind11/pybind11.h>

namespace py = pybind11;

// Forward declare the binding functions from each fiefdom's wrapper file.
// This is the "roll call" for each branch of the family.
void bind_prime(py::module_& m);
void bind_numpy(py::module_& m);
void bind_camera(py::module_& m);


// PYBIND11_MODULE defines the entry point for the 'directport' kingdom.
PYBIND11_MODULE(directport, m) {
    m.doc() = "The ancestral library for high-performance GPU operations in Python.";

    // Call the other wrappers to come and add their bindings.
    // Each function will build its own self-contained fiefdom.
    bind_prime(m);
    bind_numpy(m);
    bind_camera(m);
}