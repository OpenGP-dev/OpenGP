// SPDX-License-Identifier: GPL-3.0-or-later
//
// Solid colour (EFB clears).
#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(std140, set = 3, binding = 0) uniform ColorBlock {
  vec4 color;
};

void main() { out_color = color; }
