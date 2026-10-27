#version 450

layout(push_constant) uniform PushConstants {
  vec4 viewportScale;
  vec4 translationOverride;
  vec4 overrideClip;
  vec4 opacity;
  vec4 projectiveRows[3];
  vec4 projectiveClip;
} pushConstants;

layout(location = 0) flat in vec4 color;
layout(location = 1) flat in vec4 clipRect;
layout(location = 0) out vec4 outColor;

vec3 srgbToLinear(vec3 value) {
  bvec3 low = lessThanEqual(value, vec3(0.04045));
  vec3 lower = value / 12.92;
  vec3 upper = pow((value + 0.055) / 1.055, vec3(2.4));
  return mix(upper, lower, low);
}

void main() {
  if (gl_FragCoord.x < clipRect.x || gl_FragCoord.y < clipRect.y ||
      gl_FragCoord.x >= clipRect.x + clipRect.z || gl_FragCoord.y >= clipRect.y + clipRect.w)
    discard;
  vec4 result = color;
  if (pushConstants.translationOverride.w > 0.5)
    result.rgb = srgbToLinear(clamp(result.rgb, 0.0, 1.0));
  if (result.a <= 0.001) discard;
  outColor = result;
}
