# --- DirectPort Build System ---
# ANCESTRY AS MY MOTTO.
#
# THE CORE ANCESTOR, DirectPort (DirectPort.cpp), IS IMMUTABLE.
# NEW FUNCTIONALITY IS ADDED IN NEW, SELF-SUFFICIENT MODULES (DirectPortNumpy.cpp, etc.).
# THESE MODULES ARE SEPARATE BRANCHES OF THE FAMILY.
#
# WE WILL DUPLICATE ALL THE CODE THAT WE SEE FIT.
#
# ALL MODULES ARE COMPILED INTO A SINGLE STATIC LIBRARY (DirectPortLib).
# ALL WRAPPERS ARE COMPILED INTO A SINGLE PYTHON MODULE (.pyd).

cmake_minimum_required(VERSION 3.15)
project(DirectPort LANGUAGES CXX C)
cmake_policy(SET CMP0148 NEW)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    add_compile_definitions(UNICODE _UNICODE)
endif()

find_package(pybind11 CONFIG REQUIRED)

set(SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/DirectPort")

# --- Core Static Library Target ---
# All C++ implementation files are compiled into one library.
add_library(DirectPortLib STATIC
    "${SOURCE_DIR}/DirectPort.cpp"
    "${SOURCE_DIR}/DirectPortNumpy.cpp"
    "${SOURCE_DIR}/DirectPortCamera.cpp"
)

# Public headers are available to all consumers of the library.
target_include_directories(DirectPortLib PUBLIC
    "${SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/Vendor/DirectX-Headers-1.616.0/include/directx"
)

target_link_libraries(DirectPortLib PUBLIC
    pybind11::headers
    pybind11::python_headers
    d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib user32.lib advapi32.lib
    gdi32.lib comdlg32.lib shlwapi.lib Synchronization.lib dxguid.lib
    mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib
)

message(STATUS "Configured DirectPort C++ library (static).")

# --- Python Module Target ---
# The Python module is built from the Manifest and all wrapper files.
pybind11_add_module(directport MODULE
    "${SOURCE_DIR}/Manifest.cpp"
    "${SOURCE_DIR}/DirectPortWrapper.cpp"
    "${SOURCE_DIR}/DirectPortNumpyWrapper.cpp"
    "${SOURCE_DIR}/DirectPortCameraWrapper.cpp"
)

# The Python module links against the C++ library.
target_link_libraries(directport PRIVATE DirectPortLib)

message(STATUS "Configured 'directport' Python module.")