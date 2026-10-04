// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copies a texture (presenting the XFB).
#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D source;

void main() { out_color = vec4(texture(source, v_uv).rgb, 1.0); }
