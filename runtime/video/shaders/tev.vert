// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vertices arrive fully transformed and lit (the GX transform unit runs on
// the CPU, see runtime/gx/transform.cpp), so this only passes them through.
#version 450

layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec4 in_color0;
layout(location = 2) in vec4 in_color1;
layout(location = 3) in vec3 in_tex0;
layout(location = 4) in vec3 in_tex1;
layout(location = 5) in vec3 in_tex2;
layout(location = 6) in vec3 in_tex3;
layout(location = 7) in vec3 in_tex4;
layout(location = 8) in vec3 in_tex5;
layout(location = 9) in vec3 in_tex6;
layout(location = 10) in vec3 in_tex7;

layout(location = 0) out vec4 v_color0;
layout(location = 1) out vec4 v_color1;
layout(location = 2) out vec3 v_tex[8];

void main() {
  gl_Position = in_pos;
  v_color0 = in_color0;
  v_color1 = in_color1;
  v_tex[0] = in_tex0;
  v_tex[1] = in_tex1;
  v_tex[2] = in_tex2;
  v_tex[3] = in_tex3;
  v_tex[4] = in_tex4;
  v_tex[5] = in_tex5;
  v_tex[6] = in_tex6;
  v_tex[7] = in_tex7;
}
