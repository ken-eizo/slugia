#version 450

// Slug coverage solve derived from Eric Lengyel's public reference implementation.
// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2017 Eric Lengyel. Adapted for Vulkan/slughorn atlas textures.

layout(set = 0, binding = 0) uniform sampler2D curveTexture;
layout(set = 0, binding = 1) uniform usampler2D bandTexture;
layout(set = 0, binding = 2, std430) readonly buffer ExternalPixels {
  uint externalPixelWords[];
};
layout(set = 0, binding = 3, std430) readonly buffer DynamicSlugCurves {
  vec4 dynamicCurveData[];
};
layout(set = 0, binding = 4, std430) readonly buffer DynamicSlugBands {
  uint dynamicBandWords[];
};
layout(set = 0, binding = 5, std430) readonly buffer PersistentSlugCurves {
  vec2 persistentCurveData[];
};
layout(constant_id = 0) const uint persistentCurveVec2Stride = 4u;
layout(constant_id = 1) const uint packedAtlasBandMode = 0u;
layout(set = 0, binding = 6, std430) readonly buffer PersistentSlugBands {
  uint persistentBandWords[];
};

layout(push_constant) uniform PushConstants {
  vec4 viewportScale;
  vec4 translationOverride;
  vec4 overrideClip;
  vec4 opacity;
  vec4 projectiveRows[3];
  vec4 projectiveClip;
} pushConstants;

layout(location = 0) in vec2 emCoord;
layout(location = 1) in vec2 uv;
layout(location = 2) flat in vec4 bandTransform;
layout(location = 3) flat in uvec4 shapeData;
layout(location = 4) flat in vec4 color0;
layout(location = 5) flat in vec4 color1;
layout(location = 6) flat in vec4 paintData;
layout(location = 7) flat in vec4 gradientData;
layout(location = 8) flat in vec4 clipRect;
layout(location = 9) flat in vec4 strokeWidths;
layout(location = 10) in vec2 localPosition;

layout(location = 0) out vec4 outColor;

const int indirectionSize = 32;
const uint externalImageShape = 0xFFFFFFFCu;
const uint dynamicSlugShape = 0xFFFFFFF8u;

uint calcRootCode(float y1, float y2, float y3) {
  uint i1 = floatBitsToUint(y1) >> 31u;
  uint i2 = floatBitsToUint(y2) >> 30u;
  uint i3 = floatBitsToUint(y3) >> 29u;
  uint shift = (i2 & 2u) | (i1 & ~2u);
  shift = (i3 & 4u) | (shift & ~4u);
  return (0x2E74u >> shift) & 0x0101u;
}

vec2 solveHorizontal(vec4 p12, vec2 p3) {
  if (all(equal(p12.zw, p3)) || all(equal(p12.xy, p12.zw))) {
    float dy = p3.y - p12.y;
    float x = dy != 0.0 ? fma(p3.x - p12.x, -p12.y / dy, p12.x) : p12.x;
    return vec2(x);
  }
  vec2 a = p12.xy - p12.zw * 2.0 + p3;
  vec2 b = p12.xy - p12.zw;
  float t1;
  float t2;
  if (a.y == 0.0) {
    t1 = b.y != 0.0 ? p12.y * 0.5 / b.y : 0.0;
    t2 = t1;
  } else {
    float d = sqrt(max(b.y * b.y - a.y * p12.y, 0.0));
    // Vieta's relation avoids b-sqrt(b*b-a*c) cancellation for nearly
    // straight curves at extreme magnification. Preserve the reference root
    // ordering because calcRootCode selects roots by that ordering.
    float q = b.y + (b.y < 0.0 ? -d : d);
    // Control-point signs can select two candidate roots even when the
    // discriminant is negative. The reference clamps it to zero, so these
    // roots MUST coincide and cancel. Vieta's c/q identity requires real roots.
    float stable = d == 0.0 ? b.y / a.y : (q != 0.0 ? p12.y / q : 0.0);
    t1 = b.y < 0.0 ? q / a.y : stable;
    t2 = b.y < 0.0 ? stable : q / a.y;
  }
  return vec2((a.x * t1 - b.x * 2.0) * t1 + p12.x,
              (a.x * t2 - b.x * 2.0) * t2 + p12.x);
}

vec2 solveVertical(vec4 p12, vec2 p3) {
  if (all(equal(p12.zw, p3)) || all(equal(p12.xy, p12.zw))) {
    float dx = p3.x - p12.x;
    float y = dx != 0.0 ? fma(p3.y - p12.y, -p12.x / dx, p12.y) : p12.y;
    return vec2(y);
  }
  vec2 a = p12.xy - p12.zw * 2.0 + p3;
  vec2 b = p12.xy - p12.zw;
  float t1;
  float t2;
  if (a.x == 0.0) {
    t1 = b.x != 0.0 ? p12.x * 0.5 / b.x : 0.0;
    t2 = t1;
  } else {
    float d = sqrt(max(b.x * b.x - a.x * p12.x, 0.0));
    float q = b.x + (b.x < 0.0 ? -d : d);
    float stable = d == 0.0 ? b.x / a.x : (q != 0.0 ? p12.x / q : 0.0);
    t1 = b.x < 0.0 ? q / a.x : stable;
    t2 = b.x < 0.0 ? stable : q / a.x;
  }
  return vec2((a.y * t1 - b.y * 2.0) * t1 + p12.y,
              (a.y * t2 - b.y * 2.0) * t2 + p12.y);
}

ivec2 bandLocation(ivec2 glyphLocation, uint offset) {
  int textureWidth = textureSize(bandTexture, 0).x;
  uint x = uint(glyphLocation.x) + offset;
  // VectorAtlas uses a power-of-two width. Integer division in every curve
  // iteration is expensive; retain a uniform fallback for other atlas layouts.
  if ((textureWidth & (textureWidth - 1)) == 0)
    return ivec2(int(x & uint(textureWidth - 1)), glyphLocation.y + int(x >> uint(findMSB(textureWidth))));
  return ivec2(int(x % uint(textureWidth)), glyphLocation.y + int(x / uint(textureWidth)));
}

ivec2 curveTailLocation(ivec2 location) {
  int width = textureSize(curveTexture, 0).x;
  location.x += 1;
  if (location.x >= width) { location.x = 0; location.y += 1; }
  return location;
}

float slugCoverage(vec2 renderCoord) {
  vec2 emsPerPixel = max(fwidth(renderCoord), vec2(1.0e-30));
  vec2 pixelsPerEm = 1.0 / emsPerPixel;
  ivec2 glyphLocation = ivec2(shapeData.xy);
  ivec2 bandMaximum = ivec2(shapeData.zw);
  bool evenOdd = (shapeData.w & 0x1000u) != 0u;
  bandMaximum.y &= 0xFF;

  int qY = clamp(int(renderCoord.y * bandTransform.y + bandTransform.w), 0, indirectionSize - 1);
  int qX = clamp(int(renderCoord.x * bandTransform.x + bandTransform.z), 0, indirectionSize - 1);
  int bandY = int(texelFetch(bandTexture, glyphLocation + ivec2(qY, 0), 0).r);
  int bandX = int(texelFetch(bandTexture, glyphLocation + ivec2(indirectionSize + qX, 0), 0).r);

  float xCoverage = 0.0;
  float xWeight = 0.0;
  uvec2 horizontal = texelFetch(bandTexture, glyphLocation + ivec2(2 * indirectionSize + bandY, 0), 0).xy;
  for (uint i = 0u; i < horizontal.x; ++i) {
    // Lists and connected curves are packed continuously, including across rows.
    // Recompute the row for each fetch, not just for the start of the list.
    ivec2 curveLocation = ivec2(texelFetch(bandTexture, bandLocation(glyphLocation, horizontal.y + i), 0).xy);
    vec4 p12 = texelFetch(curveTexture, curveLocation, 0) - vec4(renderCoord, renderCoord);
    vec2 p3 = texelFetch(curveTexture, curveTailLocation(curveLocation), 0).xy - renderCoord;
    if (max(max(p12.x, p12.z), p3.x) * pixelsPerEm.x < -0.5) break;
    uint code = calcRootCode(p12.y, p12.w, p3.y);
    if (code != 0u) {
      vec2 root = solveHorizontal(p12, p3) * pixelsPerEm.x;
      if ((code & 1u) != 0u) {
        xCoverage += clamp(root.x + 0.5, 0.0, 1.0);
        xWeight = max(xWeight, clamp(1.0 - abs(root.x) * 2.0, 0.0, 1.0));
      }
      if (code > 1u) {
        xCoverage -= clamp(root.y + 0.5, 0.0, 1.0);
        xWeight = max(xWeight, clamp(1.0 - abs(root.y) * 2.0, 0.0, 1.0));
      }
    }
  }

  float yCoverage = 0.0;
  float yWeight = 0.0;
  uvec2 vertical = texelFetch(bandTexture,
    glyphLocation + ivec2(2 * indirectionSize + bandMaximum.y + 1 + bandX, 0), 0).xy;
  for (uint i = 0u; i < vertical.x; ++i) {
    ivec2 curveLocation = ivec2(texelFetch(bandTexture, bandLocation(glyphLocation, vertical.y + i), 0).xy);
    vec4 p12 = texelFetch(curveTexture, curveLocation, 0) - vec4(renderCoord, renderCoord);
    vec2 p3 = texelFetch(curveTexture, curveTailLocation(curveLocation), 0).xy - renderCoord;
    if (max(max(p12.y, p12.w), p3.y) * pixelsPerEm.y < -0.5) break;
    uint code = calcRootCode(p12.x, p12.z, p3.x);
    if (code != 0u) {
      vec2 root = solveVertical(p12, p3) * pixelsPerEm.y;
      if ((code & 1u) != 0u) {
        yCoverage -= clamp(root.x + 0.5, 0.0, 1.0);
        yWeight = max(yWeight, clamp(1.0 - abs(root.x) * 2.0, 0.0, 1.0));
      }
      if (code > 1u) {
        yCoverage += clamp(root.y + 0.5, 0.0, 1.0);
        yWeight = max(yWeight, clamp(1.0 - abs(root.y) * 2.0, 0.0, 1.0));
      }
    }
  }
  float weighted = abs(xCoverage * xWeight + yCoverage * yWeight) /
    max(xWeight + yWeight, 1.0 / 65536.0);
  float coverage = max(weighted, min(abs(xCoverage), abs(yCoverage)));
  if (evenOdd)
    return 1.0 - abs(1.0 - fract(coverage * 0.5) * 2.0);
  return clamp(coverage, 0.0, 1.0);
}

uint slugBandWord(uint index, bool persistent) {
  return persistent ? persistentBandWords[index] : dynamicBandWords[index];
}

vec4 slugCurveP12(uint curve, bool persistent) {
  if (!persistent) return dynamicCurveData[curve * 2u];
  uint base = curve * persistentCurveVec2Stride;
  return vec4(persistentCurveData[base], persistentCurveData[base + 1u]);
}

vec2 slugCurveP3(uint curve, bool persistent) {
  return persistent ? persistentCurveData[curve * persistentCurveVec2Stride + 2u]
                    : dynamicCurveData[curve * 2u + 1u].xy;
}

uint slugBandCurve(uint offset, uint entry, bool persistent, bool packedIndices) {
  if (packedAtlasBandMode == 0u) return slugBandWord(offset + entry, persistent);
  uint word = slugBandWord(offset + (packedIndices ? entry / 2u : entry), persistent);
  return packedIndices ? (word >> ((entry & 1u) * 16u)) & 65535u : word;
}

float dynamicSlugCoverage(vec2 renderCoord) {
  vec2 emsPerPixel = max(fwidth(renderCoord), vec2(1.0e-30));
  vec2 pixelsPerEm = 1.0 / emsPerPixel;

  uint curveBase = shapeData.y;
  uint blockBase = shapeData.z;
  uint packed = shapeData.w;
  bool refined = (packed & 0x20000000u) != 0u;
  int querySize = refined ? (32 << int((packed >> 26u) & 7u)) : indirectionSize;
  bool persistent = (packed & 0x80000000u) != 0u;
  bool packedIndices = packedAtlasBandMode != 0u && (packed & 0x02000000u) != 0u;
  uint bandMaxX = packed & 0xFFu;
  uint bandMaxY = refined ? uint(querySize - 1) : (packed >> 8u) & 0xFFu;
  bool evenOdd = (packed & 0x10000u) != 0u;

  int qY = clamp(int(renderCoord.y * bandTransform.y + bandTransform.w),
                 0, querySize - 1);
  int qX = clamp(int(renderCoord.x * bandTransform.x + bandTransform.z),
                 0, querySize - 1);
  uint bandY = slugBandWord(blockBase + uint(qY), persistent);
  uint bandX = slugBandWord(blockBase + uint(querySize + qX), persistent);

  float xCoverage = 0.0;
  float xWeight = 0.0;
  uint hHeader = blockBase + uint(2 * querySize) + bandY * 2u;
  uint hCount = slugBandWord(hHeader, persistent);
  uint hOffset = slugBandWord(hHeader + 1u, persistent);

  for (uint i = 0u; i < hCount; ++i) {
    uint localCurve = slugBandCurve(hOffset, i, persistent, packedIndices);
    uint curveIndex = curveBase + localCurve;
    vec4 p12 = slugCurveP12(curveIndex, persistent) - vec4(renderCoord, renderCoord);
    vec2 p3 = slugCurveP3(curveIndex, persistent) - renderCoord;

    if (max(max(p12.x, p12.z), p3.x) * pixelsPerEm.x < -0.5) break;

    uint code = calcRootCode(p12.y, p12.w, p3.y);
    if (code != 0u) {
      vec2 root = solveHorizontal(p12, p3) * pixelsPerEm.x;
      if ((code & 1u) != 0u) {
        xCoverage += clamp(root.x + 0.5, 0.0, 1.0);
        xWeight = max(xWeight, clamp(1.0 - abs(root.x) * 2.0, 0.0, 1.0));
      }
      if (code > 1u) {
        xCoverage -= clamp(root.y + 0.5, 0.0, 1.0);
        xWeight = max(xWeight, clamp(1.0 - abs(root.y) * 2.0, 0.0, 1.0));
      }
    }
  }

  float yCoverage = 0.0;
  float yWeight = 0.0;
  uint vHeader = blockBase + uint(2 * querySize) +
                 (bandMaxY + 1u) * 2u + bandX * 2u;
  uint vCount = slugBandWord(vHeader, persistent);
  uint vOffset = slugBandWord(vHeader + 1u, persistent);

  for (uint i = 0u; i < vCount; ++i) {
    uint localCurve = slugBandCurve(vOffset, i, persistent, packedIndices);
    uint curveIndex = curveBase + localCurve;
    vec4 p12 = slugCurveP12(curveIndex, persistent) - vec4(renderCoord, renderCoord);
    vec2 p3 = slugCurveP3(curveIndex, persistent) - renderCoord;

    if (max(max(p12.y, p12.w), p3.y) * pixelsPerEm.y < -0.5) break;

    uint code = calcRootCode(p12.x, p12.z, p3.x);
    if (code != 0u) {
      vec2 root = solveVertical(p12, p3) * pixelsPerEm.y;
      if ((code & 1u) != 0u) {
        yCoverage -= clamp(root.x + 0.5, 0.0, 1.0);
        yWeight = max(yWeight, clamp(1.0 - abs(root.x) * 2.0, 0.0, 1.0));
      }
      if (code > 1u) {
        yCoverage += clamp(root.y + 0.5, 0.0, 1.0);
        yWeight = max(yWeight, clamp(1.0 - abs(root.y) * 2.0, 0.0, 1.0));
      }
    }
  }

  float coverage = max(
    abs(xCoverage * xWeight + yCoverage * yWeight) /
      max(xWeight + yWeight, 1.0 / 65536.0),
    min(abs(xCoverage), abs(yCoverage)));

  if (evenOdd)
    return 1.0 - abs(1.0 - fract(coverage * 0.5) * 2.0);
  return clamp(coverage, 0.0, 1.0);
}

vec4 evaluatePaint() {
  int kind = int(paintData.x + 0.5);
  float t = 0.0;
  vec2 origin = gradientData.xy;
  vec2 direction = gradientData.zw - origin;
  if (kind == 1) t = dot(uv - origin, direction) / max(dot(direction, direction), 0.00001);
  else if (kind == 2) {
    vec2 normalized = abs(uv - origin) / max(abs(direction), vec2(0.0001));
    t = normalized.x + normalized.y;
  } else if (kind == 3) t = length(uv - origin) / max(length(direction), 0.0001);
  else if (kind == 4) t = 0.5 + 0.5 * sin((uv.x * 1.7 + uv.y + paintData.z) * 24.0);
  else if (kind == 5) {
    vec2 centered = uv - origin;
    float hue = fract(atan(centered.x, -centered.y) / 6.28318530718 + paintData.z + 1.0);
    vec3 rgb = clamp(abs(mod(hue * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0,
                     0.0, 1.0);
    return vec4(rgb, color0.a);
  }
  return mix(color0, color1, clamp(t, 0.0, 1.0));
}

vec3 linearToSrgb(vec3 value) {
  bvec3 low = lessThanEqual(value, vec3(0.0031308));
  vec3 srgbLow = value * 12.92;
  vec3 srgbHigh = 1.055 * pow(max(value, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
  return mix(srgbHigh, srgbLow, low);
}

vec3 srgbToLinear(vec3 value) {
  bvec3 low = lessThanEqual(value, vec3(0.04045));
  vec3 linearLow = value / 12.92;
  vec3 linearHigh = pow((value + 0.055) / 1.055, vec3(2.4));
  return mix(linearHigh, linearLow, low);
}

void main() {
  vec2 clipPoint=pushConstants.opacity.y>0.5?localPosition:gl_FragCoord.xy;
  if (clipPoint.x < clipRect.x || clipPoint.y < clipRect.y ||
      clipPoint.x >= clipRect.x + clipRect.z || clipPoint.y >= clipRect.y + clipRect.w) discard;
  if(pushConstants.opacity.y>0.5){
    vec4 clip=pushConstants.projectiveClip;
    if(gl_FragCoord.x<clip.x||gl_FragCoord.y<clip.y||gl_FragCoord.x>=clip.x+clip.z||gl_FragCoord.y>=clip.y+clip.w)discard;
  }
  if (shapeData.x == externalImageShape) {
    uvec2 size = max(uvec2(shapeData.yz), uvec2(1u));
    uvec2 coordinate = min(uvec2(uv * vec2(size)), size - 1u);
    uint offset = (coordinate.y * size.x + coordinate.x) * 4u;
    vec4 color = vec4(uintBitsToFloat(externalPixelWords[offset + 1u]),
                      uintBitsToFloat(externalPixelWords[offset + 2u]),
                      uintBitsToFloat(externalPixelWords[offset + 3u]),
                      uintBitsToFloat(externalPixelWords[offset]));
    if (pushConstants.translationOverride.w <= 0.5) color.rgb = linearToSrgb(color.rgb);
    color.a *= paintData.y;
    outColor = color;
    return;
  }
  float coverage;
  if (shapeData.x == dynamicSlugShape)
    coverage = dynamicSlugCoverage(emCoord);
  else
    coverage = slugCoverage(emCoord);
  if (coverage <= 0.001) discard;
  vec4 color = evaluatePaint();
  if (pushConstants.translationOverride.w > 0.5)
    color.rgb = srgbToLinear(clamp(color.rgb, 0.0, 1.0));
  color.a *= coverage * paintData.y;
  outColor = color;
}
