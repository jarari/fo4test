Texture2D<float4> t2 : register(t2);

Texture2D<float4> t1 : register(t1);

Texture2D<float4> t0 : register(t0);

SamplerState s2_s : register(s2);

SamplerState s1_s : register(s1);

SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0)
{
  float4 cb0[3];
}

cbuffer cb12 : register(b12)
{
  float4 cb12[28];
}

// xy converts logical screen UV to allocation UV. zw is its reciprocal.
cbuffer cb13 : register(b13)
{
  float4 cb13[1];
}

#define cmp -

void main(
  float4 v0 : SV_POSITION0,
  float2 v1 : TEXCOORD0,
  out float2 o0 : SV_Target0,
  out float4 o1 : SV_Target1)
{
  float4 r0,r1,r2,r3,r4,r5,r6;
  uint4 bitmask, uiDest;
  float4 fDest;

  // Fallout's dynamic fullscreen triangle supplies allocation UVs. Projection
  // matrices still operate in logical 0..1 screen UVs.
  float2 logicalUV = v1.xy * cb13[0].zw;

  r0.x = t2.Sample(s2_s, v1.xy).x;
  r0.x = r0.x * cb0[2].z + -0.00999999978;
  r0.x = cmp(r0.x < 0);
  if (r0.x != 0) discard;
  r0.z = t0.SampleLevel(s0_s, v1.xy, 0).x;
  r0.w = cmp(0.00999999978 >= r0.z);
  if (r0.w != 0) {
    r1.z = 100 * r0.z;
    r2.xyzw = cb12[24].xyzw;
    r3.xyzw = cb12[25].xyzw;
    r4.xyzw = cb12[26].xyzw;
    r5.xyzw = cb12[27].xyzw;
  } else {
    r1.z = r0.z * 1.00999999 + -0.00999999978;
    r2.xyzw = cb12[20].xyzw;
    r3.xyzw = cb12[21].xyzw;
    r4.xyzw = cb12[22].xyzw;
    r5.xyzw = cb12[23].xyzw;
  }
  r0.xy = logicalUV.yx;
  r6.xy = r0.yx * float2(1,-1) + float2(0,1);
  r1.xy = r6.xy * float2(2,2) + float2(-1,-1);
  r1.w = 1;
  r2.x = dot(r2.xyzw, r1.xyzw);
  r2.y = dot(r3.xyzw, r1.xyzw);
  r2.z = dot(r4.xyzw, r1.xyzw);
  r0.w = dot(r5.xyzw, r1.xyzw);
  r1.xyz = r2.xyz / r0.www;
  r0.w = dot(-r1.xyz, -r1.xyz);
  r0.w = rsqrt(r0.w);
  r2.xyz = -r1.xyz * r0.www;
  r3.xy = t1.Sample(s1_s, v1.xy).xy;
  r3.xy = r3.xy * float2(4,4) + float2(-2,-2);
  r0.w = dot(r3.xy, r3.xy);
  r3.zw = -r0.ww * float2(0.25,0.5) + float2(1,1);
  r0.w = sqrt(r3.z);
  r3.xy = r3.xy * r0.ww;
  r3.z = -r3.w;
  r0.w = dot(r3.xyz, r3.xyz);
  r0.w = rsqrt(r0.w);
  r3.xyz = r3.xyz * r0.www;
  r0.w = dot(r3.xyz, r2.xyz);
  r0.w = cmp(r0.w >= 0);
  r4.x = dot(cb12[12].xyz, r3.xyz);
  r4.y = dot(cb12[13].xyz, r3.xyz);
  r1.w = dot(cb12[14].xyz, r3.xyz);
  r4.z = cb0[2].x * r1.w;
  r1.w = dot(r4.xyz, r4.xyz);
  r1.w = rsqrt(r1.w);
  r3.xyz = r4.xyz * r1.www;
  r4.x = dot(cb12[0].xyz, r3.xyz);
  r4.y = dot(cb12[1].xyz, r3.xyz);
  r4.z = dot(cb12[2].xyz, r3.xyz);
  r1.w = dot(r4.xyz, r4.xyz);
  r1.w = rsqrt(r1.w);
  r3.xyz = r4.xyz * r1.www;
  r1.w = dot(-r2.xyz, r3.xyz);
  r1.w = r1.w + r1.w;
  r2.xyz = r3.xyz * -r1.www + -r2.xyz;
  r1.w = cmp(cb0[1].y < r2.z);
  r2.xyz = r2.xyz * float3(1000,1000,1000) + r1.xyz;
  r2.w = 1;
  r3.x = dot(cb12[4].xyzw, r2.xyzw);
  r3.y = dot(cb12[5].xyzw, r2.xyzw);
  r3.z = dot(cb12[6].xyzw, r2.xyzw);
  r2.x = dot(cb12[7].xyzw, r2.xyzw);
  r2.y = cmp(r2.x == 0.000000);
  r2.xzw = r3.xyz / r2.xxx;
  r2.xyz = r2.yyy ? float3(1,1,1) : r2.xzw;
  r2.xyz = r2.xyz * float3(0.5,-0.5,1) + float3(0.5,0.5,0);

  // Re-enter allocation UV space before generating the ray equation consumed
  // by the raytracing pass.
  r2.xy = r2.xy * cb13[0].xy;
  r2.xyz = r2.xyz + -float3(v1.xy, r0.z);
  r0.xy = r2.xy / r2.zz;
  r1.xy = -r0.zz * r0.xy + v1.xy;
  r0.xyz = r1.www ? r1.xyz : 0;
  r0.xyz = r0.www ? r0.xyz : 0;
  o1.x = r0.z;
  o1.yzw = float3(0,0,0);
  o0.xy = r0.xy;
  return;
}
