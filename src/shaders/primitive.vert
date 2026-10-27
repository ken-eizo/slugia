#version 450

layout(location = 0) in vec4 inPositionRect;
layout(location = 1) in vec4 inColor;
layout(location = 2) in uvec4 inClip;

layout(push_constant) uniform PushConstants {
  vec4 viewportScale;
  vec4 translationOverride;
  vec4 overrideClip;
  vec4 opacity;
  vec4 projectiveRows[3];
  vec4 projectiveClip;
} pushConstants;

layout(location = 0) flat out vec4 color;
layout(location = 1) flat out vec4 clipRect;

void main() {
  const vec2 corners[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
  vec2 corner = corners[gl_VertexIndex];
  vec2 positionPx = mix(inPositionRect.xy, inPositionRect.zw, corner);
  positionPx = positionPx * pushConstants.viewportScale.zw + pushConstants.translationOverride.xy;
  gl_Position = vec4(positionPx.x * 2.0 / pushConstants.viewportScale.x - 1.0,
                     positionPx.y * 2.0 / pushConstants.viewportScale.y - 1.0,
                     0.0, 1.0);
  color = inColor;
  color.a *= pushConstants.opacity.x;
  if (pushConstants.translationOverride.z > 0.5) {
    clipRect = pushConstants.overrideClip;
  } else {
    vec4 normalizedClip = vec4(inClip) / 65535.0;
    vec2 clipMin = normalizedClip.xy * pushConstants.viewportScale.xy;
    vec2 clipMax = normalizedClip.zw * pushConstants.viewportScale.xy;
    clipRect = vec4(clipMin, max(clipMax - clipMin, vec2(0.0)));
  }
}
