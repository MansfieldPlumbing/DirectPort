// --- smpte.hlsl ---
// DirectPort SMPTE 75% Color Bars Shader with Sub-blocks
cbuffer Constants : register(b0) {
    float4 u_resolution; // xy = resolution
    float4 u_time;       // x = elapsed seconds
    float4 u_mouse;
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

PSInput VSMain(uint id : SV_VertexID) {
    PSInput output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv.x * 2.0 - 1.0, 1.0 - output.uv.y * 2.0, 0.0, 1.0);
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    float2 uv = input.uv;

    // Standard 7 top bars (0.0 to 0.67 height)
    if (uv.y < 0.67) {
        float x = uv.x * 7.0;
        if (x < 1.0) return float4(0.75, 0.75, 0.75, 1.0); // 75% Gray
        if (x < 2.0) return float4(0.75, 0.75, 0.00, 1.0); // Yellow
        if (x < 3.0) return float4(0.00, 0.75, 0.75, 1.0); // Cyan
        if (x < 4.0) return float4(0.00, 0.75, 0.00, 1.0); // Green
        if (x < 5.0) return float4(0.75, 0.00, 0.75, 1.0); // Magenta
        if (x < 6.0) return float4(0.75, 0.00, 0.00, 1.0); // Red
        return float4(0.00, 0.00, 0.75, 1.0);               // Blue
    }
    // Cast section (0.67 to 0.75 height)
    else if (uv.y < 0.75) {
        float x = uv.x * 7.0;
        if (x < 1.0) return float4(0.00, 0.00, 0.75, 1.0); // Blue
        if (x < 2.0) return float4(0.07, 0.07, 0.07, 1.0); // Black
        if (x < 3.0) return float4(0.75, 0.00, 0.75, 1.0); // Magenta
        if (x < 4.0) return float4(0.07, 0.07, 0.07, 1.0); // Black
        if (x < 5.0) return float4(0.00, 0.75, 0.75, 1.0); // Cyan
        if (x < 6.0) return float4(0.07, 0.07, 0.07, 1.0); // Black
        return float4(0.75, 0.75, 0.75, 1.0);               // Gray
    }
    // Sync & Pluge section (0.75 to 1.0 height)
    else {
        float x = uv.x;
        if (x < 0.177) return float4(0.00, 0.13, 0.30, 1.0); // I-Signal Navy
        if (x < 0.354) return float4(1.00, 1.00, 1.00, 1.0); // 100% White
        if (x < 0.531) return float4(0.20, 0.00, 0.42, 1.0); // Q-Signal Purple
        if (x < 0.708) return float4(0.04, 0.04, 0.04, 1.0); // Black
        // Pluge bars (-2%, 0%, +2%)
        float px = (x - 0.708) / (1.0 - 0.708);
        if (px < 0.33) return float4(0.00, 0.00, 0.00, 1.0); // -2% Black
        if (px < 0.66) return float4(0.04, 0.04, 0.04, 1.0); // 0% Black
        return float4(0.08, 0.08, 0.08, 1.0);                 // +2% Black
    }
}
