/* Direct3D 12 backend for the GX emulation core (gx_core.c, gx_gpu.h).
 *
 * Mirrors render_opengl.c: one uber shader (HLSL port of the GLSL, same
 * constants), GL's state semantics (see docs/d3d12.md), offscreen colour +
 * depth target that is resolved/copied into a DXGI flip-model swap chain at
 * present.  Plain C through COBJMACROS; d3d12.dll / dxgi.dll /
 * d3dcompiler_47.dll are loaded at runtime so the exe still starts (and falls
 * back to GL) where they are missing.  64-bit clean: no pointer <-> 32-bit
 * integer casts. */
#ifdef _WIN32
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include "perf.h"
#include <d3dcompiler.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "render.h"
#include "gx_gpu.h"
#include "platform.h"

#pragma GCC diagnostic ignored "-Woverlength-strings"

/* ------------------------------------------------------------------ HLSL */
static const char* s_hlsl =
"cbuffer DrawC : register(b0) {\n"
"    float4x4 u_projection;\n"
"    float4x4 u_modelview;\n"
"    float4x4 u_gx_nrm;\n"
"    float4x4 u_texmatrix[8];\n"
"    int4 u_mode;\n"        /* gx_mode, ui_tev, has_tex, uvclamp */
"    int4 u_tg;\n"          /* posmask, srcsel, nrmmask, normmask */
"    int4 u_tg2;\n"         /* postmask, tex_valid, mtx2x4 mask (q = 1) */
"    int4 u_gx_acmp;\n"
"    float4 u_tev_c0;\n"
"    float4 u_tev_c1;\n"
"    float4 u_fog;\n"
"    float4 u_fog_color;\n"
"    float4 u_efb_scale;\n"
"};\n"
"cbuffer TevC : register(b1) {\n"
"    float4x4 u_tg_post[8];\n"
"    int4 u_gx_chan[2];\n"
"    int4 u_gx_chanfn[2];\n"
"    float4 u_gx_mat;\n"
"    float4 u_gx_amb;\n"
"    float4 u_gx_lpos[8];\n"
"    float4 u_gx_ldir[8];\n"
"    float4 u_gx_lcos[8];\n"
"    float4 u_gx_ldist[8];\n"
"    float4 u_gx_lcol[8];\n"
"    int4 u_gx_cin[8];\n"
"    int4 u_gx_ain[8];\n"
"    int4 u_gx_cop[8];\n"
"    int4 u_gx_aop[8];\n"
"    int4 u_gx_ord[8];\n"
"    float4 u_gx_konst[8];\n"
"    float4 u_gx_reg[4];\n"
"    int4 u_misc;\n"        /* tev_n, ind_on, ind_n */
"    int4 u_gx_iord[4];\n"
"    float4 u_gx_imtx[6];\n"
"    int4 u_gx_tind[8];\n"
"};\n"
"Texture2D u_tex0 : register(t0); Texture2D u_tex1 : register(t1);\n"
"Texture2D u_tex2 : register(t2); Texture2D u_tex3 : register(t3);\n"
"Texture2D u_tex4 : register(t4); Texture2D u_tex5 : register(t5);\n"
"Texture2D u_tex6 : register(t6); Texture2D u_tex7 : register(t7);\n"
"SamplerState u_smp0 : register(s0); SamplerState u_smp1 : register(s1);\n"
"SamplerState u_smp2 : register(s2); SamplerState u_smp3 : register(s3);\n"
"SamplerState u_smp4 : register(s4); SamplerState u_smp5 : register(s5);\n"
"SamplerState u_smp6 : register(s6); SamplerState u_smp7 : register(s7);\n"
"struct VSIn {\n"
"    float3 a_position : POSITION;\n"
"    float3 a_normal : NORMAL;\n"
"    float4 a_color : COLOR;\n"
"    float2 a_texcoord0 : TEXCOORD0;\n"
"    float2 a_texcoord1 : TEXCOORD1;\n"
"    float2 a_texcoord2 : TEXCOORD2;\n"
"    float4 a_uvrect : TEXCOORD4;\n"
"};\n"
"struct VSOut {\n"
"    float4 pos : SV_Position;\n"
"    float4 v_color : COLOR0;\n"
"    float2 v_texcoord0 : TEXCOORD0;\n"
"    float3 v_tcq[8] : TEXCOORD1;\n"
"    float v_eyez : TEXCOORD9;\n"
"    nointerpolation float4 v_uvrect : TEXCOORD10;\n"
"};\n"
/* ---- vertex shader: same math as the GLSL ---- */
"float4 gx_chan(int ch, float3 P, float3 N, float4 a_color) {\n"
"    int4 c = u_gx_chan[ch];\n"
"    float4 mat = (c.y != 0) ? a_color : u_gx_mat;\n"
"    float4 res = mat;\n"
"    if (c.x != 0) {\n"
"    float4 acc = (c.z != 0) ? a_color : u_gx_amb;\n"
"    [loop] for (int i = 0; i < 8; i++) {\n"
"        if ((c.w & (1 << i)) != 0) {\n"
"        float3 L = u_gx_lpos[i].xyz - P;\n"
"        float d2 = dot(L, L);\n"
"        float d = sqrt(d2);\n"
"        L = L / max(d, 1e-6);\n"
"        float nl = dot(L, N);\n"
"        float diff = (u_gx_chanfn[ch].x == 0) ? 1.0 :\n"
"                     (u_gx_chanfn[ch].x == 1) ? nl : max(nl, 0.0);\n"
"        float attn = 1.0;\n"
"        float3 a = u_gx_lcos[i].xyz, k = u_gx_ldist[i].xyz;\n"
"        if (u_gx_chanfn[ch].y == 1) {\n"
"            float cs = max(0.0, dot(L, u_gx_ldir[i].xyz));\n"
"            attn = max(0.0, a.x + a.y * cs + a.z * cs * cs) /\n"
"                   max(dot(k, float3(1.0, d, d2)), 1e-6);\n"
"        } else if (u_gx_chanfn[ch].y == 0) {\n"
"            float cs = (nl >= 0.0) ? max(0.0, dot(N, u_gx_ldir[i].xyz)) : 0.0;\n"
"            attn = max(0.0, a.x + a.y * cs + a.z * cs * cs) /\n"
"                   max(k.x + k.y * cs + k.z * cs * cs, 1e-6);\n"
"        }\n"
"        acc += diff * attn * u_gx_lcol[i];\n"
"        }\n"
"    }\n"
"    res = mat * clamp(acc, 0.0, 1.0);\n"
"    }\n"
"    return res;\n"
"}\n"
"VSOut VSMain(VSIn v) {\n"
"    VSOut o;\n"
"    float4 pos = mul(u_modelview, float4(v.a_position, 1.0));\n"
"    o.pos = mul(u_projection, pos);\n"
/* GX clip z/w is in [-1 (near), 0 (far)].  The depth buffer stores it
 * reversed, -z/w = 1 - GX depth (compares and clears flipped to match):
 * z/w + 1 lies near 1.0 for distant geometry, where float rounding of z + w
 * and of the interpolation is a D24 step or more, enough to z-fight layers
 * 1-2 steps apart (2-5 lava blocks); -z/w keeps the precision. */
"    o.pos.z = -o.pos.z;\n"
/* GL rasterizes in y-up window space, D3D in y-down: a triangle edge that
 * lies exactly on a row of pixel centres (common for 2D quads at the 1.5x
 * EFB scale) is filled on opposite sides.  Nudging everything 1/256 pixel
 * down makes D3D pick the same rows as GL. */
"    o.pos.y -= u_efb_scale.z * o.pos.w;\n"
"    o.v_color = v.a_color;\n"
"    if (u_mode.x != 0) {\n"
"        float3 N = mul((float3x3)u_gx_nrm, v.a_normal);\n"
"        N = (dot(N, N) > 0.0) ? normalize(N) : float3(0.0, 0.0, 1.0);\n"
"        o.v_color = float4(gx_chan(0, pos.xyz, N, v.a_color).rgb, gx_chan(1, pos.xyz, N, v.a_color).a);\n"
"    }\n"
"    o.v_eyez = -pos.z;\n"
"    float2 tc0 = v.a_texcoord0;\n"
"    [unroll] for (int i = 0; i < 8; i++) {\n"
"        int tsel = (u_tg.y >> (2 * i)) & 3;\n"
"        float2 tsrc = (tsel == 1) ? v.a_texcoord1 : ((tsel == 2) ? v.a_texcoord2 : v.a_texcoord0);\n"
"        float2 vt = mul(u_texmatrix[i], float4(tsrc, 0.0, 1.0)).xy;\n"
"        if (((u_tg.x >> i) & 1) != 0) {\n"
"            o.v_tcq[i] = mul(u_texmatrix[i], float4(v.a_position, 1.0)).xyz;\n"
"            if (((u_tg2.z >> i) & 1) != 0) o.v_tcq[i].z = 1.0;\n"
"            if (u_mode.x == 0) vt = v.a_texcoord0;\n"
"        } else if (((u_tg.z >> i) & 1) != 0) {\n"
"            float3 t = mul(u_texmatrix[i], float4(v.a_normal, 1.0)).xyz;\n"
"            if (((u_tg2.z >> i) & 1) != 0) t.z = 1.0;\n"
"            if (((u_tg.w >> i) & 1) != 0 && dot(t, t) > 0.0) t = normalize(t);\n"
"            if (((u_tg2.x >> i) & 1) != 0) t = mul(u_tg_post[i], float4(t, 1.0)).xyz;\n"
"            o.v_tcq[i] = t;\n"
"            if (u_mode.x == 0) vt = v.a_texcoord0;\n"
"        } else\n"
"            o.v_tcq[i] = float3(vt, 1.0);\n"
"        if (i == 0) tc0 = vt;\n"
"    }\n"
"    o.v_texcoord0 = tc0;\n"
"    o.v_uvrect = float4(-1e30, -1e30, 1e30, 1e30);\n"
"    if (u_mode.w != 0 && (u_tg.x & 1) == 0 && (u_tg.y & 3) == 0) {\n"
"        float2 c0 = mul(u_texmatrix[0], float4(v.a_uvrect.xy, 0.0, 1.0)).xy;\n"
"        float2 c1 = mul(u_texmatrix[0], float4(v.a_uvrect.zy, 0.0, 1.0)).xy;\n"
"        float2 c2 = mul(u_texmatrix[0], float4(v.a_uvrect.xw, 0.0, 1.0)).xy;\n"
"        float2 c3 = mul(u_texmatrix[0], float4(v.a_uvrect.zw, 0.0, 1.0)).xy;\n"
"        o.v_uvrect = float4(min(min(c0, c1), min(c2, c3)), max(max(c0, c1), max(c2, c3)));\n"
"    }\n"
"    return o;\n"
"}\n"
/* ---- pixel shader ---- */
"static float2 g_uvh;\n"
"static float4 g_uvrect;\n"
"float2 gx_uvclamp(float2 uv) {\n"
"    float2 mid = 0.5 * (g_uvrect.xy + g_uvrect.zw);\n"
"    float2 r = clamp(uv, min(g_uvrect.xy + g_uvh, mid), max(g_uvrect.zw - g_uvh, mid));\n"
"    return (u_mode.w == 0) ? uv : r;\n"
"}\n"
"float2 gx_tcq(float3 q) { return q.xy / ((abs(q.z) > 1e-6) ? q.z : 1e-6); }\n"
"float4 gx_fog(float4 c, float eyez) {\n"
"    if (u_fog.x >= 0.5) {\n"
"    int ft = int(u_fog.x + 0.5) & 7;\n"
"    float t = clamp((eyez - u_fog.y) / max(u_fog.z - u_fog.y, 1e-4), 0.0, 1.0);\n"
"    float f = t;\n"
"    if (ft == 4) f = 1.0 - exp2(-8.0 * t);\n"
"    else if (ft == 5) f = 1.0 - exp2(-8.0 * t * t);\n"
"    else if (ft == 6) f = exp2(-8.0 * (1.0 - t));\n"
"    else if (ft == 7) f = exp2(-8.0 * (1.0 - t) * (1.0 - t));\n"
"    c.rgb = lerp(c.rgb, u_fog_color.rgb, clamp(f, 0.0, 1.0));\n"
"    }\n"
"    return c;\n"
"}\n"
/* gradients come from the unmodified texcoord (computed outside the TEV
 * loop: implicit-derivative sampling is not allowed in a dynamic loop) */
"float4 gx_tex(int m, float2 uv, float2 dx, float2 dy) {\n"
"    float4 r = float4(1.0, 1.0, 1.0, 1.0);\n"
"    if (m >= 0 && m <= 7 && (u_tg2.y & (1 << m)) != 0) {\n"
"    [branch] switch (m) {\n"
"    case 0: r = u_tex0.SampleGrad(u_smp0, uv, dx, dy); break;\n"
"    case 1: r = u_tex1.SampleGrad(u_smp1, uv, dx, dy); break;\n"
"    case 2: r = u_tex2.SampleGrad(u_smp2, uv, dx, dy); break;\n"
"    case 3: r = u_tex3.SampleGrad(u_smp3, uv, dx, dy); break;\n"
"    case 4: r = u_tex4.SampleGrad(u_smp4, uv, dx, dy); break;\n"
"    case 5: r = u_tex5.SampleGrad(u_smp5, uv, dx, dy); break;\n"
"    case 6: r = u_tex6.SampleGrad(u_smp6, uv, dx, dy); break;\n"
"    default: r = u_tex7.SampleGrad(u_smp7, uv, dx, dy); break;\n"
"    }\n"
"    }\n"
"    return r;\n"
"}\n"
"float2 gx_tsize(int m) {\n"
"    uint w = 1, h = 1;\n"
"    [branch] switch (m) {\n"
"    case 0: u_tex0.GetDimensions(w, h); break; case 1: u_tex1.GetDimensions(w, h); break;\n"
"    case 2: u_tex2.GetDimensions(w, h); break; case 3: u_tex3.GetDimensions(w, h); break;\n"
"    case 4: u_tex4.GetDimensions(w, h); break; case 5: u_tex5.GetDimensions(w, h); break;\n"
"    case 6: u_tex6.GetDimensions(w, h); break; default: u_tex7.GetDimensions(w, h); break;\n"
"    }\n"
"    return float2(w, h);\n"
"}\n"
"float gx_iwrap(float c, int w) {\n"
"    float t = float(512 >> clamp(w, 1, 5));\n"
"    float r = c - t * floor(c / t);\n"
"    return (w == 0) ? c : ((w >= 6) ? 0.0 : r);\n"
"}\n"
"static float2 g_tc[8], g_tdx[8], g_tdy[8];\n"
/* constant-index selects: a dynamically indexed array goes to local
 * memory (~2.5x the pixel cost at 4K with MSAA); selects stay in registers */
"#define GX_SEL(a, c) ((c) == 0 ? a[0] : (c) == 1 ? a[1] : (c) == 2 ? a[2] : (c) == 3 ? a[3] : \\\n"
"                      (c) == 4 ? a[4] : (c) == 5 ? a[5] : (c) == 6 ? a[6] : a[7])\n"
"float2 gx_ind_uv(int s, int m, float2 uv, inout float2 acc) {\n"
"    float2 sz = max(gx_tsize(m), float2(1.0, 1.0));\n"
"    float2 fc = uv * sz;\n"
"    float2 res = uv;\n"
"    if (((u_misc.y >> s) & 1) == 0) { acc = fc; } else {\n"
"    int4 ti = u_gx_tind[s];\n"
"    int bt = ti.x & 3, fmt = (ti.x >> 2) & 3, bias = (ti.x >> 4) & 7;\n"
"    float3 ind = float3(0.0, 0.0, 0.0);\n"
"    if (bt < u_misc.z) {\n"
"        int4 io = u_gx_iord[bt];\n"
"        float2 isc = float2(float(1 << io.z), float(1 << io.w));\n"
"        float2 ic = GX_SEL(g_tc, io.x) / isc;\n"
"        int3 ii = (int3)floor(gx_tex(io.y, ic, GX_SEL(g_tdx, io.x) / isc, GX_SEL(g_tdy, io.x) / isc).abg * 255.0 + 0.5);\n"
"        int mask = (fmt == 0) ? 255 : (fmt == 1) ? 31 : (fmt == 2) ? 15 : 7;\n"
"        int badd = (fmt == 0) ? -128 : 1;\n"
"        ii &= int3(mask, mask, mask);\n"
"        if ((bias & 1) != 0) ii.x += badd;\n"
"        if ((bias & 2) != 0) ii.y += badd;\n"
"        if ((bias & 4) != 0) ii.z += badd;\n"
"        ind = float3(ii);\n"
"    }\n"
"    float2 tr = float2(0.0, 0.0);\n"
"    int mid = ti.y;\n"
"    if (mid >= 1 && mid <= 3) {\n"
"        float4 r0 = u_gx_imtx[(mid - 1) * 2], r1 = u_gx_imtx[(mid - 1) * 2 + 1];\n"
"        tr = float2(dot(r0.xyz, ind), dot(r1.xyz, ind)) * exp2(r0.w);\n"
"    } else if ((mid >= 5 && mid <= 7) || (mid >= 9 && mid <= 11)) {\n"
"        float e = u_gx_imtx[((mid & 3) - 1) * 2].w;\n"
"        tr = fc * ((mid < 8) ? ind.x : ind.y) / 256.0 * exp2(e);\n"
"    }\n"
"    float2 wc = float2(gx_iwrap(fc.x, ti.z & 7), gx_iwrap(fc.y, (ti.z >> 3) & 7));\n"
"    acc = (ti.w != 0) ? acc + wc + tr : wc + tr;\n"
"    res = acc / sz;\n"
"    }\n"
"    return res;\n"
"}\n"
"float3 gx_cin(int i, float4 r0, float4 r1, float4 r2, float4 r3, float4 t, float4 ras, float4 k) {\n"
"    float3 v = float3(0.0, 0.0, 0.0);\n"
"    if (i == 0) v = r0.rgb; else if (i == 1) v = r0.aaa;\n"
"    else if (i == 2) v = r1.rgb; else if (i == 3) v = r1.aaa;\n"
"    else if (i == 4) v = r2.rgb; else if (i == 5) v = r2.aaa;\n"
"    else if (i == 6) v = r3.rgb; else if (i == 7) v = r3.aaa;\n"
"    else if (i == 8) v = t.rgb; else if (i == 9) v = t.aaa;\n"
"    else if (i == 10) v = ras.rgb; else if (i == 11) v = ras.aaa;\n"
"    else if (i == 12) v = float3(1.0, 1.0, 1.0); else if (i == 13) v = float3(0.5, 0.5, 0.5);\n"
"    else if (i == 14) v = k.rgb;\n"
"    return v;\n"
"}\n"
"float gx_ain(int i, float4 r0, float4 r1, float4 r2, float4 r3, float4 t, float4 ras, float4 k) {\n"
"    float v = 0.0;\n"
"    if (i == 0) v = r0.a; else if (i == 1) v = r1.a; else if (i == 2) v = r2.a;\n"
"    else if (i == 3) v = r3.a; else if (i == 4) v = t.a; else if (i == 5) v = ras.a;\n"
"    else if (i == 6) v = k.a;\n"
"    return v;\n"
"}\n"
"float gx_bias(int b) { return b == 1 ? 0.5 : (b == 2 ? -0.5 : 0.0); }\n"
"float gx_scale(int s) { return s == 1 ? 2.0 : (s == 2 ? 4.0 : (s == 3 ? 0.5 : 1.0)); }\n"
"bool gx_acmp(int f, float a, float ref) {\n"
"    bool r = true;\n"
"    if (f == 0) r = false; else if (f == 1) r = a < ref; else if (f == 2) r = a == ref;\n"
"    else if (f == 3) r = a <= ref; else if (f == 4) r = a > ref; else if (f == 5) r = a != ref;\n"
"    else if (f == 6) r = a >= ref;\n"
"    return r;\n"
"}\n"
"bool gx_alpha_pass(float a8) {\n"
"    bool p0 = gx_acmp(u_gx_acmp.x, a8, float(u_gx_acmp.w & 255));\n"
"    bool p1 = gx_acmp(u_gx_acmp.y, a8, float((u_gx_acmp.w >> 8) & 255));\n"
"    return (u_gx_acmp.z == 0) ? (p0 && p1) : (u_gx_acmp.z == 1) ? (p0 || p1) :\n"
"           (u_gx_acmp.z == 2) ? (p0 != p1) : (p0 == p1);\n"
"}\n"
"float4 gx_swap(float4 v, int sw) {\n"
"    return float4(v[sw & 3], v[(sw >> 2) & 3], v[(sw >> 4) & 3], v[(sw >> 6) & 3]);\n"
"}\n"
"float4 gx_tev(float4 v_color) {\n"
"    float4 r0 = u_gx_reg[0], r1 = u_gx_reg[1], r2 = u_gx_reg[2], r3 = u_gx_reg[3];\n"
"    float4 last = v_color;\n"
"    float2 iacc = float2(0.0, 0.0);\n"
"    [loop] for (int s = 0; s < u_misc.x; s++) {\n"
"        int4 o = u_gx_ord[s];\n"
"        float2 uv = GX_SEL(g_tc, o.x), dx = GX_SEL(g_tdx, o.x), dy = GX_SEL(g_tdy, o.x);\n"
"        if (o.x == 0 && ((u_misc.y >> s) & 1) == 0) uv = gx_uvclamp(uv);\n"
"        if (u_misc.y != 0 && o.y >= 0) {\n"
/* an indirect stage samples with the LOD of its offset coordinate
 * (utcLod = GX_FALSE in all the game's calls), like the GL path's
 * implicit-LOD texture(); the unmodified coordinate's gradients picked too
 * fine a mip for warped surfaces (the lava came out sharper / noisier
 * than on GL and Dolphin) */
"            uv = gx_ind_uv(s, o.y, uv, iacc);\n"
"            if (((u_misc.y >> s) & 1) != 0) { dx = ddx(uv); dy = ddy(uv); }\n"
"        }\n"
"        float4 t = (o.y >= 0) ? gx_tex(o.y, uv, dx, dy) : float4(1.0, 1.0, 1.0, 1.0);\n"
"        float4 ras = (o.z != 0) ? v_color : float4(0.0, 0.0, 0.0, 0.0);\n"
"        if (o.w != 0) {  /* TEV swap tables (gx_core gxfx_swap_bits) */\n"
"            t = gx_swap(t, (o.w & 255) ^ 228);\n"
"            ras = gx_swap(ras, ((o.w >> 8) & 255) ^ 228);\n"
"        }\n"
"        float4 k = u_gx_konst[s];\n"
"        int4 ci = u_gx_cin[s], ai = u_gx_ain[s], co = u_gx_cop[s], ao = u_gx_aop[s];\n"
"        float3 ca = gx_cin(ci.x, r0, r1, r2, r3, t, ras, k);\n"
"        float3 cb = gx_cin(ci.y, r0, r1, r2, r3, t, ras, k);\n"
"        float3 cc = gx_cin(ci.z, r0, r1, r2, r3, t, ras, k);\n"
"        float3 cd = gx_cin(ci.w, r0, r1, r2, r3, t, ras, k);\n"
"        float aa = gx_ain(ai.x, r0, r1, r2, r3, t, ras, k);\n"
"        float ab = gx_ain(ai.y, r0, r1, r2, r3, t, ras, k);\n"
"        float ac = gx_ain(ai.z, r0, r1, r2, r3, t, ras, k);\n"
"        float ad = gx_ain(ai.w, r0, r1, r2, r3, t, ras, k);\n"
"        float3 cm = lerp(ca, cb, cc);\n"
"        float am = lerp(aa, ab, ac);\n"
"        float3 cr = (cd + ((co.x == 1) ? -cm : cm) + gx_bias(co.y)) * gx_scale(co.z);\n"
"        float ar = (ad + ((ao.x == 1) ? -am : am) + gx_bias(ao.y)) * gx_scale(ao.z);\n"
"        cr = ((co.w & 1) != 0) ? clamp(cr, 0.0, 1.0) : clamp(cr, -4.0, 4.0);\n"
"        ar = ((ao.w & 1) != 0) ? clamp(ar, 0.0, 1.0) : clamp(ar, -4.0, 4.0);\n"
"        int cdst = co.w >> 1, adst = ao.w >> 1;\n"
"        if (cdst == 0) r0.rgb = cr; else if (cdst == 1) r1.rgb = cr;\n"
"        else if (cdst == 2) r2.rgb = cr; else r3.rgb = cr;\n"
"        if (adst == 0) r0.a = ar; else if (adst == 1) r1.a = ar;\n"
"        else if (adst == 2) r2.a = ar; else r3.a = ar;\n"
"        last = float4(cr, ar);\n"
"    }\n"
"    last = clamp(last, 0.0, 1.0);\n"
"    {\n"
"        float a8 = floor(last.a * 255.0 + 0.5);\n"
"        if (!gx_alpha_pass(a8)) discard;\n"
"    }\n"
"    return last;\n"
"}\n"
"float4 PSMain(VSOut i) : SV_Target {\n"
"    g_uvrect = i.v_uvrect;\n"
"    float2 dx0 = ddx(i.v_texcoord0), dy0 = ddy(i.v_texcoord0);\n"
"    g_uvh = float2(0.0, 0.0);\n"
"    if (u_mode.w != 0)\n"
"        g_uvh = 0.5 * (abs(dx0) * u_efb_scale.x + abs(dy0) * u_efb_scale.y);\n"
"    int tcmask = 0;\n"   /* texcoords the TEV / indirect stages read (uniform) */
"    if (u_mode.x != 0 && u_mode.y == 0) {\n"
"        [loop] for (int s = 0; s < u_misc.x; s++) tcmask |= 1 << clamp(u_gx_ord[s].x, 0, 7);\n"
"        [loop] for (int b = 0; b < u_misc.z; b++) tcmask |= 1 << clamp(u_gx_iord[b].x, 0, 7);\n"
"    }\n"
"    [unroll] for (int c = 0; c < 8; c++) {\n"
"        g_tc[c] = float2(0.0, 0.0); g_tdx[c] = g_tc[c]; g_tdy[c] = g_tc[c];\n"
"        [branch] if ((tcmask >> c) & 1) {\n"
"            g_tc[c] = gx_tcq(i.v_tcq[c]);\n"
"            g_tdx[c] = ddx(g_tc[c]);\n"
"            g_tdy[c] = ddy(g_tc[c]);\n"
"        }\n"
"    }\n"
"    float4 tex = u_tex0.Sample(u_smp0, gx_uvclamp(i.v_texcoord0));\n"
"    float4 frag_color = i.v_color;\n"
"    if (u_mode.x != 0 && u_mode.y == 0) {\n"
"#if DEBUGVIS == 1\n"
"        frag_color = float4(1, 0, 1, 1);\n"
"#else\n"
"        frag_color = gx_tev(i.v_color);\n"
"#endif\n"
"    } else if (u_mode.z != 0) {\n"
"        if (u_mode.y != 0) {\n"
"            frag_color = clamp(tex * u_tev_c0 + u_tev_c1, 0.0, 1.0);\n"
"            if (!gx_alpha_pass(floor(frag_color.a * 255.0 + 0.5))) discard;\n"
"        } else\n"
"            frag_color *= tex;\n"
"    }\n"
"    return gx_fog(frag_color, i.v_eyez);\n"
"}\n"
/* ---- F1 overlay restore: full-screen triangle sampling the capture ---- */
"cbuffer BlitC : register(b0) { float4 u_blit; };\n"
"Texture2D u_cap : register(t0);\n"
"SamplerState u_capsmp : register(s0);\n"
"struct BlitOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"BlitOut BlitVS(uint id : SV_VertexID) {\n"
"    BlitOut o;\n"
"    float2 p = float2((id << 1) & 2, id & 2);\n"
"    o.uv = float2(p.x, 1.0 - p.y);\n"
"    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"    return o;\n"
"}\n"
"float4 BlitPS(BlitOut i) : SV_Target {\n"
"    return float4(u_cap.Sample(u_capsmp, i.uv).rgb * u_blit.x, 1.0);\n"
"}\n"
/* ---- EFB copy: area resample of the resolved target on the GPU (exactly
 * GXCopyTex's CPU loop: box [x*W/dw, (x+1)*W/dw) x [y*H/dh, ...), at least
 * one texel, integer mean; texels outside the target count as 0) ---- */
"cbuffer BoxC : register(b0) { int4 u_box0; int4 u_box1; int4 u_box2; };\n"
"Texture2D<float4> u_boxsrc : register(t0);\n"
"uint4 box_mean(int ox, int oy) {\n"
"    int W = u_box0.z, H = u_box0.w, dw = u_box1.x, dh = u_box1.y;\n"
"    int xa = ox * W / dw, xb = (ox + 1) * W / dw;\n"
"    int ya = oy * H / dh, yb = (oy + 1) * H / dh;\n"
"    uint4 acc = uint4(0, 0, 0, 0); uint n = 0;\n"
"    if (xb <= xa) xb = xa + 1;\n"
"    if (yb <= ya) yb = ya + 1;\n"
"    xb = min(xb, W); yb = min(yb, H);\n"
"    for (int yy = ya; yy < yb; yy++)\n"
"    for (int xx = xa; xx < xb; xx++) {\n"
"        int sx = u_box0.x + xx, sy = u_box0.y + yy;\n"
"        if (sx >= 0 && sy >= 0 && sx < u_box1.z && sy < u_box1.w)\n"
"            acc += (uint4)round(u_boxsrc.Load(int3(sx, sy, 0)) * 255.0);\n"
"        n++;\n"
"    }\n"
"    return n ? acc / n : uint4(0, 0, 0, 0);\n"
"}\n"
"uint4 BoxPS(BlitOut i) : SV_Target {\n"
"    return box_mean((int)i.pos.x, (int)i.pos.y);\n"
"}\n"
/* copy format cf written to GC memory and read back through its natural
 * sample format, per texel (gx_core.c tex_copy_texel + gx_decode_texture;
 * the EFB has no alpha, so a = 255) */
"uint4 copy_conv(int cf, uint4 p) {\n"
"    uint r = p.x, g = p.y, b = p.z, a = 255;\n"
"    uint l = min(255u, (66 * r + 129 * g + 25 * b + 128) / 256 + 16);\n"
"    uint v = 0;\n"
"    if (cf == 0x00) { v = (l >> 4) * 17; return uint4(v, v, v, v); }\n"
"    if (cf == 0x01) return uint4(l, l, l, l);\n"
"    if (cf == 0x02) { v = (l >> 4) * 17; return uint4(v, v, v, (a >> 4) * 17); }\n"
"    if (cf == 0x03) return uint4(l, l, l, a);\n"
"    if (cf == 0x20) { v = (r >> 4) * 17; return uint4(v, v, v, v); }\n"
"    if (cf == 0x28) return uint4(r, r, r, r);\n"
"    if (cf == 0x29) return uint4(g, g, g, g);\n"
"    if (cf == 0x2A) return uint4(b, b, b, b);\n"
"    if (cf == 0x27) return uint4(a, a, a, a);\n"
"    if (cf == 0x22) { v = (r >> 4) * 17; return uint4(v, v, v, (a >> 4) * 17); }\n"
"    if (cf == 0x23) return uint4(r, r, r, a);\n"
"    if (cf == 0x2B) return uint4(r, r, r, g);\n"
"    if (cf == 0x2C) return uint4(g, g, g, b);\n"
"    if (cf == 0x04) {\n"
"        uint r5 = r >> 3, g6 = g >> 2, b5 = b >> 3;\n"
"        return uint4((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2), 255);\n"
"    }\n"
"    if (cf == 0x05) {\n"   /* a = 255 >= 0xE0: the RGB555 branch */
"        uint r5 = r >> 3, g5 = g >> 3, b5 = b >> 3;\n"
"        return uint4((r5 << 3) | (r5 >> 2), (g5 << 3) | (g5 >> 2), (b5 << 3) | (b5 >> 2), 255);\n"
"    }\n"
"    return uint4(r, g, b, a);\n"   /* 0x06 / 0x26 RGBA8 */
"}\n"
"struct CopyOut { uint4 raw : SV_Target0; float4 conv : SV_Target1; };\n"
"CopyOut CopyPS(BlitOut i) {\n"
"    CopyOut o;\n"
"    o.raw = box_mean((int)i.pos.x, (int)i.pos.y);\n"
"    o.conv = float4(copy_conv(u_box2.x, o.raw)) / 255.0;\n"
"    return o;\n"
"}\n";

/* --------------------------------------------------------------- loading */
typedef HRESULT (WINAPI* PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
typedef HRESULT (WINAPI* PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(const D3D12_ROOT_SIGNATURE_DESC*,
                                                             D3D_ROOT_SIGNATURE_VERSION,
                                                             ID3DBlob**, ID3DBlob**);
static PFN_D3D12_CREATE_DEVICE p_D3D12CreateDevice;
static PFN_D3D12_GET_DEBUG_INTERFACE p_D3D12GetDebugInterface;
static PFN_D3D12_SERIALIZE_ROOT_SIGNATURE p_D3D12SerializeRootSignature;
static PFN_CreateDXGIFactory2 p_CreateDXGIFactory2;
static pD3DCompile p_D3DCompile;

#define NFRAMES 2
#define NSH 6                              /* compiled shader entry points */
#define NBACK 6                            /* back buffers at most (D.nback in use) */
#define NBACK_DEFAULT 3
#define SEG_SIZE ((size_t)32 << 20)        /* upload ring bytes per frame slot */
#define SRV_SEG 65536                      /* shader-visible SRVs per frame slot */
#define SMP_HEAP 2048                      /* shader-visible samplers */
#define CPU_SRV_CHUNK 4096
#define MAX_UNIQUE_SAMPLERS 255

typedef struct DxTex {
    ID3D12Resource* res;
    int w, h, mips;
    D3D12_CPU_DESCRIPTOR_HANDLE srv;
    unsigned version;           /* bumps when res changes */
    /* sampler state (GL texture-object parameters) */
    uint8_t wrapS, wrapT, minf, magf, mipmap, aniso;
    float minlod, maxlod, bias;
    int samp;                   /* unique sampler id, -1 = recompute */
    int rt;                     /* res allows RENDER_TARGET (copy_tex target) */
} DxTex;

typedef struct { IUnknown* obj; UINT64 fence; } Deferred;

static struct {
    int ok;
    int width, height;          /* drawable / target size */
    int msaa;
    int vsync, tearing;
    int debug;
    HWND hwnd;
    IDXGIFactory4* factory;
    ID3D12Device* dev;
    ID3D12CommandQueue* queue;
    IDXGISwapChain3* swap;
    ID3D12CommandAllocator* alloc[NFRAMES];
    ID3D12GraphicsCommandList* cl;
    ID3D12Fence* fence;
    HANDLE fence_event;
    UINT64 fence_value;
    UINT64 frame_fence[NFRAMES];
    int slot;
    ID3D12InfoQueue* infoq;
    IDXGIAdapter3* adapter3;          /* --perf video memory log */

    /* targets */
    ID3D12DescriptorHeap* rtv_heap;   /* 0..NBACK-1 back buffers, NBACK main rt */
    ID3D12DescriptorHeap* dsv_heap;
    UINT rtv_inc, srv_inc, smp_inc;
    ID3D12Resource* bb[NBACK];
    int nback;                        /* back buffers in use (present_queue) */
    int si;                           /* sync interval of the next presents (present_interval) */
    ID3D12Resource* rt;               /* main colour target (MSAA = msaa) */
    ID3D12Resource* ds;
    ID3D12Resource* rt_res;           /* resolved copy (msaa > 1) */
    D3D12_RESOURCE_STATES rt_state, rt_res_state;
    ID3D12Resource* cap;              /* F1 overlay capture */
    int cap_w, cap_h, cap_ok;
    D3D12_CPU_DESCRIPTOR_HANDLE cap_srv;
    ID3D12Resource* readback;         /* READBACK heap buffer */
    size_t readback_size;

    /* pipeline */
    ID3D12RootSignature* rootsig;
    ID3D12RootSignature* blit_rootsig;
    D3D12_SHADER_BYTECODE sh[NSH];    /* VSMain, PSMain, BlitVS, BlitPS, BoxPS, CopyPS */
    ID3D12PipelineState* pso[1 << 16];
    ID3D12PipelineState* blit_pso;
    /* GPU EFB-copy resample (dx_read_pixels_box) */
    ID3D12RootSignature* box_rootsig;
    ID3D12PipelineState* box_pso;
    ID3D12PipelineState* copy_pso;    /* async GXCopyTex: raw (UINT) + converted (UNORM) */
    ID3D12DescriptorHeap* box_rtv_heap;   /* 0 box_rt, 1 copy_tex target */
    ID3D12Resource* box_rt;
    int box_w, box_h;
    D3D12_CPU_DESCRIPTOR_HANDLE box_srv;
    ID3D12PipelineState* cur_pso;
    int cur_topo;                     /* D3D_PRIMITIVE_TOPOLOGY last set */

    /* upload ring */
    ID3D12Resource* upload;
    uint8_t* upload_ptr;
    D3D12_GPU_VIRTUAL_ADDRESS upload_va;
    size_t seg_off;

    /* static buffers */
    ID3D12Resource* ibuf;             /* quad indices then fan indices (u16) */
    D3D12_INDEX_BUFFER_VIEW ibv;
    UINT fan_first;                   /* index offset of the fan list */
    ID3D12Resource* zero_vb;          /* 16 zero bytes: rect stream when unused */

    /* descriptors */
    ID3D12DescriptorHeap* srv_gpu;    /* shader visible, NFRAMES segments */
    UINT srv_gpu_off;                 /* within the current segment */
    ID3D12DescriptorHeap** srv_cpu;   /* chain of CPU-only heaps */
    int srv_cpu_n, srv_cpu_used;
    D3D12_CPU_DESCRIPTOR_HANDLE* srv_free;   /* slots of destroyed textures */
    int n_srv_free, cap_srv_free;
    ID3D12DescriptorHeap* smp_cpu;    /* unique samplers (CPU only) */
    ID3D12DescriptorHeap* smp_gpu;    /* shader visible tables */
    struct { D3D12_SAMPLER_DESC d; } usamp[MAX_UNIQUE_SAMPLERS];
    int n_usamp;
    struct { uint64_t key; UINT off; int used; } stab[512];
    int n_stab;
    DxTex* dummy;                     /* 1x1 (0,0,0,1): GL's incomplete texture */

    /* GX-facing state */
    DxTex* bound[8];
    unsigned bound_ver[8];
    int tables_dirty;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_table, smp_table;
    int vp[4], sc[4], sc_on;          /* GL coords */
    int blend_en, blend_src, blend_dst, blend_sub;
    int zfunc, zwrite, cull;
    GxDrawConsts last_dc; int last_dc_ok; D3D12_GPU_VIRTUAL_ADDRESS dc_va;
    GxTevConsts last_tc; int last_tc_ok; unsigned last_tc_serial; D3D12_GPU_VIRTUAL_ADDRESS tc_va;
    int root_dirty;

    Deferred* def; int n_def, cap_def;
    unsigned char* frame_rgb; size_t frame_rgb_size;
    long frames;
    /* GPU time per presented frame (timestamps at slot begin / present) */
    ID3D12QueryHeap* qheap; ID3D12Resource* qread; UINT64 qfreq; int q_ok;
    unsigned q_pid[NFRAMES];          /* SMB2_PACE_LOG: present id of the slot's frame */
    UINT64 cal_gpu, cal_cpu;          /* GetClockCalibration: GPU timestamp <-> QPC */
    unsigned long long gpu_end_ns;    /* latest finished frame's GPU end (platform clock) */
    int q_valid[NFRAMES]; double gpu_ms_acc; long gpu_ms_n;
} D;

/* ---------------------------------------------------------------- errors */
static void dx_check_removed(HRESULT hr, const char* what) {
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
        hr == DXGI_ERROR_DEVICE_HUNG) {
        HRESULT why = D.dev ? ID3D12Device_GetDeviceRemovedReason(D.dev) : hr;
        fprintf(stderr, "[d3d12] device removed during %s (hr=0x%08lx, reason=0x%08lx) - exiting\n",
                what, (unsigned long)hr, (unsigned long)why);
        fflush(NULL);
        exit(3);
    }
}

static void dx_dump_messages(void) {
    UINT64 n, i;
    if (!D.infoq) return;
    n = ID3D12InfoQueue_GetNumStoredMessages(D.infoq);
    for (i = 0; i < n && i < 64; i++) {
        SIZE_T len = 0;
        D3D12_MESSAGE* m;
        ID3D12InfoQueue_GetMessage(D.infoq, i, NULL, &len);
        m = (D3D12_MESSAGE*)malloc(len);
        if (!m) break;
        if (SUCCEEDED(ID3D12InfoQueue_GetMessage(D.infoq, i, m, &len)))
            fprintf(stderr, "[d3d12-debug] sev %d id %d: %.*s\n", (int)m->Severity, (int)m->ID,
                    (int)m->DescriptionByteLength, m->pDescription);
        free(m);
    }
    ID3D12InfoQueue_ClearStoredMessages(D.infoq);
}

/* ------------------------------------------------------------ helpers */
static void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b;
    if (before == after) return;
    memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(D.cl, 1, &b);
}

static void rt_to(D3D12_RESOURCE_STATES s) {
    barrier(D.rt, D.rt_state, s);
    D.rt_state = s;
}

static void defer_release(IUnknown* o) {
    if (!o) return;
    if (D.n_def == D.cap_def) {
        int nc = D.cap_def ? D.cap_def * 2 : 64;
        Deferred* nd = (Deferred*)realloc(D.def, (size_t)nc * sizeof(Deferred));
        if (!nd) { IUnknown_Release(o); return; }  /* leak-safe fallback */
        D.def = nd; D.cap_def = nc;
    }
    D.def[D.n_def].obj = o;
    D.def[D.n_def].fence = D.fence_value + 1;   /* released after the next signal */
    D.n_def++;
}

static void process_deferred(void) {
    UINT64 done = ID3D12Fence_GetCompletedValue(D.fence);
    int i, j = 0;
    for (i = 0; i < D.n_def; i++) {
        if (D.def[i].fence <= done) IUnknown_Release(D.def[i].obj);
        else D.def[j++] = D.def[i];
    }
    D.n_def = j;
}

static void wait_fence(UINT64 v) {
    if (ID3D12Fence_GetCompletedValue(D.fence) >= v) return;
    ID3D12Fence_SetEventOnCompletion(D.fence, v, D.fence_event);
    while (WaitForSingleObject(D.fence_event, 2000) == WAIT_TIMEOUT) {
        dx_check_removed(ID3D12Device_GetDeviceRemovedReason(D.dev), "fence wait");
    }
}

static D3D12_CPU_DESCRIPTOR_HANDLE heap_cpu(ID3D12DescriptorHeap* h, UINT idx, UINT inc) {
    D3D12_CPU_DESCRIPTOR_HANDLE c = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(h);
    c.ptr += (SIZE_T)idx * inc;
    return c;
}
static D3D12_GPU_DESCRIPTOR_HANDLE heap_gpu(ID3D12DescriptorHeap* h, UINT idx, UINT inc) {
    D3D12_GPU_DESCRIPTOR_HANDLE g = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(h);
    g.ptr += (UINT64)idx * inc;
    return g;
}

static ID3D12DescriptorHeap* make_heap(D3D12_DESCRIPTOR_HEAP_TYPE t, UINT n, int shader_visible) {
    D3D12_DESCRIPTOR_HEAP_DESC d;
    ID3D12DescriptorHeap* h = NULL;
    memset(&d, 0, sizeof d);
    d.Type = t;
    d.NumDescriptors = n;
    d.Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(ID3D12Device_CreateDescriptorHeap(D.dev, &d, &IID_ID3D12DescriptorHeap, (void**)&h)))
        return NULL;
    return h;
}

static D3D12_CPU_DESCRIPTOR_HANDLE alloc_cpu_srv(void) {
    D3D12_CPU_DESCRIPTOR_HANDLE none = {0};
    if (D.n_srv_free) return D.srv_free[--D.n_srv_free];
    if (D.srv_cpu_n == 0 || D.srv_cpu_used == CPU_SRV_CHUNK) {
        ID3D12DescriptorHeap** nh = (ID3D12DescriptorHeap**)realloc(
            D.srv_cpu, (size_t)(D.srv_cpu_n + 1) * sizeof(*nh));
        if (!nh) return none;
        D.srv_cpu = nh;
        D.srv_cpu[D.srv_cpu_n] = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, CPU_SRV_CHUNK, 0);
        if (!D.srv_cpu[D.srv_cpu_n]) return none;
        D.srv_cpu_n++;
        D.srv_cpu_used = 0;
    }
    return heap_cpu(D.srv_cpu[D.srv_cpu_n - 1], (UINT)D.srv_cpu_used++, D.srv_inc);
}

static ID3D12Resource* make_buffer(D3D12_HEAP_TYPE heap, size_t size, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp;
    D3D12_RESOURCE_DESC rd;
    ID3D12Resource* r = NULL;
    memset(&hp, 0, sizeof hp);
    hp.Type = heap;
    memset(&rd, 0, sizeof rd);
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(ID3D12Device_CreateCommittedResource(D.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, st,
                                                    NULL, &IID_ID3D12Resource, (void**)&r)))
        return NULL;
    return r;
}

static ID3D12Resource* make_tex2d(int w, int h, int mips, DXGI_FORMAT fmt, int samples,
                                  D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st,
                                  const D3D12_CLEAR_VALUE* clear) {
    D3D12_HEAP_PROPERTIES hp;
    D3D12_RESOURCE_DESC rd;
    ID3D12Resource* r = NULL;
    HRESULT hr;
    memset(&hp, 0, sizeof hp);
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    memset(&rd, 0, sizeof rd);
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)w;
    rd.Height = (UINT)h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = (UINT16)mips;
    rd.Format = fmt;
    rd.SampleDesc.Count = (UINT)samples;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = flags;
    hr = ID3D12Device_CreateCommittedResource(D.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, st, clear,
                                              &IID_ID3D12Resource, (void**)&r);
    if (FAILED(hr)) {
        dx_check_removed(hr, "CreateCommittedResource");
        fprintf(stderr, "[d3d12] texture %dx%d fmt %d create failed 0x%08lx\n", w, h, (int)fmt,
                (unsigned long)hr);
        return NULL;
    }
    return r;
}

/* ------------------------------------------------- command list lifecycle */
static void apply_frame_state(void);

/* Execute what has been recorded and wait for the GPU to go idle, then
 * reopen the list (mid-frame readback, ring overflow). */
long g_dx_midsync;   /* statistics: mid-frame GPU drains */
static void submit_and_wait(void) {
    ID3D12CommandList* lists[1];
    g_dx_midsync++;
    HRESULT hr = ID3D12GraphicsCommandList_Close(D.cl);
    if (FAILED(hr)) {
        dx_check_removed(hr, "Close");
        fprintf(stderr, "[d3d12] command list close failed 0x%08lx\n", (unsigned long)hr);
        dx_dump_messages();
    }
    lists[0] = (ID3D12CommandList*)D.cl;
    ID3D12CommandQueue_ExecuteCommandLists(D.queue, 1, lists);
    D.fence_value++;
    ID3D12CommandQueue_Signal(D.queue, D.fence, D.fence_value);
    wait_fence(D.fence_value);
    process_deferred();
    ID3D12CommandAllocator_Reset(D.alloc[D.slot]);
    ID3D12GraphicsCommandList_Reset(D.cl, D.alloc[D.slot], NULL);
    D.seg_off = 0;
    D.srv_gpu_off = 0;
    apply_frame_state();
    if (D.debug) dx_dump_messages();
}

static void* ring_alloc(size_t size, size_t align, D3D12_GPU_VIRTUAL_ADDRESS* va) {
    size_t off = (D.seg_off + align - 1) & ~(align - 1);
    if (off + size > SEG_SIZE) {
        if (size > SEG_SIZE) return NULL;
        submit_and_wait();
        off = 0;
    }
    D.seg_off = off + size;
    off += (size_t)D.slot * SEG_SIZE;
    *va = D.upload_va + off;
    return D.upload_ptr + off;
}

/* ---------------------------------------------------------- frame state */
static D3D12_CPU_DESCRIPTOR_HANDLE rt_rtv(void) { return heap_cpu(D.rtv_heap, NBACK, D.rtv_inc); }
static D3D12_CPU_DESCRIPTOR_HANDLE rt_dsv(void) {
    return ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(D.dsv_heap);
}

static void apply_viewport(void) {
    D3D12_VIEWPORT v;
    v.TopLeftX = (float)D.vp[0];
    v.TopLeftY = (float)(D.height - (D.vp[1] + D.vp[3]));
    v.Width = (float)(D.vp[2] > 0 ? D.vp[2] : 1);
    v.Height = (float)(D.vp[3] > 0 ? D.vp[3] : 1);
    v.MinDepth = 0.0f;
    v.MaxDepth = 1.0f;
    ID3D12GraphicsCommandList_RSSetViewports(D.cl, 1, &v);
}

static void apply_scissor(void) {
    D3D12_RECT r;
    if (!D.sc_on) {
        r.left = 0; r.top = 0; r.right = D.width; r.bottom = D.height;
    } else {
        long x0 = D.sc[0], x1 = (long)D.sc[0] + D.sc[2];
        long y0 = D.height - ((long)D.sc[1] + D.sc[3]), y1 = D.height - (long)D.sc[1];
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > D.width) x1 = D.width;
        if (y1 > D.height) y1 = D.height;
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;
        r.left = x0; r.top = y0; r.right = x1; r.bottom = y1;
    }
    ID3D12GraphicsCommandList_RSSetScissorRects(D.cl, 1, &r);
}

static void apply_frame_state(void) {
    ID3D12DescriptorHeap* heaps[2];
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rt_rtv(), dsv = rt_dsv();
    heaps[0] = D.srv_gpu;
    heaps[1] = D.smp_gpu;
    ID3D12GraphicsCommandList_SetDescriptorHeaps(D.cl, 2, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(D.cl, D.rootsig);
    ID3D12GraphicsCommandList_OMSetRenderTargets(D.cl, 1, &rtv, FALSE, &dsv);
    ID3D12GraphicsCommandList_IASetIndexBuffer(D.cl, &D.ibv);
    apply_viewport();
    apply_scissor();
    D.cur_pso = NULL;
    D.cur_topo = -1;
    D.last_dc_ok = D.last_tc_ok = 0;
    D.tables_dirty = 1;
    D.root_dirty = 1;
}

/* ---------------------------------------------------------- PSO cache */
enum { PK_POINT = 1 << 15, PK_NOCLIP = 1 << 14 };

static D3D12_BLEND gx_blend_src(int f) {
    switch (f) {
    case GX_BL_ZERO: return D3D12_BLEND_ZERO;
    case GX_BL_ONE: return D3D12_BLEND_ONE;
    case GX_BL_DSTCLR: return D3D12_BLEND_DEST_COLOR;
    case GX_BL_INVDSTCLR: return D3D12_BLEND_INV_DEST_COLOR;
    case GX_BL_SRCALPHA: return D3D12_BLEND_SRC_ALPHA;
    case GX_BL_INVSRCALPHA: return D3D12_BLEND_INV_SRC_ALPHA;
    case GX_BL_DSTALPHA: return D3D12_BLEND_DEST_ALPHA;
    case GX_BL_INVDSTALPHA: return D3D12_BLEND_INV_DEST_ALPHA;
    default: return D3D12_BLEND_ONE;
    }
}
static D3D12_BLEND gx_blend_dst(int f) {
    switch (f) {
    case GX_BL_ZERO: return D3D12_BLEND_ZERO;
    case GX_BL_ONE: return D3D12_BLEND_ONE;
    case GX_BL_SRCCLR: return D3D12_BLEND_SRC_COLOR;
    case GX_BL_INVSRCCLR: return D3D12_BLEND_INV_SRC_COLOR;
    case GX_BL_SRCALPHA: return D3D12_BLEND_SRC_ALPHA;
    case GX_BL_INVSRCALPHA: return D3D12_BLEND_INV_SRC_ALPHA;
    case GX_BL_DSTALPHA: return D3D12_BLEND_DEST_ALPHA;
    case GX_BL_INVDSTALPHA: return D3D12_BLEND_INV_DEST_ALPHA;
    default: return D3D12_BLEND_ZERO;
    }
}
/* the depth buffer is reversed (see VSMain): nearer = greater */
static D3D12_COMPARISON_FUNC gx_cmp(int f) {
    switch (f) {
    case GX_NEVER: return D3D12_COMPARISON_FUNC_NEVER;
    case GX_LESS: return D3D12_COMPARISON_FUNC_GREATER;
    case GX_EQUAL: return D3D12_COMPARISON_FUNC_EQUAL;
    case GX_LEQUAL: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case GX_GREATER: return D3D12_COMPARISON_FUNC_LESS;
    case GX_NEQUAL: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case GX_GEQUAL: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    default: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

static ID3D12PipelineState* get_pso(unsigned key) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
    D3D12_INPUT_ELEMENT_DESC il[7];
    D3D12_RENDER_TARGET_BLEND_DESC* b;
    ID3D12PipelineState* p = D.pso[key];
    HRESULT hr;
    int cull = (key >> 12) & 3;
    if (p) return p;
    memset(il, 0, sizeof il);
    il[0].SemanticName = "POSITION"; il[0].Format = DXGI_FORMAT_R32G32B32_FLOAT; il[0].AlignedByteOffset = 0;
    il[1].SemanticName = "NORMAL"; il[1].Format = DXGI_FORMAT_R32G32B32_FLOAT; il[1].AlignedByteOffset = 12;
    il[2].SemanticName = "COLOR"; il[2].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; il[2].AlignedByteOffset = 24;
    il[3].SemanticName = "TEXCOORD"; il[3].SemanticIndex = 0; il[3].Format = DXGI_FORMAT_R32G32_FLOAT; il[3].AlignedByteOffset = 40;
    il[4].SemanticName = "TEXCOORD"; il[4].SemanticIndex = 1; il[4].Format = DXGI_FORMAT_R32G32_FLOAT; il[4].AlignedByteOffset = 48;
    il[5].SemanticName = "TEXCOORD"; il[5].SemanticIndex = 2; il[5].Format = DXGI_FORMAT_R32G32_FLOAT; il[5].AlignedByteOffset = 56;
    il[6].SemanticName = "TEXCOORD"; il[6].SemanticIndex = 4; il[6].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    il[6].InputSlot = 1; il[6].AlignedByteOffset = 0;

    memset(&d, 0, sizeof d);
    d.pRootSignature = D.rootsig;
    d.VS = D.sh[0];
    d.PS = D.sh[1];
    b = &d.BlendState.RenderTarget[0];
    b->BlendEnable = (key & 1) ? TRUE : FALSE;
    b->SrcBlend = gx_blend_src((key >> 1) & 7);
    b->DestBlend = gx_blend_dst((key >> 4) & 7);
    b->BlendOp = (key & 0x80) ? D3D12_BLEND_OP_REV_SUBTRACT : D3D12_BLEND_OP_ADD;
    b->SrcBlendAlpha = D3D12_BLEND_ONE;
    b->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    b->BlendOpAlpha = b->BlendOp;
    b->LogicOp = D3D12_LOGIC_OP_NOOP;
    b->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = 0xFFFFFFFFu;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = cull == GX_CULL_FRONT ? D3D12_CULL_MODE_FRONT :
                                 cull == GX_CULL_BACK ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    /* GL path: glFrontFace(GL_CW) */
    d.RasterizerState.FrontCounterClockwise = getenv("SMB2_D3D12_FLIPFACE") ? TRUE : FALSE;
    if (getenv("SMB2_D3D12_NOCULL")) d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = (key & PK_NOCLIP) ? FALSE : TRUE;
    d.RasterizerState.MultisampleEnable = D.msaa > 1;
    d.DepthStencilState.DepthEnable = TRUE;
    d.DepthStencilState.DepthWriteMask = (key & 0x800) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    d.DepthStencilState.DepthFunc = gx_cmp((key >> 8) & 7);
    if (getenv("SMB2_D3D12_NODEPTH")) d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    d.InputLayout.pInputElementDescs = il;
    d.InputLayout.NumElements = 7;
    d.PrimitiveTopologyType = (key & PK_POINT) ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
                                               : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    d.SampleDesc.Count = (UINT)D.msaa;
    {
        uint64_t t0 = SDL_GetPerformanceCounter();
        hr = ID3D12Device_CreateGraphicsPipelineState(D.dev, &d, &IID_ID3D12PipelineState, (void**)&p);
        if (g_perf_on) {   /* first use of a blend/depth/cull combination */
            static int n;
            fprintf(stderr, "[d3d12] PSO #%d key 0x%04x created in %.2f ms\n", ++n, key,
                    (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 /
                    (double)SDL_GetPerformanceFrequency());
        }
    }
    if (FAILED(hr)) {
        dx_check_removed(hr, "CreateGraphicsPipelineState");
        fprintf(stderr, "[d3d12] PSO 0x%04x creation failed 0x%08lx\n", key, (unsigned long)hr);
        dx_dump_messages();
        return NULL;
    }
    D.pso[key] = p;
    return p;
}

/* --------------------------------------------------------------- shaders */
static const char* const s_entries[NSH][2] = {
    { "VSMain", "vs_5_1" }, { "PSMain", "ps_5_1" }, { "BlitVS", "vs_5_1" }, { "BlitPS", "ps_5_1" },
    { "BoxPS", "ps_5_1" }, { "CopyPS", "ps_5_1" } };

static ID3DBlob* compile(const char* entry, const char* target) {
    ID3DBlob *code = NULL, *err = NULL;
    D3D_SHADER_MACRO defs[2];
    const char* dv = getenv("SMB2_D3D12_DEBUGVIS");
    HRESULT hr;
    defs[0].Name = "DEBUGVIS";
    defs[0].Definition = dv && dv[0] ? dv : "0";
    defs[1].Name = NULL;
    defs[1].Definition = NULL;
    hr = p_D3DCompile(s_hlsl, strlen(s_hlsl), "gx_uber.hlsl", defs, NULL, entry, target,
                              D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (err) {
        fprintf(stderr, "[d3d12] %s %s: %.*s\n", entry, FAILED(hr) ? "errors" : "warnings",
                (int)ID3D10Blob_GetBufferSize(err), (const char*)ID3D10Blob_GetBufferPointer(err));
        ID3D10Blob_Release(err);
    }
    if (FAILED(hr)) return NULL;
    return code;
}

/* Bytecode cache next to settings.dat: the uber shader takes ~1 s to
 * compile, so the blobs are kept keyed by a hash of the HLSL source + the
 * compile options (a changed shader just recompiles). */
static uint64_t shader_key(void) {
    const char* dv = getenv("SMB2_D3D12_DEBUGVIS");
    const char* parts[3];
    uint64_t k = 0xcbf29ce484222325ull;
    int i;
    parts[0] = s_hlsl;
    parts[1] = dv ? dv : "0";
    parts[2] = "v3 OPT1 sm5.1 nsh6";
    for (i = 0; i < 3; i++) {
        const unsigned char* p = (const unsigned char*)parts[i];
        for (; *p; p++) k = (k ^ *p) * 0x100000001b3ull;
        k = (k ^ 0xFF) * 0x100000001b3ull;
    }
    return k;
}

static void cache_path(char* out, size_t n) {
    snprintf(out, n, "%sd3d12_shader_cache.bin", platform_get_save_dir());
}

static int load_shaders(void) {
    char path[1200];
    uint64_t key = shader_key(), fkey = 0;
    FILE* f;
    int i, ok = 0;
    cache_path(path, sizeof path);
    f = fopen(path, "rb");
    if (f) {
        uint32_t len[NSH];
        if (fread(&fkey, 8, 1, f) == 1 && fkey == key && fread(len, 4, NSH, f) == NSH) {
            ok = 1;
            for (i = 0; i < NSH && ok; i++) {
                void* p = len[i] && len[i] < (64u << 20) ? malloc(len[i]) : NULL;
                if (!p || fread(p, 1, len[i], f) != len[i]) { free(p); ok = 0; break; }
                D.sh[i].pShaderBytecode = p;
                D.sh[i].BytecodeLength = len[i];
            }
        }
        fclose(f);
        if (ok) return 1;
        for (i = 0; i < NSH; i++) { free((void*)D.sh[i].pShaderBytecode); D.sh[i].pShaderBytecode = NULL; }
    }
    for (i = 0; i < NSH; i++) {
        ID3DBlob* b = compile(s_entries[i][0], s_entries[i][1]);
        size_t n;
        void* p;
        if (!b) return 0;
        n = ID3D10Blob_GetBufferSize(b);
        p = malloc(n);
        if (!p) { ID3D10Blob_Release(b); return 0; }
        memcpy(p, ID3D10Blob_GetBufferPointer(b), n);
        ID3D10Blob_Release(b);
        D.sh[i].pShaderBytecode = p;
        D.sh[i].BytecodeLength = n;
    }
    f = fopen(path, "wb");
    if (f) {
        uint32_t len[NSH];
        for (i = 0; i < NSH; i++) len[i] = (uint32_t)D.sh[i].BytecodeLength;
        fwrite(&key, 8, 1, f);
        fwrite(len, 4, NSH, f);
        for (i = 0; i < NSH; i++) fwrite(D.sh[i].pShaderBytecode, 1, len[i], f);
        fclose(f);
    }
    return 1;
}

static ID3D12RootSignature* make_rootsig(const D3D12_ROOT_SIGNATURE_DESC* rs) {
    ID3DBlob *blob = NULL, *err = NULL;
    ID3D12RootSignature* sig = NULL;
    HRESULT hr = p_D3D12SerializeRootSignature(rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[d3d12] root signature: %.*s\n",
                err ? (int)ID3D10Blob_GetBufferSize(err) : 0,
                err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    hr = ID3D12Device_CreateRootSignature(D.dev, 0, ID3D10Blob_GetBufferPointer(blob),
                                          ID3D10Blob_GetBufferSize(blob),
                                          &IID_ID3D12RootSignature, (void**)&sig);
    ID3D10Blob_Release(blob);
    return SUCCEEDED(hr) ? sig : NULL;
}

static int init_pipeline(void) {
    D3D12_ROOT_PARAMETER rp[4];
    D3D12_DESCRIPTOR_RANGE srv_range, smp_range, cap_range;
    D3D12_ROOT_SIGNATURE_DESC rs;
    D3D12_STATIC_SAMPLER_DESC ss;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
    uint64_t t0 = SDL_GetPerformanceCounter();

    if (!load_shaders()) return 0;
    fprintf(stderr, "[d3d12] shaders ready in %.0f ms\n",
            (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());

    memset(rp, 0, sizeof rp);
    memset(&srv_range, 0, sizeof srv_range);
    memset(&smp_range, 0, sizeof smp_range);
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 8;
    smp_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    smp_range.NumDescriptors = 8;
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[0].Descriptor.ShaderRegister = 0;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[1].Descriptor.ShaderRegister = 1;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[2].DescriptorTable.NumDescriptorRanges = 1;
    rp[2].DescriptorTable.pDescriptorRanges = &srv_range;
    rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[3].DescriptorTable.NumDescriptorRanges = 1;
    rp[3].DescriptorTable.pDescriptorRanges = &smp_range;
    rp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    memset(&rs, 0, sizeof rs);
    rs.NumParameters = 4;
    rs.pParameters = rp;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    D.rootsig = make_rootsig(&rs);
    if (!D.rootsig) return 0;

    /* blit: 4 root constants (b0) + 1 SRV table, static linear sampler */
    memset(rp, 0, sizeof rp);
    memset(&cap_range, 0, sizeof cap_range);
    cap_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    cap_range.NumDescriptors = 1;
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.Num32BitValues = 4;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &cap_range;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    memset(&ss, 0, sizeof ss);
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    memset(&rs, 0, sizeof rs);
    rs.NumParameters = 2;
    rs.pParameters = rp;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &ss;
    D.blit_rootsig = make_rootsig(&rs);
    if (!D.blit_rootsig) return 0;

    memset(&d, 0, sizeof d);
    d.pRootSignature = D.blit_rootsig;
    d.VS = D.sh[2];
    d.PS = D.sh[3];
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = 0xFFFFFFFFu;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.DepthStencilState.DepthEnable = FALSE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    d.SampleDesc.Count = (UINT)D.msaa;
    if (FAILED(ID3D12Device_CreateGraphicsPipelineState(D.dev, &d, &IID_ID3D12PipelineState,
                                                        (void**)&D.blit_pso)))
        return 0;

    /* EFB-copy resample: 8 root constants (b0) + 1 SRV table; integer
     * single-sample target, no depth.  Optional: GXCopyTex falls back to the
     * full-size readback without it. */
    memset(rp, 0, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.Num32BitValues = 12;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &cap_range;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    memset(&rs, 0, sizeof rs);
    rs.NumParameters = 2;
    rs.pParameters = rp;
    D.box_rootsig = make_rootsig(&rs);
    if (D.box_rootsig) {
        d.pRootSignature = D.box_rootsig;
        d.PS = D.sh[4];
        d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UINT;
        d.DSVFormat = DXGI_FORMAT_UNKNOWN;
        d.SampleDesc.Count = 1;
        if (FAILED(ID3D12Device_CreateGraphicsPipelineState(D.dev, &d, &IID_ID3D12PipelineState,
                                                            (void**)&D.box_pso)))
            D.box_pso = NULL;
        d.PS = D.sh[5];
        d.NumRenderTargets = 2;
        d.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.BlendState.IndependentBlendEnable = FALSE;
        d.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(ID3D12Device_CreateGraphicsPipelineState(D.dev, &d, &IID_ID3D12PipelineState,
                                                            (void**)&D.copy_pso)))
            D.copy_pso = NULL;
    }
    D.box_rtv_heap = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, 0);
    D.box_srv = alloc_cpu_srv();
    return 1;
}

/* -------------------------------------------------------------- targets */
static void release_targets(void) {
    int i;
    for (i = 0; i < NBACK; i++)
        if (D.bb[i]) { ID3D12Resource_Release(D.bb[i]); D.bb[i] = NULL; }
    if (D.rt) { ID3D12Resource_Release(D.rt); D.rt = NULL; }
    if (D.ds) { ID3D12Resource_Release(D.ds); D.ds = NULL; }
    if (D.rt_res) { ID3D12Resource_Release(D.rt_res); D.rt_res = NULL; }
}

static int create_targets(void) {
    D3D12_CLEAR_VALUE cv;
    D3D12_DEPTH_STENCIL_VIEW_DESC dv;
    int i;
    for (i = 0; i < D.nback; i++) {
        if (FAILED(IDXGISwapChain3_GetBuffer(D.swap, (UINT)i, &IID_ID3D12Resource, (void**)&D.bb[i])))
            return 0;
        ID3D12Device_CreateRenderTargetView(D.dev, D.bb[i], NULL, heap_cpu(D.rtv_heap, (UINT)i, D.rtv_inc));
    }
    /* no optimized clear colour: the game clears to many different colours */
    D.rt = make_tex2d(D.width, D.height, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D.msaa,
                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL);
    if (!D.rt) return 0;
    D.rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12Device_CreateRenderTargetView(D.dev, D.rt, NULL, rt_rtv());
    memset(&cv, 0, sizeof cv);
    cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    cv.DepthStencil.Depth = 0.0f;   /* reversed depth: GX far */
    D.ds = make_tex2d(D.width, D.height, 1, DXGI_FORMAT_D24_UNORM_S8_UINT, D.msaa,
                      D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv);
    if (!D.ds) return 0;
    memset(&dv, 0, sizeof dv);
    dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dv.ViewDimension = D.msaa > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    ID3D12Device_CreateDepthStencilView(D.dev, D.ds, &dv, rt_dsv());
    if (D.msaa > 1) {
        D.rt_res = make_tex2d(D.width, D.height, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1,
                              D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_SOURCE, NULL);
        if (!D.rt_res) return 0;
        D.rt_res_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    return 1;
}

/* Make a single-sample copy of the main target available as a copy source:
 * returns the resource (rt itself, or rt_res after a resolve) in
 * COPY_SOURCE state.  Callers restore with rt_to(RENDER_TARGET). */
static ID3D12Resource* resolved_source(void) {
    if (D.msaa <= 1) {
        rt_to(D3D12_RESOURCE_STATE_COPY_SOURCE);
        return D.rt;
    }
    rt_to(D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    barrier(D.rt_res, D.rt_res_state, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    ID3D12GraphicsCommandList_ResolveSubresource(D.cl, D.rt_res, 0, D.rt, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    barrier(D.rt_res, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D.rt_res_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    return D.rt_res;
}

static int ensure_readback(size_t size) {
    if (D.readback && D.readback_size >= size) return 1;
    if (D.readback) defer_release((IUnknown*)D.readback);
    D.readback = make_buffer(D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
    D.readback_size = D.readback ? size : 0;
    return D.readback != NULL;
}

/* -------------------------------------------------------------- textures */
static int sampler_id(DxTex* t) {
    D3D12_SAMPLER_DESC s;
    int i, minlin, maglin, miplin, use_mip;
    static const D3D12_TEXTURE_ADDRESS_MODE am[3] = {
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR };
    if (t->samp >= 0) return t->samp;
    memset(&s, 0, sizeof s);
    /* GX/GL min filter: 0 NEAREST, 1 LINEAR, 2 NEAR_MIP_NEAR, 3 LIN_MIP_NEAR,
     * 4 NEAR_MIP_LIN, 5 LIN_MIP_LIN; without mips only the base level */
    minlin = t->minf & 1;
    use_mip = t->mipmap && t->minf >= 2;
    miplin = t->minf >= 4;
    maglin = t->magf != 0;
    s.Filter = (D3D12_FILTER)((minlin ? 0x10 : 0) | (maglin ? 0x4 : 0) | (miplin ? 0x1 : 0));
    if (t->aniso > 1 && use_mip && minlin) {
        s.Filter = D3D12_FILTER_ANISOTROPIC;
        s.MaxAnisotropy = t->aniso;
    } else {
        s.MaxAnisotropy = 1;
    }
    s.AddressU = am[t->wrapS < 3 ? t->wrapS : 0];
    s.AddressV = am[t->wrapT < 3 ? t->wrapT : 0];
    s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    if (use_mip) {
        s.MinLOD = t->minlod;
        s.MaxLOD = t->maxlod;
        s.MipLODBias = t->bias < -16.0f ? -16.0f : t->bias > 15.99f ? 15.99f : t->bias;
    } else {
        s.MinLOD = 0.0f;
        s.MaxLOD = 0.0f;
    }
    for (i = 0; i < D.n_usamp; i++)
        if (!memcmp(&D.usamp[i].d, &s, sizeof s)) break;
    if (i == D.n_usamp) {
        if (D.n_usamp == MAX_UNIQUE_SAMPLERS) i = 0;   /* table full: default */
        else {
            D.usamp[i].d = s;
            ID3D12Device_CreateSampler(D.dev, &s, heap_cpu(D.smp_cpu, (UINT)i, D.smp_inc));
            D.n_usamp++;
        }
    }
    t->samp = i;
    return i;
}

static DxTex* tex_new(int w, int h, int mips) {
    DxTex* t = (DxTex*)calloc(1, sizeof(DxTex));
    if (!t) return NULL;
    t->res = make_tex2d(w, h, mips, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_COPY_DEST, NULL);
    if (!t->res) { free(t); return NULL; }
    t->w = w; t->h = h; t->mips = mips;
    t->srv = alloc_cpu_srv();
    ID3D12Device_CreateShaderResourceView(D.dev, t->res, NULL, t->srv);
    t->magf = 1;
    t->minf = 1;
    t->minlod = -1000.0f;
    t->maxlod = 1000.0f;
    t->samp = -1;
    t->version = 1;
    return t;
}

/* upload level data (RGBA8 rows, top first) into t->res (in COPY_DEST) */
static void tex_upload_levels(DxTex* t, const uint32_t* const* lv) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16];
    UINT rows[16];
    UINT64 rowsz[16], total = 0;
    D3D12_RESOURCE_DESC rd;
    D3D12_GPU_VIRTUAL_ADDRESS va;
    ID3D12Resource* src = NULL;
    uint8_t* cpu;
    UINT64 base = 0;
    int m;
    rd = ID3D12Resource_GetDesc(t->res);
    ID3D12Device_GetCopyableFootprints(D.dev, &rd, 0, (UINT)t->mips, 0, fp, rows, rowsz, &total);
    if (total <= SEG_SIZE / 4) {
        cpu = (uint8_t*)ring_alloc((size_t)total, 512, &va);
        if (!cpu) return;
        src = D.upload;
        base = va - D.upload_va;
    } else {   /* big texture: its own upload buffer, released after use */
        D3D12_RANGE none = {0, 0};
        void* p = NULL;
        src = make_buffer(D3D12_HEAP_TYPE_UPLOAD, (size_t)total, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!src) return;
        ID3D12Resource_Map(src, 0, &none, &p);
        cpu = (uint8_t*)p;
        defer_release((IUnknown*)src);
    }
    for (m = 0; m < t->mips; m++) {
        D3D12_TEXTURE_COPY_LOCATION dst, sl;
        UINT y;
        for (y = 0; y < rows[m]; y++)
            memcpy(cpu + fp[m].Offset + (UINT64)y * fp[m].Footprint.RowPitch,
                   (const uint8_t*)lv[m] + (size_t)y * fp[m].Footprint.Width * 4,
                   (size_t)fp[m].Footprint.Width * 4);
        memset(&dst, 0, sizeof dst);
        dst.pResource = t->res;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = (UINT)m;
        memset(&sl, 0, sizeof sl);
        sl.pResource = src;
        sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sl.PlacedFootprint = fp[m];
        sl.PlacedFootprint.Offset += base;
        ID3D12GraphicsCommandList_CopyTextureRegion(D.cl, &dst, 0, 0, 0, &sl, NULL);
    }
}

static int mip_count(int w, int h) {
    int n = 1;
    while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; }
    return n;
}

/* 2x2 box filter (like glGenerateMipmap) */
static uint32_t* mip_down(const uint32_t* s, int w, int h, int* ow, int* oh) {
    int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1, x, y, c;
    uint32_t* d = (uint32_t*)malloc((size_t)nw * nh * 4);
    if (!d) return NULL;
    for (y = 0; y < nh; y++)
        for (x = 0; x < nw; x++) {
            int x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : w - 1;
            int y0 = y * 2 < h ? y * 2 : h - 1, y1 = y * 2 + 1 < h ? y * 2 + 1 : h - 1;
            uint32_t p[4] = { s[y0 * w + x0], s[y0 * w + x1], s[y1 * w + x0], s[y1 * w + x1] };
            uint32_t o = 0;
            for (c = 0; c < 32; c += 8) {
                uint32_t a = ((p[0] >> c) & 255) + ((p[1] >> c) & 255) + ((p[2] >> c) & 255) +
                             ((p[3] >> c) & 255);
                o |= ((a + 2) / 4) << c;
            }
            d[y * nw + x] = o;
        }
    *ow = nw; *oh = nh;
    return d;
}

static void* dx_tex_create(const uint32_t* rgba, int w, int h, int wrapS, int wrapT, int mipmap) {
    DxTex* t;
    const uint32_t* lv[16];
    uint32_t* owned[16];
    int mips = mipmap ? mip_count(w, h) : 1, m, cw = w, ch = h;
    if (w <= 0 || h <= 0) return NULL;
    if (mips > 16) mips = 16;
    t = tex_new(w, h, mips);
    if (!t) return NULL;
    t->wrapS = (uint8_t)wrapS; t->wrapT = (uint8_t)wrapT;
    t->mipmap = (uint8_t)(mipmap != 0);
    t->minf = mipmap ? 5 : 1;   /* GL: LINEAR_MIPMAP_LINEAR / LINEAR */
    lv[0] = rgba;
    owned[0] = NULL;
    for (m = 1; m < mips; m++) {
        owned[m] = lv[m - 1] ? mip_down(lv[m - 1], cw, ch, &cw, &ch) : NULL;
        lv[m] = owned[m];
        if (!lv[m]) { mips = m; t->mips = m; break; }
    }
    tex_upload_levels(t, lv);
    for (m = 1; m < mips; m++) free(owned[m]);
    barrier(t->res, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return t;
}

static void dx_tex_image(void** handle, const uint32_t* rgba, int w, int h) {
    DxTex* t = (DxTex*)*handle;
    const uint32_t* lv[1];
    if (w <= 0 || h <= 0) return;
    if (!t) {
        t = tex_new(w, h, 1);
        if (!t) return;
        *handle = t;
    } else if (t->w != w || t->h != h || t->mips != 1) {
        ID3D12Resource* r = make_tex2d(w, h, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_RESOURCE_FLAG_NONE,
                                       D3D12_RESOURCE_STATE_COPY_DEST, NULL);
        if (!r) return;
        defer_release((IUnknown*)t->res);
        t->res = r;
        t->w = w; t->h = h; t->mips = 1;
        t->rt = 0;
        ID3D12Device_CreateShaderResourceView(D.dev, t->res, NULL, t->srv);
        t->version++;
    } else {
        barrier(t->res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    t->mipmap = 0;
    t->minf = 1; t->magf = 1;
    t->samp = -1;
    lv[0] = rgba;
    tex_upload_levels(t, lv);
    barrier(t->res, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    D.tables_dirty = 1;
}

static void dx_tex_wrap(void* handle, int wrapS, int wrapT) {
    DxTex* t = (DxTex*)handle;
    if (!t) return;
    if (t->wrapS == wrapS && t->wrapT == wrapT) return;
    t->wrapS = (uint8_t)wrapS; t->wrapT = (uint8_t)wrapT;
    t->samp = -1;
    D.tables_dirty = 1;
}

static void dx_tex_lod(void* handle, int mipmap, int minFilt, int magFilt, float minLod,
                       float maxLod, float lodBias, int maxAniso) {
    DxTex* t = (DxTex*)handle;
    if (!t) return;
    t->minf = (uint8_t)minFilt;
    t->magf = (uint8_t)(magFilt != 0);
    if (mipmap) {
        t->minlod = minLod;
        t->maxlod = maxLod > 0.0f ? maxLod : 1000.0f;
        t->bias = lodBias;
        t->aniso = (uint8_t)(1 << (maxAniso & 3));
    }
    t->samp = -1;
    D.tables_dirty = 1;
}

static void dx_tex_destroy(void* handle) {
    DxTex* t = (DxTex*)handle;
    int i;
    if (!t) return;
    for (i = 0; i < 8; i++) if (D.bound[i] == t) D.bound[i] = NULL;
    defer_release((IUnknown*)t->res);
    /* its CPU-only SRV slot can be reused at once: descriptor tables hold
     * copies (build_tables), and the resource lives until the GPU is done */
    if (t->srv.ptr) {
        if (D.n_srv_free == D.cap_srv_free) {
            int nc = D.cap_srv_free ? D.cap_srv_free * 2 : 256;
            D3D12_CPU_DESCRIPTOR_HANDLE* nf = (D3D12_CPU_DESCRIPTOR_HANDLE*)realloc(
                D.srv_free, (size_t)nc * sizeof *nf);
            if (nf) { D.srv_free = nf; D.cap_srv_free = nc; }
        }
        if (D.n_srv_free < D.cap_srv_free) D.srv_free[D.n_srv_free++] = t->srv;
    }
    free(t);
}

static void dx_bind_tex(int stage, void* handle) {
    if (D.bound[stage] != (DxTex*)handle) {
        D.bound[stage] = (DxTex*)handle;
        D.tables_dirty = 1;
    }
}

/* descriptor tables for the 8 bound textures */
static void build_tables(void) {
    D3D12_CPU_DESCRIPTOR_HANDLE src[8], dst;
    UINT ones[8] = {1, 1, 1, 1, 1, 1, 1, 1}, eight = 8;
    uint64_t key = 0;
    int i, s;
    for (i = 0; i < 8; i++) {
        DxTex* t = D.bound[i] ? D.bound[i] : D.dummy;
        src[i] = t->srv;
        key |= (uint64_t)(sampler_id(t) & 0xFF) << (i * 8);
        D.bound_ver[i] = t->version;
    }
    if (D.srv_gpu_off + 8 > SRV_SEG) submit_and_wait();
    dst = heap_cpu(D.srv_gpu, (UINT)(D.slot * SRV_SEG) + D.srv_gpu_off, D.srv_inc);
    ID3D12Device_CopyDescriptors(D.dev, 1, &dst, &eight, 8, src, ones,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D.srv_table = heap_gpu(D.srv_gpu, (UINT)(D.slot * SRV_SEG) + D.srv_gpu_off, D.srv_inc);
    D.srv_gpu_off += 8;

    /* sampler table cache (persistent; reset when the heap fills up) */
    s = (int)((key ^ (key >> 29) ^ (key >> 41)) % 512);
    while (D.stab[s].used && D.stab[s].key != key) s = (s + 1) % 512;
    if (!D.stab[s].used) {
        D3D12_CPU_DESCRIPTOR_HANDLE ss[8], sd;
        if ((D.n_stab + 1) * 8 > SMP_HEAP || D.n_stab >= 256) {
            submit_and_wait();   /* tables may be in use: drain, then start over */
            memset(D.stab, 0, sizeof D.stab);
            D.n_stab = 0;
            s = (int)((key ^ (key >> 29) ^ (key >> 41)) % 512);
        }
        for (i = 0; i < 8; i++) ss[i] = heap_cpu(D.smp_cpu, (UINT)((key >> (i * 8)) & 0xFF), D.smp_inc);
        sd = heap_cpu(D.smp_gpu, (UINT)(D.n_stab * 8), D.smp_inc);
        ID3D12Device_CopyDescriptors(D.dev, 1, &sd, &eight, 8, ss, ones,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        D.stab[s].used = 1;
        D.stab[s].key = key;
        D.stab[s].off = (UINT)(D.n_stab * 8);
        D.n_stab++;
    }
    D.smp_table = heap_gpu(D.smp_gpu, D.stab[s].off, D.smp_inc);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(D.cl, 2, D.srv_table);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(D.cl, 3, D.smp_table);
    D.tables_dirty = 0;
}

/* ------------------------------------------------------------ GxGpu ops */
static void dx_set_viewport(int x, int y, int w, int h) {
    if (D.vp[0] == x && D.vp[1] == y && D.vp[2] == w && D.vp[3] == h) return;
    if (w < 0 || h < 0) return;   /* GL: INVALID_VALUE, viewport unchanged */
    D.vp[0] = x; D.vp[1] = y; D.vp[2] = w; D.vp[3] = h;
    apply_viewport();
}

static void dx_set_scissor(int x, int y, int w, int h) {
    if (w < 0 || h < 0) return;
    if (D.sc_on && D.sc[0] == x && D.sc[1] == y && D.sc[2] == w && D.sc[3] == h) return;
    D.sc[0] = x; D.sc[1] = y; D.sc[2] = w; D.sc[3] = h;
    D.sc_on = 1;
    apply_scissor();
}

/* Out-of-range GX values (the game does pass some, e.g. GXSetZMode func 74)
 * fall back exactly like the GL backend's switch defaults. */
static void dx_set_blend(int type, int src, int dst, int op) {
    D.blend_en = type != GX_BM_NONE;
    D.blend_src = (src >= 0 && src <= 7) ? src : GX_BL_ONE;
    D.blend_dst = (dst >= 0 && dst <= 7) ? dst : GX_BL_ZERO;
    D.blend_sub = op == GX_BM_SUBTRACT;
}

static void dx_set_depth(int compare, int func, int update) {
    D.zfunc = !compare ? GX_ALWAYS : (func >= 0 && func <= 7) ? func : GX_LEQUAL;
    D.zwrite = update != 0;
}

static void dx_set_cull(int mode) {
    if (mode >= GX_CULL_NONE && mode <= GX_CULL_ALL) D.cull = mode;   /* GL ignores others */
}

static void dx_clear(const int* rect, uint32_t rgba, float depth, int force) {
    FLOAT c[4];
    D3D12_RECT r;
    const D3D12_RECT* pr = NULL;
    c[0] = ((rgba >> 24) & 0xFF) / 255.0f;
    c[1] = ((rgba >> 16) & 0xFF) / 255.0f;
    c[2] = ((rgba >> 8) & 0xFF) / 255.0f;
    c[3] = (rgba & 0xFF) / 255.0f;
    if (rect) {
        long x0 = rect[0], x1 = (long)rect[0] + rect[2];
        long y0 = D.height - ((long)rect[1] + rect[3]), y1 = D.height - (long)rect[1];
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > D.width) x1 = D.width;
        if (y1 > D.height) y1 = D.height;
        if (x1 <= x0 || y1 <= y0) return;
        r.left = x0; r.top = y0; r.right = x1; r.bottom = y1;
        pr = &r;
    }
    rt_to(D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_ClearRenderTargetView(D.cl, rt_rtv(), c, pr ? 1 : 0, pr);
    /* glClear obeys the depth write mask; the copy-clear forces it on */
    if (force || D.zwrite)
        ID3D12GraphicsCommandList_ClearDepthStencilView(D.cl, rt_dsv(),
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f - depth, 0, pr ? 1 : 0, pr);
}

static void dx_draw(const GxDrawCall* d) {
    unsigned key;
    ID3D12PipelineState* pso;
    D3D12_VERTEX_BUFFER_VIEW vb[2];
    D3D12_GPU_VIRTUAL_ADDRESS va;
    D3D_PRIMITIVE_TOPOLOGY topo;
    void* p;
    int n = d->nverts, i;
    int is_point = d->prim == GXD_POINTS;

    if (n <= 0 || !D.ok) return;
    if (d->prim == GXD_QUADS && n < 4) return;
    if (d->prim != GXD_QUADS && !is_point && d->prim != GXD_TRIANGLES && n < 3) return;
    /* GL culls every polygon with GL_FRONT_AND_BACK (points are not polygons) */
    if (!d->no_cull && !is_point && D.cull == GX_CULL_ALL) return;

    key = 0;
    if (D.blend_en)
        key |= 1u | ((unsigned)D.blend_src << 1) | ((unsigned)D.blend_dst << 4) |
               (D.blend_sub ? 0x80u : 0u);
    key |= (unsigned)D.zfunc << 8;
    if (D.zwrite) key |= 0x800;
    if (!d->no_cull && !is_point) key |= (unsigned)(D.cull & 3) << 12;
    if (d->depth_clamp) key |= PK_NOCLIP;
    if (is_point) key |= PK_POINT;
    pso = get_pso(key);
    if (!pso) return;

    /* tables first: building them may drain the GPU (resets the ring) */
    if (!D.tables_dirty)
        for (i = 0; i < 8; i++)
            if ((D.bound[i] && D.bound[i]->version != D.bound_ver[i]) ||
                (D.bound[i] && D.bound[i]->samp < 0)) { D.tables_dirty = 1; break; }
    if (D.tables_dirty) build_tables();

    /* vertices (+ quad UV rects) */
    p = ring_alloc((size_t)n * GXV_FLOATS * sizeof(float), 16, &va);
    if (!p) return;
    if (D.tables_dirty) build_tables();   /* ring wrap drained and reset state */
    memcpy(p, d->verts, (size_t)n * GXV_FLOATS * sizeof(float));
    vb[0].BufferLocation = va;
    vb[0].SizeInBytes = (UINT)(n * GXV_FLOATS * sizeof(float));
    vb[0].StrideInBytes = GXV_FLOATS * sizeof(float);
    if (d->rects) {
        D3D12_GPU_VIRTUAL_ADDRESS rva;
        void* rp = ring_alloc((size_t)n * 16, 16, &rva);
        if (!rp) return;
        if (D.seg_off == (size_t)n * 16) {   /* the rect alloc wrapped: redo verts */
            p = ring_alloc((size_t)n * GXV_FLOATS * sizeof(float), 16, &va);
            memcpy(p, d->verts, (size_t)n * GXV_FLOATS * sizeof(float));
            vb[0].BufferLocation = va;
            if (D.tables_dirty) build_tables();
        }
        memcpy(rp, d->rects, (size_t)n * 16);
        vb[1].BufferLocation = rva;
        vb[1].SizeInBytes = (UINT)(n * 16);
        vb[1].StrideInBytes = 16;
    } else {
        vb[1].BufferLocation = ID3D12Resource_GetGPUVirtualAddress(D.zero_vb);
        vb[1].SizeInBytes = 16;
        vb[1].StrideInBytes = 0;
    }

    /* constants, deduplicated against the last upload */
    if (!D.last_dc_ok || memcmp(&D.last_dc, d->dc, sizeof(GxDrawConsts)) != 0) {
        void* c = ring_alloc((sizeof(GxDrawConsts) + 255) & ~(size_t)255, 256, &D.dc_va);
        if (!c) return;
        memcpy(c, d->dc, sizeof(GxDrawConsts));
        memcpy(&D.last_dc, d->dc, sizeof(GxDrawConsts));
        D.last_dc_ok = 1;
        D.root_dirty |= 1;
    }
    if (!D.last_tc_ok || (D.last_tc_serial != d->tc_serial &&
                          memcmp(&D.last_tc, d->tc, sizeof(GxTevConsts)) != 0)) {
        void* c = ring_alloc((sizeof(GxTevConsts) + 255) & ~(size_t)255, 256, &D.tc_va);
        if (!c) return;
        memcpy(c, d->tc, sizeof(GxTevConsts));
        memcpy(&D.last_tc, d->tc, sizeof(GxTevConsts));
        D.last_tc_ok = 1;
        D.root_dirty |= 2;
    }
    D.last_tc_serial = d->tc_serial;
    if (!D.last_dc_ok || !D.last_tc_ok) return;   /* a wrap invalidated them: rare, skip */
    if (D.tables_dirty) build_tables();
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(D.cl, 0, D.dc_va);
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(D.cl, 1, D.tc_va);
    D.root_dirty = 0;

    rt_to(D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (pso != D.cur_pso) {
        ID3D12GraphicsCommandList_SetPipelineState(D.cl, pso);
        D.cur_pso = pso;
    }
    switch (d->prim) {
    case GXD_TRISTRIP: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
    case GXD_POINTS:   topo = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; break;
    default:           topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    }
    if ((int)topo != D.cur_topo) {
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(D.cl, topo);
        D.cur_topo = (int)topo;
    }
    ID3D12GraphicsCommandList_IASetVertexBuffers(D.cl, 0, 2, vb);
    if (d->prim == GXD_QUADS)
        ID3D12GraphicsCommandList_DrawIndexedInstanced(D.cl, (UINT)(n / 4) * 6, 1, 0, 0, 0);
    else if (d->prim == GXD_TRIFAN)
        ID3D12GraphicsCommandList_DrawIndexedInstanced(D.cl, (UINT)(n - 2) * 3, 1, D.fan_first, 0, 0);
    else
        ID3D12GraphicsCommandList_DrawInstanced(D.cl, (UINT)n, 1, 0, 0);
}

/* copy a GL-space rect of the (resolved) target into out: colour RGBA8 or
 * depth float, rows bottom-up like glReadPixels */
/* box resample helpers (dx_read_pixels_box, dx_copy_tex) */
static int box_ensure_rt(int dw, int dh) {
    if (!D.box_rt || D.box_w < dw || D.box_h < dh) {
        int nw = dw > D.box_w ? dw : D.box_w, nh = dh > D.box_h ? dh : D.box_h;
        ID3D12Resource* r = make_tex2d(nw, nh, 1, DXGI_FORMAT_R8G8B8A8_UINT, 1,
                                       D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET, NULL);
        if (!r) return 0;
        if (D.box_rt) defer_release((IUnknown*)D.box_rt);
        D.box_rt = r; D.box_w = nw; D.box_h = nh;
        ID3D12Device_CreateRenderTargetView(D.dev, D.box_rt, NULL,
            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(D.box_rtv_heap));
    }
    return 1;
}

/* the single-sample main target as a shader resource (msaa: resolved into
 * rt_res); returns its shader-visible table.  Undo with box_release_source. */
static D3D12_GPU_DESCRIPTOR_HANDLE box_bind_source(void) {
    D3D12_CPU_DESCRIPTOR_HANDLE sdst;
    ID3D12Resource* srcres;
    if (D.msaa > 1) {
        srcres = resolved_source();            /* rt_res in COPY_SOURCE */
        barrier(D.rt_res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        D.rt_res_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        rt_to(D3D12_RESOURCE_STATE_RENDER_TARGET);
    } else {
        rt_to(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        srcres = D.rt;
    }
    ID3D12Device_CreateShaderResourceView(D.dev, srcres, NULL, D.box_srv);
    sdst = heap_cpu(D.srv_gpu, (UINT)(D.slot * SRV_SEG) + D.srv_gpu_off, D.srv_inc);
    ID3D12Device_CopyDescriptorsSimple(D.dev, 1, sdst, D.box_srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D.srv_gpu_off++;
    return heap_gpu(D.srv_gpu, (UINT)(D.slot * SRV_SEG) + D.srv_gpu_off - 1, D.srv_inc);
}

static void box_release_source(void) {
    if (D.msaa > 1) {
        barrier(D.rt_res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D.rt_res_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    } else {
        rt_to(D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
}

static void box_draw(D3D12_GPU_DESCRIPTOR_HANDLE tab, int x, int y, int w, int h, int dw, int dh,
                     int cf) {
    D3D12_VIEWPORT v;
    D3D12_RECT sc;
    int32_t k[12];
    /* D3D texel row of rect row yy (0 = top of the GL rect) = top + yy */
    k[0] = x; k[1] = D.height - (y + h); k[2] = w; k[3] = h;
    k[4] = dw; k[5] = dh; k[6] = D.width; k[7] = D.height;
    k[8] = cf; k[9] = k[10] = k[11] = 0;
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(D.cl, 0, 12, k, 0);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(D.cl, 1, tab);
    v.TopLeftX = 0; v.TopLeftY = 0; v.Width = (float)dw; v.Height = (float)dh;
    v.MinDepth = 0; v.MaxDepth = 1;
    ID3D12GraphicsCommandList_RSSetViewports(D.cl, 1, &v);
    sc.left = 0; sc.top = 0; sc.right = dw; sc.bottom = dh;
    ID3D12GraphicsCommandList_RSSetScissorRects(D.cl, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(D.cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(D.cl, 3, 1, 0, 0);
}

/* ---- asynchronous GXCopyTex (gx_gpu.h copy_tex) ----
 * One pass renders the box resample (UINT, for GC memory) and its converted
 * texels (UNORM, straight into the game's texture), both are copied to a
 * per-ticket READBACK buffer, and nothing waits: the ticket completes with
 * the frame's fence. */
#define CT_SLOTS 128
static struct {
    ID3D12Resource* buf;
    size_t size;
    UINT64 fence;
    uint64_t ticket;
    int dw, dh;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 conv_off;
} s_ct[CT_SLOTS];
static uint64_t s_ct_next = 1;

static int ct_target(DxTex** pt, int w, int h) {
    DxTex* t = *pt;
    if (!t) {
        t = (DxTex*)calloc(1, sizeof(DxTex));
        if (!t) return 0;
        t->srv = alloc_cpu_srv();
        t->magf = 1; t->minf = 1;
        t->minlod = -1000.0f; t->maxlod = 1000.0f;
        t->samp = -1;
        t->version = 1;
        *pt = t;
    }
    if (!t->res || !t->rt || t->w != w || t->h != h || t->mips != 1) {
        ID3D12Resource* r = make_tex2d(w, h, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1,
                                       D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL);
        if (!r) return 0;
        if (t->res) defer_release((IUnknown*)t->res);
        t->res = r;
        t->w = w; t->h = h; t->mips = 1; t->rt = 1;
        ID3D12Device_CreateShaderResourceView(D.dev, t->res, NULL, t->srv);
        t->version++;
    }
    t->mipmap = 0;
    t->minf = 1; t->magf = 1;
    t->samp = -1;
    D.tables_dirty = 1;
    return 1;
}

static unsigned long long dx_gpu_mem(void) {
    DXGI_QUERY_VIDEO_MEMORY_INFO mi;
    if (!D.adapter3 ||
        FAILED(IDXGIAdapter3_QueryVideoMemoryInfo(D.adapter3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi)))
        return 0;
    return mi.CurrentUsage;
}

static uint64_t dx_copy_tex(void** handle, int x, int y, int w, int h, int dw, int dh, int cf) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[2];
    D3D12_GPU_DESCRIPTOR_HANDLE tab;
    D3D12_RESOURCE_DESC rd;
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    D3D12_BOX box;
    UINT64 total = 0, need;
    UINT nrows = 0;
    UINT64 rowsz = 0;
    DxTex* t = (DxTex*)*handle;
    uint64_t ticket;
    int slot;
    int norb = (cf & GXCOPY_NO_READBACK) != 0;   /* HFR replay: texture only */
    cf &= 0xFFFF;
    if (!D.ok || !D.copy_pso || !D.box_rtv_heap || !D.box_srv.ptr || w <= 0 || h <= 0 ||
        dw <= 0 || dh <= 0 || dw > 4096 || dh > 4096)
        return 0;
    if (D.srv_gpu_off + 1 > SRV_SEG) submit_and_wait();
    if (!box_ensure_rt(dw, dh) || !ct_target(&t, dw, dh)) return 0;
    *handle = t;
    if (norb) {
        tab = box_bind_source();
        rtv[0] = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(D.box_rtv_heap);
        rtv[1] = heap_cpu(D.box_rtv_heap, 1, D.rtv_inc);
        ID3D12Device_CreateRenderTargetView(D.dev, t->res, NULL, rtv[1]);
        barrier(t->res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12GraphicsCommandList_OMSetRenderTargets(D.cl, 2, rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(D.cl, D.box_rootsig);
        ID3D12GraphicsCommandList_SetPipelineState(D.cl, D.copy_pso);
        box_draw(tab, x, y, w, h, dw, dh, cf);
        box_release_source();
        barrier(t->res, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        apply_frame_state();
        return 1;
    }

    /* a free slot (collected / discarded ticket), preferring one whose
     * buffer the GPU is done with; the ticket encodes it in its low byte */
    {
        UINT64 done = ID3D12Fence_GetCompletedValue(D.fence);
        int i, any = -1;
        slot = -1;
        for (i = 0; i < CT_SLOTS; i++) {
            if (s_ct[i].ticket) continue;
            if (!s_ct[i].buf || done >= s_ct[i].fence) { slot = i; break; }
            if (any < 0) any = i;
        }
        if (slot < 0) slot = any >= 0 ? any : (int)(s_ct_next % CT_SLOTS);
    }
    ticket = (s_ct_next << 8) | (uint64_t)slot;
    if (s_ct[slot].buf && ID3D12Fence_GetCompletedValue(D.fence) < s_ct[slot].fence) {
        /* still on the GPU (only when all slots are taken) */
        if (s_ct[slot].fence > D.fence_value) submit_and_wait();
        else wait_fence(s_ct[slot].fence);
    }
    rd = ID3D12Resource_GetDesc(D.box_rt);
    rd.Width = (UINT64)dw;
    rd.Height = (UINT)dh;
    ID3D12Device_GetCopyableFootprints(D.dev, &rd, 0, 1, 0, &s_ct[slot].fp, &nrows, &rowsz, &total);
    s_ct[slot].conv_off = (total + 511) & ~(UINT64)511;
    need = s_ct[slot].conv_off + total;
    if (!s_ct[slot].buf || s_ct[slot].size < need) {
        if (s_ct[slot].buf) defer_release((IUnknown*)s_ct[slot].buf);
        s_ct[slot].buf = make_buffer(D3D12_HEAP_TYPE_READBACK, (size_t)need, D3D12_RESOURCE_STATE_COPY_DEST);
        s_ct[slot].size = s_ct[slot].buf ? (size_t)need : 0;
        if (!s_ct[slot].buf) return 0;
    }

    tab = box_bind_source();
    rtv[0] = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(D.box_rtv_heap);
    rtv[1] = heap_cpu(D.box_rtv_heap, 1, D.rtv_inc);
    ID3D12Device_CreateRenderTargetView(D.dev, t->res, NULL, rtv[1]);
    barrier(t->res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_OMSetRenderTargets(D.cl, 2, rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(D.cl, D.box_rootsig);
    ID3D12GraphicsCommandList_SetPipelineState(D.cl, D.copy_pso);
    box_draw(tab, x, y, w, h, dw, dh, cf);
    box_release_source();

    barrier(D.box_rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(t->res, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    box.left = 0; box.top = 0; box.front = 0;
    box.right = (UINT)dw; box.bottom = (UINT)dh; box.back = 1;
    memset(&dst, 0, sizeof dst);
    dst.pResource = s_ct[slot].buf;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = s_ct[slot].fp;
    memset(&src, 0, sizeof src);
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = D.box_rt;
    ID3D12GraphicsCommandList_CopyTextureRegion(D.cl, &dst, 0, 0, 0, &src, &box);
    dst.PlacedFootprint.Offset = s_ct[slot].conv_off;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.pResource = t->res;
    ID3D12GraphicsCommandList_CopyTextureRegion(D.cl, &dst, 0, 0, 0, &src, &box);
    barrier(D.box_rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(t->res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    apply_frame_state();   /* the game's root signature, targets, viewport */

    s_ct[slot].fence = D.fence_value + 1;   /* signalled at the next submit */
    s_ct[slot].ticket = ticket;
    s_ct[slot].dw = dw; s_ct[slot].dh = dh;
    s_ct_next++;
    return ticket;
}

static int dx_copy_tex_result(uint64_t ticket, void* raw, void* conv, int wait) {
    int slot = (int)(ticket & 0xFF), j;
    D3D12_RANGE rr;
    uint8_t* map = NULL;
    size_t row;
    if (!ticket || slot >= CT_SLOTS || s_ct[slot].ticket != ticket || !s_ct[slot].buf) return -1;
    if (wait < 0) { s_ct[slot].ticket = 0; return 1; }
    if (ID3D12Fence_GetCompletedValue(D.fence) < s_ct[slot].fence) {
        if (!wait) return 0;
        if (s_ct[slot].fence > D.fence_value) submit_and_wait();
        else wait_fence(s_ct[slot].fence);
    }
    rr.Begin = 0; rr.End = s_ct[slot].size;
    if (FAILED(ID3D12Resource_Map(s_ct[slot].buf, 0, &rr, (void**)&map))) return -1;
    row = (size_t)s_ct[slot].dw * 4;
    for (j = 0; j < s_ct[slot].dh; j++) {
        const uint8_t* s0 = map + s_ct[slot].fp.Offset + (UINT64)j * s_ct[slot].fp.Footprint.RowPitch;
        if (raw) memcpy((uint8_t*)raw + (size_t)j * row, s0, row);
        if (conv) memcpy((uint8_t*)conv + (size_t)j * row, s0 + s_ct[slot].conv_off, row);
    }
    rr.End = 0;
    ID3D12Resource_Unmap(s_ct[slot].buf, 0, &rr);
    s_ct[slot].ticket = 0;
    return 1;
}

/* GXCopyTex fast path: area-resample the GL-space rect (x,y,w,h) of the
 * resolved target to dw x dh on the GPU and read back only that (RGBA8,
 * output rows top-down), instead of a full-size readback and CPU resample
 * (the stage select copies every frame). */
static int dx_read_pixels_box(int x, int y, int w, int h, int dw, int dh, void* out) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    D3D12_BOX box;
    D3D12_RANGE rr;
    D3D12_RESOURCE_DESC rd;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    D3D12_GPU_DESCRIPTOR_HANDLE tab;
    UINT64 total = 0;
    UINT nrows = 0;
    UINT64 rowsz = 0;
    uint8_t* map = NULL;
    int j;
    if (!D.ok || !D.box_pso || !D.box_rtv_heap || !D.box_srv.ptr || w <= 0 || h <= 0 ||
        dw <= 0 || dh <= 0 || dw > 4096 || dh > 4096)
        return 0;
    if (D.srv_gpu_off + 1 > SRV_SEG) submit_and_wait();
    if (!box_ensure_rt(dw, dh)) return 0;
    tab = box_bind_source();

    rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(D.box_rtv_heap);
    ID3D12GraphicsCommandList_OMSetRenderTargets(D.cl, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(D.cl, D.box_rootsig);
    ID3D12GraphicsCommandList_SetPipelineState(D.cl, D.box_pso);
    box_draw(tab, x, y, w, h, dw, dh, 0);

    /* source back to its tracked state, result to the readback buffer */
    box_release_source();
    barrier(D.box_rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    rd = ID3D12Resource_GetDesc(D.box_rt);
    rd.Width = (UINT64)dw;
    rd.Height = (UINT)dh;
    ID3D12Device_GetCopyableFootprints(D.dev, &rd, 0, 1, 0, &fp, &nrows, &rowsz, &total);
    if (!ensure_readback((size_t)total)) {
        barrier(D.box_rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        apply_frame_state();
        return 0;
    }
    memset(&src, 0, sizeof src);
    src.pResource = D.box_rt;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    memset(&dst, 0, sizeof dst);
    dst.pResource = D.readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    box.left = 0; box.top = 0; box.front = 0;
    box.right = (UINT)dw; box.bottom = (UINT)dh; box.back = 1;
    ID3D12GraphicsCommandList_CopyTextureRegion(D.cl, &dst, 0, 0, 0, &src, &box);
    barrier(D.box_rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    submit_and_wait();                         /* also restores the game's state */
    rr.Begin = 0; rr.End = (SIZE_T)total;
    if (FAILED(ID3D12Resource_Map(D.readback, 0, &rr, (void**)&map))) return 0;
    for (j = 0; j < dh; j++)
        memcpy((uint8_t*)out + (size_t)j * dw * 4, map + fp.Offset + (UINT64)j * fp.Footprint.RowPitch,
               (size_t)dw * 4);
    rr.End = 0;
    ID3D12Resource_Unmap(D.readback, 0, &rr);
    return 1;
}

static int dx_read_pixels(int x, int y, int w, int h, int depth, void* out) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    D3D12_BOX box;
    D3D12_RANGE rr;
    UINT64 total = 0;
    UINT nrows = 0;
    UINT64 rowsz = 0;
    D3D12_RESOURCE_DESC rd;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h, bx0, by0, bw, bh, j;
    uint8_t* map = NULL;
    size_t px = depth ? 4 : 4;
    if (!D.ok || w <= 0 || h <= 0) return 0;
    memset(out, 0, (size_t)w * h * px);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > D.width) x1 = D.width;
    if (y1 > D.height) y1 = D.height;
    if (x1 <= x0 || y1 <= y0) return 1;
    if (depth && D.msaa > 1) return 0;   /* multisampled depth cannot be resolved */
    bx0 = x0; bw = x1 - x0; bh = y1 - y0;
    by0 = D.height - y1;                 /* top row in D3D space */

    memset(&src, 0, sizeof src);
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    if (depth) {
        rt_to(D3D12_RESOURCE_STATE_RENDER_TARGET);
        barrier(D.ds, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        src.pResource = D.ds;
    } else {
        src.pResource = resolved_source();
    }
    rd = ID3D12Resource_GetDesc(src.pResource);
    rd.Width = (UINT64)bw;
    rd.Height = (UINT)bh;
    rd.SampleDesc.Count = 1;
    ID3D12Device_GetCopyableFootprints(D.dev, &rd, 0, 1, 0, &fp, &nrows, &rowsz, &total);
    if (!ensure_readback((size_t)total)) return 0;
    memset(&dst, 0, sizeof dst);
    dst.pResource = D.readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    box.left = (UINT)bx0; box.top = (UINT)by0; box.front = 0;
    box.right = (UINT)(bx0 + bw); box.bottom = (UINT)(by0 + bh); box.back = 1;
    ID3D12GraphicsCommandList_CopyTextureRegion(D.cl, &dst, 0, 0, 0, &src, &box);
    if (depth)
        barrier(D.ds, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    rt_to(D3D12_RESOURCE_STATE_RENDER_TARGET);
    submit_and_wait();
    rr.Begin = 0; rr.End = (SIZE_T)total;
    if (FAILED(ID3D12Resource_Map(D.readback, 0, &rr, (void**)&map))) return 0;
    for (j = 0; j < bh; j++) {
        /* D3D row j (top-down) is GL row (y1 - 1 - j); out row index from y */
        int gl_row = y1 - 1 - j, orow = gl_row - y;
        const uint8_t* s = map + fp.Offset + (UINT64)j * fp.Footprint.RowPitch;
        uint8_t* o = (uint8_t*)out + ((size_t)orow * w + (bx0 - x)) * px;
        if (depth) {
            float* of = (float*)(void*)o;
            int i;
            for (i = 0; i < bw; i++) {
                uint32_t v;
                memcpy(&v, s + i * 4, 4);
                of[i] = (float)(0xFFFFFFu - (v & 0xFFFFFFu)) / 16777215.0f;   /* reversed */
            }
        } else {
            memcpy(o, s, (size_t)bw * 4);
        }
    }
    rr.End = 0;
    ID3D12Resource_Unmap(D.readback, 0, &rr);
    if (getenv("SMB2_D3D12_TRACECOPY")) {
        static int n;
        if (n++ < 40) {
            const uint8_t* o = (const uint8_t*)out + ((size_t)(h / 2) * w + w / 2) * 4;
            fprintf(stderr, "[d3d12] read_pixels %d,%d %dx%d depth=%d center=%u,%u,%u,%u\n",
                    x, y, w, h, depth, o[0], o[1], o[2], o[3]);
        }
    }
    return 1;
}

static int dx_read_frame(unsigned char** rgb, int* pw, int* ph) {
    size_t n = (size_t)D.width * D.height * 4;
    uint8_t* tmp;
    int y, x;
    if (!D.ok) return 0;
    tmp = (uint8_t*)malloc(n);
    if (!tmp) return 0;
    if (!dx_read_pixels(0, 0, D.width, D.height, 0, tmp)) { free(tmp); return 0; }
    if (D.frame_rgb_size < (size_t)D.width * D.height * 3) {
        free(D.frame_rgb);
        D.frame_rgb_size = (size_t)D.width * D.height * 3;
        D.frame_rgb = (unsigned char*)malloc(D.frame_rgb_size);
        if (!D.frame_rgb) { D.frame_rgb_size = 0; free(tmp); return 0; }
    }
    for (y = 0; y < D.height; y++) {          /* bottom-up -> top-down */
        const uint8_t* s = tmp + (size_t)(D.height - 1 - y) * D.width * 4;
        unsigned char* o = D.frame_rgb + (size_t)y * D.width * 3;
        for (x = 0; x < D.width; x++) {
            o[x * 3 + 0] = s[x * 4 + 0];
            o[x * 3 + 1] = s[x * 4 + 1];
            o[x * 3 + 2] = s[x * 4 + 2];
        }
    }
    free(tmp);
    *rgb = D.frame_rgb; *pw = D.width; *ph = D.height;
    return 1;
}

static void begin_slot(void) {
    wait_fence(D.frame_fence[D.slot]);
    process_deferred();
    if (D.q_ok && D.q_valid[D.slot]) {   /* that frame is done: its GPU time */
        D3D12_RANGE rr;
        UINT64* t = NULL;
        rr.Begin = (SIZE_T)D.slot * 16; rr.End = rr.Begin + 16;
        if (SUCCEEDED(ID3D12Resource_Map(D.qread, 0, &rr, (void**)&t)) && t) {
            UINT64 a = t[D.slot * 2], b = t[D.slot * 2 + 1];
            /* (a list executed without its end timestamp leaves a stale
             * one: only plausible frame times count) */
            if (b > a && D.qfreq && (double)(b - a) < 0.1 * (double)D.qfreq) {
                D.gpu_ms_acc += (double)(b - a) * 1000.0 / (double)D.qfreq; D.gpu_ms_n++;
            }
            if (b > a && D.qfreq && (double)(b - a) < 0.1 * (double)D.qfreq) {
                /* GPU start / end on the platform clock (calibrated once
                 * per second: GPU timestamp and QPC drift apart slowly) */
                extern unsigned long long platform_counter_time_ns(unsigned long long counter);
                extern void pace_log_gpu(unsigned pid, unsigned long long t0, unsigned long long t1);
                extern int pace_log_on(void);
                static unsigned cal_n;
                LARGE_INTEGER qf;
                if ((cal_n++ % 240) == 0 || !D.cal_cpu)
                    ID3D12CommandQueue_GetClockCalibration(D.queue, &D.cal_gpu, &D.cal_cpu);
                QueryPerformanceFrequency(&qf);
                if (D.cal_cpu) {
                    double qa = (double)D.cal_cpu + ((double)a - (double)D.cal_gpu) * (double)qf.QuadPart / (double)D.qfreq;
                    double qb = (double)D.cal_cpu + ((double)b - (double)D.cal_gpu) * (double)qf.QuadPart / (double)D.qfreq;
                    unsigned long long ta = qa > 0 ? platform_counter_time_ns((unsigned long long)qa) : 0;
                    unsigned long long tb = qb > 0 ? platform_counter_time_ns((unsigned long long)qb) : 0;
                    if (tb > D.gpu_end_ns) D.gpu_end_ns = tb;
                    if (pace_log_on()) pace_log_gpu(D.q_pid[D.slot], ta, tb);
                }
            }
            rr.End = rr.Begin;
            ID3D12Resource_Unmap(D.qread, 0, &rr);
        }
        D.q_valid[D.slot] = 0;
    }
    ID3D12CommandAllocator_Reset(D.alloc[D.slot]);
    ID3D12GraphicsCommandList_Reset(D.cl, D.alloc[D.slot], NULL);
    if (D.q_ok)
        ID3D12GraphicsCommandList_EndQuery(D.cl, D.qheap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)D.slot * 2);
    D.seg_off = 0;
    D.srv_gpu_off = 0;
    apply_frame_state();
}

/* mean GPU time of the frames finished since the last call (ms), 0 = n/a */
static double dx_gpu_frame_ms(void) {
    double r = D.gpu_ms_n ? D.gpu_ms_acc / (double)D.gpu_ms_n : 0.0;
    D.gpu_ms_acc = 0.0; D.gpu_ms_n = 0;
    return r;
}

/* SMB2_PACE_LOG: a thread polls the swap chain's frame statistics and
 * keeps every change (which present reached the screen at which refresh,
 * the latest vblank's time), written at exit by pace_log.c */
typedef struct { uint64_t t, sync_ns; UINT pc, prc, src; signed char mode; } DxDispRec;
static DxDispRec* s_dd; static volatile LONG s_dd_n; static LONG s_dd_cap;
static DWORD WINAPI dx_disp_thread(LPVOID u) {
    extern unsigned long long platform_get_time_ns(void);
    extern unsigned long long platform_counter_time_ns(unsigned long long counter);
    UINT last_pc = 0, last_src = 0;
    IDXGISwapChainMedia* media = NULL;
    uint64_t last_media = 0;
    int mode = -1;
    (void)u;
    for (;;) {
        DXGI_FRAME_STATISTICS st;
        uint64_t now = platform_get_time_ns();
        if (!media && D.swap && mode == -1) {
            HRESULT h = IDXGISwapChain3_QueryInterface(D.swap, &IID_IDXGISwapChainMedia, (void**)&media);
            if (FAILED(h)) { mode = -2; printf("[pacelog] no IDXGISwapChainMedia (0x%08lx)\n", (unsigned long)h); }
        }
        if (media && now - last_media > 250000000ULL) {
            DXGI_FRAME_STATISTICS_MEDIA sm;
            HRESULT h;
            last_media = now;
            h = IDXGISwapChainMedia_GetFrameStatisticsMedia(media, &sm);
            if (SUCCEEDED(h)) mode = (int)sm.CompositionMode;
            else mode = -3;
        }
        if (D.swap && SUCCEEDED(IDXGISwapChain3_GetFrameStatistics(D.swap, &st)) &&
            (st.PresentCount != last_pc || st.SyncRefreshCount != last_src)) {
            LONG i = s_dd_n;
            last_pc = st.PresentCount; last_src = st.SyncRefreshCount;
            if (i < s_dd_cap) {
                s_dd[i].t = now;
                s_dd[i].sync_ns = platform_counter_time_ns((unsigned long long)st.SyncQPCTime.QuadPart);
                s_dd[i].pc = st.PresentCount; s_dd[i].prc = st.PresentRefreshCount;
                s_dd[i].src = st.SyncRefreshCount; s_dd[i].mode = (signed char)mode;
                InterlockedExchange(&s_dd_n, i + 1);
            }
        }
        Sleep(1);   /* flips are >= 4 ms apart; SyncQPCTime is exact anyway */
    }
    return 0;
}
static void dx_disp_dump(void) {
    LONG i, n = s_dd_n;
    for (i = 0; i < n; i++)
        printf("[pd] %llu %u %u %u %llu %d\n", (unsigned long long)s_dd[i].t, s_dd[i].pc, s_dd[i].prc,
               s_dd[i].src, (unsigned long long)s_dd[i].sync_ns, s_dd[i].mode);
}
static void dx_disp_log_start(void) {
    extern int pace_log_on(void);
    extern void (*g_pace_disp_dump)(void);
    static int started;
    if (started || !pace_log_on()) return;
    started = 1;
    s_dd_cap = 1 << 20;
    s_dd = (DxDispRec*)calloc((size_t)s_dd_cap, sizeof *s_dd);
    if (!s_dd) return;
    g_pace_disp_dump = dx_disp_dump;
    CreateThread(NULL, 0, dx_disp_thread, NULL, 0, NULL);
}
static unsigned dx_present_id(void) {
    UINT n = 0;
    if (!D.ok || !D.swap || FAILED(IDXGISwapChain3_GetLastPresentCount(D.swap, &n))) return 0;
    return n;
}

static void dx_present(void) {
    ID3D12CommandList* lists[1];
    ID3D12Resource* bb;
    UINT idx;
    HRESULT hr;
    if (!D.ok) return;
    idx = IDXGISwapChain3_GetCurrentBackBufferIndex(D.swap);
    bb = D.bb[idx];
    if (D.msaa > 1) {
        rt_to(D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        ID3D12GraphicsCommandList_ResolveSubresource(D.cl, bb, 0, D.rt, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        barrier(bb, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_PRESENT);
    } else {
        rt_to(D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12GraphicsCommandList_CopyResource(D.cl, bb, D.rt);
        barrier(bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    }
    rt_to(D3D12_RESOURCE_STATE_RENDER_TARGET);
    {   /* SMB2_PACE_MARK=1: the present id in the top-left corner for
         * tools/hfr/dispcap (white, black, 16 bits MSB first) */
        static int mark = -1;
        if (mark < 0) mark = getenv("SMB2_PACE_MARK") && atoi(getenv("SMB2_PACE_MARK"));
        if (mark) {
            static const FLOAT wh[4] = { 1, 1, 1, 1 }, bk[4] = { 0, 0, 0, 1 };
            D3D12_RECT rw[18], rb[18];
            int nw = 0, nb = 0, b;
            unsigned id = dx_present_id() + 1;
            for (b = 0; b < 18; b++) {
                int one = b == 0 ? 1 : b == 1 ? 0 : (int)((id >> (15 - (b - 2))) & 1);
                D3D12_RECT r = { 8 * b, 0, 8 * b + 8, 8 };
                if (one) rw[nw++] = r; else rb[nb++] = r;
            }
            barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            ID3D12GraphicsCommandList_ClearRenderTargetView(D.cl, heap_cpu(D.rtv_heap, idx, D.rtv_inc), wh, (UINT)nw, rw);
            ID3D12GraphicsCommandList_ClearRenderTargetView(D.cl, heap_cpu(D.rtv_heap, idx, D.rtv_inc), bk, (UINT)nb, rb);
            barrier(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        }
    }
    if (D.q_ok) {
        ID3D12GraphicsCommandList_EndQuery(D.cl, D.qheap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)D.slot * 2 + 1);
        ID3D12GraphicsCommandList_ResolveQueryData(D.cl, D.qheap, D3D12_QUERY_TYPE_TIMESTAMP,
                                                   (UINT)D.slot * 2, 2, D.qread, (UINT64)D.slot * 16);
        D.q_valid[D.slot] = 1;
    }
    hr = ID3D12GraphicsCommandList_Close(D.cl);
    if (FAILED(hr)) {
        dx_check_removed(hr, "Close");
        fprintf(stderr, "[d3d12] command list close failed 0x%08lx\n", (unsigned long)hr);
        dx_dump_messages();
    }
    lists[0] = (ID3D12CommandList*)D.cl;
    ID3D12CommandQueue_ExecuteCommandLists(D.queue, 1, lists);
    dx_disp_log_start();
    hr = IDXGISwapChain3_Present(D.swap, D.vsync ? (UINT)D.si : 0,
                                 (!D.vsync && D.tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0);
    dx_check_removed(hr, "Present");
    D.q_pid[D.slot] = dx_present_id();
    D.fence_value++;
    ID3D12CommandQueue_Signal(D.queue, D.fence, D.fence_value);
    D.frame_fence[D.slot] = D.fence_value;
    D.slot = (D.slot + 1) % NFRAMES;
    D.frames++;
    if (D.debug) dx_dump_messages();
    begin_slot();
}

static void wait_idle(void) {
    D.fence_value++;
    ID3D12CommandQueue_Signal(D.queue, D.fence, D.fence_value);
    wait_fence(D.fence_value);
}

static void dx_resize(int width, int height) {
    HRESULT hr;
    ID3D12CommandList* lists[1];
    if (!D.ok || width <= 0 || height <= 0) return;
    if (width == D.width && height == D.height) return;
    /* finish the open list, drain the GPU, rebuild the size-dependent targets */
    D.q_valid[D.slot] = 0;   /* this list ends without its end timestamp */
    ID3D12GraphicsCommandList_Close(D.cl);
    lists[0] = (ID3D12CommandList*)D.cl;
    ID3D12CommandQueue_ExecuteCommandLists(D.queue, 1, lists);
    wait_idle();
    process_deferred();
    release_targets();
    D.width = width;
    D.height = height;
    hr = IDXGISwapChain3_ResizeBuffers(D.swap, (UINT)D.nback, (UINT)width, (UINT)height,
                                       DXGI_FORMAT_R8G8B8A8_UNORM,
                                       D.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    dx_check_removed(hr, "ResizeBuffers");
    if (FAILED(hr) || !create_targets()) {
        fprintf(stderr, "[d3d12] resize to %dx%d failed (0x%08lx)\n", width, height, (unsigned long)hr);
        fflush(NULL);
        exit(3);
    }
    fprintf(stderr, "[d3d12] resized to %dx%d\n", width, height);
    ID3D12CommandAllocator_Reset(D.alloc[D.slot]);
    ID3D12GraphicsCommandList_Reset(D.cl, D.alloc[D.slot], NULL);
    D.seg_off = 0;
    D.srv_gpu_off = 0;
    apply_frame_state();
}

static void dx_set_vsync(int on) { D.vsync = on != 0; }

/* ---- frame pacing (docs/hfr.md section 4) ---- */
static void dx_present_interval(int si) { D.si = si < 1 ? 1 : si > 4 ? 4 : si; }

/* The HFR display-grid pacer queues a tick's frames ahead (each with the
 * sync interval that puts it on its refresh): more back buffers, so a
 * frame never waits on the GPU for one that is still queued. */
static void dx_present_queue(int n) {
    ID3D12CommandList* lists[1];
    HRESULT hr;
    int i, want = n <= 0 ? NBACK_DEFAULT : n + 2;
    if (want > NBACK) want = NBACK;
    if (want < NBACK_DEFAULT) want = NBACK_DEFAULT;
    if (!D.ok || want == D.nback) return;
    D.q_valid[D.slot] = 0;   /* this list ends without its end timestamp */
    ID3D12GraphicsCommandList_Close(D.cl);
    lists[0] = (ID3D12CommandList*)D.cl;
    ID3D12CommandQueue_ExecuteCommandLists(D.queue, 1, lists);
    wait_idle();
    process_deferred();
    for (i = 0; i < NBACK; i++)
        if (D.bb[i]) { ID3D12Resource_Release(D.bb[i]); D.bb[i] = NULL; }
    hr = IDXGISwapChain3_ResizeBuffers(D.swap, (UINT)want, (UINT)D.width, (UINT)D.height,
                                       DXGI_FORMAT_R8G8B8A8_UNORM,
                                       D.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    dx_check_removed(hr, "ResizeBuffers");
    if (SUCCEEDED(hr)) D.nback = want;
    for (i = 0; i < D.nback; i++) {
        if (FAILED(IDXGISwapChain3_GetBuffer(D.swap, (UINT)i, &IID_ID3D12Resource, (void**)&D.bb[i]))) {
            fprintf(stderr, "[d3d12] back buffers lost (0x%08lx)\n", (unsigned long)hr);
            fflush(NULL);
            exit(3);
        }
        ID3D12Device_CreateRenderTargetView(D.dev, D.bb[i], NULL, heap_cpu(D.rtv_heap, (UINT)i, D.rtv_inc));
    }
    fprintf(stderr, "[d3d12] %d back buffers\n", D.nback);
    ID3D12CommandAllocator_Reset(D.alloc[D.slot]);
    ID3D12GraphicsCommandList_Reset(D.cl, D.alloc[D.slot], NULL);
    D.seg_off = 0;
    D.srv_gpu_off = 0;
    apply_frame_state();
}

/* the swap chain's frame statistics: the latest vblank, the refresh period
 * and the latest present on screen.  SyncQPCTime is when the scheduler read
 * the clock near refresh SyncRefreshCount: sometimes late (0.5-1.5 ms),
 * never early, so a two-point average is biased.  Over the last ~2 s of
 * samples: the period is the median of the consecutive ratios, the vblank
 * the earliest stamp relative to that grid.  Fails (0) until 20 ratios
 * exist, and after a gap or a mode change (DISJOINT, the count going back)
 * it starts over. */
#define DT_RING 256
static int dx_display_timing(GxDisplayTiming* t) {
    extern unsigned long long platform_counter_time_ns(unsigned long long counter);
    static unsigned long long rt[DT_RING]; static UINT rc[DT_RING]; static int rn, rh;
    DXGI_FRAME_STATISTICS st;
    unsigned long long tv, vb;
    double rat[DT_RING], P, x;
    HRESULT hr;
    int i, j, k, n;
    if (!D.ok || !D.swap || !D.vsync) return 0;
    hr = IDXGISwapChain3_GetFrameStatistics(D.swap, &st);
    if (getenv("SMB2_PACE_DBG")) {
        static int dn;
        if (dn++ % 240 == 0) printf("[dxstat] hr 0x%08lx pc %u prc %u src %u qpc %lld\n", (unsigned long)hr,
                                    st.PresentCount, st.PresentRefreshCount, st.SyncRefreshCount, (long long)st.SyncQPCTime.QuadPart);
    }
    if (FAILED(hr) || st.SyncQPCTime.QuadPart <= 0) { rn = 0; return 0; }
    tv = platform_counter_time_ns((unsigned long long)st.SyncQPCTime.QuadPart);
    if (!tv) return 0;
    if (rn) {
        int last = (rh + DT_RING - 1) % DT_RING;
        if (st.SyncRefreshCount < rc[last] || tv < rt[last] ||
            tv - rt[last] > 1000000000ULL) rn = 0;            /* mode change / gap */
        else if (st.SyncRefreshCount == rc[last]) goto have;   /* nothing new */
    }
    rt[rh] = tv; rc[rh] = st.SyncRefreshCount;
    rh = (rh + 1) % DT_RING;
    if (rn < DT_RING) rn++;
    /* keep ~2 s */
    while (rn > 2 && tv - rt[(rh + DT_RING - rn) % DT_RING] > 2000000000ULL) rn--;
have:
    for (i = 1, n = 0; i < rn; i++) {
        int a0 = (rh + DT_RING - rn + i - 1) % DT_RING, a1 = (a0 + 1) % DT_RING;
        UINT dc = rc[a1] - rc[a0];
        if (dc >= 1 && dc <= 8) rat[n++] = (double)(rt[a1] - rt[a0]) / (double)dc;
    }
    if (n < 20) return 0;
    for (i = 1; i < n; i++) {   /* insertion sort, n < 256 */
        x = rat[i];
        for (j = i - 1; j >= 0 && rat[j] > x; j--) rat[j + 1] = rat[j];
        rat[j + 1] = x;
    }
    P = rat[n / 2];
    k = (rh + DT_RING - 1) % DT_RING;
    vb = rt[k];
    for (i = 0; i < rn; i++) {
        int a = (rh + DT_RING - rn + i) % DT_RING;
        double proj = (double)rt[a] + (double)(rc[k] - rc[a]) * P;
        if (proj < (double)vb) vb = (unsigned long long)proj;
    }
    t->vblank_ns = vb;
    t->vblank_count = rc[k];
    t->period_ns = P;
    t->shown_id = st.PresentCount;
    t->shown_count = st.PresentRefreshCount;
    return 1;
}

static void dx_begin_frame(void) {}

/* F1 overlay */
static int dx_capture(void) {
    ID3D12Resource* src;
    if (!D.ok) return 0;
    if (!D.cap || D.cap_w != D.width || D.cap_h != D.height) {
        if (D.cap) defer_release((IUnknown*)D.cap);
        D.cap = make_tex2d(D.width, D.height, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, NULL);
        if (!D.cap) { D.cap_ok = 0; return 0; }
        if (!D.cap_srv.ptr) D.cap_srv = alloc_cpu_srv();
        ID3D12Device_CreateShaderResourceView(D.dev, D.cap, NULL, D.cap_srv);
        D.cap_w = D.width; D.cap_h = D.height;
    } else {
        barrier(D.cap, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    src = resolved_source();
    ID3D12GraphicsCommandList_CopyResource(D.cl, D.cap, src);
    barrier(D.cap, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    rt_to(D3D12_RESOURCE_STATE_RENDER_TARGET);
    D.cap_ok = 1;
    return 1;
}

static void dx_restore(float brightness) {
    D3D12_CPU_DESCRIPTOR_HANDLE dst;
    D3D12_GPU_DESCRIPTOR_HANDLE tab;
    D3D12_VIEWPORT v;
    D3D12_RECT r;
    FLOAT black[4] = {0, 0, 0, 1};
    float k[4];
    if (!D.ok || !D.cap_ok) return;
    rt_to(D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_ClearRenderTargetView(D.cl, rt_rtv(), black, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(D.cl, rt_dsv(),
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, NULL);
    if (D.srv_gpu_off + 1 > SRV_SEG) submit_and_wait();
    dst = heap_cpu(D.srv_gpu, (UINT)(D.slot * SRV_SEG) + D.srv_gpu_off, D.srv_inc);
    ID3D12Device_CopyDescriptorsSimple(D.dev, 1, dst, D.cap_srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    tab = heap_gpu(D.srv_gpu, (UINT)(D.slot * SRV_SEG) + D.srv_gpu_off, D.srv_inc);
    D.srv_gpu_off++;
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(D.cl, D.blit_rootsig);
    ID3D12GraphicsCommandList_SetPipelineState(D.cl, D.blit_pso);
    k[0] = brightness; k[1] = k[2] = k[3] = 0.0f;
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(D.cl, 0, 4, k, 0);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(D.cl, 1, tab);
    v.TopLeftX = 0; v.TopLeftY = 0; v.Width = (float)D.width; v.Height = (float)D.height;
    v.MinDepth = 0; v.MaxDepth = 1;
    ID3D12GraphicsCommandList_RSSetViewports(D.cl, 1, &v);
    r.left = 0; r.top = 0; r.right = D.width; r.bottom = D.height;
    ID3D12GraphicsCommandList_RSSetScissorRects(D.cl, 1, &r);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(D.cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(D.cl, 3, 1, 0, 0);
    /* back to the game's state */
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(D.cl, D.rootsig);
    apply_viewport();
    apply_scissor();
    D.cur_pso = NULL;
    D.cur_topo = -1;
    D.tables_dirty = 1;
    D.last_dc_ok = D.last_tc_ok = 0;
}

/* ----------------------------------------------------------------- init */
static int load_dlls(void) {
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    HMODULE comp = LoadLibraryA("d3dcompiler_47.dll");
    if (!d3d12 || !dxgi || !comp) {
        fprintf(stderr, "[d3d12] missing %s\n", !d3d12 ? "d3d12.dll" : !dxgi ? "dxgi.dll" : "d3dcompiler_47.dll");
        return 0;
    }
    p_D3D12CreateDevice = (PFN_D3D12_CREATE_DEVICE)(void (*)(void))GetProcAddress(d3d12, "D3D12CreateDevice");
    p_D3D12GetDebugInterface = (PFN_D3D12_GET_DEBUG_INTERFACE)(void (*)(void))GetProcAddress(d3d12, "D3D12GetDebugInterface");
    p_D3D12SerializeRootSignature = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(void (*)(void))GetProcAddress(d3d12, "D3D12SerializeRootSignature");
    p_CreateDXGIFactory2 = (PFN_CreateDXGIFactory2)(void (*)(void))GetProcAddress(dxgi, "CreateDXGIFactory2");
    p_D3DCompile = (pD3DCompile)(void (*)(void))GetProcAddress(comp, "D3DCompile");
    return p_D3D12CreateDevice && p_D3D12SerializeRootSignature && p_CreateDXGIFactory2 && p_D3DCompile;
}

static IDXGIAdapter1* pick_adapter(void) {
    IDXGIFactory6* f6 = NULL;
    IDXGIAdapter1* a = NULL;
    if (SUCCEEDED(IDXGIFactory4_QueryInterface(D.factory, &IID_IDXGIFactory6, (void**)&f6))) {
        UINT i;
        for (i = 0; ; i++) {
            DXGI_ADAPTER_DESC1 ad;
            if (FAILED(IDXGIFactory6_EnumAdapterByGpuPreference(f6, i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                &IID_IDXGIAdapter1, (void**)&a)))
                break;
            IDXGIAdapter1_GetDesc1(a, &ad);
            if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                SUCCEEDED(p_D3D12CreateDevice((IUnknown*)a, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, NULL)))
                break;
            IDXGIAdapter1_Release(a);
            a = NULL;
        }
        IDXGIFactory6_Release(f6);
    }
    return a;
}

static bool dx_init(int width, int height, bool vsync, int msaa) {
    SDL_SysWMinfo wm;
    SDL_Window* win = (SDL_Window*)platform_sdl_window();
    D3D12_COMMAND_QUEUE_DESC qd;
    DXGI_SWAP_CHAIN_DESC1 sd;
    IDXGISwapChain1* sc1 = NULL;
    IDXGIAdapter1* adapter;
    HRESULT hr;
    int i, pw = 0, ph = 0;
    const char* dbg = getenv("SMB2_D3D12_DEBUG");

    memset(&D, 0, sizeof D);
    if (!win) return false;
    if (getenv("SMB2_D3D12_FORCEFAIL")) {   /* test hook for the GL fallback */
        fprintf(stderr, "[d3d12] SMB2_D3D12_FORCEFAIL set - failing init\n");
        return false;
    }
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(win, &wm) || wm.subsystem != SDL_SYSWM_WINDOWS) {
        fprintf(stderr, "[d3d12] no HWND: %s\n", SDL_GetError());
        return false;
    }
    D.hwnd = wm.info.win.window;
    if (!load_dlls()) return false;
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    D.width = pw > 0 ? pw : width;
    D.height = ph > 0 ? ph : height;
    D.vsync = vsync;
    D.debug = dbg && dbg[0] && dbg[0] != '0';

    if (D.debug && p_D3D12GetDebugInterface) {
        ID3D12Debug* dbgi = NULL;
        if (SUCCEEDED(p_D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&dbgi))) {
            ID3D12Debug_EnableDebugLayer(dbgi);
            if (dbg[0] == '2') {
                ID3D12Debug1* d1 = NULL;
                if (SUCCEEDED(ID3D12Debug_QueryInterface(dbgi, &IID_ID3D12Debug1, (void**)&d1))) {
                    ID3D12Debug1_SetEnableGPUBasedValidation(d1, TRUE);
                    ID3D12Debug1_Release(d1);
                }
            }
            ID3D12Debug_Release(dbgi);
            fprintf(stderr, "[d3d12] debug layer enabled%s\n", dbg[0] == '2' ? " (+GPU validation)" : "");
        } else {
            fprintf(stderr, "[d3d12] debug layer not available (Graphics Tools not installed?)\n");
        }
    }
    hr = p_CreateDXGIFactory2(D.debug ? DXGI_CREATE_FACTORY_DEBUG : 0, &IID_IDXGIFactory4, (void**)&D.factory);
    if (FAILED(hr))
        hr = p_CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void**)&D.factory);
    if (FAILED(hr)) { fprintf(stderr, "[d3d12] CreateDXGIFactory2 failed 0x%08lx\n", (unsigned long)hr); return false; }

    adapter = pick_adapter();
    hr = p_D3D12CreateDevice((IUnknown*)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&D.dev);
    if (adapter) {
        DXGI_ADAPTER_DESC1 ad;
        IDXGIAdapter1_GetDesc1(adapter, &ad);
        fprintf(stderr, "[d3d12] adapter: %ls (%u MB)\n", ad.Description,
                (unsigned)(ad.DedicatedVideoMemory >> 20));
        IDXGIAdapter1_QueryInterface(adapter, &IID_IDXGIAdapter3, (void**)&D.adapter3);
        IDXGIAdapter1_Release(adapter);
    }
    if (FAILED(hr)) { fprintf(stderr, "[d3d12] D3D12CreateDevice failed 0x%08lx\n", (unsigned long)hr); return false; }
    if (D.debug &&
        SUCCEEDED(ID3D12Device_QueryInterface(D.dev, &IID_ID3D12InfoQueue, (void**)&D.infoq))) {
        D3D12_INFO_QUEUE_FILTER f;
        D3D12_MESSAGE_ID deny[2];
        ID3D12InfoQueue_SetMessageCountLimit(D.infoq, 4096);
        /* perf-only notes: clears to colours other than the optimized one */
        deny[0] = D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE;
        deny[1] = D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE;
        memset(&f, 0, sizeof f);
        f.DenyList.NumIDs = 2;
        f.DenyList.pIDList = deny;
        ID3D12InfoQueue_AddStorageFilterEntries(D.infoq, &f);
    }

    /* MSAA: the requested count if the RT and depth formats support it */
    D.msaa = 1;
    if (msaa > 1) {
        int s;
        for (s = msaa > 8 ? 8 : msaa; s > 1; s /= 2) {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q;
            memset(&q, 0, sizeof q);
            q.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            q.SampleCount = (UINT)s;
            if (FAILED(ID3D12Device_CheckFeatureSupport(D.dev, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof q)) ||
                !q.NumQualityLevels) continue;
            q.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            if (FAILED(ID3D12Device_CheckFeatureSupport(D.dev, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof q)) ||
                !q.NumQualityLevels) continue;
            D.msaa = s;
            break;
        }
    }

    memset(&qd, 0, sizeof qd);
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(ID3D12Device_CreateCommandQueue(D.dev, &qd, &IID_ID3D12CommandQueue, (void**)&D.queue))) return false;
    for (i = 0; i < NFRAMES; i++)
        if (FAILED(ID3D12Device_CreateCommandAllocator(D.dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       &IID_ID3D12CommandAllocator, (void**)&D.alloc[i])))
            return false;
    if (FAILED(ID3D12Device_CreateCommandList(D.dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, D.alloc[0], NULL,
                                              &IID_ID3D12GraphicsCommandList, (void**)&D.cl)))
        return false;
    if (FAILED(ID3D12Device_CreateFence(D.dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&D.fence)))
        return false;
    D.fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    {   /* GPU frame timestamps (statistics only; optional) */
        D3D12_QUERY_HEAP_DESC qh;
        memset(&qh, 0, sizeof qh);
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = NFRAMES * 2;
        if (SUCCEEDED(ID3D12Device_CreateQueryHeap(D.dev, &qh, &IID_ID3D12QueryHeap, (void**)&D.qheap)) &&
            (D.qread = make_buffer(D3D12_HEAP_TYPE_READBACK, NFRAMES * 16, D3D12_RESOURCE_STATE_COPY_DEST)) != NULL &&
            SUCCEEDED(ID3D12CommandQueue_GetTimestampFrequency(D.queue, &D.qfreq)))
            D.q_ok = 1;
    }

    /* swap chain (flip model) */
    {
        IDXGIFactory5* f5 = NULL;
        BOOL allow = FALSE;
        if (SUCCEEDED(IDXGIFactory4_QueryInterface(D.factory, &IID_IDXGIFactory5, (void**)&f5))) {
            if (FAILED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow)))
                allow = FALSE;
            IDXGIFactory5_Release(f5);
        }
        D.tearing = allow != FALSE;
    }
    memset(&sd, 0, sizeof sd);
    sd.Width = (UINT)D.width;
    sd.Height = (UINT)D.height;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    D.nback = NBACK_DEFAULT;
    D.si = 1;
    sd.BufferCount = (UINT)D.nback;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    sd.Flags = D.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    hr = IDXGIFactory4_CreateSwapChainForHwnd(D.factory, (IUnknown*)D.queue, D.hwnd, &sd, NULL, NULL, &sc1);
    if (FAILED(hr)) { fprintf(stderr, "[d3d12] CreateSwapChainForHwnd failed 0x%08lx\n", (unsigned long)hr); return false; }
    hr = IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void**)&D.swap);
    IDXGISwapChain1_Release(sc1);
    if (FAILED(hr)) return false;
    /* SDL handles Alt+Enter (pcs_toggle_fullscreen) */
    IDXGIFactory4_MakeWindowAssociation(D.factory, D.hwnd, DXGI_MWA_NO_ALT_ENTER);

    D.rtv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(D.dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D.srv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(D.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D.smp_inc = ID3D12Device_GetDescriptorHandleIncrementSize(D.dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    D.rtv_heap = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, NBACK + 1, 0);
    D.dsv_heap = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, 0);
    D.srv_gpu = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SRV_SEG * NFRAMES, 1);
    D.smp_cpu = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, MAX_UNIQUE_SAMPLERS, 0);
    D.smp_gpu = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SMP_HEAP, 1);
    if (!D.rtv_heap || !D.dsv_heap || !D.srv_gpu || !D.smp_cpu || !D.smp_gpu) return false;
    if (!create_targets()) return false;
    if (!init_pipeline()) return false;

    /* upload ring */
    {
        D3D12_RANGE none = {0, 0};
        void* p = NULL;
        D.upload = make_buffer(D3D12_HEAP_TYPE_UPLOAD, SEG_SIZE * NFRAMES, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!D.upload || FAILED(ID3D12Resource_Map(D.upload, 0, &none, &p))) return false;
        D.upload_ptr = (uint8_t*)p;
        D.upload_va = ID3D12Resource_GetGPUVirtualAddress(D.upload);
    }
    /* static index buffer: quads (0 1 2, 0 2 3) then fans (0 i i+1) */
    {
        UINT nq = GX_MAX_BATCH_VERTS / 4, nf = GX_MAX_BATCH_VERTS - 2, k;
        size_t bytes = ((size_t)nq * 6 + (size_t)nf * 3) * sizeof(uint16_t);
        D3D12_GPU_VIRTUAL_ADDRESS va;
        uint16_t* ix = (uint16_t*)ring_alloc(bytes, 16, &va);
        if (!ix) return false;
        for (k = 0; k < nq; k++) {
            ix[k * 6 + 0] = (uint16_t)(k * 4 + 0); ix[k * 6 + 1] = (uint16_t)(k * 4 + 1);
            ix[k * 6 + 2] = (uint16_t)(k * 4 + 2); ix[k * 6 + 3] = (uint16_t)(k * 4 + 0);
            ix[k * 6 + 4] = (uint16_t)(k * 4 + 2); ix[k * 6 + 5] = (uint16_t)(k * 4 + 3);
        }
        D.fan_first = nq * 6;
        for (k = 0; k < nf; k++) {
            ix[D.fan_first + k * 3 + 0] = 0;
            ix[D.fan_first + k * 3 + 1] = (uint16_t)(k + 1);
            ix[D.fan_first + k * 3 + 2] = (uint16_t)(k + 2);
        }
        /* buffers start in COMMON and are promoted to COPY_DEST by the copy */
        D.ibuf = make_buffer(D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COMMON);
        D.zero_vb = make_buffer(D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_COMMON);
        if (!D.ibuf || !D.zero_vb) return false;
        ID3D12GraphicsCommandList_CopyBufferRegion(D.cl, D.ibuf, 0, D.upload, va - D.upload_va, bytes);
        barrier(D.ibuf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
        {   /* zero_vb: zero 256 bytes from the ring */
            D3D12_GPU_VIRTUAL_ADDRESS zva;
            void* z = ring_alloc(256, 16, &zva);
            if (!z) return false;
            memset(z, 0, 256);
            ID3D12GraphicsCommandList_CopyBufferRegion(D.cl, D.zero_vb, 0, D.upload, zva - D.upload_va, 256);
            barrier(D.zero_vb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        }
        D.ibv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(D.ibuf);
        D.ibv.SizeInBytes = (UINT)bytes;
        D.ibv.Format = DXGI_FORMAT_R16_UINT;
    }
    /* 1x1 (0,0,0,1): what GL samples from an unbound unit */
    {
        uint32_t black = 0xFF000000u;
        D.dummy = (DxTex*)dx_tex_create(&black, 1, 1, 0, 0, 0);
        if (!D.dummy) return false;
    }
    /* initial GL-equivalent state (gx_core starts in the same state) */
    D.vp[2] = D.width; D.vp[3] = D.height;
    D.zfunc = GX_LEQUAL; D.zwrite = 1;
    D.cull = GX_CULL_BACK;
    D.ok = 1;
    /* run the setup copies, then start frame slot 0 cleanly */
    submit_and_wait();
    fprintf(stderr, "[d3d12] initialized %dx%d msaa %d vsync %d tearing %d\n",
            D.width, D.height, D.msaa, D.vsync, D.tearing);
    printf("D3D12 render backend initialized: %dx%d (MSAA %dx)\n", D.width, D.height, D.msaa);
    return true;
}

static void dx_shutdown(void) {
    if (!D.ok) return;
    ID3D12GraphicsCommandList_Close(D.cl);
    wait_idle();
    D.ok = 0;
    /* process exit releases the rest */
}

const GxGpu gx_gpu_d3d12 = {
    .name = "Direct3D 12",
    .init = dx_init,
    .shutdown = dx_shutdown,
    .begin_frame = dx_begin_frame,
    .resize = dx_resize,
    .present = dx_present,
    .set_vsync = dx_set_vsync,
    .set_viewport = dx_set_viewport,
    .set_scissor = dx_set_scissor,
    .set_blend = dx_set_blend,
    .set_depth = dx_set_depth,
    .set_cull = dx_set_cull,
    .clear = dx_clear,
    .tex_create = dx_tex_create,
    .tex_image = dx_tex_image,
    .tex_wrap = dx_tex_wrap,
    .tex_lod = dx_tex_lod,
    .tex_destroy = dx_tex_destroy,
    .bind_tex = dx_bind_tex,
    .draw = dx_draw,
    .read_pixels = dx_read_pixels,
    .read_pixels_box = dx_read_pixels_box,
    .copy_tex = dx_copy_tex,
    .copy_tex_result = dx_copy_tex_result,
    .gpu_mem = dx_gpu_mem,
    .gpu_frame_ms = dx_gpu_frame_ms,
    .present_id = dx_present_id,
    .display_timing = dx_display_timing,
    .present_interval = dx_present_interval,
    .present_queue = dx_present_queue,
    .read_frame = dx_read_frame,
    .capture = dx_capture,
    .restore = dx_restore,
};
#endif /* _WIN32 */
