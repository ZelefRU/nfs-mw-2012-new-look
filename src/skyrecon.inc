// ---------------------------------------------------------------- sky research (Shift+T)
// The sky is drawn first in the lighting run, by a pixel shader with 4 cloud textures (BaseCloudSampler..4) and a
// cloud colour (BaseCloudColour). To learn how it works and what it is fed:
// - every sky-like pixel shader (cloud / sky / moon / star in its names) is disassembled once, when the game makes
//   it, into scripts\NFSLight_sky.txt (its constant buffers with their layout, its textures, its code);
// - on Shift+T, the main-view draws of the frame are listed by pixel shader (sky: lines), and the first draw of each
//   sky shader logs every constant it reads (name = values, from the buffers bound) and its textures' sizes.

struct SkyPs { ID3D11PixelShader* ps; void* code; SIZE_T size; char name[96]; bool dumped; };
static SkyPs g_skyPs[16];
static volatile LONG g_skyPsN;
static int g_skyLines;
static ID3D11PixelShader* g_skyLastLogged;

static bool SkyLike(const char* n)
{
    if (n[0] == 't' && (n[1] == 'D' || n[1] == 'N' || n[1] == 'H')) return false;   // ours (tDest, tDepth, ...: tCloud is the sun's)
    char l[96];
    int i = 0;
    for (; n[i] && i < 95; ++i) l[i] = (char)tolower((unsigned char)n[i]);
    l[i] = 0;
    return strstr(l, "cloud") || strstr(l, "sky") || strstr(l, "moon") || strstr(l, "star") || strstr(l, "atmos");
}

static void SkyNote(const void* code, SIZE_T size, ID3D11PixelShader* ps, const char* name)
{
    if (!SkyLike(name)) return;
    LONG k = InterlockedIncrement(&g_skyPsN) - 1;
    if (k >= 16) { g_skyPsN = 16; return; }
    SkyPs& s = g_skyPs[k];
    s.code = malloc(size);
    if (s.code) memcpy(s.code, code, size);
    s.size = size; s.ps = ps; s.dumped = false;
    strncpy(s.name, name, 95);
    typedef HRESULT(WINAPI* DisFn)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);
    HMODULE m = GetModuleHandleA("d3dcompiler_47.dll");
    DisFn dis = m ? (DisFn)GetProcAddress(m, "D3DDisassemble") : 0;
    ID3DBlob* b = 0;
    if (!dis || FAILED(dis(code, size, 0, 0, &b)) || !b) return;
    char p[MAX_PATH];
    sprintf(p, "%sNFSLight_sky.txt", g_dir);
    FILE* f = fopen(p, k ? "a" : "w");
    if (f) { fprintf(f, "==== sky shader %ld  ps %p  [%s]\n", k, ps, name); fwrite(b->GetBufferPointer(), 1, b->GetBufferSize() - 1, f); fprintf(f, "\n"); fclose(f); }
    b->Release();
    Log("sky shader %ld: ps %p [%s] (code in NFSLight_sky.txt)", k, ps, name);
}

// the game's own wetness: pixel shaders with a constant named wet / rain / puddle / driveable are disassembled once
// into scripts\NFSLight_wet.txt (to find what makes its roads wet everywhere)
static void WetNote(const void* code, SIZE_T size, ID3D11PixelShader* ps, const char* name)
{
    static int n;
    if (n >= 40 || !g_reflect) return;
    ID3D11ShaderReflection* r = 0;
    if (FAILED(g_reflect(code, size, __uuidof(ID3D11ShaderReflection), (void**)&r)) || !r) return;
    D3D11_SHADER_DESC sd;
    r->GetDesc(&sd);
    char hit[200] = "";
    for (UINT c = 0; c < sd.ConstantBuffers; ++c) {
        ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByIndex(c);
        D3D11_SHADER_BUFFER_DESC cbd;
        if (!cb || FAILED(cb->GetDesc(&cbd))) continue;
        for (UINT v = 0; v < cbd.Variables; ++v) {
            D3D11_SHADER_VARIABLE_DESC vd;
            if (FAILED(cb->GetVariableByIndex(v)->GetDesc(&vd))) continue;
            char l[96];
            int i = 0;
            for (; vd.Name[i] && i < 95; ++i) l[i] = (char)tolower((unsigned char)vd.Name[i]);
            l[i] = 0;
            if ((strstr(l, "wet") || strstr(l, "rain") || strstr(l, "puddle") || strstr(l, "driveable") || strstr(l, "damp")) && strlen(hit) + strlen(vd.Name) < 190) {
                if (hit[0]) strcat(hit, ",");
                strcat(hit, vd.Name);
            }
        }
    }
    r->Release();
    if (!hit[0]) return;
    typedef HRESULT(WINAPI* DisFn)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);
    HMODULE m = GetModuleHandleA("d3dcompiler_47.dll");
    DisFn dis = m ? (DisFn)GetProcAddress(m, "D3DDisassemble") : 0;
    ID3DBlob* b = 0;
    if (!dis || FAILED(dis(code, size, 0, 0, &b)) || !b) return;
    char p[MAX_PATH];
    sprintf(p, "%sNFSLight_wet.txt", g_dir);
    FILE* f = fopen(p, n ? "a" : "w");
    if (f) { fprintf(f, "==== wet shader %d  ps %p  [%s]  constants: %s\n", n, ps, name, hit); fwrite(b->GetBufferPointer(), 1, b->GetBufferSize() - 1, f); fprintf(f, "\n"); fclose(f); }
    b->Release();
    Log("wet shader %d: ps %p [%s] constants %s", n, ps, name, hit);
    ++n;
}

static SkyPs* SkyFind(ID3D11PixelShader* ps)
{
    LONG n = g_skyPsN;
    for (LONG i = 0; i < n; ++i) if (g_skyPs[i].ps == ps) return &g_skyPs[i];
    return 0;
}

// every constant the sky shader reads, from the buffers bound now; its textures
static void SkyDump(ID3D11DeviceContext* ctx, SkyPs& s)
{
    ID3D11ShaderReflection* r = 0;
    if (!s.code || !g_reflect || FAILED(g_reflect(s.code, s.size, __uuidof(ID3D11ShaderReflection), (void**)&r)) || !r) return;
    D3D11_SHADER_DESC sd;
    r->GetDesc(&sd);
    for (UINT c = 0; c < sd.ConstantBuffers; ++c) {
        ID3D11ShaderReflectionConstantBuffer* rc = r->GetConstantBufferByIndex(c);
        D3D11_SHADER_BUFFER_DESC cbd;
        D3D11_SHADER_INPUT_BIND_DESC bd;
        if (!rc || FAILED(rc->GetDesc(&cbd)) || FAILED(r->GetResourceBindingDescByName(cbd.Name, &bd))) continue;
        ID3D11Buffer* cb = 0;
        ctx->PSGetConstantBuffers(bd.BindPoint, 1, &cb);
        if (!cb) { Log("sky:   cbuffer %s (b%u): nothing bound", cbd.Name, bd.BindPoint); continue; }
        D3D11_BUFFER_DESC bufd;
        cb->GetDesc(&bufd);
        D3D11_BUFFER_DESC std = {};
        std.ByteWidth = bufd.ByteWidth; std.Usage = D3D11_USAGE_STAGING; std.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Buffer* st = 0;
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_dev->CreateBuffer(&std, 0, &st))) {
            ctx->CopyResource(st, cb);
            if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                Log("sky:   cbuffer %s (b%u, %u bytes bound)", cbd.Name, bd.BindPoint, bufd.ByteWidth);
                for (UINT v = 0; v < cbd.Variables; ++v) {
                    D3D11_SHADER_VARIABLE_DESC vd;
                    if (FAILED(rc->GetVariableByIndex(v)->GetDesc(&vd))) continue;
                    if (!(vd.uFlags & D3D_SVF_USED)) continue;
                    char line[400];
                    int len = sprintf(line, "sky:     %-28s +%-4u =", vd.Name, vd.StartOffset);
                    UINT nf = vd.Size / 4;
                    if (nf > 24) nf = 24;
                    for (UINT q = 0; q < nf && vd.StartOffset + q * 4 + 4 <= bufd.ByteWidth && len < 380; ++q)
                        len += sprintf(line + len, " %.4g", ((const float*)((const char*)m.pData + vd.StartOffset))[q]);
                    Log("%s", line);
                }
                ctx->Unmap(st, 0);
            }
            st->Release();
        }
        cb->Release();
    }
    for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC bd;
        if (FAILED(r->GetResourceBindingDesc(i, &bd)) || bd.Type != D3D_SIT_TEXTURE) continue;
        ID3D11ShaderResourceView* srv = 0;
        ctx->PSGetShaderResources(bd.BindPoint, 1, &srv);
        if (!srv) { Log("sky:   texture %s (t%u): nothing bound", bd.Name, bd.BindPoint); continue; }
        ID3D11Resource* res = 0;
        srv->GetResource(&res);
        D3D11_RESOURCE_DIMENSION dim;
        res->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC td;
            ((ID3D11Texture2D*)res)->GetDesc(&td);
            Log("sky:   texture %s (t%u): %ux%u fmt %d mips %u array %u%s", bd.Name, bd.BindPoint, td.Width, td.Height, td.Format, td.MipLevels, td.ArraySize,
                (td.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) ? " cube" : "");
        } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
            D3D11_TEXTURE3D_DESC td;
            ((ID3D11Texture3D*)res)->GetDesc(&td);
            Log("sky:   texture %s (t%u): 3D %ux%ux%u fmt %d", bd.Name, bd.BindPoint, td.Width, td.Height, td.Depth, td.Format);
        } else Log("sky:   texture %s (t%u): dimension %d", bd.Name, bd.BindPoint, dim);
        res->Release();
        srv->Release();
    }
    r->Release();
}

// called for every draw of the Shift+T frame
static void SkyReconDraw(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    ID3D11RenderTargetView* rtv = 0;
    ctx->OMGetRenderTargets(1, &rtv, 0);
    ID3D11Resource* rp = 0; UINT w = 0, h = 0; int fmt = 0;
    TexInfo(rtv, &rp, &w, &h, &fmt);
    if (rtv) rtv->Release();
    SkyPs* s = SkyFind(g_lastPS);
    bool mainView = w * 2 >= g_bbW;   // the full or the half-size view (not shadow maps, not small buffers)
    if (mainView && g_skyLines < 400 && (g_lastPS != g_skyLastLogged || s)) {
        ++g_skyLines;
        g_skyLastLogged = g_lastPS;
        ID3D11BlendState* bs = 0; float bf[4]; UINT sm;
        ctx->OMGetBlendState(&bs, bf, &sm);
        D3D11_BLEND_DESC bdsc = {};
        if (bs) { bs->GetDesc(&bdsc); bs->Release(); }
        Log("sky: draw k%d n%u rt %ux%u f%d blend %d(%d,%d) ps %p %s[%s]", (int)d.kind, d.a, w, h, fmt, bdsc.RenderTarget[0].BlendEnable,
            bdsc.RenderTarget[0].SrcBlend, bdsc.RenderTarget[0].DestBlend, g_lastPS, s ? "SKY " : "", PsNameOf(g_lastPS));
    }
    if (s && !s->dumped) { s->dumped = true; SkyDump(ctx, *s); }
}
static void SkyReconStart() { g_skyLines = 0; g_skyLastLogged = 0; for (LONG i = 0; i < g_skyPsN; ++i) g_skyPs[i].dumped = false; }
