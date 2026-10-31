// Luma sharpen for the eye images (config Sharpen = strength, 0 = off; SharpenClamp = max change per pixel).
// The back buffer is first scaled into a temporary texture at the eye size (StretchRect, linear), then a pixel shader
// draws it 1:1 into the eye rectangle of the VR atlas, adding back the luma difference to a 4-tap bilinear blur
// (LumaSharpen "pattern 2"). The shader is compiled at runtime with d3dcompiler_47.dll (part of Windows).
#include <windows.h>
#include <d3d9.h>
#include <d3dcommon.h>
#include <cstring>
#include "core.h"
#include "common/log.h"
#include "common/config.h"

namespace svr {

static float g_strength = 0.f, g_clamp = 0.035f, g_radius = 1.0f;
static IDirect3DPixelShader9* g_ps = nullptr;
static IDirect3DTexture9* g_tmp = nullptr;
static IDirect3DSurface9* g_tmpSurf = nullptr;
static unsigned g_tw = 0, g_th = 0;
static IDirect3DStateBlock9* g_sb = nullptr;
static bool g_failed = false;

void Sharpen_LoadSettings(const xrb::Config& c) {
    g_strength = c.GetFloat("Sharpen", 0.f);
    g_clamp = c.GetFloat("SharpenClamp", 0.035f);
    g_radius = c.GetFloat("SharpenRadius", 1.0f);
}
void Sharpen_Set(float strength, float clampV, float radius) { g_strength = strength; g_clamp = clampV; g_radius = radius; g_failed = false; }
bool Sharpen_Enabled() { return g_strength > 0.001f && !g_failed; }

// ReShade LumaSharpen (pattern 2, "normal"), same parameters: strength, clamp, offset bias (radius).
static const char* kPs =
    "sampler2D s0 : register(s0);\n"
    "float4 p : register(c0);\n"      // x = 1/w, y = 1/h, z = strength, w = clamp
    "float4 q : register(c1);\n"      // x = offset bias (radius)
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float3 ori = tex2D(s0, uv).rgb;\n"
    "  float2 o = float2(p.x, p.y) * 0.5 * q.x;\n"
    "  float3 blur = tex2D(s0, uv + float2(o.x, -o.y)).rgb;\n"
    "  blur += tex2D(s0, uv - float2(o.x, -o.y)).rgb;\n"
    "  blur += tex2D(s0, uv + o).rgb;\n"
    "  blur += tex2D(s0, uv - o).rgb;\n"
    "  blur *= 0.25;\n"
    "  float3 sharp = ori - blur;\n"
    "  float3 sl = float3(0.2126, 0.7152, 0.0722) * p.z;\n"
    "  float4 slc = float4(sl * (0.5 / p.w), 0.5);\n"
    "  float l = saturate(dot(float4(sharp, 1.0), slc));\n"
    "  l = (p.w * 2.0) * l - p.w;\n"
    "  return float4(saturate(ori + l), 1);\n"
    "}\n";

typedef HRESULT(WINAPI* PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

static bool CreateShader(IDirect3DDevice9* dev) {
    if (g_ps) return true;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    PFN_D3DCompile comp = dc ? (PFN_D3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!comp) { LOGW("sharpen: d3dcompiler_47.dll not available - sharpen off"); return false; }
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = comp(kPs, strlen(kPs), "sharpen", nullptr, nullptr, "main", "ps_3_0", 0, 0, &code, &err);
    if (FAILED(hr) || !code) {
        LOGW("sharpen: shader compile failed 0x%08lx %s", hr, err ? (const char*)err->GetBufferPointer() : "");
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();
    hr = dev->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &g_ps);
    code->Release();
    if (FAILED(hr)) { LOGW("sharpen: CreatePixelShader failed 0x%08lx", hr); g_ps = nullptr; return false; }
    LOGI("sharpen: ready (strength %.2f, clamp %.3f, radius %.2f)", g_strength, g_clamp, g_radius);
    return true;
}

static bool EnsureTemp(IDirect3DDevice9* dev, unsigned w, unsigned h) {
    if (g_tmp && g_tw == w && g_th == h) return true;
    if (g_tmpSurf) { g_tmpSurf->Release(); g_tmpSurf = nullptr; }
    if (g_tmp) { g_tmp->Release(); g_tmp = nullptr; }
    if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_tmp, nullptr)) || !g_tmp) return false;
    g_tmp->GetSurfaceLevel(0, &g_tmpSurf);
    g_tw = w; g_th = h;
    return g_tmpSurf != nullptr;
}

// Copy the back buffer into `dst` (rect `r`) with sharpening. False: not done (caller falls back to StretchRect).
bool Sharpen_CopyBackBuffer(IDirect3DDevice9* dev, IDirect3DSurface9* dst, const RECT& r) {
    if (!Sharpen_Enabled()) return false;
    if (!CreateShader(dev)) { g_failed = true; return false; }
    unsigned w = r.right - r.left, h = r.bottom - r.top;
    if (!EnsureTemp(dev, w, h)) { g_failed = true; LOGW("sharpen: temp texture failed - sharpen off"); return false; }
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    HRESULT hr = dev->StretchRect(bb, nullptr, g_tmpSurf, nullptr, D3DTEXF_LINEAR);
    bb->Release();
    if (FAILED(hr)) return false;

    if (!g_sb && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_sb))) g_sb = nullptr;
    if (g_sb) g_sb->Capture();
    IDirect3DSurface9* oldRt = nullptr; IDirect3DSurface9* oldDs = nullptr;
    dev->GetRenderTarget(0, &oldRt);
    dev->GetDepthStencilSurface(&oldDs);
    IDirect3DSurface9* oldMrt[3] = {};
    for (DWORD i = 1; i < 4; i++) { dev->GetRenderTarget(i, &oldMrt[i - 1]); if (oldMrt[i - 1]) dev->SetRenderTarget(i, nullptr); }

    dev->SetRenderTarget(0, dst);
    dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = { (DWORD)r.left, (DWORD)r.top, w, h, 0.f, 1.f };
    dev->SetViewport(&vp);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(g_ps);
    float c0[4] = { 1.f / w, 1.f / h, g_strength, g_clamp > 0.0005f ? g_clamp : 0.0005f };
    dev->SetPixelShaderConstantF(0, c0, 1);
    float c1[4] = { g_radius, 0, 0, 0 };
    dev->SetPixelShaderConstantF(1, c1, 1);
    dev->SetTexture(0, g_tmp);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    struct V { float x, y, z, rhw, u, v; };
    float x0 = r.left - 0.5f, y0 = r.top - 0.5f, x1 = r.right - 0.5f, y1 = r.bottom - 0.5f;
    V q[4] = { { x0, y0, 0, 1, 0, 0 }, { x1, y0, 0, 1, 1, 0 }, { x0, y1, 0, 1, 0, 1 }, { x1, y1, 0, 1, 1, 1 } };
    hr = dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));

    if (oldRt) dev->SetRenderTarget(0, oldRt);
    for (DWORD i = 1; i < 4; i++) if (oldMrt[i - 1]) { dev->SetRenderTarget(i, oldMrt[i - 1]); oldMrt[i - 1]->Release(); }
    dev->SetDepthStencilSurface(oldDs);
    if (oldRt) oldRt->Release();
    if (oldDs) oldDs->Release();
    if (g_sb) g_sb->Apply();
    if (FAILED(hr)) {
        static int n = 0;
        if (n++ < 3) LOGW("sharpen: draw failed 0x%08lx", hr);
        return false;
    }
    return true;
}

void Sharpen_Release() {
    if (g_tmpSurf) { g_tmpSurf->Release(); g_tmpSurf = nullptr; }
    if (g_tmp) { g_tmp->Release(); g_tmp = nullptr; }
    if (g_sb) { g_sb->Release(); g_sb = nullptr; }
    if (g_ps) { g_ps->Release(); g_ps = nullptr; }
    g_tw = g_th = 0;
}

} // namespace svr
