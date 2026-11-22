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
layout(location = 11) flat in uint roundedClipRef;

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

vec3 slugCoverageWithFootprint(vec2 renderCoord,vec2 emsPerPixel,uvec4 sourceShape,vec4 sourceBand) {
  // Explicit footprint permits adaptive subpixel integration without relying
  // on derivatives evaluated inside non-uniform branches.
  emsPerPixel=max(abs(emsPerPixel),vec2(1.0e-30));
  vec2 pixelsPerEm = 1.0 / emsPerPixel;
  ivec2 glyphLocation = ivec2(sourceShape.xy);
  ivec2 bandMaximum = ivec2(sourceShape.zw);
  bool evenOdd = (sourceShape.w & 0x1000u) != 0u;
  bandMaximum.y &= 0xFF;

  int qY = clamp(int(renderCoord.y * sourceBand.y + sourceBand.w), 0, indirectionSize - 1);
  int qX = clamp(int(renderCoord.x * sourceBand.x + sourceBand.z), 0, indirectionSize - 1);
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
  float value=evenOdd
    ? 1.0 - abs(1.0 - fract(coverage * 0.5) * 2.0)
    : clamp(coverage, 0.0, 1.0);
  return vec3(value,xWeight,yWeight);
}

// Slug's two-axis analytical estimate is extremely efficient for ordinary
// edges, but the 4-way checker corners of an even-odd compound Figma path
// can produce small coverage outliers. Re-evaluate ONLY fractional boundary
// pixels using four half-size analytical footprints. Interior/empty pixels
// retain the original single solve; extra GPU work is local to the contour.
// Derivatives are computed once before any divergent branch.
float slugCoverageHighQualityAt(vec2 renderCoord,uvec4 sourceShape,vec4 sourceBand) {
  // Fill-rule bits are flat per primitive. Avoid introducing oriented-pixel
  // derivative calculations for the overwhelmingly common non-even-odd paths.
  // This uniform path preserves their original single-solve shader workload.
  bool evenOdd=(sourceShape.w & 0x1000u)!=0u;
  if(!evenOdd) {
    vec2 footprint=max(abs(fwidth(renderCoord)),vec2(1.0e-30));
    return slugCoverageWithFootprint(renderCoord,footprint,
                                     sourceShape,sourceBand).x;
  }
  // The two screen-pixel axes are generally NOT aligned with Slug em-space
  // after rotation, shear, reflection or perspective projection. fwidth()
  // provides an axis-aligned *extent*, not valid subpixel sample positions.
  // Compute both derivatives before any divergent branch, and place samples
  // at the true quarter-pixel centers of the local affine footprint.
  vec2 dx=dFdx(renderCoord);
  vec2 dy=dFdy(renderCoord);
  vec2 wholePixel=max(abs(dx)+abs(dy),vec2(1.0e-30));
  vec3 centerResult=slugCoverageWithFootprint(renderCoord,wholePixel,sourceShape,sourceBand);
  float center=centerResult.x;
  // Non-axis-crossing AA already has correct 1-D Slug coverage.
  if(center<=0.002 || center>=0.998 ||
     centerResult.y<=0.001 || centerResult.z<=0.001)return center;
  vec2 halfPixel=wholePixel*0.5;
  vec2 quarterX=dx*0.25;
  vec2 quarterY=dy*0.25;
  float sum=0.0;
  sum+=slugCoverageWithFootprint(renderCoord-quarterX-quarterY,halfPixel,sourceShape,sourceBand).x;
  sum+=slugCoverageWithFootprint(renderCoord+quarterX-quarterY,halfPixel,sourceShape,sourceBand).x;
  sum+=slugCoverageWithFootprint(renderCoord-quarterX+quarterY,halfPixel,sourceShape,sourceBand).x;
  sum+=slugCoverageWithFootprint(renderCoord+quarterX+quarterY,halfPixel,sourceShape,sourceBand).x;
  return clamp(sum*0.25,0.0,1.0);
}

float slugCoverage(vec2 renderCoord) {
  return slugCoverageHighQualityAt(renderCoord,shapeData,bandTransform);
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

vec3 dynamicSlugCoverageWithFootprint(
    vec2 renderCoord,uvec4 shape,vec4 band,vec2 emsPerPixel) {
  emsPerPixel=max(abs(emsPerPixel),vec2(1.0e-30));
  vec2 pixelsPerEm = 1.0 / emsPerPixel;

  uint curveBase = shape.y;
  uint blockBase = shape.z;
  uint packed = shape.w;
  bool refined = (packed & 0x20000000u) != 0u;
  int querySize = refined ? (32 << int((packed >> 26u) & 7u)) : indirectionSize;
  bool persistent = (packed & 0x80000000u) != 0u;
  bool packedIndices = packedAtlasBandMode != 0u && (packed & 0x02000000u) != 0u;
  uint bandMaxX = packed & 0xFFu;
  uint bandMaxY = refined ? uint(querySize - 1) : (packed >> 8u) & 0xFFu;
  bool evenOdd = (packed & 0x10000u) != 0u;

  int qY = clamp(int(renderCoord.y * band.y + band.w),
                 0, querySize - 1);
  int qX = clamp(int(renderCoord.x * band.x + band.z),
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

  float value=evenOdd
    ? 1.0 - abs(1.0 - fract(coverage * 0.5) * 2.0)
    : clamp(coverage,0.0,1.0);
  return vec3(value,xWeight,yWeight);
}

float dynamicSlugCoverageAt(vec2 coord,uvec4 shape,vec4 band) {
  return dynamicSlugCoverageWithFootprint(
    coord,shape,band,max(abs(fwidth(coord)),vec2(1.0e-30))).x;
}

float dynamicSlugCoverageHighQualityAt(vec2 coord,uvec4 shape,vec4 band) {
  bool evenOdd=(shape.w&0x10000u)!=0u;
  if(!evenOdd) {
    vec2 footprint=max(abs(fwidth(coord)),vec2(1.0e-30));
    return dynamicSlugCoverageWithFootprint(coord,shape,band,footprint).x;
  }
  // Match atlas Slug's oriented screen-space sample locations. This matters
  // for even-odd dynamic contours inside projected/rotated DrawLists too.
  vec2 dx=dFdx(coord);
  vec2 dy=dFdy(coord);
  vec2 span=max(abs(dx)+abs(dy),vec2(1.0e-30));
  vec3 centerResult=dynamicSlugCoverageWithFootprint(coord,shape,band,span);
  float center=centerResult.x;
  if(center<=0.002||center>=0.998||
     centerResult.y<=0.001||centerResult.z<=0.001)return center;
  vec2 halfFootprint=span*0.5;
  vec2 quarterX=dx*0.25;
  vec2 quarterY=dy*0.25;
  float sum=0.0;
  sum+=dynamicSlugCoverageWithFootprint(coord-quarterX-quarterY,shape,band,halfFootprint).x;
  sum+=dynamicSlugCoverageWithFootprint(coord+quarterX-quarterY,shape,band,halfFootprint).x;
  sum+=dynamicSlugCoverageWithFootprint(coord-quarterX+quarterY,shape,band,halfFootprint).x;
  sum+=dynamicSlugCoverageWithFootprint(coord+quarterX+quarterY,shape,band,halfFootprint).x;
  return clamp(sum*0.25,0.0,1.0);
}

float dynamicSlugCoverage(vec2 renderCoord) {
  return dynamicSlugCoverageHighQualityAt(renderCoord,shapeData,bandTransform);
}

vec4 evaluatePaintAt(vec2 paintUv) {
  int kind = int(paintData.x + 0.5);
  float t = 0.0;
  vec2 origin = gradientData.xy;
  vec2 direction = gradientData.zw - origin;
  if (kind == 1 || kind == 6) t = dot(paintUv - origin, direction) / max(dot(direction, direction), 0.00001);
  else if (kind == 2) {
    vec2 normalized = abs(paintUv - origin) / max(abs(direction), vec2(0.0001));
    t = normalized.x + normalized.y;
  } else if (kind == 3) t = length(paintUv - origin) / max(length(direction), 0.0001);
  else if (kind == 4) t = 0.5 + 0.5 * sin((paintUv.x * 1.7 + paintUv.y + paintData.z) * 24.0);
  else if (kind == 5) {
    vec2 centered = paintUv - origin;
    float hue = fract(atan(centered.x, -centered.y) / 6.28318530718 + paintData.z + 1.0);
    vec3 rgb = clamp(abs(mod(hue * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0,
                     0.0, 1.0);
    return vec4(rgb, color0.a);
  }
  if (kind >= 7 && kind <= 9) {
    // Figma's matrix maps normalized object UV to gradient space.
    // The angular 0-position is at the top, increasing clockwise.
    uint base = uint(paintData.z + 0.5);
    vec3 xRow = vec3(
      uintBitsToFloat(dynamicBandWords[base + 1u]),
      uintBitsToFloat(dynamicBandWords[base + 2u]),
      uintBitsToFloat(dynamicBandWords[base + 3u]));
    vec3 yRow = vec3(
      uintBitsToFloat(dynamicBandWords[base + 4u]),
      uintBitsToFloat(dynamicBandWords[base + 5u]),
      uintBitsToFloat(dynamicBandWords[base + 6u]));
    vec2 g = vec2(dot(xRow, vec3(paintUv, 1.0)),
                  dot(yRow, vec3(paintUv, 1.0))) - vec2(0.5);
    if (kind == 7)
      t = fract(atan(g.x, -g.y) / 6.28318530718 + 1.0);
    else if (kind == 8)
      t = length(g) * 2.0;
    else
      t = (abs(g.x) + abs(g.y)) * 2.0;
  }
  if (kind >= 6 && kind <= 9) {
    // Stop tables are appended to the dynamic Slug band SSBO. Binary search
    // gives O(log N) reads per pixel even for 256-stop ramps. This path is
    // used ONLY for authored multi-stop paints, never common solid/2-stop UI.
    uint base = uint(paintData.z + 0.5);
    uint count = clamp(dynamicBandWords[base], 2u, 256u);
    uint head = base + 1u + (kind == 6 ? 0u : 6u);
    uint low = 0u, high = count - 1u;
    float firstPosition = uintBitsToFloat(dynamicBandWords[head]);
    float lastPosition = uintBitsToFloat(dynamicBandWords[head + high * 5u]);
    if (t <= firstPosition) high = low;
    else if (t >= lastPosition) low = high;
    else {
      for (int iteration = 0; iteration < 8 && high - low > 1u; ++iteration) {
        uint middle = (low + high) >> 1u;
        float position = uintBitsToFloat(dynamicBandWords[head + middle * 5u]);
        if (t < position) high = middle;
        else low = middle;
      }
    }
    uint left = head + 1u + low * 5u;
    uint right = head + 1u + high * 5u;
    vec4 from = vec4(uintBitsToFloat(dynamicBandWords[left]),
                     uintBitsToFloat(dynamicBandWords[left + 1u]),
                     uintBitsToFloat(dynamicBandWords[left + 2u]),
                     uintBitsToFloat(dynamicBandWords[left + 3u]));
    if (low == high) return from;
    vec4 to = vec4(uintBitsToFloat(dynamicBandWords[right]),
                   uintBitsToFloat(dynamicBandWords[right + 1u]),
                   uintBitsToFloat(dynamicBandWords[right + 2u]),
                   uintBitsToFloat(dynamicBandWords[right + 3u]));
    float l = uintBitsToFloat(dynamicBandWords[left - 1u]);
    float r = uintBitsToFloat(dynamicBandWords[right - 1u]);
    return mix(from, to, clamp((t - l) / max(r - l, 1.0e-10), 0.0, 1.0));
  }
  return mix(color0, color1, clamp(t, 0.0, 1.0));
}

vec4 evaluatePaint() { return evaluatePaintAt(uv); }

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

// A rounded clip table is shared by every descendant, not copied to each
// instance. Rectangular clipping remains in the preexisting cheap clipRect test.
// Each 28-word mask carries exact Slug curve/band references, the inverse
// world affine and its explicit coordinate-space tag. Rotated children do not
// rotate an inherited parent's Clip contents contour.
float roundedClipAlpha() {
  if(roundedClipRef==0u)return 1.0;
  uint base=roundedClipRef-1u;
  uint countWord=dynamicBandWords[base];
  // The CPU intersects compatible rectangular Clip contents geometrically.
  // A disjoint pair cannot be represented by zero-area Slug curves: reject
  // it directly, without reading masks or introducing a translucent fringe.
  if(countWord==0xffffffffu)return 0.0;
  uint count=min(countWord,64u);
  // For a projected child, inherited masks remain anchored to their world
  // coordinates; locally projected masks have an explicit descriptor tag.
  // For ordinary 2D quads the interpolated coordinate follows the same
  // rounding path as the painted contour. For PROJECTIVE quads, however,
  // localPosition is pre-projection; a parent's inherited world mask must
  // continue to use the actual screen pixel coordinate.
  vec2 screenPoint=pushConstants.opacity.y>0.5
    ? gl_FragCoord.xy : localPosition;
  vec2 screenWorld=(screenPoint-pushConstants.translationOverride.xy)/
                      pushConstants.viewportScale.zw;
  float coverage=1.0;
  for(uint i=0u;i<count;++i){
    uint at=base+1u+i*28u;
    // The Frame's mask is in screen/world space even when its child is
    // counterrotated. Only an explicitly local projected DrawList mask
    // should follow the child content's projective transform.
    bool localMask=dynamicBandWords[at+27u]!=0u;
    vec2 world=(localMask&&pushConstants.opacity.y>0.5)
      ? localPosition : screenWorld;
    vec3 inverseX=vec3(
      uintBitsToFloat(dynamicBandWords[at+12u]),
      uintBitsToFloat(dynamicBandWords[at+13u]),
      uintBitsToFloat(dynamicBandWords[at+14u]));
    vec3 inverseY=vec3(
      uintBitsToFloat(dynamicBandWords[at+15u]),
      uintBitsToFloat(dynamicBandWords[at+16u]),
      uintBitsToFloat(dynamicBandWords[at+17u]));
    vec2 p=vec2(dot(inverseX,vec3(world,1.0)),
                dot(inverseY,vec3(world,1.0)));
    // Do not introduce AABB or interior shortcuts here: the fragment quad's
    // derivative/root-coverage boundary behavior must match its painted
    // Slug path exactly, even for .375px offsets and projected curves.
    uvec4 shape=uvec4(0xfffffff8u,
      dynamicBandWords[at+18u],dynamicBandWords[at+19u],
      dynamicBandWords[at+20u]);
    vec4 band=vec4(
      uintBitsToFloat(dynamicBandWords[at+21u]),
      uintBitsToFloat(dynamicBandWords[at+22u]),
      uintBitsToFloat(dynamicBandWords[at+23u]),
      uintBitsToFloat(dynamicBandWords[at+24u]));
    vec2 pathOrigin=vec2(
      uintBitsToFloat(dynamicBandWords[at+25u]),
      uintBitsToFloat(dynamicBandWords[at+26u]));
    // Both the frame fill and its mask are evaluated by the same quadratic
    // solver and use the same fractional coordinate system.
    float mask=dynamicSlugCoverageAt(p-pathOrigin,shape,band);
    // Identical nested masks must NOT square their edge alpha (A*A).
    // For nested/coincident contours, the minimum is the shared silhouette.
    coverage=min(coverage,mask);
    if(coverage<=0.001)return 0.0;
  }
  return coverage;
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
    color.a *= paintData.y * roundedClipAlpha();
    outColor = color;
    return;
  }
  float coverage;
  if (shapeData.x == dynamicSlugShape)
    coverage = dynamicSlugCoverage(emCoord);
  else
    coverage = slugCoverage(emCoord);
  if (coverage <= 0.001) discard;
  // A Figma UI border is geometrically partitioned into INSIDE FILL,
  // INSIDE STROKE and OUTSIDE STROKE. Do NOT compose two independently
  // anti-aliased Slug paths: F + R*(1-F) overfills the silhouette by up to
  // 64/255 at a fractional inside edge. Evaluate all three signed coverages
  // at the SAME fragment and emit one straight-alpha source-over sample.
  if (shapeData.x == dynamicSlugShape &&
      (shapeData.w & 0x00800000u) != 0u) {
    uint offset = uint(strokeWidths.x + 0.5);
    vec4 borderColor = vec4(
      uintBitsToFloat(dynamicBandWords[offset + 0u]),
      uintBitsToFloat(dynamicBandWords[offset + 1u]),
      uintBitsToFloat(dynamicBandWords[offset + 2u]),
      uintBitsToFloat(dynamicBandWords[offset + 3u]));
    float borderOpacity = uintBitsToFloat(dynamicBandWords[offset + 4u]);
    float fillOpacity = uintBitsToFloat(dynamicBandWords[offset + 5u]);
    uvec4 baseShape = uvec4(dynamicSlugShape,
      dynamicBandWords[offset + 6u],dynamicBandWords[offset + 7u],
      dynamicBandWords[offset + 8u]);
    vec4 baseBand = vec4(
      uintBitsToFloat(dynamicBandWords[offset + 9u]),
      uintBitsToFloat(dynamicBandWords[offset + 10u]),
      uintBitsToFloat(dynamicBandWords[offset + 11u]),
      uintBitsToFloat(dynamicBandWords[offset + 12u]));
    vec2 baseOffset = vec2(
      uintBitsToFloat(dynamicBandWords[offset + 13u]),
      uintBitsToFloat(dynamicBandWords[offset + 14u]));
    float baseCover = clamp(dynamicSlugCoverageAt(emCoord + baseOffset,
                                                   baseShape, baseBand),
                            0.0, coverage);
    float innerCover = 0.0;
    if (dynamicBandWords[offset + 17u] != 0xffffffffu) {
      uvec4 innerShape = uvec4(dynamicSlugShape,
        dynamicBandWords[offset + 15u],dynamicBandWords[offset + 16u],
        dynamicBandWords[offset + 17u]);
      vec4 innerBand = vec4(
        uintBitsToFloat(dynamicBandWords[offset + 18u]),
        uintBitsToFloat(dynamicBandWords[offset + 19u]),
        uintBitsToFloat(dynamicBandWords[offset + 20u]),
        uintBitsToFloat(dynamicBandWords[offset + 21u]));
      vec2 innerOffset = vec2(
        uintBitsToFloat(dynamicBandWords[offset + 22u]),
        uintBitsToFloat(dynamicBandWords[offset + 23u]));
      innerCover = clamp(dynamicSlugCoverageAt(emCoord + innerOffset,
                                                 innerShape, innerBand),
                         0.0, baseCover);
    }
    vec2 fillUvScale = vec2(
      uintBitsToFloat(dynamicBandWords[offset + 26u]),
      uintBitsToFloat(dynamicBandWords[offset + 27u]));
    vec2 fillUvOffset = vec2(
      uintBitsToFloat(dynamicBandWords[offset + 24u]),
      uintBitsToFloat(dynamicBandWords[offset + 25u]));
    vec2 authoredUv = emCoord * fillUvScale + fillUvOffset;
    vec4 fillColor = evaluatePaintAt(authoredUv);
    if ((shapeData.w & 0x00400000u) != 0u) {
      // The first 28 descriptor words describe stroke and Slug contours.
      // Remaining words encode the authored Figma paint order: solid/linear
      // layers are composited PRE coverage in one pixel. Reapplying the same
      // fractional shape mask on each layer causes over-opaque AA edges.
      uint count=min(dynamicBandWords[offset+28u],64u);
      bool maskedStack=(shapeData.w&0x00200000u)!=0u;
      uint stride=maskedStack?30u:14u;
      vec4 stack=vec4(0.0);
      for(uint layer=0u;layer<count;++layer) {
        uint at=offset+29u+layer*stride;
        uint kind=dynamicBandWords[at];
        vec4 start=vec4(
          uintBitsToFloat(dynamicBandWords[at+1u]),
          uintBitsToFloat(dynamicBandWords[at+2u]),
          uintBitsToFloat(dynamicBandWords[at+3u]),
          uintBitsToFloat(dynamicBandWords[at+4u]));
        vec4 end=vec4(
          uintBitsToFloat(dynamicBandWords[at+5u]),
          uintBitsToFloat(dynamicBandWords[at+6u]),
          uintBitsToFloat(dynamicBandWords[at+7u]),
          uintBitsToFloat(dynamicBandWords[at+8u]));
        vec2 origin=vec2(
          uintBitsToFloat(dynamicBandWords[at+9u]),
          uintBitsToFloat(dynamicBandWords[at+10u]));
        vec2 target=vec2(
          uintBitsToFloat(dynamicBandWords[at+11u]),
          uintBitsToFloat(dynamicBandWords[at+12u]));
        float opacity=uintBitsToFloat(dynamicBandWords[at+13u]);
        float shapeCoverage=1.0;
        vec2 layerUv=authoredUv;
        if(maskedStack) {
          uvec4 layerShape=uvec4(dynamicBandWords[at+14u],
            dynamicBandWords[at+15u],dynamicBandWords[at+16u],
            dynamicBandWords[at+17u]);
          if(layerShape.x!=0xffffffffu) {
            vec4 layerBand=vec4(
              uintBitsToFloat(dynamicBandWords[at+18u]),
              uintBitsToFloat(dynamicBandWords[at+19u]),
              uintBitsToFloat(dynamicBandWords[at+20u]),
              uintBitsToFloat(dynamicBandWords[at+21u]));
            vec4 position=vec4(
              uintBitsToFloat(dynamicBandWords[at+22u]),
              uintBitsToFloat(dynamicBandWords[at+23u]),
              uintBitsToFloat(dynamicBandWords[at+24u]),
              uintBitsToFloat(dynamicBandWords[at+25u]));
            vec4 emBounds=vec4(
              uintBitsToFloat(dynamicBandWords[at+26u]),
              uintBitsToFloat(dynamicBandWords[at+27u]),
              uintBitsToFloat(dynamicBandWords[at+28u]),
              uintBitsToFloat(dynamicBandWords[at+29u]));
            // The masked child geometry is stored in authored DrawList
            // coordinates. An orthographic retained view has already
            // scaled and translated localPosition into framebuffer pixels.
            // Undo that view transform BEFORE evaluating the child's mask
            // and its own gradient UVs; otherwise zoom/pan samples the
            // wrong portion (often no checker geometry at all).
            // Projected DrawLists expose their preprojection local frame.
            vec2 authoredPoint=pushConstants.opacity.y>0.5
              ? localPosition
              : (localPosition-pushConstants.translationOverride.xy)/
                max(pushConstants.viewportScale.zw,vec2(1.0e-8));
            layerUv=(authoredPoint-position.xy)/
              max(position.zw-position.xy,vec2(1.0e-8));
            vec2 layerEm=mix(emBounds.xy,emBounds.zw,layerUv);
            shapeCoverage=layerShape.x==dynamicSlugShape
              ? dynamicSlugCoverageHighQualityAt(layerEm,layerShape,layerBand)
              : slugCoverageHighQualityAt(layerEm,layerShape,layerBand);
          }
        }
        float t=0.0;
        if(kind==1u) {
          vec2 direction=target-origin;
          t=dot(layerUv-origin,direction)/
              max(dot(direction,direction),1.0e-5);
        }
        vec4 sampleColor=mix(start,end,clamp(t,0.0,1.0));
        sampleColor.a=clamp(sampleColor.a*opacity*shapeCoverage,0.0,1.0);
        stack.rgb=sampleColor.rgb*sampleColor.a +
                  stack.rgb*(1.0-sampleColor.a);
        stack.a=sampleColor.a+stack.a*(1.0-sampleColor.a);
      }
      fillColor=vec4(stack.rgb/max(stack.a,1.0e-8),stack.a);
      fillOpacity=1.0;
    }
    if (pushConstants.translationOverride.w > 0.5) {
      fillColor.rgb = srgbToLinear(clamp(fillColor.rgb,0.0,1.0));
      borderColor.rgb = srgbToLinear(clamp(borderColor.rgb,0.0,1.0));
    }
    float asrc = clamp(borderColor.a * borderOpacity,0.0,1.0);
    float afill = clamp(fillColor.a * fillOpacity,0.0,1.0);
    float strokeWeight = asrc * max(coverage-innerCover,0.0);
    float fillWeight = afill * max(baseCover -
                          asrc*max(baseCover-innerCover,0.0),0.0);
    float alpha = strokeWeight+fillWeight;
    if (alpha <= 0.001) discard;
    vec3 premul = fillColor.rgb*fillWeight +
                  borderColor.rgb*strokeWeight;
    outColor = vec4(premul / max(alpha,1.0e-8),
                    min(alpha,1.0) * paintData.y * roundedClipAlpha());
    return;
  }
  vec4 color = evaluatePaint();
  if (pushConstants.translationOverride.w > 0.5)
    color.rgb = srgbToLinear(clamp(color.rgb, 0.0, 1.0));
  color.a *= coverage * paintData.y * roundedClipAlpha();
  outColor = color;
}
