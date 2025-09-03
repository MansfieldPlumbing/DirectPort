# --- CMakeLists.txt for the DirectPort Module ---

find_package(pybind11 CONFIG REQUIRED)

add_library(DirectPortLib STATIC
    "DirectPort.cpp"
)

target_include_directories(DirectPortLib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(DirectPortLib PUBLIC
    d3d11.lib
    d3d12.lib
    dxgi.lib
    advapi32.lib
    user32.lib
    Synchronization.lib
    d3dcompiler.lib
)

pybind11_add_module(directport
    "pybind_wrapper.cpp"
)

target_link_libraries(directport PRIVATE
    DirectPortLib
)

message(STATUS "Configured DirectPort C++ library and Python module.")