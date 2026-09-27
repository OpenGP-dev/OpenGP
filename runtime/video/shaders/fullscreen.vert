// SPDX-License-Identifier: GPL-3.0-or-later
//
// One triangle covering the render target (restricted by the viewport or
// scissor). v_uv runs across `rect` (x0, y0, x1, y1) as the triangle
// covers the target, and depth is `rect_z.x`.
#version 450

layout(std140, set = 1, binding = 0) uniform FullscreenBlock {
  vec4 rect;
  vec4 rect_z;
};

layout(location = 0) out vec2 v_uv;

void main() {
  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);  // (0,0), (2,0), (0,2)
  v_uv = rect.xy + (rect.zw - rect.xy) * p;
  gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, rect_z.x, 1.0);
}
