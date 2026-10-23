// ---------------------------------------------------------------- texture replacement (1.09)
// Every texture the game creates with its data (CreateTexture2D with initial data) gets a fingerprint: a 64-bit hash of
// its top mip level, its size and format. A file NewLook_textures\replace\<hash>.dds (the first 16 characters of its
// name) is created instead: any size, any number of mip levels, the format of the file (the views the game makes of it
// are given that format: CreateShaderResourceView is hooked too; sRGB follows the game's own choice).
// Finding which file to make: Shift+X picks the material at the screen centre: the next frame every G-buffer draw is
// drawn once more with its number into an ID target (depth test "equal or nearer", nothing written to the game's
// targets), the number under the centre read back; that draw's textures are saved as DDS into NewLook_textures\picked
// (<n>_<slot name>_<hash>.dds), each named after the game's own sampler name (DiffuseTextureSampler, Normal..., ...).
// Edit / replace a picked file, rename it to the hash alone (or keep the name: only its first 16 characters count) and
// put it in replace\; it is used when the game next loads that texture (re-enter the area or restart the game).

typedef HRESULT(STDMETHODCALLTYPE* CreateTex2DFn)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
typedef HRESULT(STDMETHODCALLTYPE* CreateSRVFn)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);
static CreateTex2DFn oCreateTex2D;
static CreateSRVFn oCreateSRV;
static CRITICAL_SECTION g_txLock;
static volatile LONG g_txLockInit;
struct TxInfo { ID3D11Texture2D* t; unsigned long long hash; unsigned short w, h; unsigned char fmt, mips, replaced, origFmt;
                void* mapPtr; UINT mapPitch; ID3D11Texture2D* repl; };   // replaced: 1 at creation, 2 in place, 3 ours bound instead
static TxInfo g_tx[65536];                       // by texture pointer (open addressing; a reused pointer is overwritten)
static unsigned long long g_txRepl[4096];        // hashes that have a file in replace\ (open addressing, 0 = empty)
static int g_txReplN;
static volatile LONG g_txSeen, g_txWithData, g_txReplaced, g_txReplFailed;
static char g_txDir[MAX_PATH];

// ---- formats
static bool TxFmt(DXGI_FORMAT f, int* bpe, bool* bc)
{
    *bc = false;
    switch (f) {
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM: *bc = true; *bpe = 8; return true;
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: *bc = true; *bpe = 16; return true;
    default: break;
    }
    int v = (int)f;
    if (v >= 1 && v <= 4) { *bpe = 16; return true; }
    if (v >= 9 && v <= 18) { *bpe = 8; return true; }
    if ((v >= 23 && v <= 43) || (v >= 87 && v <= 93)) { *bpe = 4; return true; }
    if ((v >= 48 && v <= 59) || v == 85 || v == 86) { *bpe = 2; return true; }
    if (v >= 60 && v <= 65) { *bpe = 1; return true; }
    return false;
}
static void TxMipSize(DXGI_FORMAT f, UINT w, UINT h, UINT* rowBytes, UINT* rows)
{
    int bpe; bool bc;
    TxFmt(f, &bpe, &bc);
    if (bc) { *rowBytes = ((w + 3) / 4 > 0 ? (w + 3) / 4 : 1) * bpe; *rows = (h + 3) / 4 > 0 ? (h + 3) / 4 : 1; }
    else { *rowBytes = (w ? w : 1) * bpe; *rows = h ? h : 1; }
}
// the sRGB / plain twin of a format (for matching the game's choice)
static DXGI_FORMAT TxSrgb(DXGI_FORMAT f, bool srgb)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: return srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: return srgb ? DXGI_FORMAT_BC2_UNORM_SRGB : DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: return srgb ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
    case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: return srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
    default: return f;
    }
}
static bool TxIsSrgb(DXGI_FORMAT f) { return TxSrgb(f, true) == f && TxSrgb(f, false) != f; }
// the typeless family of a format with an sRGB twin (a texture of it can be viewed either way)
static DXGI_FORMAT TxTypeless(DXGI_FORMAT f)
{
    switch (TxSrgb(f, false)) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_BC1_UNORM: return DXGI_FORMAT_BC1_TYPELESS;
    case DXGI_FORMAT_BC2_UNORM: return DXGI_FORMAT_BC2_TYPELESS;
    case DXGI_FORMAT_BC3_UNORM: return DXGI_FORMAT_BC3_TYPELESS;
    case DXGI_FORMAT_BC7_UNORM: return DXGI_FORMAT_BC7_TYPELESS;
    default: return f;
    }
}

// ---- the fingerprint: 64-bit words of the top level's rows (its size and format mixed in)
static unsigned long long TxHash(const D3D11_TEXTURE2D_DESC* d, const D3D11_SUBRESOURCE_DATA* s)
{
    UINT rb, rows;
    TxMipSize(d->Format, d->Width, d->Height, &rb, &rows);
    unsigned long long h = 0x9E3779B97F4A7C15ull ^ ((unsigned long long)d->Width << 32) ^ ((unsigned long long)d->Height << 16) ^ (unsigned)d->Format;
    for (UINT y = 0; y < rows; ++y) {
        const unsigned char* p = (const unsigned char*)s->pSysMem + (size_t)y * s->SysMemPitch;
        UINT n8 = rb / 8;
        const unsigned long long* q = (const unsigned long long*)p;
        for (UINT i = 0; i < n8; ++i) { h ^= q[i]; h *= 0x100000001B3ull; h ^= h >> 29; }
        for (UINT i = n8 * 8; i < rb; ++i) { h ^= p[i]; h *= 0x100000001B3ull; }
    }
    return h ? h : 1;
}

// a fingerprint from 64 rows at most, 4 KB of each (mapped memory may be slow to read)
static unsigned long long TxHashSampled(const D3D11_TEXTURE2D_DESC* d, const D3D11_SUBRESOURCE_DATA* s)
{
    UINT rb, rows;
    TxMipSize(d->Format, d->Width, d->Height, &rb, &rows);
    unsigned long long h = 0x51ED27F1A3C5B9E7ull ^ ((unsigned long long)d->Width << 32) ^ ((unsigned long long)d->Height << 16) ^ (unsigned)d->Format;
    UINT step = rows > 64 ? rows / 64 : 1, n = rb < 4096 ? rb : 4096;
    for (UINT y = 0; y < rows; y += step) {
        const unsigned long long* q = (const unsigned long long*)((const unsigned char*)s->pSysMem + (size_t)y * s->SysMemPitch);
        for (UINT i = 0; i < n / 8; ++i) { h ^= q[i]; h *= 0x100000001B3ull; h ^= h >> 29; }
    }
    return h ? h : 1;
}
static bool TxIsStaging(ID3D11Texture2D* t) { D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); return d.Usage == D3D11_USAGE_STAGING; }

static TxInfo* TxFind(ID3D11Texture2D* t, bool add)
{
    unsigned hsh = ((unsigned)(size_t)t >> 4) * 2654435761u;
    for (int k = 0; k < 64; ++k) {
        TxInfo& e = g_tx[(hsh + k) & 65535];
        if (e.t == t) return &e;
        if (!e.t) { if (!add) return 0; memset(&e, 0, sizeof(e)); e.t = t; return &e; }
    }
    if (!add) return 0;
    TxInfo& e = g_tx[hsh & 65535];   // full there: the oldest slot is taken over
    memset(&e, 0, sizeof(e)); e.t = t;
    return &e;
}

static bool TxHasRepl(unsigned long long h)
{
    for (int k = 0; k < 64; ++k) { unsigned long long v = g_txRepl[(unsigned)(h + k) & 4095]; if (v == h) return true; if (!v) return false; }
    return false;
}

static void TxScan()
{
    sprintf(g_txDir, "%sNewLook_textures", g_dir);
    CreateDirectoryA(g_txDir, 0);
    char p[MAX_PATH];
    sprintf(p, "%s\\replace", g_txDir); CreateDirectoryA(p, 0);
    sprintf(p, "%s\\picked", g_txDir); CreateDirectoryA(p, 0);
    sprintf(p, "%s\\replace\\*.dds", g_txDir);
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(p, &fd);
    memset(g_txRepl, 0, sizeof(g_txRepl));
    g_txReplN = 0;
    if (f != INVALID_HANDLE_VALUE) {
        do {
            unsigned long long h = 0;
            int n = 0;
            for (; n < 16; ++n) {
                char c = fd.cFileName[n];
                int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (v < 0) break;
                h = h << 4 | (unsigned)v;
            }
            if (n < 16 || !h) continue;
            for (int k = 0; k < 64; ++k) { unsigned long long& s = g_txRepl[(unsigned)(h + k) & 4095]; if (!s || s == h) { if (!s) ++g_txReplN; s = h; break; } }
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    Log("textures: %d replacements in %s\\replace", g_txReplN, g_txDir);
}

// ---- DDS
#pragma pack(push, 1)
struct DdsPf { DWORD size, flags, fourCC, bits, r, g, b, a; };
struct DdsHdr { DWORD size, flags, height, width, pitch, depth, mips, res1[11]; DdsPf pf; DWORD caps, caps2, caps3, caps4, res2; };
struct DdsDx10 { DWORD format, dim, misc, arraySize, misc2; };
#pragma pack(pop)
static DWORD Cc(const char* s) { return (DWORD)s[0] | (DWORD)s[1] << 8 | (DWORD)s[2] << 16 | (DWORD)s[3] << 24; }

// a DDS file: its format, size, levels and the data (heap; free it)
static unsigned char* TxLoadDds(const char* path, D3D11_TEXTURE2D_DESC* d, D3D11_SUBRESOURCE_DATA* sub, int maxSub)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 128) { fclose(f); return 0; }
    unsigned char* b = (unsigned char*)malloc(n);
    if (!b || fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return 0; }
    fclose(f);
    const DdsHdr* h = (const DdsHdr*)(b + 4);
    if (memcmp(b, "DDS ", 4) || h->size != 124) { free(b); return 0; }
    long off = 128;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    if ((h->pf.flags & 4) && h->pf.fourCC == Cc("DX10")) {
        const DdsDx10* x = (const DdsDx10*)(b + 128);
        if (n < 148 || x->arraySize > 1 || (x->misc & 4)) { free(b); return 0; }   // one 2D texture only (no arrays, cubes)
        fmt = (DXGI_FORMAT)x->format;
        off = 148;
    } else if (h->pf.flags & 4) {
        DWORD c = h->pf.fourCC;
        fmt = c == Cc("DXT1") ? DXGI_FORMAT_BC1_UNORM : c == Cc("DXT2") || c == Cc("DXT3") ? DXGI_FORMAT_BC2_UNORM :
              c == Cc("DXT4") || c == Cc("DXT5") ? DXGI_FORMAT_BC3_UNORM : c == Cc("ATI1") || c == Cc("BC4U") ? DXGI_FORMAT_BC4_UNORM :
              c == Cc("ATI2") || c == Cc("BC5U") ? DXGI_FORMAT_BC5_UNORM : DXGI_FORMAT_UNKNOWN;
    } else if ((h->pf.flags & 0x40) && h->pf.bits == 32) {
        fmt = h->pf.r == 0xff ? DXGI_FORMAT_R8G8B8A8_UNORM : h->pf.r == 0xff0000 ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_UNKNOWN;
    }
    int bpe; bool bc;
    if (fmt == DXGI_FORMAT_UNKNOWN || !TxFmt(fmt, &bpe, &bc) || !h->width || !h->height) { free(b); return 0; }
    memset(d, 0, sizeof(*d));
    d->Width = h->width; d->Height = h->height; d->MipLevels = h->mips ? h->mips : 1; d->ArraySize = 1; d->Format = fmt; d->SampleDesc.Count = 1;
    UINT w = d->Width, ht = d->Height;
    for (UINT m = 0; m < d->MipLevels; ++m) {
        UINT rb, rows;
        TxMipSize(fmt, w, ht, &rb, &rows);
        if ((int)m >= maxSub || off + (long)rb * rows > n) { d->MipLevels = m; break; }
        sub[m].pSysMem = b + off; sub[m].SysMemPitch = rb; sub[m].SysMemSlicePitch = rb * rows;
        off += rb * rows;
        w = w > 1 ? w / 2 : 1; ht = ht > 1 ? ht / 2 : 1;
    }
    if (!d->MipLevels) { free(b); return 0; }
    return b;
}

// a texture's levels written as DDS (legacy header where tools read it best, else DX10)
static bool TxWriteDds(const char* path, DXGI_FORMAT fmt, UINT w, UINT h, UINT mips, const void* const* data, const UINT* pitch)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    DdsHdr hd;
    memset(&hd, 0, sizeof(hd));
    hd.size = 124; hd.flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000; hd.height = h; hd.width = w; hd.mips = mips;
    hd.pf.size = 32; hd.caps = 0x1000 | (mips > 1 ? 0x400008 : 0);
    bool dx10 = false;
    DXGI_FORMAT plain = TxSrgb(fmt, false);
    switch (plain) {
    case DXGI_FORMAT_BC1_UNORM: hd.pf.flags = 4; hd.pf.fourCC = Cc("DXT1"); break;
    case DXGI_FORMAT_BC2_UNORM: hd.pf.flags = 4; hd.pf.fourCC = Cc("DXT3"); break;
    case DXGI_FORMAT_BC3_UNORM: hd.pf.flags = 4; hd.pf.fourCC = Cc("DXT5"); break;
    case DXGI_FORMAT_BC4_UNORM: hd.pf.flags = 4; hd.pf.fourCC = Cc("ATI1"); break;
    case DXGI_FORMAT_BC5_UNORM: hd.pf.flags = 4; hd.pf.fourCC = Cc("ATI2"); break;
    case DXGI_FORMAT_R8G8B8A8_UNORM: hd.pf.flags = 0x41; hd.pf.bits = 32; hd.pf.r = 0xff; hd.pf.g = 0xff00; hd.pf.b = 0xff0000; hd.pf.a = 0xff000000; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM: hd.pf.flags = 0x41; hd.pf.bits = 32; hd.pf.r = 0xff0000; hd.pf.g = 0xff00; hd.pf.b = 0xff; hd.pf.a = 0xff000000; break;
    default: hd.pf.flags = 4; hd.pf.fourCC = Cc("DX10"); dx10 = true; break;
    }
    fwrite("DDS ", 1, 4, f);
    fwrite(&hd, sizeof(hd), 1, f);
    if (dx10) { DdsDx10 x = { (DWORD)fmt, 3, 0, 1, 0 }; fwrite(&x, sizeof(x), 1, f); }
    UINT mw = w, mh = h;
    for (UINT m = 0; m < mips; ++m) {
        UINT rb, rows;
        TxMipSize(fmt, mw, mh, &rb, &rows);
        for (UINT y = 0; y < rows; ++y) fwrite((const char*)data[m] + (size_t)y * pitch[m], 1, rb, f);
        mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1;
    }
    fclose(f);
    return true;
}

// a GPU texture saved (read back through a staging copy)
static bool TxSaveGpu(ID3D11DeviceContext* ctx, ID3D11Texture2D* t, const char* path)
{
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    if (d.ArraySize != 1 || d.SampleDesc.Count != 1) return false;
    int bpe; bool bc;
    if (!TxFmt(d.Format, &bpe, &bc)) return false;
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0;
    ID3D11Texture2D* st = 0;
    if (FAILED(oCreateTex2D(g_dev, &s, 0, &st))) return false;
    ctx->CopyResource(st, t);
    const void* data[16] = {};
    UINT pitch[16] = {};
    D3D11_MAPPED_SUBRESOURCE m[16];
    UINT mips = d.MipLevels < 16 ? d.MipLevels : 16, got = 0;
    for (; got < mips; ++got) {
        if (FAILED(ctx->Map(st, got, D3D11_MAP_READ, 0, &m[got]))) break;
        data[got] = m[got].pData; pitch[got] = m[got].RowPitch;
    }
    bool ok = got == mips && TxWriteDds(path, d.Format, d.Width, d.Height, mips, data, pitch);
    for (UINT k = 0; k < got; ++k) ctx->Unmap(st, k);
    st->Release();
    return ok;
}

// counts, now and then (the 10 s stats)
static volatile LONG g_txUpd0, g_txMapped, g_txCopied, g_txSwaps, g_txInPlace, g_txEmpty;
static void TxStats()
{
    static LONG last = -1;
    LONG sum = g_txWithData + g_txEmpty + g_txUpd0 + g_txMapped + g_txCopied;
    if (sum == last) return;
    last = sum;
    Log("textures: made with data %ld, made empty %ld; top levels seen by update %ld, by map %ld, copied in %ld; replaced %ld (in place %ld, "
        "ours bound instead %ld), unusable %ld; %d files in replace\\", g_txWithData, g_txEmpty, g_txUpd0, g_txMapped, g_txCopied, g_txReplaced,
        g_txInPlace, g_txSwaps, g_txReplFailed, g_txReplN);
}

// ---- the hooks
static HRESULT STDMETHODCALLTYPE HkCreateTex2D(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* d, const D3D11_SUBRESOURCE_DATA* s, ID3D11Texture2D** out)
{
    int bpe; bool bc;
    if (!d || !out || d->ArraySize != 1 || d->SampleDesc.Count != 1 || (d->MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) ||
        (d->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) || !TxFmt(d->Format, &bpe, &bc) ||
        (!(d->BindFlags & D3D11_BIND_SHADER_RESOURCE) && d->Usage != D3D11_USAGE_STAGING))
        return oCreateTex2D(dev, d, s, out);
    if (!InterlockedExchange(&g_txLockInit, 1)) InitializeCriticalSection(&g_txLock);
    if (!s || !s->pSysMem) {   // made empty, filled later (staging ones too: copied into the real one)
        HRESULT hr0 = oCreateTex2D(dev, d, s, out);
        if (SUCCEEDED(hr0) && *out) {
            EnterCriticalSection(&g_txLock);
            TxInfo* e = TxFind(*out, true);
            if (e->repl) { e->repl->Release(); }
            memset(e, 0, sizeof(*e));
            e->t = *out; e->w = (unsigned short)d->Width; e->h = (unsigned short)d->Height; e->fmt = (unsigned char)d->Format;
            e->mips = (unsigned char)d->MipLevels; e->origFmt = (unsigned char)d->Format;
            LeaveCriticalSection(&g_txLock);
            InterlockedIncrement(&g_txEmpty);
        }
        return hr0;
    }
    InterlockedIncrement(&g_txWithData);
    unsigned long long h = TxHash(d, s);
    HRESULT hr = E_FAIL;
    bool replaced = false;
    if (g_txReplN && TxHasRepl(h)) {
        char path[MAX_PATH];
        sprintf(path, "%s\\replace\\%016llx.dds", g_txDir, h);
        // the file may carry more after the hash: find it
        WIN32_FIND_DATAA fd;
        char pat[MAX_PATH];
        sprintf(pat, "%s\\replace\\%016llx*.dds", g_txDir, h);
        HANDLE ff = FindFirstFileA(pat, &fd);
        if (ff != INVALID_HANDLE_VALUE) { sprintf(path, "%s\\replace\\%s", g_txDir, fd.cFileName); FindClose(ff); }
        D3D11_TEXTURE2D_DESC nd;
        D3D11_SUBRESOURCE_DATA sub[16];
        unsigned char* blob = TxLoadDds(path, &nd, sub, 16);
        if (blob) {
            nd.Format = TxSrgb(nd.Format, TxIsSrgb(d->Format));
            nd.Usage = d->Usage; nd.BindFlags = d->BindFlags; nd.CPUAccessFlags = d->CPUAccessFlags; nd.MiscFlags = d->MiscFlags & ~D3D11_RESOURCE_MISC_GENERATE_MIPS;
            hr = oCreateTex2D(dev, &nd, sub, out);
            free(blob);
            if (SUCCEEDED(hr)) { replaced = true; InterlockedIncrement(&g_txReplaced); }
        }
        if (!replaced) {
            InterlockedIncrement(&g_txReplFailed);
            static int said;
            if (said++ < 20) Log("textures: %s could not be used (format / size), the game's kept", path);
        }
    }
    if (!replaced) hr = oCreateTex2D(dev, d, s, out);
    if (SUCCEEDED(hr) && *out) {
        EnterCriticalSection(&g_txLock);
        TxInfo* e = TxFind(*out, true);
        if (e->repl) { e->repl->Release(); e->repl = 0; }
        e->mapPtr = 0;
        D3D11_TEXTURE2D_DESC fd;
        (*out)->GetDesc(&fd);
        e->hash = h; e->w = (unsigned short)d->Width; e->h = (unsigned short)d->Height; e->fmt = (unsigned char)fd.Format;
        e->mips = (unsigned char)d->MipLevels; e->replaced = replaced; e->origFmt = (unsigned char)d->Format;
        LeaveCriticalSection(&g_txLock);
        InterlockedIncrement(&g_txSeen);
    }
    return hr;
}

// a view of a replaced texture gets the replacement's format (sRGB as the game asked) and all its levels
struct TxView;
static TxView* TxViewFind(ID3D11ShaderResourceView* v, bool add);
static void TxViewNote(ID3D11ShaderResourceView* v, ID3D11Texture2D* t, bool srgb);
static HRESULT STDMETHODCALLTYPE HkCreateSRV(ID3D11Device* dev, ID3D11Resource* r, const D3D11_SHADER_RESOURCE_VIEW_DESC* d, ID3D11ShaderResourceView** out)
{
    if (r && out && g_txLockInit) {   // which texture a view shows (a later swap: 1.10)
        D3D11_RESOURCE_DIMENSION dim;
        r->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            EnterCriticalSection(&g_txLock);
            TxInfo* e = TxFind((ID3D11Texture2D*)r, false);
            LeaveCriticalSection(&g_txLock);
            if (e && e->replaced != 1) {   // (1: replaced at creation: its view is fixed below)
                HRESULT hr = oCreateSRV(dev, r, d, out);
                if (SUCCEEDED(hr) && *out) TxViewNote(*out, (ID3D11Texture2D*)r, d ? TxIsSrgb(d->Format) : TxIsSrgb((DXGI_FORMAT)e->fmt));
                return hr;
            }
        }
    }
    if (r && d && g_txReplaced) {
        ID3D11Texture2D* t = 0;
        if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t) {
            t->Release();
            TxInfo* e = 0;
            if (g_txLockInit) { EnterCriticalSection(&g_txLock); e = TxFind(t, false); LeaveCriticalSection(&g_txLock); }
            if (e && e->replaced && d->ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
                D3D11_SHADER_RESOURCE_VIEW_DESC nd = *d;
                nd.Format = TxSrgb((DXGI_FORMAT)e->fmt, TxIsSrgb(d->Format));
                nd.Texture2D.MostDetailedMip = 0; nd.Texture2D.MipLevels = (UINT)-1;
                return oCreateSRV(dev, r, &nd, out);
            }
        }
    }
    return oCreateSRV(dev, r, d, out);
}


// ---- textures made empty and filled later (1.10: the game does this for nearly all; 1.09 saw 2 with data).
// Their fingerprint is taken when their top level arrives: UpdateSubresource of level 0, or a write Map / Unmap of a
// staging or dynamic texture's level 0 then a copy into the texture. A replacement of the same size and format is
// written into the game's texture at once (every level; the game's later uploads to it are dropped); another size or
// format becomes a texture of ours, put in place of the game's wherever it is bound for a pixel shader.
typedef void(STDMETHODCALLTYPE* UpdSubFn)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* MapFn)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
typedef void(STDMETHODCALLTYPE* UnmapFn)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
typedef void(STDMETHODCALLTYPE* CopyResFn)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
typedef void(STDMETHODCALLTYPE* CopySubFn)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
typedef void(STDMETHODCALLTYPE* PSSetSRVFn)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
static UpdSubFn oUpdSub;
static MapFn oMap;
static UnmapFn oUnmap;
static CopyResFn oCopyRes;
static CopySubFn oCopySub;
static PSSetSRVFn oPSSetSRV;
// a view of a tracked texture -> its texture (for the swap)
struct TxView { ID3D11ShaderResourceView* v; ID3D11Texture2D* t; ID3D11ShaderResourceView* repl; bool srgb; };
static TxView g_txv[65536];

static TxView* TxViewFind(ID3D11ShaderResourceView* v, bool add)
{
    unsigned hsh = ((unsigned)(size_t)v >> 4) * 2654435761u;
    for (int k = 0; k < 64; ++k) {
        TxView& e = g_txv[(hsh + k) & 65535];
        if (e.v == v) return &e;
        if (!e.v) { if (!add) return 0; memset(&e, 0, sizeof(e)); e.v = v; return &e; }
    }
    if (!add) return 0;
    TxView& e = g_txv[hsh & 65535];
    memset(&e, 0, sizeof(e)); e.v = v;
    return &e;
}

static bool TxIsTex2D(ID3D11Resource* r)
{
    if (!r) return false;
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    return dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D;
}

// the top level of texture t is known (data at s with pitch): its fingerprint, and its replacement if there is one
static void TxApply(ID3D11DeviceContext* ctx, ID3D11Texture2D* t, TxInfo* e);
static void TxGotTop(ID3D11DeviceContext* ctx, ID3D11Texture2D* t, TxInfo* e, const void* data, UINT pitch)
{
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    D3D11_SUBRESOURCE_DATA s = { data, pitch, 0 };
    e->hash = TxHash(&d, &s);
    TxApply(ctx, t, e);
}
// texture t's fingerprint is known: its replacement, if there is one
static void TxApply(ID3D11DeviceContext* ctx, ID3D11Texture2D* t, TxInfo* e)
{
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    if (!g_txReplN || !TxHasRepl(e->hash) || e->replaced) return;
    char path[MAX_PATH], pat[MAX_PATH];
    sprintf(path, "%s\\replace\\%016llx.dds", g_txDir, e->hash);
    sprintf(pat, "%s\\replace\\%016llx*.dds", g_txDir, e->hash);
    WIN32_FIND_DATAA fd;
    HANDLE ff = FindFirstFileA(pat, &fd);
    if (ff != INVALID_HANDLE_VALUE) { sprintf(path, "%s\\replace\\%s", g_txDir, fd.cFileName); FindClose(ff); }
    D3D11_TEXTURE2D_DESC nd;
    D3D11_SUBRESOURCE_DATA sub[16];
    unsigned char* blob = TxLoadDds(path, &nd, sub, 16);
    if (!blob) { InterlockedIncrement(&g_txReplFailed); Log("textures: %s could not be read", path); return; }
    nd.Format = TxSrgb(nd.Format, TxIsSrgb(d.Format));
    bool sameKind = TxSrgb(nd.Format, false) == TxSrgb(d.Format, false) || (d.Format == DXGI_FORMAT_BC1_TYPELESS && TxSrgb(nd.Format, false) == DXGI_FORMAT_BC1_UNORM) ||
                    (d.Format == DXGI_FORMAT_BC2_TYPELESS && TxSrgb(nd.Format, false) == DXGI_FORMAT_BC2_UNORM) ||
                    (d.Format == DXGI_FORMAT_BC3_TYPELESS && TxSrgb(nd.Format, false) == DXGI_FORMAT_BC3_UNORM);
    if (sameKind && nd.Width == d.Width && nd.Height == d.Height && nd.MipLevels >= d.MipLevels && d.Usage == D3D11_USAGE_DEFAULT) {
        for (UINT m = 0; m < d.MipLevels; ++m) oUpdSub(ctx, t, m, 0, sub[m].pSysMem, sub[m].SysMemPitch, sub[m].SysMemSlicePitch);
        e->replaced = 2;   // in place: the game's own uploads to it are dropped from now on
        InterlockedIncrement(&g_txInPlace); InterlockedIncrement(&g_txReplaced);
    } else {
        nd.Usage = D3D11_USAGE_IMMUTABLE; nd.BindFlags = D3D11_BIND_SHADER_RESOURCE; nd.CPUAccessFlags = 0; nd.MiscFlags = 0;
        nd.Format = TxTypeless(nd.Format);   // viewed sRGB or not, as each of the game's views asks
        ID3D11Texture2D* rt = 0;
        if (SUCCEEDED(oCreateTex2D(g_dev, &nd, sub, &rt))) {
            if (e->repl) e->repl->Release();
            e->repl = rt;
            e->replaced = 3;   // ours instead, at every pixel-shader binding of its views
            InterlockedIncrement(&g_txSwaps); InterlockedIncrement(&g_txReplaced);
        } else { InterlockedIncrement(&g_txReplFailed); Log("textures: %s: a %ux%u fmt %d texture could not be made", path, nd.Width, nd.Height, nd.Format); }
    }
    free(blob);
}

static void STDMETHODCALLTYPE HkUpdSub(ID3D11DeviceContext* ctx, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* data, UINT rp, UINT dp)
{
    if (g_cbsCopy && sub == 0) CbsOnUpdate(r, box, data);   // an object's constants (worldedit.inc)
    if (g_txLockInit && data && TxIsTex2D(r)) {
        EnterCriticalSection(&g_txLock);
        TxInfo* e = TxFind((ID3D11Texture2D*)r, false);
        bool drop = e && e->replaced == 2;
        if (e && !drop && sub == 0 && !box) { InterlockedIncrement(&g_txUpd0); TxGotTop(ctx, (ID3D11Texture2D*)r, e, data, rp); drop = e->replaced == 2; }
        LeaveCriticalSection(&g_txLock);
        if (drop) return;
    }
    oUpdSub(ctx, r, sub, box, data, rp, dp);
}

static HRESULT STDMETHODCALLTYPE HkMap(ID3D11DeviceContext* ctx, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* m)
{
    HRESULT hr = oMap(ctx, r, sub, type, flags, m);
    if (g_cbsCopy && SUCCEEDED(hr) && sub == 0) CbsOnMap(r, type, m);   // an object's constants: written into our copy (worldedit.inc)
    // (1.13: staging textures only: a dynamic one the game rewrites twice a second was hashed at every Unmap, read back
    // from GPU memory: a stutter twice a second, even with NewLook off)
    if (SUCCEEDED(hr) && m && sub == 0 && type != D3D11_MAP_READ && g_txLockInit && TxIsTex2D(r) && TxIsStaging((ID3D11Texture2D*)r)) {
        EnterCriticalSection(&g_txLock);
        TxInfo* e = TxFind((ID3D11Texture2D*)r, false);
        if (e) { e->mapPtr = m->pData; e->mapPitch = m->RowPitch; }
        LeaveCriticalSection(&g_txLock);
    }
    return hr;
}

static void STDMETHODCALLTYPE HkUnmap(ID3D11DeviceContext* ctx, ID3D11Resource* r, UINT sub)
{
    if (sub == 0) CbsOnUnmap(r);   // our copy of an object's constants into the driver's memory (worldedit.inc)
    if (sub == 0 && g_txLockInit && TxIsTex2D(r)) {
        EnterCriticalSection(&g_txLock);
        TxInfo* e = TxFind((ID3D11Texture2D*)r, false);
        if (e && e->mapPtr) {   // its top level as written (a staging texture: copied into the real one next)
            D3D11_TEXTURE2D_DESC d;
            ((ID3D11Texture2D*)r)->GetDesc(&d);
            D3D11_SUBRESOURCE_DATA s = { e->mapPtr, e->mapPitch, 0 };
            e->hash = TxHashSampled(&d, &s);
            e->mapPtr = 0;
            InterlockedIncrement(&g_txMapped);
        }
        LeaveCriticalSection(&g_txLock);
    }
    oUnmap(ctx, r, sub);
}

// a copy from a staging texture whose top level we know: the destination's fingerprint (and its replacement)
static void TxCopied(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src)
{
    if (!g_txLockInit || !TxIsTex2D(dst) || !TxIsTex2D(src)) return;
    EnterCriticalSection(&g_txLock);
    TxInfo* s = TxFind((ID3D11Texture2D*)src, false);
    TxInfo* e = TxFind((ID3D11Texture2D*)dst, false);
    if (s && e && s->hash && !e->replaced) {
        InterlockedIncrement(&g_txCopied);
        e->hash = s->hash;
        TxApply(ctx, (ID3D11Texture2D*)dst, e);
    }
    LeaveCriticalSection(&g_txLock);
}
static void STDMETHODCALLTYPE HkCopyRes(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src)
{
    oCopyRes(ctx, dst, src);
    TxCopied(ctx, dst, src);
}
static void STDMETHODCALLTYPE HkCopySub(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT ss, const D3D11_BOX* box)
{
    bool drop = false;
    if (g_txLockInit && ds != 0 && TxIsTex2D(dst)) {   // later levels into a texture replaced in place: dropped
        EnterCriticalSection(&g_txLock);
        TxInfo* e = TxFind((ID3D11Texture2D*)dst, false);
        drop = e && e->replaced == 2;
        LeaveCriticalSection(&g_txLock);
    }
    if (drop) return;
    oCopySub(ctx, dst, ds, x, y, z, src, ss, box);
    if (ds == 0 && ss == 0 && !box) TxCopied(ctx, dst, src);
}

// pixel-shader views of a texture replaced by one of another size / format: ours instead
static void STDMETHODCALLTYPE HkPSSetSRV(ID3D11DeviceContext* ctx, UINT start, UINT n, ID3D11ShaderResourceView* const* v)
{
    if (g_txSwaps && v && n <= 32) {
        ID3D11ShaderResourceView* w[32];
        bool any = false;
        EnterCriticalSection(&g_txLock);
        for (UINT i = 0; i < n; ++i) {
            w[i] = v[i];
            if (!v[i]) continue;
            TxView* tv = TxViewFind(v[i], false);
            if (!tv) continue;
            TxInfo* e = TxFind(tv->t, false);
            if (!e || e->replaced != 3 || !e->repl) continue;
            if (!tv->repl) {
                D3D11_TEXTURE2D_DESC rd;
                e->repl->GetDesc(&rd);
                D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
                DXGI_FORMAT plain = rd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : rd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ?
                    DXGI_FORMAT_B8G8R8A8_UNORM : rd.Format == DXGI_FORMAT_BC1_TYPELESS ? DXGI_FORMAT_BC1_UNORM : rd.Format == DXGI_FORMAT_BC2_TYPELESS ?
                    DXGI_FORMAT_BC2_UNORM : rd.Format == DXGI_FORMAT_BC3_TYPELESS ? DXGI_FORMAT_BC3_UNORM : rd.Format == DXGI_FORMAT_BC7_TYPELESS ?
                    DXGI_FORMAT_BC7_UNORM : rd.Format;
                sd.Format = TxSrgb(plain, tv->srgb); sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = (UINT)-1;
                oCreateSRV(g_dev, e->repl, &sd, &tv->repl);
            }
            if (tv->repl) { w[i] = tv->repl; any = true; }
        }
        LeaveCriticalSection(&g_txLock);
        if (any) { oPSSetSRV(ctx, start, n, w); return; }
    }
    oPSSetSRV(ctx, start, n, v);
}

static void TxViewNote(ID3D11ShaderResourceView* v, ID3D11Texture2D* t, bool srgb)
{
    EnterCriticalSection(&g_txLock);
    TxView* tv = TxViewFind(v, true);
    if (tv->repl) tv->repl->Release();
    tv->t = t; tv->repl = 0; tv->srgb = srgb;
    LeaveCriticalSection(&g_txLock);
}

// ---- the picker (Shift+X)
struct PsSlots { ID3D11PixelShader* ps; char name[16][28]; void* code; SIZE_T size; };
static PsSlots g_psSlots[4096];
static void TxNotePs(const void* code, SIZE_T size, ID3D11PixelShader* ps)
{
    if (!g_reflect) return;
    ID3D11ShaderReflection* r = 0;
    if (FAILED(g_reflect(code, size, __uuidof(ID3D11ShaderReflection), (void**)&r)) || !r) return;
    D3D11_SHADER_DESC sd;
    r->GetDesc(&sd);
    unsigned hsh = ((unsigned)(size_t)ps >> 4) * 2654435761u;
    PsSlots* e = 0;
    for (int k = 0; k < 64 && !e; ++k) {
        PsSlots& c = g_psSlots[(hsh + k) & 4095];
        if (!c.ps || c.ps == ps) e = &c;
    }
    if (e) {
        free(e->code);
        memset(e, 0, sizeof(*e));
        e->ps = ps;
        e->code = malloc(size);   // kept for the picker (its disassembly is saved with the textures)
        if (e->code) { memcpy(e->code, code, size); e->size = size; }
        for (UINT i = 0; i < sd.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC bd;
            if (SUCCEEDED(r->GetResourceBindingDesc(i, &bd)) && bd.Type == D3D_SIT_TEXTURE && bd.BindPoint < 16)
                strncpy(e->name[bd.BindPoint], bd.Name, 27);
        }
    }
    r->Release();
}
static const char* TxSlotName(ID3D11PixelShader* ps, int slot)
{
    unsigned hsh = ((unsigned)(size_t)ps >> 4) * 2654435761u;
    for (int k = 0; k < 64; ++k) {
        PsSlots& c = g_psSlots[(hsh + k) & 4095];
        if (c.ps == ps) return c.name[slot][0] ? c.name[slot] : "slot";
        if (!c.ps) break;
    }
    return "slot";
}

static volatile LONG g_pickReq;            // 1: asked, 2: this frame records, 3: waiting for the read-back
static bool g_txMsgDirty;                   // the overlay text changed
static HalfRT g_pickId;
static ID3D11Texture2D* g_pickStage;
static ID3D11PixelShader* g_psPickId;
static ID3D11Buffer* g_pickCb;
static ID3D11DepthStencilState* g_pickDs;
struct PickDraw { ID3D11PixelShader* ps; ID3D11Texture2D* tex[16]; };
static PickDraw* g_pickDraws;
static int g_pickN;
static unsigned g_pickFrame;
static const char* kPickSrc = R"HLSL(
cbuffer P : register(b13) { float4 Id; };
float4 ps(float4 pos : SV_Position) : SV_Target { return Id.xxxx; }
)HLSL";

static void PickRelease()
{
    for (int i = 0; i < g_pickN; ++i)
        for (int s = 0; s < 16; ++s) if (g_pickDraws[i].tex[s]) g_pickDraws[i].tex[s]->Release();
    g_pickN = 0;
}

// every G-buffer draw of the pick frame: its number into the ID target where it is the visible surface
static void TxPickAfterDraw(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    if (g_pickReq != 2 || !g_air.normal || !g_psPickId) return;
    ID3D11RenderTargetView* rtv[4] = {};
    ID3D11DepthStencilView* dsv = 0;
    ctx->OMGetRenderTargets(4, rtv, &dsv);
    ID3D11Resource* nres = 0;
    g_air.normal->GetResource(&nres);
    if (nres) nres->Release();
    bool gb = false;
    for (int k = 0; k < 4; ++k) if (rtv[k]) { ID3D11Resource* r = 0; rtv[k]->GetResource(&r); if (r == nres) gb = true; if (r) r->Release(); }
    if (gb && dsv && g_pickN < 8192) {
        D3D11_TEXTURE2D_DESC nd;
        ((ID3D11Texture2D*)nres)->GetDesc(&nd);
        if (!g_pickId.t) {
            if (!MakeRT(g_pickId, nd.Width, nd.Height, DXGI_FORMAT_R32_FLOAT, false)) { g_pickReq = 0; Log("textures: pick target failed"); }
        }
        if (g_pickId.t) {
            if (!g_pickN) { float z[4] = {}; ctx->ClearRenderTargetView(g_pickId.rtv, z); }
            PickDraw& pd = g_pickDraws[g_pickN];
            memset(&pd, 0, sizeof(pd));
            ctx->PSGetShader(&pd.ps, 0, 0);
            if (pd.ps) pd.ps->Release();   // (a name only)
            ID3D11ShaderResourceView* sv[16] = {};
            ctx->PSGetShaderResources(0, 16, sv);
            for (int s = 0; s < 16; ++s) {
                if (!sv[s]) continue;
                ID3D11Resource* r = 0;
                sv[s]->GetResource(&r);
                if (r && FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pd.tex[s]))) pd.tex[s] = 0;
                if (r) r->Release();
                sv[s]->Release();
            }
            ++g_pickN;
            float id[4] = { (float)g_pickN, 0, 0, 0 };
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(g_pickCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, id, 16); ctx->Unmap(g_pickCb, 0); }
            ID3D11PixelShader* sPs = 0; ctx->PSGetShader(&sPs, 0, 0);
            ID3D11Buffer* sCb = 0; ctx->PSGetConstantBuffers(13, 1, &sCb);
            ID3D11DepthStencilState* sDs = 0; UINT sRef = 0; ctx->OMGetDepthStencilState(&sDs, &sRef);
            ID3D11BlendState* sBl = 0; FLOAT bf[4]; UINT bm; ctx->OMGetBlendState(&sBl, bf, &bm);
            ctx->OMSetRenderTargets(1, &g_pickId.rtv, dsv);
            ctx->OMSetDepthStencilState(g_pickDs, 0);
            ctx->OMSetBlendState(0, 0, 0xffffffff);
            oPSSetShader(ctx, g_psPickId, 0, 0);
            ctx->PSSetConstantBuffers(13, 1, &g_pickCb);
            Replay(ctx, d);
            oPSSetShader(ctx, sPs, 0, 0);
            ctx->PSSetConstantBuffers(13, 1, &sCb);
            ctx->OMSetDepthStencilState(sDs, sRef);
            ctx->OMSetBlendState(sBl, bf, bm);
            ctx->OMSetRenderTargets(4, rtv, dsv);
            IUnknown* rel[] = { sPs, sCb, sDs, sBl };
            for (int k = 0; k < 4; ++k) if (rel[k]) rel[k]->Release();
        }
    }
    for (int k = 0; k < 4; ++k) if (rtv[k]) rtv[k]->Release();
    if (dsv) dsv->Release();
}

// at Present: start a pick, copy its centre, read it and save the textures
static void TxPickFrame()
{
    ID3D11DeviceContext* ctx = 0;
    if (!g_dev || !g_pickReq) return;
    if (g_pickReq == 1) {
        if (!g_psPickId) {
            ID3DBlob *code = 0, *err = 0;
            if (g_compile && SUCCEEDED(g_compile(kPickSrc, strlen(kPickSrc), "pick", 0, 0, "ps", "ps_4_0", 0, 0, &code, &err)))
                g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), 0, &g_psPickId);
            if (code) code->Release();
            if (err) err->Release();
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            g_dev->CreateBuffer(&bd, 0, &g_pickCb);
            D3D11_DEPTH_STENCIL_DESC dd = {};
            dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
            g_dev->CreateDepthStencilState(&dd, &g_pickDs);
            D3D11_TEXTURE2D_DESC sd = {};
            sd.Width = sd.Height = 1; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = DXGI_FORMAT_R32_FLOAT; sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            oCreateTex2D(g_dev, &sd, 0, &g_pickStage);
            g_pickDraws = (PickDraw*)calloc(8192, sizeof(PickDraw));
        }
        if (!g_psPickId || !g_pickCb || !g_pickDs || !g_pickStage || !g_pickDraws) { g_pickReq = 0; strcpy(g_lampMsg, "texture pick: not available"); return; }
        PickRelease();
        g_pickReq = 2;
        return;
    }
    g_dev->GetImmediateContext(&ctx);
    if (!ctx) return;
    if (g_pickReq == 2) {   // the frame is drawn: the centre's number
        if (g_pickN && g_pickId.t) {
            D3D11_TEXTURE2D_DESC d;
            g_pickId.t->GetDesc(&d);
            D3D11_BOX box = { d.Width / 2, d.Height / 2, 0, d.Width / 2 + 1, d.Height / 2 + 1, 1 };
            ctx->CopySubresourceRegion(g_pickStage, 0, 0, 0, 0, g_pickId.t, 0, &box);
            g_pickReq = 3;
            g_pickFrame = g_frame;
        } else { g_pickReq = 0; strcpy(g_lampMsg, "texture pick: no G-buffer draws seen"); }
    } else if (g_pickReq == 3 && g_frame > g_pickFrame + 1) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(g_pickStage, 0, D3D11_MAP_READ, 0, &m))) {
            int id = (int)(*(const float*)m.pData + 0.5f);
            ctx->Unmap(g_pickStage, 0);
            if (id < 1 || id > g_pickN) sprintf(g_lampMsg, "texture pick: nothing at the centre (%d of %d draws)", id, g_pickN);
            else {
                static int pickNo;
                ++pickNo;
                const PickDraw& pd = g_pickDraws[id - 1];
                int saved = 0;
                for (int s = 0; s < 16; ++s) {
                    if (!pd.tex[s]) continue;
                    TxInfo* e = 0;
                    if (g_txLockInit) { EnterCriticalSection(&g_txLock); e = TxFind(pd.tex[s], false); LeaveCriticalSection(&g_txLock); }
                    D3D11_TEXTURE2D_DESC td;
                    pd.tex[s]->GetDesc(&td);
                    if (!e || !e->hash) { Log("texture pick %d: slot %d %s: %ux%u fmt %d, %s", pickNo, s, TxSlotName(pd.ps, s), td.Width, td.Height,
                                  td.Format, e ? "its content never seen arriving (filled another way)" : "not a game texture (a render target?)"); continue; }
                    char path[MAX_PATH];
                    sprintf(path, "%s\\picked\\%016llx_%03d_%s.dds", g_txDir, e->hash, pickNo, TxSlotName(pd.ps, s));
                    bool ok = TxSaveGpu(ctx, pd.tex[s], path);
                    if (ok) ++saved;
                    Log("texture pick %d: slot %d %s: %016llx %ux%u fmt %d mips %u%s -> %s", pickNo, s, TxSlotName(pd.ps, s), e->hash, td.Width,
                        td.Height, td.Format, td.MipLevels, e->replaced ? " (replaced)" : "", ok ? path : "not saved");
                }
                // the material's pixel shader: its disassembly (for shader work: parallax)
                unsigned hsh = ((unsigned)(size_t)pd.ps >> 4) * 2654435761u;
                for (int k = 0; k < 64; ++k) {
                    PsSlots& c = g_psSlots[(hsh + k) & 4095];
                    if (c.ps != pd.ps) { if (!c.ps) break; continue; }
                    typedef HRESULT(WINAPI* DisFn)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);
                    HMODULE dm = GetModuleHandleA("d3dcompiler_47.dll");
                    DisFn dis = dm ? (DisFn)GetProcAddress(dm, "D3DDisassemble") : 0;
                    ID3DBlob* b = 0;
                    if (c.code && dis && SUCCEEDED(dis(c.code, c.size, 0, 0, &b)) && b) {
                        unsigned long long ph = 1469598103934665603ull;
                        for (SIZE_T q = 0; q < c.size; ++q) { ph ^= ((const unsigned char*)c.code)[q]; ph *= 1099511628211ull; }
                        char path[MAX_PATH];
                        sprintf(path, "%s\\picked\\%03d_shader_%016llx.txt", g_txDir, pickNo, ph);
                        FILE* f = fopen(path, "wb");
                        if (f) { fwrite(b->GetBufferPointer(), 1, b->GetBufferSize() - 1, f); fclose(f); }
                        Log("texture pick %d: its pixel shader %016llx -> %s", pickNo, ph, path);
                        b->Release();
                    }
                    break;
                }
                sprintf(g_lampMsg, "texture pick %d: %d textures saved to NewLook_textures\\picked (see the log)", pickNo, saved);
            }
        }
        PickRelease();
        g_pickReq = 0;
        g_txMsgDirty = true;
    }
    ctx->Release();
}
