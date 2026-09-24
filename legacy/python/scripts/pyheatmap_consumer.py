# src/Scripts/pyheatmap_consumer.py

import directport
import time
import sys

HEATMAP_SHADER_HLSL = """
Texture2D<float> inputTexture : register(t0);
SamplerState linearSampler : register(s0);

float4 colorize(float value) {
    float4 blue = float4(0, 0, 1, 1);
    float4 green = float4(0, 1, 0, 1);
    float4 red = float4(1, 0, 0, 1);
    
    float4 bottom_color = lerp(blue, green, value * 2.0);
    float4 top_color = lerp(green, red, (value - 0.5) * 2.0);
    
    return (value < 0.5) ? bottom_color : top_color;
}

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
    float raw_value = inputTexture.Sample(linearSampler, uv).r;
    return colorize(raw_value);
}
"""

def main():
    print("--- DirectPort Heatmap Consumer ---")
    try:
        device = directport.DeviceD3D11.create()
    except RuntimeError as e:
        print(f"Fatal: Failed to create D3D11 device: {e}", file=sys.stderr)
        sys.exit(1)
        
    window = device.create_window(1280, 720, "Heatmap Consumer - Searching...")
    
    consumer = None
    
    print("Searching for 'heatmap_float_stream' producer...")
    try:
        while window.process_events():
            if consumer is None or not consumer.is_alive():
                if consumer:
                    print("Producer disconnected. Resuming search...")
                    consumer = None
                
                producers = directport.discover()
                target_producer = next((p for p in producers if p.stream_name == "heatmap_float_stream"), None)
                
                if target_producer:
                    print(f"Found producer PID {target_producer.pid}. Connecting...")
                    consumer = device.connect_to_producer(target_producer.pid)
                    if consumer:
                        print("Successfully connected.")
                        # This texture will receive the final, colored output of our shader
                        colored_output_texture = device.create_texture(
                            consumer.get_texture().width,
                            consumer.get_texture().height,
                            directport.DXGI_FORMAT.B8G8R8A8_UNORM
                        )
                    else:
                        print("Failed to connect.")
                else:
                    device.clear(window, 0.1, 0.1, 0.4, 1.0)
                    window.present()
                    time.sleep(1)
            
            if consumer:
                window.set_title(f"Heatmap Consumer | Connected to PID {consumer.pid}")
                if consumer.wait_for_frame():
                    # 1. Get the shared texture (contains raw float data)
                    shared_float_texture = consumer.get_shared_texture()
                    
                    # 2. Apply the heatmap shader.
                    #    INPUT: The shared float texture.
                    #    OUTPUT: Our local RGBA texture.
                    device.apply_shader(
                        output=colored_output_texture,
                        shader=HEATMAP_SHADER_HLSL,
                        entry_point="main",
                        inputs=[shared_float_texture]
                    )
                    
                    # 3. Blit the final, colored result to the window.
                    device.blit(colored_output_texture, window)
                    window.present()

    except KeyboardInterrupt:
        print("\nConsumer stopped.")
    finally:
        print("Shutting down.")

if __name__ == "__main__":
    main()