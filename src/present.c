/*
 * present.c - the remaster's presenter: the game's frame scaled on the GPU
 * (Direct3D 11) into the window, or offscreen for --present shots. Started
 * from the Gunman Chronicles recomp's presenter (same author, MIT).
 *
 *   sharp    sharp-bilinear: whole pixels stay crisp, only their edges blend
 *            (the default)
 *   smooth   bilinear
 *   crt      scanlines and an aperture-grille mask, faded in from 1.5x
 *   nearest  plain pixels;  integer: nearest at whole multiples only
 *
 * On top, both optional: a vivid colour look, and beside the picture a dark
 * blur of the picture itself instead of black bars (as in the Red Alert 2
 * recomp). The blur is made on the CPU from an eighth-size copy -- a few
 * thousand adds a frame -- so the GPU does one pass.
 *
 * No glow (the SimCity 2000 recomp's light from bright, saturated pixels):
 * here the brightest saturated colour is the tables' yellow plastic, not the
 * lamps, and it bloomed the whole playfield.
 *
 * Any failure (no D3D11, no d3dcompiler_47) returns 0 and host.c draws with
 * GDI instead. Without a GPU, WARP is still a D3D11 device.
 */
#define COBJMACROS
#include "machine.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <math.h>

typedef HRESULT (WINAPI *compile_fn)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR,
                                     UINT, UINT, ID3DBlob **, ID3DBlob **);

static const char k_hlsl[] =
"Texture2D t0 : register(t0);\n"                   /* the frame */
"Texture2D tb : register(t1);\n"                   /* backdrop */
"SamplerState s_lin : register(s0);\n"
"SamplerState s_pt : register(s1);\n"
"cbuffer C : register(b0) { float2 src; float2 pad0; float4 pic; float4 cover;\n"
"                           int mode; int look; int bars; int pad1; };\n"
"struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"V vs(uint id : SV_VertexID) {\n"
"  V o; float2 p = float2((id << 1) & 2, id & 2);\n"
"  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;\n"
"}\n"
"float3 sharp(float2 uv) {\n"
"  float2 scale = max(pic.zw / src, 1.0);\n"
"  float2 texel = uv * src, fl = floor(texel), c = frac(texel) - 0.5;\n"
"  float2 range = 0.5 - 0.5 / scale;\n"
"  float2 f = (c - clamp(c, -range, range)) * scale + 0.5;\n"
"  return t0.Sample(s_lin, (fl + f) / src).rgb;\n"
"}\n"
"float3 crt(float2 uv, float2 px) {\n"
"  float k = saturate((pic.w / src.y - 1.5) / 1.5);\n"
"  float3 col = sharp(uv);\n"
"  float d = frac(uv.y * src.y) - 0.5;\n"
"  float beam = lerp(1.0, exp(-d * d * 8.0) * 1.6, k);\n"
"  uint m = (uint)px.x % 3u;\n"
"  float3 mask = m == 0u ? float3(1.25, 0.87, 0.87) : m == 1u ? float3(0.87, 1.25, 0.87)\n"
"                                                  : float3(0.87, 0.87, 1.25);\n"
"  return col * beam * lerp(float3(1, 1, 1), mask, k);\n"
"}\n"
"float4 ps(V i) : SV_Target {\n"
"  float2 uv = (i.pos.xy - pic.xy) / pic.zw;\n"
"  if (any(uv < 0) || any(uv >= 1))\n"
"    return float4(bars ? tb.Sample(s_lin, (i.pos.xy - cover.xy) / cover.zw).rgb : float3(0, 0, 0), 1);\n"
"  float3 c;\n"
"  if (mode == 0) c = sharp(uv);\n"
"  else if (mode == 1) c = t0.Sample(s_lin, uv).rgb;\n"
"  else if (mode == 2) c = crt(uv, i.pos.xy);\n"
"  else c = t0.Sample(s_pt, uv).rgb;\n"
"  if (look == 1) {\n"                               /* vivid: saturation, a little contrast */
"    float l = dot(c, float3(0.299, 0.587, 0.114));\n"
"    c = lerp(float3(l, l, l), c, 1.3);\n"
"    c = saturate((c - 0.5) * 1.08 + 0.5);\n"
"  }\n"
"  return float4(saturate(c), 1);\n"
"}\n";

typedef struct { ID3D11Texture2D *tex; ID3D11ShaderResourceView *srv; int w, h; } Tex;

static struct {
    int failed;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11SamplerState *samp[2];
    ID3D11Buffer *cb;
    Tex frame, back;
    /* the window */
    HWND hwnd;
    IDXGISwapChain *sc;
    ID3D11RenderTargetView *rtv;
    int cw, ch;
} d;

static int fail(const char *what, HRESULT hr)
{
    fprintf(stderr, "[present] Direct3D 11 unavailable (%s, 0x%08lX): using GDI\n", what, (unsigned long)hr);
    d.failed = 1;
    return 0;
}

static int init(void)
{
    if (d.dev) return 1;
    if (d.failed) return 0;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION,
                                   &d.dev, NULL, &d.ctx);
    if (FAILED(hr))   /* no GPU, or a session without one */
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, &fl, 1, D3D11_SDK_VERSION,
                               &d.dev, NULL, &d.ctx);
    if (FAILED(hr)) return fail("device", hr);

    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    compile_fn compile = dc ? (compile_fn)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!compile) return fail("d3dcompiler_47.dll", 0);
    ID3DBlob *vb = NULL, *pb = NULL, *err = NULL;
    hr = compile(k_hlsl, sizeof k_hlsl - 1, "present", NULL, NULL, "vs", "vs_4_0", 0, 0, &vb, &err);
    if (SUCCEEDED(hr))
        hr = compile(k_hlsl, sizeof k_hlsl - 1, "present", NULL, NULL, "ps", "ps_4_0", 0, 0, &pb, &err);
    if (FAILED(hr)) {
        if (err) fprintf(stderr, "[present] %s\n", (const char *)ID3D10Blob_GetBufferPointer(err));
        return fail("shader", hr);
    }
    ID3D11Device_CreateVertexShader(d.dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &d.vs);
    ID3D11Device_CreatePixelShader(d.dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &d.ps);
    ID3D10Blob_Release(vb); ID3D10Blob_Release(pb);

    D3D11_SAMPLER_DESC s = {0};
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ID3D11Device_CreateSamplerState(d.dev, &s, &d.samp[0]);
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ID3D11Device_CreateSamplerState(d.dev, &s, &d.samp[1]);

    D3D11_BUFFER_DESC b = {0};
    b.ByteWidth = 64;
    b.Usage = D3D11_USAGE_DEFAULT;
    b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(d.dev, &b, NULL, &d.cb);
    return 1;
}

static void upload(Tex *t, const uint32_t *px, int w, int h)
{
    if (!t->tex || t->w != w || t->h != h) {
        if (t->srv) ID3D11ShaderResourceView_Release(t->srv);
        if (t->tex) ID3D11Texture2D_Release(t->tex);
        D3D11_TEXTURE2D_DESC td = {0};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Device_CreateTexture2D(d.dev, &td, NULL, &t->tex);
        ID3D11Device_CreateShaderResourceView(d.dev, (ID3D11Resource *)t->tex, NULL, &t->srv);
        t->w = w; t->h = h;
    }
    ID3D11DeviceContext_UpdateSubresource(d.ctx, (ID3D11Resource *)t->tex, 0, NULL, px, w * 4, 0);
}

/* ---- the backdrop, on the CPU ---------------------------------------------------- */

/* Three box blurs of radius r, across then down: close to a Gaussian.
 * One channel of w x h, in place; tmp holds max(w, h). */
static void blur1(uint16_t *c, int w, int h, int r, uint16_t *tmp)
{
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < h; y++) {
            uint16_t *p = c + y * w; int sum = 0, n = 2 * r + 1;
            for (int x = -r - 1; x < r; x++) sum += p[x < 0 ? 0 : x >= w ? w - 1 : x];
            for (int x = 0; x < w; x++) {
                sum += p[x + r >= w ? w - 1 : x + r] - p[x - r - 1 < 0 ? 0 : x - r - 1];
                tmp[x] = (uint16_t)(sum / n);
            }
            memcpy(p, tmp, w * 2);
        }
        for (int x = 0; x < w; x++) {
            int sum = 0, n = 2 * r + 1;
            for (int y = -r - 1; y < r; y++) sum += c[(y < 0 ? 0 : y >= h ? h - 1 : y) * w + x];
            for (int y = 0; y < h; y++) {
                sum += c[(y + r >= h ? h - 1 : y + r) * w + x] - c[(y - r - 1 < 0 ? 0 : y - r - 1) * w + x];
                tmp[y] = (uint16_t)(sum / n);
            }
            for (int y = 0; y < h; y++) c[y * w + x] = tmp[y];
        }
    }
}

/* The picture, an eighth the size, blurred and darkened: what sits beside it.
 * Frames are at most 640x480, so the eighth is at most 80x60. */
static uint32_t back_buf[80 * 60];
static uint16_t chan[3][80 * 60], tmp16[80];

static void make_backdrop(const uint32_t *f, int w, int h, int *bw, int *bh)
{
    int W = w / 8, H = h / 8;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int s[3] = {0};
            for (int j = 0; j < 8; j++)
                for (int i = 0; i < 8; i++) {
                    uint32_t p = f[(y * 8 + j) * w + x * 8 + i];
                    s[0] += p >> 16 & 255; s[1] += p >> 8 & 255; s[2] += p & 255;
                }
            for (int c = 0; c < 3; c++) chan[c][y * W + x] = (uint16_t)(s[c] / 64);
        }
    for (int c = 0; c < 3; c++) blur1(chan[c], W, H, 2, tmp16);
    for (int i = 0; i < W * H; i++)                    /* at 40% */
        back_buf[i] = 0xFF000000u | (uint32_t)(chan[0][i] * 2 / 5) << 16 | (uint32_t)(chan[1][i] * 2 / 5) << 8 | (uint32_t)(chan[2][i] * 2 / 5);
    *bw = W; *bh = H;
}

/* ---- drawing -------------------------------------------------------------------- */

void present_rect(int cw, int ch, int w, int h, double aspect, int mode, RECT *r)
{
    int pw, ph;
    if (mode == PRESENT_INTEGER) {
        int k = cw / w < ch / h ? cw / w : ch / h;
        if (k < 1) k = 1;
        pw = w * k; ph = h * k;
    } else {
        pw = cw; ph = (int)(cw / aspect + 0.5);
        if (ph > ch) { ph = ch; pw = (int)(ch * aspect + 0.5); }
    }
    r->left = (cw - pw) / 2; r->top = (ch - ph) / 2; r->right = r->left + pw; r->bottom = r->top + ph;
}

static void draw(ID3D11RenderTargetView *rtv, int cw, int ch, const uint32_t *f, int w, int h,
                 double aspect, const Present *s)
{
    int bw, bh;
    upload(&d.frame, f, w, h);
    if (s->bars) { make_backdrop(f, w, h, &bw, &bh); upload(&d.back, back_buf, bw, bh); }
    else if (!d.back.tex) upload(&d.back, f, 1, 1);

    RECT r; present_rect(cw, ch, w, h, aspect, s->mode, &r);
    float covw = (float)cw, covh = (float)(cw / aspect);       /* the backdrop covers the window */
    if (covh < ch) { covh = (float)ch; covw = (float)(ch * aspect); }
    struct { float src[2], pad0[2], pic[4], cover[4]; int mode, look, bars, pad1; } c = {
        { (float)w, (float)h }, { 0, 0 },
        { (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top) },
        { (cw - covw) / 2, (ch - covh) / 2, covw, covh },
        s->mode == PRESENT_INTEGER ? PRESENT_NEAREST : s->mode, s->look, s->bars, 0 };
    ID3D11DeviceContext_UpdateSubresource(d.ctx, (ID3D11Resource *)d.cb, 0, NULL, &c, 0, 0);

    D3D11_VIEWPORT vp = { 0, 0, (float)cw, (float)ch, 0, 1 };
    ID3D11ShaderResourceView *srv[2] = { d.frame.srv, d.back.srv };
    ID3D11DeviceContext_RSSetViewports(d.ctx, 1, &vp);
    ID3D11DeviceContext_OMSetRenderTargets(d.ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(d.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(d.ctx, d.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(d.ctx, d.ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(d.ctx, 0, 2, srv);
    ID3D11DeviceContext_PSSetSamplers(d.ctx, 0, 2, d.samp);
    ID3D11DeviceContext_PSSetConstantBuffers(d.ctx, 0, 1, &d.cb);
    ID3D11DeviceContext_Draw(d.ctx, 3, 0);
}

int present_window(HWND hw, const uint32_t *f, int w, int h, double aspect, const Present *s)
{
    if (!init()) return 0;
    RECT cr; GetClientRect(hw, &cr);
    int cw = cr.right, ch = cr.bottom;
    if (cw <= 0 || ch <= 0) return 1;
    if (!d.sc) {
        IDXGIDevice *xd = NULL; IDXGIAdapter *ad = NULL; IDXGIFactory *fa = NULL;
        DXGI_SWAP_CHAIN_DESC sd = {0};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hw;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        HRESULT hr = ID3D11Device_QueryInterface(d.dev, &IID_IDXGIDevice, (void **)&xd);
        if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(xd, &ad);
        if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&fa);
        if (SUCCEEDED(hr)) hr = IDXGIFactory_CreateSwapChain(fa, (IUnknown *)d.dev, &sd, &d.sc);
        if (SUCCEEDED(hr))   /* Alt+Enter is ours (borderless), not DXGI's exclusive mode */
            IDXGIFactory_MakeWindowAssociation(fa, hw, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        if (fa) IDXGIFactory_Release(fa);
        if (ad) IDXGIAdapter_Release(ad);
        if (xd) IDXGIDevice_Release(xd);
        if (FAILED(hr)) return fail("swap chain", hr);
        d.hwnd = hw;
        fprintf(stderr, "[present] Direct3D 11\n");
    }
    if (!d.rtv || cw != d.cw || ch != d.ch) {
        if (d.rtv) { ID3D11RenderTargetView_Release(d.rtv); d.rtv = NULL; }
        ID3D11DeviceContext_OMSetRenderTargets(d.ctx, 0, NULL, NULL);
        if (FAILED(IDXGISwapChain_ResizeBuffers(d.sc, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0))) return 0;
        ID3D11Texture2D *back = NULL;
        IDXGISwapChain_GetBuffer(d.sc, 0, &IID_ID3D11Texture2D, (void **)&back);
        ID3D11Device_CreateRenderTargetView(d.dev, (ID3D11Resource *)back, NULL, &d.rtv);
        ID3D11Texture2D_Release(back);
        d.cw = cw; d.ch = ch;
    }
    draw(d.rtv, cw, ch, f, w, h, aspect, s);
    IDXGISwapChain_Present(d.sc, 0, 0);
    return 1;
}

/* The same picture at ow x oh into out (0xAARRGGBB), with no window: --present
 * makes --shot-at frames this way, so the presenter is tested headless. */
int present_offscreen(const uint32_t *f, int w, int h, double aspect, const Present *s,
                      int ow, int oh, uint32_t *out)
{
    if (!init()) return 0;
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = ow; td.Height = oh; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *rt = NULL, *st = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(d.dev, &td, NULL, &rt))) return 0;
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(d.dev, &td, NULL, &st);
    ID3D11Device_CreateRenderTargetView(d.dev, (ID3D11Resource *)rt, NULL, &rtv);
    draw(rtv, ow, oh, f, w, h, aspect, s);
    ID3D11DeviceContext_CopyResource(d.ctx, (ID3D11Resource *)st, (ID3D11Resource *)rt);
    D3D11_MAPPED_SUBRESOURCE m;
    int ok = SUCCEEDED(ID3D11DeviceContext_Map(d.ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
        for (int y = 0; y < oh; y++) memcpy(out + (size_t)y * ow, (uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)ow * 4);
        ID3D11DeviceContext_Unmap(d.ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11RenderTargetView_Release(rtv); ID3D11Texture2D_Release(st); ID3D11Texture2D_Release(rt);
    return ok;
}
