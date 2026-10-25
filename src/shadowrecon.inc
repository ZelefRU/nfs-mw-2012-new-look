// ---------------------------------------------------------------- shadow pass research (Shift+T)
// Goal: lamp shadow maps drawn with the game's own shadow casters. Needed first: how the game draws its sun shadow
// map (which draws, which depth target, viewports per cascade) and where every vertex shader takes its view matrix
// (ViewProjectionModified: its constant buffer slot and offset differ per shader). Every vertex shader is reflected
// when the game creates it; Shift+T logs one frame of draws grouped into runs (same targets and viewport), with the
// view matrix of the first draw of each run read back. Log lines start with "recon:".

struct VsInfo { ID3D11VertexShader* vs; short vpSlot, vpOff, wSlot, wOff; unsigned char car, named, carBits; char what[40];
                unsigned char nOut, posReg, gsState; int sigAt; ID3D11GeometryShader* gs;    // outputs (sunshadow.inc: a GS after it)
                unsigned char wClass, wRows, wCols; short wPrevOff;     // "world": its type; a previous frame's world beside it (worldedit.inc)
                unsigned short cbMask, srvMask; };                       // the constant buffers / textures it reads (1.27: replays recorded)
// every vertex shader's outputs: semantic, index, register, mask, component type, system value
struct VsSigEl { char sem[28]; unsigned char idx, reg, mask, type, sv; };
static VsSigEl g_vsSig[16384];
static volatile LONG g_vsSigN;
static VsInfo g_vsInfo[8192];             // open addressing by pointer
static volatile LONG g_vsCount, g_vsWithVP;
typedef HRESULT(STDMETHODCALLTYPE* CreateVSFn)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
static CreateVSFn oCreateVS;
static CRITICAL_SECTION g_vsLock;
static volatile LONG g_vsLockInit;
extern ID3D11Resource* g_sunShadowTex;

static VsInfo* VsFind(ID3D11VertexShader* vs, bool add)
{
    if (!vs) return 0;
    unsigned h = ((unsigned)(size_t)vs >> 4) * 2654435761u;
    for (int k = 0; k < 8192; ++k) {
        VsInfo& e = g_vsInfo[(h + k) & 8191];
        if (e.vs == vs) return &e;
        if (!e.vs) {
            if (!add) return 0;
            memset(&e, 0, sizeof(e));
            e.vpSlot = e.wSlot = e.wPrevOff = -1;
            e.vs = vs;
            return &e;
        }
    }
    return 0;
}

static void VsExamine(const void* code, SIZE_T size, ID3D11VertexShader* vs)
{
    if (!g_reflect) return;
    ID3D11ShaderReflection* r = 0;
    if (FAILED(g_reflect(code, size, __uuidof(ID3D11ShaderReflection), (void**)&r)) || !r) return;
    if (!InterlockedExchange(&g_vsLockInit, 1)) InitializeCriticalSection(&g_vsLock);
    EnterCriticalSection(&g_vsLock);
    VsInfo* e = VsFind(vs, true);
    if (e) {
        D3D11_SHADER_DESC sd;
        r->GetDesc(&sd);
        for (UINT b = 0; b < sd.BoundResources; ++b) {   // which constant buffers and textures it reads (sunshadow.inc keeps those)
            D3D11_SHADER_INPUT_BIND_DESC rb;
            if (FAILED(r->GetResourceBindingDesc(b, &rb)) || rb.BindPoint >= 16) continue;
            if (rb.Type == D3D_SIT_CBUFFER) e->cbMask |= (unsigned short)(1u << rb.BindPoint);
            else if (rb.Type == D3D_SIT_TEXTURE || rb.Type == D3D_SIT_TBUFFER) e->srvMask |= (unsigned short)(1u << rb.BindPoint);
        }
        for (UINT c = 0; c < sd.ConstantBuffers; ++c) {
            ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByIndex(c);
            D3D11_SHADER_BUFFER_DESC bd;
            if (!cb || FAILED(cb->GetDesc(&bd))) continue;
            D3D11_SHADER_INPUT_BIND_DESC bind;
            if (FAILED(r->GetResourceBindingDescByName(bd.Name, &bind))) continue;
            char names[400] = "";   // this buffer's variables (logged for the world editor: what else is beside "world")
            short prevOff = -1;
            bool hasWorld = false;
            for (UINT v = 0; v < bd.Variables; ++v) {
                ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
                D3D11_SHADER_VARIABLE_DESC vd;
                if (!var || FAILED(var->GetDesc(&vd))) continue;
                if (strlen(names) + strlen(vd.Name) < sizeof(names) - 16) sprintf(names + strlen(names), " %s@%u/%u", vd.Name, vd.StartOffset, vd.Size);
                if (!strcmp(vd.Name, "ViewProjectionModified")) { e->vpSlot = (short)bind.BindPoint; e->vpOff = (short)vd.StartOffset; }
                else if (!strcmp(vd.Name, "world")) {
                    e->wSlot = (short)bind.BindPoint; e->wOff = (short)vd.StartOffset;
                    hasWorld = true;
                    ID3D11ShaderReflectionType* ty = var->GetType();
                    D3D11_SHADER_TYPE_DESC td;
                    if (ty && SUCCEEDED(ty->GetDesc(&td))) { e->wClass = (unsigned char)td.Class; e->wRows = (unsigned char)td.Rows; e->wCols = (unsigned char)td.Columns; }
                } else if ((strstr(vd.Name, "orld") || strstr(vd.Name, "ORLD")) && vd.Size >= 48 &&
                           (strstr(vd.Name, "rev") || strstr(vd.Name, "Prev") || strstr(vd.Name, "ast") || strstr(vd.Name, "Old") || strstr(vd.Name, "old")))
                    prevOff = (short)vd.StartOffset;
                if (strstr(vd.Name, "wheel") || strstr(vd.Name, "damage") || strstr(vd.Name, "Damage") || strstr(vd.Name, "Bone")) e->car = 1;
                if (strstr(vd.Name, "wheel")) e->carBits |= 1;
                if (strstr(vd.Name, "damage") || strstr(vd.Name, "Damage")) e->carBits |= 2;
                if (strstr(vd.Name, "Bone")) e->carBits |= 4;
                if (strlen(e->what) + strlen(vd.Name) < 38 && (strstr(vd.Name, "wheel") || strstr(vd.Name, "amage") || strstr(vd.Name, "Bone"))) {
                    if (e->what[0]) strcat(e->what, ",");
                    strcat(e->what, vd.Name);
                }
            }
            if (hasWorld) {
                e->wPrevOff = prevOff;
                static unsigned seen[24];
                static int nSeen;
                unsigned h = 2166136261u;
                for (const char* q = names; *q; ++q) h = (h ^ (unsigned char)*q) * 16777619u;
                bool known = false;
                for (int k = 0; k < nSeen; ++k) known |= seen[k] == h;
                if (!known && nSeen < 24) {
                    seen[nSeen++] = h;
                    Log("world editor: object constants %s (slot %u, %u bytes), world class %d %dx%d:%s", bd.Name, bind.BindPoint, bd.Size,
                        e->wClass, e->wRows, e->wCols, names);
                }
            }
        }
        e->posReg = 255;
        if (sd.OutputParameters && sd.OutputParameters <= 16) {
            LONG at = InterlockedExchangeAdd(&g_vsSigN, (LONG)sd.OutputParameters);
            if (at + (LONG)sd.OutputParameters <= 16384) {
                for (UINT o = 0; o < sd.OutputParameters; ++o) {
                    D3D11_SIGNATURE_PARAMETER_DESC pd;
                    VsSigEl& s = g_vsSig[at + o];
                    memset(&s, 0, sizeof(s));
                    if (FAILED(r->GetOutputParameterDesc(o, &pd))) continue;
                    strncpy(s.sem, pd.SemanticName ? pd.SemanticName : "", sizeof(s.sem) - 1);
                    s.idx = (unsigned char)pd.SemanticIndex; s.reg = (unsigned char)pd.Register; s.mask = pd.Mask;
                    s.type = (unsigned char)pd.ComponentType; s.sv = (unsigned char)pd.SystemValueType;
                    if (pd.SystemValueType == D3D_NAME_POSITION) e->posReg = (unsigned char)pd.Register;
                }
                e->sigAt = at; e->nOut = (unsigned char)sd.OutputParameters;
            }
        }
        e->named = 1;
        InterlockedIncrement(&g_vsCount);
        if (e->vpSlot >= 0) InterlockedIncrement(&g_vsWithVP);
    }
    LeaveCriticalSection(&g_vsLock);
    r->Release();
}

static void CarVsDump(const void* code, SIZE_T size);   // carlights.inc
// every vertex shader's code (to disassemble one later: VsDumpCode)
struct VsCode { ID3D11VertexShader* vs; void* code; SIZE_T size; };
static VsCode g_vsCode[2048];
static volatile LONG g_vsCodeN;
// a VS's DXBC checksum (its kept code; 0 if not kept)
static const unsigned char* VsSumOf(void* vs)
{
    LONG n = g_vsCodeN;
    for (LONG i = 0; i < n && i < 2048; ++i)
        if (g_vsCode[i].vs == vs && g_vsCode[i].code && g_vsCode[i].size > 20) return (const unsigned char*)g_vsCode[i].code + 4;
    return 0;
}
static void VsDumpCode(ID3D11VertexShader* vs, const char* title)
{
    LONG n = g_vsCodeN;
    for (LONG i = 0; i < n; ++i) {
        if (g_vsCode[i].vs != vs || !g_vsCode[i].code) continue;
        typedef HRESULT(WINAPI* DisFn)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);
        HMODULE m = GetModuleHandleA("d3dcompiler_47.dll");
        DisFn dis = m ? (DisFn)GetProcAddress(m, "D3DDisassemble") : 0;
        ID3DBlob* b = 0;
        if (!dis || FAILED(dis(g_vsCode[i].code, g_vsCode[i].size, 0, 0, &b)) || !b) return;
        char p[MAX_PATH];
        sprintf(p, "%sNewLook_sky.txt", g_dir);
        FILE* f = fopen(p, "a");
        if (f) { fprintf(f, "==== %s  vs %p\n", title, vs); fwrite(b->GetBufferPointer(), 1, b->GetBufferSize() - 1, f); fprintf(f, "\n"); fclose(f); }
        b->Release();
        Log("sky: %s %p disassembled into NewLook_sky.txt", title, vs);
        return;
    }
    Log("sky: %s %p: code not kept", title, vs);
}
static HRESULT STDMETHODCALLTYPE HkCreateVS(ID3D11Device* dev, const void* code, SIZE_T size, ID3D11ClassLinkage* cl, ID3D11VertexShader** out)
{
    HtScope ht(HT_SH, HT_SHDRV, true);   // (1.81d: hitch.inc)
    HRESULT hr;
    HT_CALL(ht, hr = oCreateVS(dev, code, size, cl, out));
    if (SUCCEEDED(hr) && out && *out && code) {
        VsExamine(code, size, *out);
        CoronaNote(code, size, *out);   // the game's sun corona (sunshadow.inc)
        FlareNote(code, size, *out, true);   // its flare
        LONG k = InterlockedIncrement(&g_vsCodeN) - 1;
        if (k < 2048) { g_vsCode[k].code = malloc(size); if (g_vsCode[k].code) memcpy(g_vsCode[k].code, code, size); g_vsCode[k].size = size; g_vsCode[k].vs = *out; }
        else g_vsCodeN = 2048;
        VsInfo* e = VsFind(*out, false);
        if (e && (e->carBits & 2) && !(e->carBits & 5)) CarVsDump(code, size);   // a traffic car's shader (carlights.inc)
    }
    return hr;
}

// ---- one frame of runs
static volatile LONG g_recon;             // 1 = next frame, 2 = this frame
struct ReconRun {
    ID3D11Resource *dsv, *rtv; D3D11_VIEWPORT vp; UINT dsvW, dsvH, rtW, rtH; int dsvFmt, rtFmt;
    int draws, withVP, cars, unknownVs, noPs; bool sun;
    ID3D11PixelShader* ps0; int lights1, lights2, sunPass;
};
static int g_lightDumps, g_carDumps;
static ReconRun g_run;
static int g_runLines, g_runReads;

static void TexInfo(ID3D11View* v, ID3D11Resource** res, UINT* w, UINT* h, int* fmt)
{
    *res = 0; *w = *h = 0; *fmt = 0;
    if (!v) return;
    v->GetResource(res);
    if (!*res) return;
    ID3D11Texture2D* t = 0;
    (*res)->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t);
    if (t) { D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); *w = d.Width; *h = d.Height; *fmt = d.Format; t->Release(); }
    (*res)->Release();   // kept as a plain pointer (identity only)
}

static void ReconFlush()
{
    ReconRun& r = g_run;
    if (!r.draws) return;
    if (g_runLines++ < 600)
        Log("recon: run %3d draws  dsv %p %ux%u f%d  rt %p %ux%u f%d  vp %.0f,%.0f %.0fx%.0f  vs: view matrix known %d, car %d, unknown %d  no ps %d%s",
            r.draws, r.dsv, r.dsvW, r.dsvH, r.dsvFmt, r.rtv, r.rtW, r.rtH, r.rtFmt, r.vp.TopLeftX, r.vp.TopLeftY, r.vp.Width, r.vp.Height,
            r.withVP, r.cars, r.unknownVs, r.noPs, r.sun ? "  << SUN SHADOW MAP" : "");
    if (g_runLines < 600 && (r.lights1 || r.lights2 || r.sunPass))
        Log("recon:   ^ game lights: %d batch draws (4 lights each), %d projected spot draws%s; first ps %s", r.lights1, r.lights2,
            r.sunPass ? ", THE SUN PASS (ours runs here)" : "", PsNameOf(r.ps0));
    memset(&r, 0, sizeof(r));
}

// the 16 floats at (slot, offset) of the vertex constants now bound (waits: research only)
static bool ReadVsConst(ID3D11DeviceContext* ctx, int slot, int off, float* out)
{
    ID3D11Buffer* cb = 0;
    ctx->VSGetConstantBuffers(slot, 1, &cb);
    if (!cb) return false;
    D3D11_BUFFER_DESC bd;
    cb->GetDesc(&bd);
    bool ok = false;
    if ((UINT)off + 64 <= bd.ByteWidth) {
        D3D11_BUFFER_DESC sd = {};
        sd.ByteWidth = bd.ByteWidth; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Buffer* st = 0;
        if (SUCCEEDED(g_dev->CreateBuffer(&sd, 0, &st))) {
            ctx->CopyResource(st, cb);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) { memcpy(out, (char*)m.pData + off, 64); ctx->Unmap(st, 0); ok = true; }
            st->Release();
        }
    }
    cb->Release();
    return ok;
}

static void SkyReconDraw(ID3D11DeviceContext* ctx, const DrawCall& d);
static void SkyReconStart();
static void ReconDraw(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    SkyReconDraw(ctx, d);
    ID3D11RenderTargetView* rtv = 0; ID3D11DepthStencilView* dsv = 0;
    ctx->OMGetRenderTargets(1, &rtv, &dsv);
    ReconRun n;
    memset(&n, 0, sizeof(n));
    TexInfo(dsv, &n.dsv, &n.dsvW, &n.dsvH, &n.dsvFmt);
    TexInfo(rtv, &n.rtv, &n.rtW, &n.rtH, &n.rtFmt);
    if (rtv) rtv->Release();
    if (dsv) dsv->Release();
    UINT nv = 1;
    ctx->RSGetViewports(&nv, &n.vp);
    ReconRun& r = g_run;
    bool same = r.draws && r.dsv == n.dsv && r.rtv == n.rtv && !memcmp(&r.vp, &n.vp, sizeof(n.vp));
    if (!same) {
        ReconFlush();
        r = n;
        r.sun = n.dsv && n.dsv == g_sunShadowTex;
    }
    ++r.draws;
    ID3D11VertexShader* vs = 0;
    ctx->VSGetShader(&vs, 0, 0);
    VsInfo* e = VsFind(vs, false);
    if (!e) ++r.unknownVs;
    else { if (e->vpSlot >= 0) ++r.withVP; if (e->car) ++r.cars; }
    ID3D11PixelShader* ps = 0;
    ctx->PSGetShader(&ps, 0, 0);
    if (!ps) ++r.noPs; else ps->Release();
    if (r.draws == 1) r.ps0 = ps;
    int lk = LightKindOf(ps);
    if (lk == 1) ++r.lights1;
    if (lk == 2) ++r.lights2;
    if (FindSun(ps)) ++r.sunPass;
    // the light constants of the first light draws (cb1: colours, directions, positions [x4]; or the spot's)
    if (lk && g_lightDumps < 24) {
        ++g_lightDumps;
        ID3D11Buffer* cb = 0;
        ctx->PSGetConstantBuffers(1, 1, &cb);
        if (cb) {
            D3D11_BUFFER_DESC bd; cb->GetDesc(&bd);
            D3D11_BUFFER_DESC sd = {}; sd.ByteWidth = bd.ByteWidth; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ID3D11Buffer* st = 0;
            if (SUCCEEDED(g_dev->CreateBuffer(&sd, 0, &st))) {
                ctx->CopyResource(st, cb);
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                    const float* f = (const float*)m.pData;
                    int n4 = (int)(bd.ByteWidth / 16); if (n4 > 12) n4 = 12;
                    char line[1600], *q = line;
                    q += sprintf(q, "recon:   light draw (%s, cb1 %u bytes):", lk == 1 ? "batch" : "spot", bd.ByteWidth);
                    for (int r4 = 0; r4 < n4; ++r4) q += sprintf(q, " | %.2f %.2f %.2f %.3f", f[r4 * 4], f[r4 * 4 + 1], f[r4 * 4 + 2], f[r4 * 4 + 3]);
                    Log("%s", line);
                    ctx->Unmap(st, 0);
                }
                st->Release();
            }
            cb->Release();
        }
    }
    // the first draw of a run with a depth target: its view matrix and the object's position
    // the cars in the main view: every car-shader draw, its object matrix (world: rows, translation in row 3) and mesh
    if (e && e->car && n.rtW == g_bbW && g_carDumps < 120 && e->wSlot >= 0) {
        ++g_carDumps;
        float w[16];
        if (ReadVsConst(ctx, e->wSlot, e->wOff, w)) {
            ID3D11Buffer* vb = 0; UINT stride = 0, off = 0;
            ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &off);
            if (vb) vb->Release();
            Log("recon:   car draw k%d n%u start %u base %d stride %u vs %p [%s] at %.2f %.2f %.2f | x %.3f %.3f %.3f | y %.3f %.3f %.3f | z %.3f %.3f %.3f",
                (int)d.kind, d.a, d.b, d.base, stride, vs, e->what, w[12], w[13], w[14], w[0], w[1], w[2], w[4], w[5], w[6], w[8], w[9], w[10]);
        }
    }
    if (r.draws == 1 && n.dsv && e && e->vpSlot >= 0 && g_runReads < 80) {
        ++g_runReads;
        float m[16], w[16];
        if (ReadVsConst(ctx, e->vpSlot, e->vpOff, m))
            Log("recon:   view matrix (slot %d +%d): %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
                e->vpSlot, e->vpOff, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        if (e->wSlot >= 0 && ReadVsConst(ctx, e->wSlot, e->wOff, w))
            Log("recon:   first object (slot %d +%d) at %.2f %.2f %.2f (row 3: %.2f %.2f %.2f | column 3: %.2f %.2f %.2f)", e->wSlot, e->wOff,
                w[12], w[13], w[14], w[12], w[13], w[14], w[3], w[7], w[11]);
    }
    if (vs) vs->Release();
}

static void ReconFrameEnd()
{
    if (g_recon == 2) {
        ReconFlush();
        g_recon = 0;
        Log("recon: end (vertex shaders seen %ld, with a view matrix %ld; camera %.1f %.1f %.1f)", g_vsCount, g_vsWithVP,
            g_cam[3][0], g_cam[3][1], g_cam[3][2]);
    } else if (g_recon == 1) {
        g_recon = 2;
        g_runLines = g_runReads = g_lightDumps = g_carDumps = 0;
        SkyReconStart();
        memset(&g_run, 0, sizeof(g_run));
        Log("recon: start, sun shadow map %p, camera %.1f %.1f %.1f", g_sunShadowTex, g_cam[3][0], g_cam[3][1], g_cam[3][2]);
    }
}
