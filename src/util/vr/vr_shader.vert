#version 450 core

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_texcoord;
layout(location = 3) in uint a_texture_index;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_texcoord;
layout(location = 2) flat out uint v_texture_index;

layout(push_constant) uniform PushConstants {
  mat4 u_view_proj;
  float u_scale;
  float u_padding[3];
} pc;

void main()
{
  vec3 world_pos = a_position * pc.u_scale * 0.001;
  world_pos.z = -world_pos.z;
  gl_Position = pc.u_view_proj * vec4(world_pos, 1.0);
  v_color = a_color;
  v_texcoord = a_texcoord;
  v_texture_index = a_texture_index;
}
