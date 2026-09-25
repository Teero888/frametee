#version 450
// The RDP's color combiner and what the blender does beyond blending
// (f3d/f3d.h): each cycle is (a - b) * c + d, for color and alpha, with the
// inputs G_SETCOMBINE selects. The second cycle reads the first's result as
// COMBINED, and the two textures swapped.
layout(push_constant) uniform State {
    vec4 prim;
    vec4 env;
    vec4 fog_color;
    uvec2 combine;
    uint flags;
    float alpha_threshold;
    float prim_lod_frac;
    float noise_seed;
} s;

layout(set = 0, binding = 0) uniform sampler2D texture0;
layout(set = 0, binding = 1) uniform sampler2D texture1;

layout(location = 0) in vec4 uv;
layout(location = 1) in vec4 shade;
layout(location = 2) in float fog;

layout(location = 0) out vec4 color;

const uint TWO_CYCLE = 1u, FILL = 2u, COPY = 4u, FOG = 8u, ALPHA_TEST = 16u, COVERAGE_ALPHA = 32u, BLEND = 64u;
const uint TRANSPARENT_TARGET = 1u << 30; // sm64_vulkan.c

float noise() {
    return fract(sin(dot(gl_FragCoord.xy + s.noise_seed, vec2(12.9898, 78.233))) * 43758.5453);
}

vec3 rgb_a(uint x, vec4 comb, vec4 t0, vec4 t1) {
    switch (x) {
        case 0u: return comb.rgb;
        case 1u: return t0.rgb;
        case 2u: return t1.rgb;
        case 3u: return s.prim.rgb;
        case 4u: return shade.rgb;
        case 5u: return s.env.rgb;
        case 6u: return vec3(1.0);
        case 7u: return vec3(noise());
        default: return vec3(0.0);
    }
}

vec3 rgb_b(uint x, vec4 comb, vec4 t0, vec4 t1) {
    switch (x) {
        case 0u: return comb.rgb;
        case 1u: return t0.rgb;
        case 2u: return t1.rgb;
        case 3u: return s.prim.rgb;
        case 4u: return shade.rgb;
        case 5u: return s.env.rgb;
        default: return vec3(0.0); // key center, K4
    }
}

vec3 rgb_c(uint x, vec4 comb, vec4 t0, vec4 t1) {
    switch (x) {
        case 0u: return comb.rgb;
        case 1u: return t0.rgb;
        case 2u: return t1.rgb;
        case 3u: return s.prim.rgb;
        case 4u: return shade.rgb;
        case 5u: return s.env.rgb;
        case 7u: return vec3(comb.a);
        case 8u: return vec3(t0.a);
        case 9u: return vec3(t1.a);
        case 10u: return vec3(s.prim.a);
        case 11u: return vec3(shade.a);
        case 12u: return vec3(s.env.a);
        case 14u: return vec3(s.prim_lod_frac);
        default: return vec3(0.0); // key scale, LOD fraction, K5
    }
}

vec3 rgb_d(uint x, vec4 comb, vec4 t0, vec4 t1) {
    switch (x) {
        case 0u: return comb.rgb;
        case 1u: return t0.rgb;
        case 2u: return t1.rgb;
        case 3u: return s.prim.rgb;
        case 4u: return shade.rgb;
        case 5u: return s.env.rgb;
        case 6u: return vec3(1.0);
        default: return vec3(0.0);
    }
}

float alpha_abd(uint x, vec4 comb, vec4 t0, vec4 t1) {
    switch (x) {
        case 0u: return comb.a;
        case 1u: return t0.a;
        case 2u: return t1.a;
        case 3u: return s.prim.a;
        case 4u: return shade.a;
        case 5u: return s.env.a;
        case 6u: return 1.0;
        default: return 0.0;
    }
}

float alpha_c(uint x, vec4 t0, vec4 t1) {
    switch (x) {
        case 1u: return t0.a;
        case 2u: return t1.a;
        case 3u: return s.prim.a;
        case 4u: return shade.a;
        case 5u: return s.env.a;
        case 6u: return s.prim_lod_frac;
        default: return 0.0; // LOD fraction, 0
    }
}

// One cycle, from its eight selectors.
vec4 cycle(uint a, uint b, uint c, uint d, uint aa, uint ab, uint ac, uint ad, vec4 comb, vec4 t0, vec4 t1) {
    vec3 rgb = (rgb_a(a, comb, t0, t1) - rgb_b(b, comb, t0, t1)) * rgb_c(c, comb, t0, t1) + rgb_d(d, comb, t0, t1);
    float alpha = (alpha_abd(aa, comb, t0, t1) - alpha_abd(ab, comb, t0, t1)) * alpha_c(ac, t0, t1)
                  + alpha_abd(ad, comb, t0, t1);
    return clamp(vec4(rgb, alpha), 0.0, 1.0);
}

void main() {
    vec4 t0 = texture(texture0, uv.xy);
    vec4 t1 = texture(texture1, uv.zw);
    vec4 result;
    if ((s.flags & FILL) != 0u) {
        result = shade;
    } else if ((s.flags & COPY) != 0u) {
        result = t0;
    } else {
        uint w0 = s.combine.x, w1 = s.combine.y;
        result = cycle((w0 >> 20) & 15u, (w1 >> 28) & 15u, (w0 >> 15) & 31u, (w1 >> 15) & 7u,
                       (w0 >> 12) & 7u, (w1 >> 12) & 7u, (w0 >> 9) & 7u, (w1 >> 9) & 7u, vec4(0.0), t0, t1);
        if ((s.flags & TWO_CYCLE) != 0u) {
            result = cycle((w0 >> 5) & 15u, (w1 >> 24) & 15u, w0 & 31u, (w1 >> 6) & 7u,
                           (w1 >> 21) & 7u, (w1 >> 3) & 7u, (w1 >> 18) & 7u, w1 & 7u, result, t1, t0);
        }
    }
    if ((s.flags & ALPHA_TEST) != 0u && (result.a < s.alpha_threshold || result.a == 0.0)) {
        discard;
    }
    if ((s.flags & COVERAGE_ALPHA) != 0u && result.a < 0.5) {
        discard;
    }
    if ((s.flags & FOG) != 0u) {
        result.rgb = mix(result.rgb, s.fog_color.rgb, fog);
    }
    // On a transparent target an unblended surface is opaque, whatever alpha
    // the combiner left (the blender ignores it there).
    if ((s.flags & TRANSPARENT_TARGET) != 0u && (s.flags & BLEND) == 0u) {
        result.a = 1.0;
    }
    color = result;
}
