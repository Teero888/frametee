#version 450
layout(push_constant) uniform PushConstants {
    uint shader_id;
} pc;

layout(set = 0, binding = 0) uniform sampler2D tex0;
layout(set = 0, binding = 1) uniform sampler2D tex1;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_fog;
layout(location = 2) in vec4 v_input0;
layout(location = 3) in vec4 v_input1;
layout(location = 4) in vec4 v_input2;
layout(location = 5) in vec4 v_input3;

layout(location = 0) out vec4 out_color;

vec4 get_item(uint code, bool alpha_only, vec4 texel0, vec4 texel1) {
    if (code == 0u) return vec4(0.0);
    if (code >= 1u && code <= 4u) {
        vec4 res;
        if (code == 1u) res = v_input0;
        else if (code == 2u) res = v_input1;
        else if (code == 3u) res = v_input2;
        else res = v_input3;
        if (alpha_only) res = vec4(res.a);
        return res;
    }
    if (code == 5u) return alpha_only ? vec4(texel0.a) : texel0;
    if (code == 6u) return vec4(texel0.a);
    if (code == 7u) return alpha_only ? vec4(texel1.a) : texel1;
    return vec4(0.0);
}

vec4 evaluate_cycle(uint cycle, bool alpha_only, vec4 texel0, vec4 texel1) {
    uint shift = cycle * 12u;
    uint a = (pc.shader_id >> shift) & 7u;
    uint b = (pc.shader_id >> (shift + 3u)) & 7u;
    uint c = (pc.shader_id >> (shift + 6u)) & 7u;
    uint d = (pc.shader_id >> (shift + 9u)) & 7u;

    vec4 itemA = get_item(a, alpha_only, texel0, texel1);
    vec4 itemB = get_item(b, alpha_only, texel0, texel1);
    vec4 itemC = get_item(c, alpha_only, texel0, texel1);
    vec4 itemD = get_item(d, alpha_only, texel0, texel1);

    if (c == 0u) return itemD;
    if (b == 0u && d == 0u) return itemA * itemC;
    if (b == d) return mix(itemB, itemA, itemC);
    return (itemA - itemB) * itemC + itemD;
}

void main() {
    vec4 texel0 = texture(tex0, v_uv);
    vec4 texel1 = texture(tex1, v_uv);

    vec4 rgb_val = evaluate_cycle(0u, false, texel0, texel1);
    vec4 result = vec4(rgb_val.rgb, 1.0);

    bool opt_alpha = (pc.shader_id & (1u << 24)) != 0u;
    if (opt_alpha) {
        vec4 a_val = evaluate_cycle(1u, true, texel0, texel1);
        result.a = a_val.a;
    }

    bool opt_texture_edge = (pc.shader_id & (1u << 26)) != 0u;
    if (opt_texture_edge && opt_alpha) {
        if (result.a > 0.3) {
            result.a = 1.0;
        } else {
            discard;
        }
    }

    bool opt_fog = (pc.shader_id & (1u << 25)) != 0u;
    if (opt_fog) {
        result.rgb = mix(result.rgb, v_fog.rgb, v_fog.a);
    }

    bool opt_noise = (pc.shader_id & (1u << 27)) != 0u;
    if (opt_alpha && opt_noise) {
        float r = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
        result.a *= floor(r + 0.5);
    }

    out_color = clamp(result, 0.0, 1.0);
}
