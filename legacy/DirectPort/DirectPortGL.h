// DirectPortGL.h
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

typedef unsigned int GLuint;

namespace DirectPortGL {

    struct BroadcastManifest {
        UINT64 frameValue;
        UINT width;
        UINT height;
        DXGI_FORMAT format;
        LUID adapterLuid;
        WCHAR textureName[256];
        WCHAR fenceName[256];
    };

    class DeviceGL;
    class TextureGL;
    class ConsumerGL;
    class ProducerGL;
    class WindowGL;

    struct ProducerInfo {
        unsigned long pid;
        std::wstring executable_name;
        std::wstring stream_name;
        std::wstring type;
    };

    std::vector<ProducerInfo> discover();

    class TextureGL {
    public:
        ~TextureGL();
        uint32_t get_width() const;
        uint32_t get_height() const;
        uint32_t get_gl_texture_id() const;

    private:
        friend class DeviceGL;
        friend class ConsumerGL;
        friend class ProducerGL;
        TextureGL();
        struct Impl;
        std::unique_ptr<Impl> pImpl;
    };

    class ConsumerGL {
    public:
        ~ConsumerGL();
        bool wait_for_frame(uint32_t timeout_ms = 1000);
        bool is_alive() const;
        std::shared_ptr<TextureGL> get_texture();
        unsigned long get_pid() const;
    private:
        friend class DeviceGL;
        ConsumerGL();
        struct Impl;
        std::unique_ptr<Impl> pImpl;
    };

    class ProducerGL {
    public:
        ~ProducerGL();
        void signal_frame();
        unsigned long get_pid() const;
    private:
        friend class DeviceGL;
        ProducerGL();
        struct Impl;
        std::unique_ptr<Impl> pImpl;
    };
    
    class WindowGL {
    public:
        ~WindowGL();
        bool process_events();
        void present();
        void set_title(const std::string& title);
        uint32_t get_width() const;
        uint32_t get_height() const;
        void make_current();

    private:
        friend class DeviceGL;
        WindowGL();
        struct Impl;
        std::unique_ptr<Impl> pImpl;
    };

    class DeviceGL : public std::enable_shared_from_this<DeviceGL> {
    public:
        static std::shared_ptr<DeviceGL> create();
        ~DeviceGL();

        std::shared_ptr<TextureGL> create_texture(uint32_t width, uint32_t height, DXGI_FORMAT format, const void* data = nullptr);
        std::shared_ptr<ProducerGL> create_producer(const std::string& stream_name, std::shared_ptr<TextureGL> texture);
        std::shared_ptr<ConsumerGL> connect_to_producer(unsigned long pid);
        std::shared_ptr<WindowGL> create_window(uint32_t width, uint32_t height, const std::string& title);
        
        void apply_shader(std::shared_ptr<TextureGL> output, const std::string& glsl_fragment_shader, const std::vector<std::shared_ptr<TextureGL>>& inputs, const std::vector<uint8_t>& constants);
        void copy_texture(std::shared_ptr<TextureGL> source, std::shared_ptr<TextureGL> destination);
        void blit(std::shared_ptr<TextureGL> source, std::shared_ptr<WindowGL> destination);
        void clear(std::shared_ptr<WindowGL> window, float r, float g, float b, float a);
        void blit_texture_to_region(std::shared_ptr<TextureGL> source, std::shared_ptr<TextureGL> destination,
                                            uint32_t dest_x, uint32_t dest_y, uint32_t dest_width, uint32_t dest_height);
    private:
        friend class WindowGL; // <-- ADD THIS LINE
        DeviceGL();
        struct Impl;
        std::unique_ptr<Impl> pImpl;
    };

}