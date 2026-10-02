#version 450

// The lightmap bake's shadow map (tmuf_bake.c, lightmap_bake_spec.md §4,
// §9.6): the casters seen from the light, inside a one-texel border, their
// depth along the light's axis in metres from its origin. Orthographic for
// the sky and the sun, fitted to the zone's box; perspective for a spot,
// from its apex.
layout(std140, binding = 0) uniform bake_ubo {
  vec4 x_axis, y_axis, z_axis; // the light's camera (z: where it looks)
  vec4 centre;                 // its origin: the zone box's centre, or the spot's apex
  vec4 extent;                 // ortho: the box's half extents along x, y, z; perspective: the half tangents x, y
                               // and the far depth; w: the border's scale (W - 2) / W
  vec4 params;                 // x: alpha cutoff (0 none); y: the depth bias (ortho m, perspective a factor);
                               // z: the sample's weight; w: the receivers' mode (tmuf_bake.c)
  vec4 persp;                  // x: 1 perspective; y, z: the frustum's centre tangents; w: its near depth
  vec4 spot_pos, spot_dir, spot_rgb;
}
ubo;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 0) out float v_depth;
layout(location = 1) out vec2 v_uv;

void main() {
  vec3 rel = in_pos - ubo.centre.xyz;
  float x = dot(ubo.x_axis.xyz, rel), y = dot(ubo.y_axis.xyz, rel), z = dot(ubo.z_axis.xyz, rel);
  v_depth = z;
  v_uv = in_uv;
  if (ubo.persp.x > 0.5) {
    // the nearer the larger z (the reversed depth test keeps it): near 1, far 0
    float n = ubo.persp.w, f = ubo.extent.z;
    gl_Position = vec4((x - ubo.persp.y * z) / ubo.extent.x * ubo.extent.w,
                       (y - ubo.persp.z * z) / ubo.extent.y * ubo.extent.w, n * (f - z) / (f - n), z);
    return;
  }
  vec2 ndc = vec2(x / ubo.extent.x, y / ubo.extent.y);
  gl_Position = vec4(ndc * ubo.extent.w, 0.5 - 0.5 * z / ubo.extent.z, 1.0);
}
