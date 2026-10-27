#version 450

layout(location = 0) in vec4 inPositionRect;
layout(location = 1) in vec4 inEmRect;
layout(location = 2) in vec4 inBandTransform;
layout(location = 3) in uvec4 inShapeData;
layout(location = 4) in vec4 inColor0;
layout(location = 5) in vec4 inColor1;
layout(location = 6) in vec4 inPaint;
layout(location = 7) in vec4 inGradient;
layout(location = 8) in vec4 inClip;
layout(location = 9) in vec4 inStrokeWidths;

layout(push_constant) uniform PushConstants {
  vec4 viewportScale;
  vec4 translationOverride;
  vec4 overrideClip;
  vec4 opacity;
  vec4 projectiveRows[3];
  vec4 projectiveClip;
} pushConstants;

layout(location = 0) out vec2 emCoord;
layout(location = 1) out vec2 uv;
layout(location = 2) flat out vec4 bandTransform;
layout(location = 3) flat out uvec4 shapeData;
layout(location = 4) flat out vec4 color0;
layout(location = 5) flat out vec4 color1;
layout(location = 6) flat out vec4 paintData;
layout(location = 7) flat out vec4 gradientData;
layout(location = 8) flat out vec4 clipRect;
layout(location = 9) flat out vec4 strokeWidths;
layout(location = 10) out vec2 localPosition;

void main() {
  const uint dynamicSlugShape = 0xFFFFFFF8u;
  const uint externalImageShape = 0xFFFFFFFCu;
  const vec2 corners[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
  vec2 corner = corners[gl_VertexIndex];
  vec2 authoredPosition = mix(inPositionRect.xy, inPositionRect.zw, corner);
  vec2 authoredEm = mix(inEmRect.xy, inEmRect.zw, corner);
  bool slugShape = inShapeData.x <= dynamicSlugShape;

  if(pushConstants.opacity.y>0.5){
    mat3 h=transpose(mat3(pushConstants.projectiveRows[0].xyz,
                         pushConstants.projectiveRows[1].xyz,
                         pushConstants.projectiveRows[2].xyz));
    vec2 scale=pushConstants.viewportScale.zw;
    vec2 translation=pushConstants.translationOverride.xy;
    if(slugShape)authoredPosition.x+=inPaint.w*(1.0-corner.y);
    vec2 local=authoredPosition*scale+translation;
    vec3 hp=h*vec3(local,1.0);
    emCoord=authoredEm;
    if(slugShape&&hp.z>1.0e-7&&abs(determinant(h))>1.0e-12){
      vec2 cx=vec2(1.0-corner.x,corner.y),cy=vec2(corner.x,1.0-corner.y);
      vec2 px=mix(inPositionRect.xy,inPositionRect.zw,cx);
      vec2 py=mix(inPositionRect.xy,inPositionRect.zw,cy);
      px.x+=inPaint.w*(1.0-cx.y);py.x+=inPaint.w*(1.0-cy.y);
      vec3 hx=h*vec3(px*scale+translation,1.0),hy=h*vec3(py*scale+translation,1.0);
      vec2 center=(inPositionRect.xy+inPositionRect.zw)*0.5;
      center.x+=inPaint.w*0.5;
      vec3 hc=h*vec3(center*scale+translation,1.0);
      if(hx.z>1.0e-7&&hy.z>1.0e-7&&hc.z>1.0e-7){
        vec2 screen=hp.xy/hp.z,inside=hc.xy/hc.z-screen;
        vec2 ex=hx.xy/hx.z-screen,ey=hy.xy/hy.z-screen;
        if(length(ex)>1.0e-8&&length(ey)>1.0e-8){
          vec2 nx=normalize(vec2(-ex.y,ex.x)),ny=normalize(vec2(-ey.y,ey.x));
          if(dot(nx,inside)>0.0)nx=-nx;if(dot(ny,inside)>0.0)ny=-ny;
          float denominator=1.0+dot(nx,ny);
          if(denominator>1.0e-6){
            vec2 expanded=screen+0.5*(nx+ny)/denominator;
            vec3 inversePoint=inverse(h)*vec3(expanded,1.0);
            vec2 expandedLocal=inversePoint.xy/inversePoint.z;
            vec2 delta=(expandedLocal-local)/scale;
            vec2 span=inPositionRect.zw-inPositionRect.xy;
            vec2 emSpan=inEmRect.zw-inEmRect.xy;
            float dy=delta.y/max(abs(span.y),1.0e-8);
            emCoord+=vec2((delta.x+inPaint.w*dy)/max(abs(span.x),1.0e-8),dy)*emSpan;
            local=expandedLocal;hp=h*vec3(local,1.0);
          }
        }
      }
    }
    localPosition=local;
    vec3 depth=vec3(pushConstants.projectiveRows[0].w,pushConstants.projectiveRows[1].w,pushConstants.projectiveRows[2].w);
    gl_Position=vec4(2.0*hp.x/pushConstants.viewportScale.x-hp.z,
                     2.0*hp.y/pushConstants.viewportScale.y-hp.z,dot(depth,vec3(local,1.0)),hp.z);
  }else{

  // Slug's current reference implementation dynamically dilates every bounding polygon by
  // exactly half a viewport pixel. For our orthographic UI transform, the optimal displacement
  // reduces to +/-0.5 px on each adjacent edge. Move the em-space sample point through the
  // inverse affine Jacobian by the same amount so the outline itself does not grow.
  if (slugShape) {
    authoredPosition.x += inPaint.w * (1.0 - corner.y);

    vec2 retainedScale = pushConstants.viewportScale.zw;
    vec2 safeScale = vec2(
      abs(retainedScale.x) > 1.0e-8 ? retainedScale.x : 1.0,
      abs(retainedScale.y) > 1.0e-8 ? retainedScale.y : 1.0);
    vec2 outward = (corner * 2.0 - 1.0) * 0.5;

    vec2 emSpan = inEmRect.zw - inEmRect.xy;
    vec2 positionSpan = inPositionRect.zw - inPositionRect.xy;
    float emDy = outward.y * emSpan.y /
      max(abs(positionSpan.y * safeScale.y), 1.0e-8);
    // Artificial italic shear couples em-y motion into screen x. Remove that contribution before
    // mapping the remaining horizontal displacement back to em x.
    float shearScreenFromEmY = 0.0;
    if (abs(emSpan.y) > 1.0e-8)
      shearScreenFromEmY = safeScale.x * (-inPaint.w / emSpan.y) * emDy;
    float emDx = (outward.x - shearScreenFromEmY) * emSpan.x /
      max(abs(positionSpan.x * safeScale.x), 1.0e-8);

    emCoord = authoredEm + vec2(emDx, emDy);
    authoredPosition = authoredPosition * retainedScale +
      pushConstants.translationOverride.xy + outward;
  } else {
    emCoord = authoredEm;
    authoredPosition = authoredPosition * pushConstants.viewportScale.zw +
      pushConstants.translationOverride.xy;
  }

  vec2 ndc = vec2(
    authoredPosition.x * 2.0 / pushConstants.viewportScale.x - 1.0,
    authoredPosition.y * 2.0 / pushConstants.viewportScale.y - 1.0
  );
  gl_Position = vec4(ndc, 0.0, 1.0);
  localPosition=authoredPosition;
  }
  uv = (inShapeData.w & 0x40000000u) != 0u
    ? mix(inStrokeWidths.xy, inStrokeWidths.zw, corner) : corner;
  bandTransform = inBandTransform;
  shapeData = inShapeData;
  color0 = inColor0;
  color1 = inColor1;
  paintData = inPaint;
  paintData.y *= pushConstants.opacity.x;
  gradientData = inGradient;
  clipRect = pushConstants.translationOverride.z > 0.5 ? pushConstants.overrideClip : inClip;
  strokeWidths = inStrokeWidths;
}
