#version 430
// Background picture, darkened by uDim (0 = unchanged, 1 = black).
uniform sampler2D uPicture;
uniform float uDim;
in vec2 vUv;
out vec4 outColour;
void main() { outColour = vec4(texture(uPicture, vUv).rgb * (1.0 - uDim), 1.0); }
