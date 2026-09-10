#version 450
layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_fog;
layout(location = 3) in vec4 in_input0;
layout(location = 4) in vec4 in_input1;
layout(location = 5) in vec4 in_input2;
layout(location = 6) in vec4 in_input3;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_fog;
layout(location = 2) out vec4 v_input0;
layout(location = 3) out vec4 v_input1;
layout(location = 4) out vec4 v_input2;
layout(location = 5) out vec4 v_input3;

void main() {
    gl_Position = vec4(in_pos.x, -in_pos.y, in_pos.z, in_pos.w);
    v_uv = in_uv;
    v_fog = in_fog;
    v_input0 = in_input0;
    v_input1 = in_input1;
    v_input2 = in_input2;
    v_input3 = in_input3;
}
