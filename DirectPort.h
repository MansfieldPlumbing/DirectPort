#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace DirectPort {

    struct ProducerInfo {
        unsigned long pid;
        std::wstring name;
        std::wstring type;
    };

    class ProducerImpl;
    class Producer {
    public:
        bool wait_for_frame();
        uintptr_t get_texture_ptr();
        unsigned int get_width() const;
        unsigned int get_height() const;
        unsigned long get_pid() const;
        
        Producer(std::unique_ptr<ProducerImpl> impl);
        ~Producer();
    private:
        friend class Consumer; // Allow Consumer to access implementation
        std::unique_ptr<ProducerImpl> pImpl;
    };

    class ConsumerImpl;
    class Consumer {
    public:
        Consumer(const std::wstring& title, int width, int height);
        ~Consumer();

        bool process_events(); // Returns false if the window was closed
        void render_frame(const std::vector<std::shared_ptr<Producer>>& producers);
        void present();

        std::vector<ProducerInfo> discover();
        std::shared_ptr<Producer> connect(unsigned long pid);
    
    private:
        std::unique_ptr<ConsumerImpl> pImpl;
    };

} // namespace DirectPort