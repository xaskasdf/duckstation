#version 450 core

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_texcoord;
layout(location = 2) flat in uint v_texture_index;

layout(location = 0) out vec4 o_color;

// Texture array - binding 0, set 0 (matches descriptor set layout)
layout(set = 0, binding = 0) uniform sampler2D u_textures[256];

void main()
{
  vec4 color;

  // texture_index >= 256 means "no texture" - use vertex color only
  if (v_texture_index >= 256u)
  {
    color = v_color;
  }
  else
  {
    // Sample the correct texture using the vertex's texture index
    vec4 tex = texture(u_textures[v_texture_index], v_texcoord);

    // PS1 hardware: result = (texel * vertex_color) >> 7
    // Vertex colors are normalized /255, but PS1 uses /128 for modulation.
    // Multiply by 2.0 so that vertex color 128 = neutral (1.0x), 255 = bright (2.0x).
    color = tex * (v_color * 2.0);
  }

  // Convert sRGB to linear: PS1 colors are sRGB but the swapchain is VK_FORMAT_*_SRGB,
  // which applies an automatic linear-to-sRGB conversion on write. Without this conversion,
  // colors get double-gamma-encoded and appear washed out.
  o_color = vec4(pow(color.rgb, vec3(2.2)), color.a);
}
