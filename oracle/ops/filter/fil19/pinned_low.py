"""Compile-time lowering of the pinned upstream SMAA LOW shader to scalar C++.

This is a comparison oracle, not an operations-library dependency. Function
bodies are extracted from the unchanged, hash-pinned third-party shader.
Only shader syntax, constructors, intrinsics and sampler interfaces are lowered.
Texture samples use deterministic RN32 software bilinear interpolation; this
does not claim to emulate any vendor's GPU texture-unit quantization.
"""
from pathlib import Path
import hashlib
import re

PIN = "4db53d92ef0b45661f8450303589ffabce93c352cae4282a6f32dd2b80efbeb9"

FUNCTIONS = (
    "SMAAEdgeDetectionVS", "SMAABlendingWeightCalculationVS",
    "SMAANeighborhoodBlendingVS", "SMAALumaEdgeDetectionPS",
    "SMAASearchLength", "SMAASearchXLeft", "SMAASearchXRight",
    "SMAASearchYUp", "SMAASearchYDown", "SMAAArea",
    "SMAABlendingWeightCalculationPS", "SMAANeighborhoodBlendingPS",
)

SHIM = r"""
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
namespace upstream_smaa {
typedef float float2 __attribute__((ext_vector_type(2)));
typedef float float3 __attribute__((ext_vector_type(3)));
typedef float float4 __attribute__((ext_vector_type(4)));
typedef int int2 __attribute__((ext_vector_type(2)));
typedef int bool2 __attribute__((ext_vector_type(2)));
typedef int bool4 __attribute__((ext_vector_type(4)));
inline float2 v2(float a, float b) { return {a,b}; }
inline float2 v2(float a) { return {a,a}; }
inline float3 v3(float a, float b, float c) { return {a,b,c}; }
inline float4 v4(float a, float b, float c, float d) { return {a,b,c,d}; }
inline float4 v4(float2 a, float2 b) { return {a.x,a.y,b.x,b.y}; }
inline int2 i2(int a,int b) { return {a,b}; }
inline bool2 b2(bool a,bool b) { return {a,b}; }
inline bool4 b4(bool a,bool b,bool c,bool d) { return {a,b,c,d}; }
inline float abs(float x) { return std::abs(x); }
inline float max(float a,float b) { return std::max(a,b); }
inline float mad(float a,float b,float c) { return std::fma(a,b,c); }
inline float step(float edge,float x) { return x>=edge?1.0F:0.0F; }
inline float2 abs(float2 x) { return {abs(x.x),abs(x.y)}; }
inline float2 max(float2 a,float2 b) { return {max(a.x,b.x),max(a.y,b.y)}; }
inline float2 sqrt(float2 x) { return {std::sqrt(x.x),std::sqrt(x.y)}; }
inline float2 round(float2 x) { return {std::nearbyint(x.x),std::nearbyint(x.y)}; }
inline float2 step(float2 a,float2 b) { return {step(a.x,b.x),step(a.y,b.y)}; }
inline float2 step(float a,float2 b) { return step(v2(a),b); }
inline float2 mad(float2 a,float2 b,float2 c) {
  return {mad(a.x,b.x,c.x),mad(a.y,b.y,c.y)};
}
inline float4 mad(float4 a,float4 b,float4 c) {
  return {mad(a.x,b.x,c.x),mad(a.y,b.y,c.y),
          mad(a.z,b.z,c.z),mad(a.w,b.w,c.w)};
}
inline float dot(float2 a,float2 b) { return a.x*b.x+a.y*b.y; }
inline float dot(float3 a,float3 b) { return (a.x*b.x+a.y*b.y)+a.z*b.z; }
inline float dot(float4 a,float4 b) {
  return ((a.x*b.x+a.y*b.y)+a.z*b.z)+a.w*b.w;
}
inline float2 choose(bool2 c,float2 a,float2 b) {
  return {c.x?b.x:a.x,c.y?b.y:a.y};
}
inline float4 choose(bool4 c,float4 a,float4 b) {
  return {c.x?b.x:a.x,c.y?b.y:a.y,c.z?b.z:a.z,c.w?b.w:a.w};
}
struct Texture {
  int width,height;
  std::vector<float4> pixels;
  Texture(int w,int h):width(w),height(h),pixels(w*h){}
  float4 at(int y,int x) const {
    return pixels[std::clamp(y,0,height-1)*width+std::clamp(x,0,width-1)];
  }
  float4 point(float2 uv) const {
    return at(static_cast<int>(std::floor(uv.y*height)),
              static_cast<int>(std::floor(uv.x*width)));
  }
  float4 linear(float2 uv,int2 offset={0,0}) const {
    const float x=uv.x*width-0.5F, y=uv.y*height-0.5F;
    const int ix=static_cast<int>(std::floor(x));
    const int iy=static_cast<int>(std::floor(y));
    const float fx=x-ix,fy=y-iy;
    const auto a=at(iy+offset.y,ix+offset.x)*(1.0F-fx)+
                 at(iy+offset.y,ix+offset.x+1)*fx;
    const auto b=at(iy+offset.y+1,ix+offset.x)*(1.0F-fx)+
                 at(iy+offset.y+1,ix+offset.x+1)*fx;
    return a*(1.0F-fy)+b*fy;
  }
};
inline float4 metrics;
#define SMAA_RT_METRICS metrics
#define SMAA_THRESHOLD 0.15F
#define SMAA_MAX_SEARCH_STEPS 4
#define SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR 2.0F
#define SMAA_AREATEX_MAX_DISTANCE 16
#define SMAA_AREATEX_PIXEL_SIZE (1.0F / v2(160.0F,560.0F))
#define SMAA_AREATEX_SUBTEX_SIZE (1.0F / 7.0F)
#define SMAA_SEARCHTEX_SIZE v2(66.0F,33.0F)
#define SMAA_SEARCHTEX_PACKED_SIZE v2(64.0F,16.0F)
#define SMAA_DISABLE_DIAG_DETECTION
#define SMAA_DISABLE_CORNER_DETECTION
#define SMAA_PREDICATION 0
#define SMAA_REPROJECTION 0
#define SMAA_FLATTEN
#define SMAA_BRANCH
#define SMAATexture2D(tex) const Texture& tex
#define SMAATexturePass2D(tex) tex
#define SMAASample(tex,coord) tex.linear(coord)
#define SMAASamplePoint(tex,coord) tex.point(coord)
#define SMAASampleLevelZero(tex,coord) tex.linear(coord)
#define SMAASampleLevelZeroOffset(tex,coord,offset) tex.linear(coord,offset)
#define SMAA_SEARCHTEX_SELECT(value) value.r
#define SMAA_AREATEX_SELECT(value) value.rg
#define SMAAMovc(c,v,a) ((v)=choose((c),(v),(a)))
#define SMAADetectHorizontalCornerPattern(...)
#define SMAADetectVerticalCornerPattern(...)
#define discard return v2(0.0F,0.0F)
"""


def header():
    source = Path(__file__).with_name("pinned").joinpath("SMAA.hlsl").read_bytes()
    if hashlib.sha256(source).hexdigest() != PIN:
        raise ValueError("pinned SMAA shader hash mismatch")
    # Comments have no semantic role; deleting them makes brace matching exact.
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", source.decode("latin1"), flags=re.S)
    functions = []
    for name in FUNCTIONS:
        match = re.search(r"(?:void|float[234]?)\s+" + name + r"\s*\(", text)
        if not match:
            raise ValueError(name)
        begin = match.start()
        brace = text.index("{", match.end())
        nesting, end = 1, brace + 1
        while nesting:
            nesting += (text[end] == "{") - (text[end] == "}")
            end += 1
        body = text[begin:end]
        body = re.sub(r"\bfloat([234])\s*\(", r"v\1(", body)
        body = re.sub(r"\bint2\s*\(", "i2(", body)
        body = re.sub(r"\bbool([24])\s*\(", r"b\1(", body)
        body = re.sub(r"\bout\s+(float[234])\s+(\w+)(?!\w)(?!\s*\[)", r"\1& \2", body)
        body = re.sub(r"\bout\s+", "", body)
        # HLSL decimal literals are float, unlike unsuffixed C++ literals.
        body = re.sub(r"(?<![\w.])(\d+\.\d*(?:[eE][+-]?\d+)?)(?![\w.])", r"\1F", body)
        functions.append(body)
    return SHIM + "\n\n".join(functions) + "\n}  // namespace upstream_smaa\n"


if __name__ == "__main__":
    import sys
    Path(sys.argv[1]).write_text(header())
