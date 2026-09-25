cmake_minimum_required(VERSION 3.20)
project(DirectPort LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(DIRECTPORT_WIN_LIBS
    d3d12
    dxgi
    d3dcompiler
    dwmapi
    shcore
    uxtheme
    msimg32
    user32
    shell32
    advapi32
    ole32
    mf
    mfplat
    mfreadwrite
    mfuuid
    ws2_32
)

add_executable(DirectPort WIN32
    src/apps/DirectPort/DirectPort.cpp
    src/apps/DirectPort/Menu.cpp
    src/apps/DirectPort/Menu.h
    src/apps/DirectPort/DirectPort.rc
    src/sdk/DirectPort_Discovery.h
    src/sdk/DirectPort_Audio.h
    src/sdk/DirectPort_Camera.h
    src/sdk/directport.h
)

target_link_libraries(DirectPort PRIVATE ${DIRECTPORT_WIN_LIBS})
target_compile_definitions(DirectPort PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
