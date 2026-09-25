#version 450
layout(binding=0, std140) uniform Frame {
  mat4 view_projection;
  mat4 reflection_projection;
  mat4 shadow_projection;
  vec4 eye_time;
  vec4 sun_direction;
  vec4 sun_color;
  vec4 ambient;
  vec4 fog_color;
  vec4 clip_plane;
  vec4 viewport;
} frame;
layout(binding=1) uniform sampler2DArray diffuse_map;
layout(binding=2) uniform sampler2DArray normal_map;
layout(binding=3) uniform sampler2DArray specular_map;
layout(binding=4) uniform sampler2DArray occlusion_map;
layout(binding=5) uniform sampler2DArray emission_map;
#ifndef REFLECTION_PASS
layout(binding=6) uniform sampler2D reflection_map;
layout(binding=7) uniform sampler2D refraction_map;
#endif
layout(binding=8) uniform sampler2DShadow shadow_map;
layout(push_constant) uniform Material {
  vec4 animation;
  uvec4 flags;
  vec4 tint;
} material;
layout(location=0) in vec3 world_position;
layout(location=1) in vec3 world_normal;
layout(location=2) in vec2 texcoord;
layout(location=3) in vec2 occlusion_coord;
layout(location=4) in vec4 vertex_color;
layout(location=0) out vec4 result;

bool flag(uint bit) { return (material.flags.x & bit) != 0u; }
float shadow(vec3 normal) {
  vec4 projected = frame.shadow_projection * vec4(world_position,1);
  vec3 p = projected.xyz / projected.w;
  p.xy = p.xy * 0.5 + 0.5;
  if (any(lessThan(p,vec3(0))) || any(greaterThan(p,vec3(1)))) return 1;
  p.z -= max(0.0002, 0.001 * (1.0 - dot(normal,frame.sun_direction.xyz)));
  vec2 step_size = 1.0 / vec2(textureSize(shadow_map,0));
  float light = 0;
  for (int y=-1;y<=1;++y) for (int x=-1;x<=1;++x)
    light += texture(shadow_map, vec3(p.xy + vec2(x,y)*step_size,p.z));
  return light / 9.0;
}

void main() {
#ifdef REFLECTION_PASS
  if (dot(vec4(world_position,1),frame.clip_plane) < 0) discard;
#endif
  float animation_frame = 0;
  if (material.animation.z > 1)
    animation_frame = mod(floor(frame.eye_time.w * material.animation.w), material.animation.z);
  if(flag(2048u)) animation_frame=frame.eye_time.w < -1 ? 2 : frame.eye_time.w < 0 ? 1 : 0;
  vec4 surface = texture(diffuse_map,vec3(texcoord,animation_frame),frame.viewport.z);
  if (flag(2u) && surface.a < 0.5) discard;
  if (!flag(4u)) surface.a = 1;
  surface *= vertex_color * material.tint;
  if (flag(256u)) {
    result=vec4(0); // Shadow pass: only depth and cutout coverage are needed.
    return;
  }
  vec3 normal = normalize(world_normal);
  if (!gl_FrontFacing && flag(128u)) normal=-normal;
  vec3 view_direction=normalize(frame.eye_time.xyz-world_position);
  if (flag(8u) && !flag(64u)) {
    vec3 dpdx=dFdx(world_position), dpdy=dFdy(world_position);
    vec2 duvdx=dFdx(texcoord), duvdy=dFdy(texcoord);
    vec3 tangent=dpdx*duvdy.y-dpdy*duvdx.y;
    vec3 bitangent=dpdy*duvdx.x-dpdx*duvdy.x;
    float orientation=sign(duvdx.x*duvdy.y-duvdx.y*duvdy.x);
    tangent*=orientation;
    bitangent*=orientation;
    float scale=max(dot(tangent,tangent),dot(bitangent,bitangent));
    if(scale>0.000001) {
      vec4 packed=texture(normal_map,vec3(texcoord,0),frame.viewport.z);
      vec3 sampled=(flag(4096u)?packed.agb:packed.rgb)*2.0-1.0;
      normal=normalize(mat3(tangent*inversesqrt(scale),bitangent*inversesqrt(scale),normal)*sampled);
    }
  }
#ifndef REFLECTION_PASS
  if (flag(64u)) {
    // TMNF's Sea material supplies an animated normal field and two rendered
    // views. Refraction uses the opaque scene captured before this pass;
    // reflection uses the mirrored camera clipped at the water plane.
    vec2 uv=world_position.xz/32.0;
    float t=frame.eye_time.w;
    vec3 wave=vec3(0,0,1);
    if(flag(8u)) {
      vec4 p0=texture(normal_map,vec3(uv+vec2(t*.013,t*.009),0));
      vec4 p1=texture(normal_map,vec3(uv*.73+vec2(-t*.007,t*.011),0));
      vec3 n0=(flag(4096u)?p0.agb:p0.rgb)*2-1;
      vec3 n1=(flag(4096u)?p1.agb:p1.rgb)*2-1;
      wave=normalize(vec3(n0.xy+n1.xy,2));
    }
    normal=normalize(vec3(wave.x, max(.3,wave.z),wave.y));
    vec2 distortion=normal.xz*.014;
    vec2 screen_uv=gl_FragCoord.xy/frame.viewport.xy;
    vec4 mirror=frame.reflection_projection*vec4(world_position,1);
    vec2 mirror_uv=mirror.xy/mirror.w*.5+.5;
    vec3 refracted=texture(refraction_map,clamp(screen_uv+distortion,vec2(.001),vec2(.999))).rgb;
    vec3 reflected=texture(reflection_map,clamp(mirror_uv+distortion,vec2(.001),vec2(.999))).rgb;
    float fresnel=.025+.975*pow(1-clamp(dot(normal,view_direction),0,1),5);
    vec3 col=mix(refracted*vec3(.76,.91,.96),reflected,fresnel);
    vec3 halfway=normalize(frame.sun_direction.xyz+view_direction);
    col+=frame.sun_color.rgb*pow(max(dot(normal,halfway),0),180)*.5;
    // Refraction already includes the background. Blending it with that same
    // background again adds it twice and turns the water milky white.
    result=vec4(col,1);
    return;
  }
#endif
  vec3 col=surface.rgb;
  if(!flag(1u)) {
    float occ=flag(32u)?texture(occlusion_map,vec3(occlusion_coord,0),frame.viewport.z).r:1;
    float visibility=shadow(normal);
    float diffuse=max(dot(normal,frame.sun_direction.xyz),0);
    vec3 lighting=frame.ambient.rgb+frame.sun_color.rgb*diffuse*visibility;
    col*=lighting*occ;
    if(flag(16u)) {
      vec4 specular=texture(specular_map,vec3(texcoord,0),frame.viewport.z);
      vec3 halfway=normalize(frame.sun_direction.xyz+view_direction);
      float power=mix(12,128,specular.a);
      col+=frame.sun_color.rgb*specular.rgb*pow(max(dot(normal,halfway),0),power)*visibility*occ;
    }
  }
  if(flag(512u)) col+=texture(emission_map,vec3(texcoord,0),frame.viewport.z).rgb;
  if(flag(1024u)) {
    result=vec4(col*material.tint.a,0);
    return;
  }
  float distance_to_eye=length(world_position-frame.eye_time.xyz);
  float fog=1-exp(-distance_to_eye*frame.fog_color.a);
  if(!flag(1u)) col=mix(col,frame.fog_color.rgb,fog);
  result=vec4(col*surface.a,surface.a);
}
