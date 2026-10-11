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
 *
 * On Linux the same modes are drawn with SDL2's renderer (the GPU when there
 * is one, else SDL's software renderer, which also makes the --present
 * shots): sharp is a nearest prescale to whole multiples, then bilinear the
 * rest of the way; CRT lays its scanlines and mask over that as a multiply;
 * vivid and the blur are the CPU's, as here.
 */
#define COBJMACROS
#include "machine.h"
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#else
#include <SDL.h>
#endif

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

#ifdef _WIN32

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

#else

/* ---- SDL2: Linux ----------------------------------------------------------------- */

typedef struct {
    SDL_Renderer *r;
    SDL_Texture *frame, *back, *mid, *crt;
    int fw, fh, bw, bh, mw, mh;
    int cx, cw, ch, csrc;                /* what crt was made for */
} Sdl;

static Sdl win_sdl;

/* A texture of this size, made again only when the size changes */
static SDL_Texture *sized(Sdl *p, SDL_Texture **t, int *tw, int *th, int w, int h, int access)
{
    if (!*t || *tw != w || *th != h) {
        if (*t) SDL_DestroyTexture(*t);
        *t = SDL_CreateTexture(p->r, SDL_PIXELFORMAT_ARGB8888, access, w, h);
        SDL_SetTextureBlendMode(*t, SDL_BLENDMODE_NONE);
        *tw = w; *th = h;
    }
    return *t;
}

/* The shader's vivid look, on the frame before it is scaled */
static uint32_t vivid_buf[640 * 480];

static const uint32_t *vivid(const uint32_t *f, int n)
{
    for (int i = 0; i < n; i++) {
        float c[3] = { (f[i] >> 16 & 255) / 255.f, (f[i] >> 8 & 255) / 255.f, (f[i] & 255) / 255.f };
        float l = c[0] * 0.299f + c[1] * 0.587f + c[2] * 0.114f;
        uint32_t o = 0xFF000000u;
        for (int k = 0; k < 3; k++) {
            float v = ((l + (c[k] - l) * 1.3f) - 0.5f) * 1.08f + 0.5f;
            o |= (uint32_t)(v < 0 ? 0 : v > 1 ? 255 : v * 255 + 0.5f) << (16 - 8 * k);
        }
        vivid_buf[i] = o;
    }
    return vivid_buf;
}

/* The shader's beam and aperture grille, as a picture to multiply by. Its
 * factors go above 1, so it is blended as 2*src*dst and stored halved; a
 * renderer without that blend (the software one) gets a plain multiply,
 * which loses the boost and comes out darker. */
static void crt_overlay(Sdl *p, const SDL_Rect *d, int src_h)
{
    if (!p->crt || p->cw != d->w || p->ch != d->h || p->cx != d->x || p->csrc != src_h) {
        static const float mask[3][3] = { { 1.25f, .87f, .87f }, { .87f, 1.25f, .87f }, { .87f, .87f, 1.25f } };
        if (p->crt) SDL_DestroyTexture(p->crt);
        p->crt = SDL_CreateTexture(p->r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, d->w, d->h);
        SDL_BlendMode twice = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_DST_COLOR, SDL_BLENDFACTOR_SRC_COLOR,
                                                         SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ZERO,
                                                         SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
        float scale = SDL_SetTextureBlendMode(p->crt, twice) == 0 ? 0.5f : 1.0f;
        if (scale == 1.0f) SDL_SetTextureBlendMode(p->crt, SDL_BLENDMODE_MOD);
        float k = ((float)d->h / src_h - 1.5f) / 1.5f;
        k = k < 0 ? 0 : k > 1 ? 1 : k;
        uint32_t *px = malloc((size_t)d->w * d->h * 4);
        for (int y = 0; px && y < d->h; y++) {
            float t = (y + 0.5f) / d->h * src_h, dd = t - floorf(t) - 0.5f;
            float beam = 1 + (expf(-dd * dd * 8) * 1.6f - 1) * k;
            for (int x = 0; x < d->w; x++) {
                const float *m = mask[(unsigned)(d->x + x) % 3];
                uint32_t o = 0xFF000000u;
                for (int c = 0; c < 3; c++) {
                    float v = beam * (1 + (m[c] - 1) * k) * scale;
                    o |= (uint32_t)(v > 1 ? 255 : v * 255 + 0.5f) << (16 - 8 * c);
                }
                px[(size_t)y * d->w + x] = o;
            }
        }
        if (px) SDL_UpdateTexture(p->crt, NULL, px, d->w * 4);
        free(px);
        p->cx = d->x; p->cw = d->w; p->ch = d->h; p->csrc = src_h;
    }
    SDL_RenderCopy(p->r, p->crt, NULL, d);
}

static void sdl_draw(Sdl *p, int cw, int ch, const uint32_t *f, int w, int h, double aspect, const Present *s)
{
    SDL_Renderer *r = p->r;
    RECT pr; present_rect(cw, ch, w, h, aspect, s->mode, &pr);
    SDL_Rect d = { (int)pr.left, (int)pr.top, (int)(pr.right - pr.left), (int)(pr.bottom - pr.top) };

    SDL_Texture *t = sized(p, &p->frame, &p->fw, &p->fh, w, h, SDL_TEXTUREACCESS_STREAMING);
    SDL_UpdateTexture(t, NULL, s->look ? vivid(f, w * h) : f, w * 4);
    if (s->mode == PRESENT_SHARP || s->mode == PRESENT_CRT) {
        /* sharp-bilinear: whole multiples with nearest, the rest of the way bilinear */
        int kx = d.w / w > 1 ? d.w / w : 1, ky = d.h / h > 1 ? d.h / h : 1;
        SDL_Texture *m = sized(p, &p->mid, &p->mw, &p->mh, w * kx, h * ky, SDL_TEXTUREACCESS_TARGET);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
        SDL_SetRenderTarget(r, m);
        SDL_RenderCopy(r, t, NULL, NULL);
        t = m;
    }
    SDL_SetRenderTarget(r, NULL);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    if (s->bars) {
        int bw, bh;
        make_backdrop(f, w, h, &bw, &bh);
        SDL_Texture *b = sized(p, &p->back, &p->bw, &p->bh, bw, bh, SDL_TEXTUREACCESS_STREAMING);
        SDL_UpdateTexture(b, NULL, back_buf, bw * 4);
        SDL_SetTextureScaleMode(b, SDL_ScaleModeLinear);
        double covw = cw, covh = cw / aspect;           /* the backdrop covers the window */
        if (covh < ch) { covh = ch; covw = ch * aspect; }
        SDL_Rect c = { (int)((cw - covw) / 2), (int)((ch - covh) / 2), (int)covw, (int)covh };
        SDL_RenderCopy(r, b, NULL, &c);
    }
    SDL_SetTextureScaleMode(t, s->mode == PRESENT_NEAREST || s->mode == PRESENT_INTEGER
                               ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
    SDL_RenderCopy(r, t, NULL, &d);
    if (s->mode == PRESENT_CRT) crt_overlay(p, &d, h);
}

int present_window(SDL_Window *wnd, const uint32_t *f, int w, int h, double aspect, const Present *s)
{
    if (!win_sdl.r) {
        win_sdl.r = SDL_CreateRenderer(wnd, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
        if (!win_sdl.r) win_sdl.r = SDL_CreateRenderer(wnd, -1, SDL_RENDERER_SOFTWARE);
        if (!win_sdl.r) { fprintf(stderr, "[present] no SDL renderer: %s\n", SDL_GetError()); return 0; }
        SDL_RendererInfo i;
        SDL_GetRendererInfo(win_sdl.r, &i);
        fprintf(stderr, "[present] SDL %s\n", i.name);
    }
    int cw, ch;
    SDL_GetRendererOutputSize(win_sdl.r, &cw, &ch);
    if (cw <= 0 || ch <= 0) return 1;
    sdl_draw(&win_sdl, cw, ch, f, w, h, aspect, s);
    SDL_RenderPresent(win_sdl.r);
    return 1;
}

/* --present: SDL's software renderer into memory, no display needed */
int present_offscreen(const uint32_t *f, int w, int h, double aspect, const Present *s,
                      int ow, int oh, uint32_t *out)
{
    SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
    Sdl p = { 0 };
    p.r = sf ? SDL_CreateSoftwareRenderer(sf) : NULL;
    if (!p.r) { SDL_FreeSurface(sf); return 0; }
    sdl_draw(&p, ow, oh, f, w, h, aspect, s);
    SDL_RenderPresent(p.r);
    for (int y = 0; y < oh; y++) {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)sf->pixels + (size_t)y * sf->pitch);
        for (int x = 0; x < ow; x++) out[(size_t)y * ow + x] = row[x] | 0xFF000000u;
    }
    SDL_DestroyRenderer(p.r);                           /* and its textures */
    SDL_FreeSurface(sf);
    return 1;
}

#endif
