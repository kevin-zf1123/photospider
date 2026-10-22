#pragma once
// Exact integer Perlin2002. Each lane owns 17 bounded device scratch slots.
// The host caps active lanes, and every accepted dispatch retains its backing.
namespace ps::plugin_internal::generation {
inline constexpr const char* perlin_metal_source = R"MSL(
#include <metal_stdlib>
using namespace metal;
#ifndef PS_PERLIN_WORDS
#define PS_PERLIN_WORDS 544u
#endif
#define SLOT_WORDS (PS_PERLIN_WORDS + 1u)
#define THREAD_WORDS (17u * SLOT_WORDS)
struct Config { uint count, input_small, output_small, lanes; };
struct Coordinate { uint cell, denominator_bits; ulong remainder; uint negative_fraction; };
inline device uint* slot(device uint* p, uint i) { return p + ulong(i) * SLOT_WORDS; }
inline uint limb(device const uint* p, uint i) { return i < p[0] ? p[i+1] : 0; }
inline void zero(device uint* p) { p[0]=0; }
inline void trim(device uint* p, uint n) { while(n && !p[n]) --n; p[0]=n; }
inline void copy(device uint* out, device const uint* a) {
  if(out==a) return; uint n=a[0]; for(uint i=0;i<n;++i)out[i+1]=a[i+1]; out[0]=n;
}
inline void u64(device uint* out, ulong n) {
  if(!n){zero(out);return;}out[1]=uint(n);out[2]=uint(n>>32);out[0]=out[2]?2:1;
}
inline void pow2(device uint* out,uint e) {
  uint n=e>>5;for(uint i=0;i<n;++i)out[i+1]=0;out[n+1]=1u<<(e&31);out[0]=n+1;
}
inline int compare(device const uint* a,device const uint* b) {
  if(a[0]!=b[0])return a[0]<b[0]?-1:1;
  for(int i=int(a[0]);i>0;--i)if(a[i]!=b[i])return a[i]<b[i]?-1:1;
  return 0;
}
inline void add(device uint* out,device const uint* a,device const uint* b) {
  uint na=a[0],nb=b[0],n=max(na,nb);ulong carry=0;
  for(uint i=0;i<n;++i){ulong v=(i<na?ulong(a[i+1]):0)+(i<nb?ulong(b[i+1]):0)+carry;
    out[i+1]=uint(v);carry=v>>32;}
  if(carry)out[++n]=uint(carry);out[0]=n;
}
inline void sub(device uint* out,device const uint* a,device const uint* b) {
  uint na=a[0],nb=b[0];ulong borrow=0;
  for(uint i=0;i<na;++i){ulong av=a[i+1],bv=(i<nb?ulong(b[i+1]):0)+borrow;
    out[i+1]=uint(av-bv);borrow=av<bv?1:0;}trim(out,na);
}
inline void mul_small(device uint* out,device const uint* a,uint factor) {
  uint n=a[0];if(!n||!factor){zero(out);return;}ulong carry=0;
  for(uint i=0;i<n;++i){ulong v=ulong(a[i+1])*factor+carry;out[i+1]=uint(v);carry=v>>32;}
  if(carry)out[++n]=uint(carry);out[0]=n;
}
// Output never aliases either input. At most 16*d+O(1) bits are required.
inline void mul(device uint* out,device const uint* a,device const uint* b) {
  uint na=a[0],nb=b[0];if(!na||!nb){zero(out);return;}uint n=na+nb;
  for(uint i=0;i<n;++i)out[i+1]=0;
  for(uint i=0;i<na;++i){ulong carry=0,av=a[i+1];
    for(uint j=0;j<nb;++j){ulong v=av*ulong(b[j+1])+ulong(out[i+j+1])+carry;
      out[i+j+1]=uint(v);carry=v>>32;}out[i+nb+1]=uint(carry);}
  trim(out,n);
}
inline void shl(device uint* out,device const uint* a,uint shift) {
  uint na=a[0];if(!na){zero(out);return;}if(!shift){copy(out,a);return;}
  uint ws=shift>>5,bs=shift&31,n=na+ws+(bs?1:0);
  for(int k=int(n)-1;k>=0;--k){int source=k-int(ws);uint v=0;
    if(source>=0&&source<int(na))v=bs?a[uint(source)+1]<<bs:a[uint(source)+1];
    if(bs&&source>0&&source-1<int(na))v|=a[uint(source)]>>(32-bs);
    out[uint(k)+1]=v;}trim(out,n);
}
inline int signed_add(device uint* out,device const uint* a,int sa,device const uint* b,int sb) {
  if(!a[0]){copy(out,b);return out[0]?sb:0;}if(!b[0]){copy(out,a);return sa;}
  if(sa==sb){add(out,a,b);return sa;}int c=compare(a,b);
  if(!c){zero(out);return 0;}if(c>0){sub(out,a,b);return sa;}sub(out,b,a);return sb;
}
inline uint bit_length(device const uint* p) { return p[0]?(p[0]-1)*32+32-clz(p[p[0]]):0; }
inline uint bit(device const uint* p,uint b) { return (b>>5)<p[0]?(p[(b>>5)+1]>>(b&31))&1:0; }
inline bool any_low(device const uint* p,uint bits) {
  uint n=bits>>5;for(uint i=0;i<min(n,p[0]);++i)if(p[i+1])return true;
  uint tail=bits&31;return tail&&n<p[0]&&(p[n+1]&((1u<<tail)-1));
}
inline ulong extract(device const uint* p,uint shift) {
  uint w=shift>>5,b=shift&31;ulong lo=ulong(limb(p,w))|(ulong(limb(p,w+1))<<32);
  return b?(lo>>b)|(ulong(limb(p,w+2))<<(64-b)):lo;
}
inline ulong rounded(device const uint* p,uint denominator,int sign,uint small) {
  uint length=bit_length(p);if(!length)return 0;
  uint f=small?23:52,precision=f+1;int minimum=small?-126:-1022,maximum=small?127:1023,bias=small?127:1023;
  ulong s=sign<0?ulong(1)<<(small?31:63):0;
  int e=int(length)-1-int(denominator),q=max(e-int(f),minimum-int(f)),shift=int(denominator)+q;
  ulong sig;
  if(shift<=0)sig=extract(p,0)<<uint(-shift);
  else{sig=extract(p,uint(shift));if(bit(p,uint(shift-1))&&(any_low(p,uint(shift-1))||(sig&1)))++sig;}
  if(!sig)return s;
  ulong hidden=ulong(1)<<f;
  if(e<minimum)return s|(sig>=hidden?hidden:sig);
  if(sig==(ulong(1)<<precision)){sig>>=1;++e;}
  if(e>maximum)return s|((ulong(small?255:2047))<<f);
  return s|(ulong(e+bias)<<f)|(sig-hidden);
}
inline Coordinate decode(ulong raw,uint small) {
  Coordinate r={0,0,0,0};ulong sig;uint neg;int exponent;
  if(small){uint b=uint(raw),e=(b>>23)&255;neg=b>>31;
    sig=ulong((b&0x7fffff)|(e?0x800000:0));exponent=e?int(e)-150:-149;}
  else{uint e=uint((raw>>52)&2047);neg=uint(raw>>63);
    sig=(raw&((ulong(1)<<52)-1))|(e?ulong(1)<<52:0);exponent=e?int(e)-1075:-1074;}
  if(!sig)return r;
  if(exponent>=0){uint v=exponent<8?(uint(sig&255)<<uint(exponent))&255:0;r.cell=neg?(0u-v)&255:v;return r;}
  uint d=uint(-exponent);ulong ip=d>=64?0:sig>>d,rem=d>=64?sig:sig&((ulong(1)<<d)-1);
  uint v=uint(ip&255);r.cell=neg?(0u-v-(rem?1u:0u))&255:v;
  if(!rem)return r;
  while(!(rem&1)){rem>>=1;--d;}
  r.denominator_bits=d;r.remainder=rem;r.negative_fraction=neg;return r;
}
inline void fade(device const uint* f,uint d,device uint* out,device uint* complement,
                 device uint* t0,device uint* t1,device uint* t2) {
  if(!f[0]){zero(out);pow2(complement,0);return;}
  mul(t0,f,f);mul(t1,t0,f);mul_small(t0,f,6);
  pow2(t2,d);mul_small(t2,t2,15);sub(t2,t2,t0);mul(t0,t2,f);
  pow2(t2,2*d);mul_small(t2,t2,10);sub(t2,t2,t0);mul(out,t1,t2);
  pow2(complement,5*d);sub(complement,complement,out);
}
inline void prepare_axis(device uint* p,uint axis,Coordinate c,uint common) {
  device uint* f=slot(p,6+2*axis);device uint* co=slot(p,7+2*axis);
  device uint* t0=slot(p,12);device uint* t1=slot(p,13);device uint* t2=slot(p,14);
  if(c.negative_fraction){pow2(f,c.denominator_bits);u64(t0,c.remainder);sub(f,f,t0);}
  else u64(f,c.remainder);
  fade(f,c.denominator_bits,slot(p,2*axis),slot(p,2*axis+1),t0,t1,t2);
  shl(f,f,common-c.denominator_bits);pow2(co,common);sub(co,co,f);
}
constant uint permutation[256]={
151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,8,99,37,240,21,10,23,
190,6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,57,177,33,
88,237,149,56,87,174,20,125,136,171,168,68,175,74,165,71,134,139,48,27,166,
77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,55,46,245,40,244,
102,143,54,65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,200,196,
135,130,116,188,159,86,164,100,109,198,173,186,3,64,52,217,226,250,124,123,
5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,
223,183,170,213,119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,
129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,218,246,97,228,
251,34,242,193,238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,107,
49,192,214,31,181,199,106,157,184,84,204,176,115,121,50,45,127,4,150,254,
138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180};
inline uint hash(uint x,uint y,uint z,uint corner) {
  uint a=permutation[(x+(corner&1))&255],b=permutation[(a+y+((corner>>1)&1))&255];
  return permutation[(b+z+(corner>>2))&255];
}
inline int gradient(device uint* p,uint corner,uint h,device uint* out) {
  h&=15;uint ua=h<8?0:1,va=h<4?1:(h==12||h==14)?0:2;
  uint uc=(corner>>ua)&1,vc=(corner>>va)&1;
  bool un=((h&1)!=0)!=(uc!=0),vn=((h&2)!=0)!=(vc!=0);
  return signed_add(out,slot(p,6+2*ua+uc),un?-1:1,slot(p,6+2*va+vc),vn?-1:1);
}
kernel void perlin2002_exact_bits(device const ulong* input [[buffer(0)]],
    device ulong* output [[buffer(1)]],constant Config& config [[buffer(2)]],
    device uint* scratch [[buffer(3)]],uint gid [[thread_position_in_grid]]) {
  if(gid>=config.lanes)return;
  device uint* p=scratch+ulong(gid)*THREAD_WORDS;
  for(ulong item=gid;item<config.count;item+=config.lanes){
    Coordinate x=decode(input[3*item],config.input_small),y=decode(input[3*item+1],config.input_small),
               z=decode(input[3*item+2],config.input_small);
    uint common=max(x.denominator_bits,max(y.denominator_bits,z.denominator_bits));
    if(!common){output[item]=0;continue;}
    prepare_axis(p,0,x,common);prepare_axis(p,1,y,common);prepare_axis(p,2,z,common);
    device uint* xy=slot(p,12);device uint* xyz=slot(p,13);device uint* g=slot(p,14);
    device uint* term=slot(p,15);device uint* sum=slot(p,16);zero(sum);int sign=0;
    for(uint c=0;c<8;++c){
      device const uint* wx=slot(p,(c&1)?0:1);device const uint* wy=slot(p,(c&2)?2:3);
      device const uint* wz=slot(p,(c&4)?4:5);if(!wx[0]||!wy[0]||!wz[0])continue;
      mul(xy,wx,wy);mul(xyz,xy,wz);int gs=gradient(p,c,hash(x.cell,y.cell,z.cell,c),g);
      if(!gs)continue;mul(term,xyz,g);sign=signed_add(sum,sum,sign,term,gs);
    }
    output[item]=rounded(sum,5*(x.denominator_bits+y.denominator_bits+z.denominator_bits)+common,sign,config.output_small);
  }
}
)MSL";
}  // namespace ps::plugin_internal::generation
