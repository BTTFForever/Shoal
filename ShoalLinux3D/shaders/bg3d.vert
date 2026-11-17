#version 430
out vec2 vNdc;
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  vNdc = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.9999, 1.0);
}
