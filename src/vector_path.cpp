#include "slugia/vector_path.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace slugia {
namespace {

void requireFinite(float value) {
  if (!std::isfinite(value))
    throw std::invalid_argument("Vector geometry values must be finite");
}
void requireFinite(Vec2 p) { requireFinite(p.x); requireFinite(p.y); }
void requireFinite(Rect r) {
  requireFinite(r.x); requireFinite(r.y); requireFinite(r.width); requireFinite(r.height);
  requireFinite(r.x + r.width); requireFinite(r.y + r.height);
}
void appendBounded(std::vector<QuadraticBezier2D>& out, QuadraticBezier2D curve) {
  requireFinite(curve.p0); requireFinite(curve.p1); requireFinite(curve.p2);
  if (out.size() >= dynamicCurveSegmentLimit)
    throw std::length_error("Adaptive vector geometry exceeds its segment budget");
  out.push_back(curve);
}

void cubicRecursive(std::vector<QuadraticBezier2D>& out,
                    Vec2 p0,Vec2 p1,Vec2 p2,Vec2 p3,
                    float toleranceSquared,int depth) {
  // Degree-reduce the cubic to one quadratic, then degree-elevate that quadratic
  // back to cubic form. The Bezier convex-hull property makes the largest control
  // point delta a strict upper bound on the curve-to-curve position error.
  const Vec2 q1=(p1*3.0f-p0+p2*3.0f-p3)*0.25f;
  requireFinite(q1);
  const Vec2 elevated1=(p0+q1*2.0f)*(1.0f/3.0f);
  const Vec2 elevated2=(q1*2.0f+p3)*(1.0f/3.0f);
  const Vec2 e1=p1-elevated1,e2=p2-elevated2;
  const float errorSquared=std::max(
    e1.x*e1.x+e1.y*e1.y,e2.x*e2.x+e2.y*e2.y);
  if(errorSquared<=toleranceSquared || depth>=20) {
    appendBounded(out,{p0,q1,p3});
    return;
  }
  const Vec2 p01=(p0+p1)*0.5f,p12=(p1+p2)*0.5f,p23=(p2+p3)*0.5f;
  const Vec2 p012=(p01+p12)*0.5f,p123=(p12+p23)*0.5f;
  const Vec2 split=(p012+p123)*0.5f;
  cubicRecursive(out,p0,p01,p012,split,toleranceSquared,depth+1);
  cubicRecursive(out,split,p123,p23,p3,toleranceSquared,depth+1);
}

template<class F>
void parametricRecursive(std::vector<QuadraticBezier2D>& out,const F& f,
                         float t0,float t1,float toleranceSquared,int depth) {
  const float tm=0.5f*(t0+t1);
  const Vec2 p0=f(t0),pm=f(tm),p2=f(t1);
  requireFinite(p0); requireFinite(pm); requireFinite(p2);
  const Vec2 p1=pm*2.0f-(p0+p2)*0.5f;
  requireFinite(p1);
  const auto eval=[&](float u){
    const float v=1.0f-u;
    return p0*(v*v)+p1*(2.0f*u*v)+p2*(u*u);
  };
  constexpr float probes[] = {0.125f,0.25f,0.375f,0.625f,0.75f,0.875f};
  float error=0.0f;
  for(float u:probes) {
    const Vec2 actual=f(t0+(t1-t0)*u);
    const Vec2 delta=actual-eval(u);
    error=std::max(error,delta.x*delta.x+delta.y*delta.y);
  }
  if(error<=toleranceSquared||depth>=18) {
    appendBounded(out,{p0,p1,p2});
    return;
  }
  parametricRecursive(out,f,t0,tm,toleranceSquared,depth+1);
  parametricRecursive(out,f,tm,t1,toleranceSquared,depth+1);
}

template<class F>
void appendParametric(std::vector<QuadraticBezier2D>& out,const F& f,
                      float t0,float t1,float tolerance) {
  const float tol=std::max(tolerance,1.0e-6f);
  requireFinite(t0); requireFinite(t1); requireFinite(tolerance);
  parametricRecursive(out,f,t0,t1,tol*tol,0);
}

Vec2 normalized(Vec2 v) {
  const float l=std::hypot(v.x,v.y);
  return l>1.0e-10f ? v*(1.0f/l) : Vec2{1.0f,0.0f};
}

template<class Position,class Tangent>
std::vector<QuadraticBezier2D> makeOpenStroke(const Position& position,
                                               const Tangent& tangent,
                                               float t0,float t1,float width,
                                               LineCap cap,float tolerance) {
  std::vector<QuadraticBezier2D> out;
  if(width<=0.0f) return out;
  const float half=0.5f*width;
  const auto side=[&](float sign){
    return [&,sign](float t){
      const Vec2 d=normalized(tangent(t));
      const Vec2 n{-d.y,d.x};
      return position(t)+n*(sign*half);
    };
  };
  const auto outer=side(1.0f),inner=side(-1.0f);
  appendParametric(out,outer,t0,t1,tolerance);

  const Vec2 endT=normalized(tangent(t1));
  const Vec2 endN{-endT.y,endT.x};
  const Vec2 outerEnd=outer(t1),innerEnd=inner(t1);
  if(cap==LineCap::Round) {
    const float angle=std::atan2(endN.y,endN.x);
    const auto circle=[&](float a){
      return position(t1)+Vec2{std::cos(a),std::sin(a)}*half;
    };
    appendParametric(out,circle,angle,angle-3.14159265358979323846f,tolerance);
  } else if(cap==LineCap::Square) {
    appendLineQuadratic(out,outerEnd,outerEnd+endT*half);
    appendLineQuadratic(out,outerEnd+endT*half,innerEnd+endT*half);
    appendLineQuadratic(out,innerEnd+endT*half,innerEnd);
  } else {
    appendLineQuadratic(out,outerEnd,innerEnd);
  }

  appendParametric(out,inner,t1,t0,tolerance);

  const Vec2 startT=normalized(tangent(t0));
  const Vec2 startN{-startT.y,startT.x};
  const Vec2 innerStart=inner(t0),outerStart=outer(t0);
  if(cap==LineCap::Round) {
    const float angle=std::atan2(startN.y,startN.x)+3.14159265358979323846f;
    const auto circle=[&](float a){
      return position(t0)+Vec2{std::cos(a),std::sin(a)}*half;
    };
    appendParametric(out,circle,angle,angle-3.14159265358979323846f,tolerance);
  } else if(cap==LineCap::Square) {
    appendLineQuadratic(out,innerStart,innerStart-startT*half);
    appendLineQuadratic(out,innerStart-startT*half,outerStart-startT*half);
    appendLineQuadratic(out,outerStart-startT*half,outerStart);
  } else {
    appendLineQuadratic(out,innerStart,outerStart);
  }
  return out;
}

} // namespace

void appendLineQuadratic(std::vector<QuadraticBezier2D>& curves, Vec2 from, Vec2 to) {
  requireFinite(from); requireFinite(to);
  if(std::abs(from.x-to.x)<=1.0e-8f && std::abs(from.y-to.y)<=1.0e-8f) return;
  curves.push_back({from,to,to});
}

void appendCubicQuadratics(std::vector<QuadraticBezier2D>& curves,
                           Vec2 p0,Vec2 p1,Vec2 p2,Vec2 p3,float tolerance) {
  requireFinite(p0); requireFinite(p1); requireFinite(p2); requireFinite(p3);
  requireFinite(tolerance);
  const std::size_t originalSize = curves.size();
  tolerance=std::max(tolerance,1.0e-6f);
  try {
    cubicRecursive(curves,p0,p1,p2,p3,tolerance*tolerance,0);
  } catch (...) {
    curves.resize(originalSize);
    throw;
  }
}
std::vector<QuadraticBezier2D> makeRoundedRectQuadratics(
    Rect b,float radius,float tolerance) {
  return makeContinuousRoundedRectQuadratics(
    b,CornerRadii::all(radius),CornerSmoothing::all(0.0f),tolerance);
}

std::vector<QuadraticBezier2D> makeContinuousRoundedRectQuadratics(
    Rect b,float radius,float smoothingPercent,float tolerance) {
  return makeContinuousRoundedRectQuadratics(
    b,CornerRadii::all(radius),CornerSmoothing::all(smoothingPercent),tolerance);
}

std::vector<QuadraticBezier2D> makeContinuousRoundedRectQuadratics(
    Rect b,CornerRadii radii,CornerSmoothing smoothing,float tolerance) {
  const std::array<Vec2,4> xy{{
    {radii.topLeft,radii.topLeft},{radii.topRight,radii.topRight},
    {radii.bottomRight,radii.bottomRight},{radii.bottomLeft,radii.bottomLeft}}};
  return makeContinuousRoundedRectQuadraticsXY(b,xy,smoothing,tolerance);
}

std::vector<QuadraticBezier2D> makeContinuousRoundedRectQuadraticsXY(
    Rect b,std::array<Vec2,4> radii,CornerSmoothing smoothing,float tolerance) {
  std::vector<QuadraticBezier2D> out;
  requireFinite(b); requireFinite(tolerance);
  for (const auto r : radii) requireFinite(r);
  requireFinite(smoothing.topLeftPercent); requireFinite(smoothing.topRightPercent);
  requireFinite(smoothing.bottomRightPercent); requireFinite(smoothing.bottomLeftPercent);
  if(b.width<=0.0f||b.height<=0.0f) return out;

  for(auto& r:radii) {
    r.x=std::max(r.x,0.0f);
    r.y=std::max(r.y,0.0f);
    if(r.x<=1.0e-7f||r.y<=1.0e-7f) r={0.0f,0.0f};
  }
  float radiusScale=1.0f;
  const auto fit=[&](float available,float sum){
    if(sum>available&&sum>1.0e-8f)
      radiusScale=std::min(radiusScale,available/sum);
  };
  fit(b.width,radii[0].x+radii[1].x);
  fit(b.width,radii[3].x+radii[2].x);
  fit(b.height,radii[0].y+radii[3].y);
  fit(b.height,radii[1].y+radii[2].y);
  for(auto& r:radii) r=r*radiusScale;

  const std::array<float,4> smooth{
    smoothing.topLeftPercent,smoothing.topRightPercent,
    smoothing.bottomRightPercent,smoothing.bottomLeftPercent};
  constexpr float halfPi=1.57079632679489661923f;
  const float x0=b.x,y0=b.y,x1=b.x+b.width,y1=b.y+b.height;

  const auto appendCorner=[&](Vec2 center,Vec2 radius,float smoothingPercent,
                              float sx,float sy,float t0,float t1){
    if(radius.x<=1.0e-7f||radius.y<=1.0e-7f) return;
    const float exponent=2.0f+3.0f*std::clamp(smoothingPercent*0.01f,0.0f,1.0f);
    const float power=2.0f/exponent;
    appendParametric(out,[=](float t){
      const float ct=std::max(std::cos(t),0.0f);
      const float st=std::max(std::sin(t),0.0f);
      return Vec2{center.x+sx*radius.x*std::pow(ct,power),
                  center.y+sy*radius.y*std::pow(st,power)};
    },t0,t1,tolerance);
  };

  appendLineQuadratic(out,{x0+radii[0].x,y0},{x1-radii[1].x,y0});
  appendCorner({x1-radii[1].x,y0+radii[1].y},radii[1],smooth[1],
               1.0f,-1.0f,halfPi,0.0f);
  appendLineQuadratic(out,{x1,y0+radii[1].y},{x1,y1-radii[2].y});
  appendCorner({x1-radii[2].x,y1-radii[2].y},radii[2],smooth[2],
               1.0f,1.0f,0.0f,halfPi);
  appendLineQuadratic(out,{x1-radii[2].x,y1},{x0+radii[3].x,y1});
  appendCorner({x0+radii[3].x,y1-radii[3].y},radii[3],smooth[3],
               -1.0f,1.0f,halfPi,0.0f);
  appendLineQuadratic(out,{x0,y1-radii[3].y},{x0,y0+radii[0].y});
  appendCorner({x0+radii[0].x,y0+radii[0].y},radii[0],smooth[0],
               -1.0f,-1.0f,0.0f,halfPi);
  return out;
}

std::vector<QuadraticBezier2D> makeEllipseQuadratics(
    Vec2 c,Vec2 r,float tolerance) {
  requireFinite(c); requireFinite(r); requireFinite(tolerance);
  std::vector<QuadraticBezier2D> out;
  if(r.x<=0.0f||r.y<=0.0f) return out;
  constexpr float halfPi=1.57079632679489661923f;
  const auto ellipse=[=](float a){
    return Vec2{c.x+r.x*std::cos(a),c.y+r.y*std::sin(a)};
  };
  for(int quadrant=0;quadrant<4;++quadrant) {
    const float a0=halfPi*static_cast<float>(quadrant);
    appendParametric(out,ellipse,a0,a0+halfPi,tolerance);
  }
  return out;
}

std::vector<QuadraticBezier2D> makeLineStrokeQuadratics(
    Vec2 from,Vec2 to,float width,LineCap cap,float tolerance) {
  requireFinite(from); requireFinite(to); requireFinite(width); requireFinite(tolerance);
  std::vector<QuadraticBezier2D> out;
  if(width<=0.0f) return out;
  const Vec2 delta=to-from;
  const float len=std::hypot(delta.x,delta.y);
  requireFinite(len);
  const float half=0.5f*width;
  if(len<=1.0e-8f) {
    if(cap==LineCap::Round) return makeEllipseQuadratics(from,{half,half},tolerance);
    if(cap==LineCap::Square) return makeRoundedRectQuadratics(
      {from.x-half,from.y-half,width,width},0.0f,tolerance);
    return out;
  }
  const Vec2 tangent{delta.x/len,delta.y/len};
  const Vec2 normal{-tangent.y,tangent.x};
  const float x0=cap==LineCap::Square?-half:0.0f;
  const float x1=cap==LineCap::Square?len+half:len;
  if(cap==LineCap::Round)
    out=makeRoundedRectQuadratics({-half,-half,len+width,width},half,tolerance);
  else
    out=makeRoundedRectQuadratics({x0,-half,x1-x0,width},0.0f,tolerance);
  const auto map=[&](Vec2 p){return from+tangent*p.x+normal*p.y;};
  for(auto& c:out){c.p0=map(c.p0);c.p1=map(c.p1);c.p2=map(c.p2);}
  return out;
}

std::vector<QuadraticBezier2D> makeCubicStrokeQuadratics(
    Vec2 p0,Vec2 p1,Vec2 p2,Vec2 p3,float width,LineCap cap,float tolerance) {
  requireFinite(p0); requireFinite(p1); requireFinite(p2); requireFinite(p3);
  requireFinite(width); requireFinite(tolerance);
  const auto position=[=](float t){
    const float u=1.0f-t;
    return p0*(u*u*u)+p1*(3.0f*u*u*t)+p2*(3.0f*u*t*t)+p3*(t*t*t);
  };
  const auto tangent=[=](float t){
    const float u=1.0f-t;
    return (p1-p0)*(3.0f*u*u)+(p2-p1)*(6.0f*u*t)+(p3-p2)*(3.0f*t*t);
  };
  return makeOpenStroke(position,tangent,0.0f,1.0f,width,cap,tolerance);
}

std::vector<QuadraticBezier2D> makeArcStrokeQuadratics(
    Vec2 center,float radius,float startRadians,float sweepRadians,float width,
    LineCap cap,float tolerance) {
  requireFinite(center); requireFinite(radius); requireFinite(startRadians);
  requireFinite(sweepRadians); requireFinite(width); requireFinite(tolerance);
  std::vector<QuadraticBezier2D> out;
  if(radius<=0.0f||width<=0.0f||std::abs(sweepRadians)<=1.0e-8f) return out;
  const auto position=[=](float t){
    const float a=startRadians+sweepRadians*t;
    return center+Vec2{std::cos(a),std::sin(a)}*radius;
  };
  const auto tangent=[=](float t){
    const float a=startRadians+sweepRadians*t;
    return Vec2{-std::sin(a),std::cos(a)}*(radius*sweepRadians);
  };
  const float tau=6.28318530717958647692f;
  if(std::abs(std::abs(sweepRadians)-tau)<=1.0e-5f) {
    const float half=0.5f*width;
    const float outerRadius=radius+half;
    const float innerRadius=std::max(radius-half,0.0f);
    const auto outer=[=](float t){
      const float a=startRadians+sweepRadians*t;
      return center+Vec2{std::cos(a),std::sin(a)}*outerRadius;
    };
    appendParametric(out,outer,0.0f,1.0f,tolerance);
    if(innerRadius>1.0e-6f) {
      const auto inner=[=](float t){
        const float a=startRadians+sweepRadians*t;
        return center+Vec2{std::cos(a),std::sin(a)}*innerRadius;
      };
      appendParametric(out,inner,1.0f,0.0f,tolerance);
    }
    return out;
  }
  return makeOpenStroke(position,tangent,0.0f,1.0f,width,cap,tolerance);
}

void reverseQuadraticContour(std::vector<QuadraticBezier2D>& curves) {
  for(auto& c:curves) std::swap(c.p0,c.p2);
  std::reverse(curves.begin(),curves.end());
}

Rect quadraticBounds(std::span<const QuadraticBezier2D> curves,float padding) {
  requireFinite(padding);
  if(curves.empty()) return {};
  float minX=std::numeric_limits<float>::infinity(),minY=minX;
  float maxX=-minX,maxY=-minX;
  for(const auto& c:curves) {
    requireFinite(c.p0); requireFinite(c.p1); requireFinite(c.p2);
    minX=std::min({minX,c.p0.x,c.p1.x,c.p2.x});
    minY=std::min({minY,c.p0.y,c.p1.y,c.p2.y});
    maxX=std::max({maxX,c.p0.x,c.p1.x,c.p2.x});
    maxY=std::max({maxY,c.p0.y,c.p1.y,c.p2.y});
  }
  padding=std::max(padding,0.0f);
  const Rect bounds{minX-padding,minY-padding,(maxX-minX)+2.0f*padding,(maxY-minY)+2.0f*padding};
  requireFinite(bounds);
  return bounds;
}

} // namespace slugia
