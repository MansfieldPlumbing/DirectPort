#pragma once

#include "DirectPort.h"
#include <memory>
#include <pybind11/numpy.h>

namespace py = pybind11;

namespace DirectPort::Numpy {

    py::array read_texture(
        std::shared_ptr<DirectPort::DeviceD3D11> device,
        std::shared_ptr<DirectPort::Texture> texture
    );

    void write_texture(
        std::shared_ptr<DirectPort::DeviceD3D11> device,
        std::shared_ptr<DirectPort::Texture> texture,
        const py::array& numpy_array
    );

}