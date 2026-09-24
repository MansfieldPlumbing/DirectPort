// --- plasma.hlsl ---
// DirectPort Procedural Cybernetic Plasma Shader
cbuffer Constants : register(b0) {
    float4 u_resolution; // xy = resolution, z = aspect ratio, w = unused
    float4 u_time;       // x = elapsed seconds, y = delta, z = frame index, w = unused
    float4 u_mouse;      // xy = mouse pos, zw = unused
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
    float2 p = (input.uv * 2.0 - 1.0);
    p.x *= (u_resolution.x / u_resolution.y);

    float t = u_time.x * 0.8;
    float v = 0.0;

    v += sin((p.x * 10.0 + t));
    v += sin((p.y * 10.0 + t) / 2.0);
    v += sin((p.x * 10.0 + p.y * 10.0 + t) / 2.0);

    float cx = p.x + 0.5 * sin(t / 5.0);
    float cy = p.y + 0.5 * cos(t / 3.0);
    v += sin(sqrt(100.0 * (cx * cx + cy * cy) + 1.0) + t);

    v = v / 2.0;

    // Palette: Machinery Red to Electric Cyan
    float3 col1 = float3(0.78, 0.06, 0.18); // Milwaukee red
    float3 col2 = float3(0.06, 0.75, 0.90); // Electric cyan
    float3 col3 = float3(0.04, 0.05, 0.07); // Cast iron dark

    float3 col = lerp(col3, col1, sin(v * 3.14159) * 0.5 + 0.5);
    col = lerp(col, col2, cos(v * 2.0) * 0.3 + 0.3);

    // Subtle edge vignette
    float vig = 1.0 - smoothstep(0.7, 1.4, length(input.uv * 2.0 - 1.0));
    col *= vig;

    return float4(col, 1.0);
}
