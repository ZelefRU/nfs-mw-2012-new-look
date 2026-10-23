// ---------------------------------------------------------------- lamps tied to breakable props
// Found with the object tools (NFSLight_objects.log, two lamp posts): a breakable prop's runtime instance in the
// game's memory, around the address A of its position:
//   A-48, A-32, A-16  its axes (rows of its world matrix, w = 1), A+0 position, A+12 = 1.0
//   A+16 .. A+44      velocity and spin, all 0 while it stands
//   A+140             flags: 0x01010400 standing; bit 0x800 set once hit (0x01010C00, later 0x00000C00)
//   A+156             bounding radius (2.5 for a lamp post), A+172: 0x00FF0003 standing
// A lamp tied to a prop is found by the prop's standing position (exact float match + this pattern), then read
// every frame: hit -> the lamp flickers and goes out (or falls with the post, KnockedLamp = 1). When the game puts
// the post back, it is found standing again and the lamp is lit again. The search runs on a worker thread:
// first in the memory blocks props were found in before, the whole address space at most every 15 s.


static bool ReadProp(const unsigned char* a, float* f /* 60 floats from A-48 */)
{
    return SafeCopy(f, a - 48, 240);
}
static unsigned U32(const float* f, int i) { unsigned u; memcpy(&u, &f[i], 4); return u; }
// f[12] = A+0. Offsets: A+140 = f[12 + 35], A+156 = f[12 + 39], A+172 = f[12 + 43]
static bool PropStanding(const float* f, const float* T)
{
    if (f[12] != T[0] || f[13] != T[1] || f[14] != T[2] || f[15] != 1.0f) return false;
    for (int k = 16; k < 24; ++k) if (f[k] != 0.0f) return false;
    unsigned fl = U32(f, 47);   // standing: not hit (other prop kinds have other flag bits)
    if ((fl & 0x800) || !(fl & 0x400) || (fl & 0xFF)) return false;
    float r1 = f[4] * f[4] + f[5] * f[5] + f[6] * f[6];
    return r1 > 0.9f && r1 < 1.1f;
}
// Other kinds of props may use other flag bits: a prop found by its matrix alone ("loose", only inside the memory the
// game keeps its props in, standing still) is judged by its position and its up axis only.
static bool PropLooseStanding(const float* f, const float* T)
{
    if (f[12] != T[0] || f[13] != T[1] || f[14] != T[2] || f[15] != 1.0f) return false;
    for (int k = 16; k < 24; ++k) if (f[k] != 0.0f) return false;   // standing still (render copies hold pointers there)
    for (int r = 0; r < 3; ++r) {
        float l = f[r * 4] * f[r * 4] + f[r * 4 + 1] * f[r * 4 + 1] + f[r * 4 + 2] * f[r * 4 + 2];
        if (!(l > 0.98f && l < 1.02f)) return false;
    }
    float d01 = f[0] * f[4] + f[1] * f[5] + f[2] * f[6], d12 = f[4] * f[8] + f[5] * f[9] + f[6] * f[10];
    return fabsf(d01) < 0.02f && fabsf(d12) < 0.02f;   // a rotation
}

// still the same prop (standing or hit): the pattern holds and it is near where it stood
static bool PropAlive(const float* f, const float* T, bool loose)
{
    if (f[15] != 1.0f) return false;
    unsigned fl = U32(f, 47);
    if (!loose && (!(fl & 0x400) || (fl & 0xFF))) return false;
    for (int r = 0; r < 3; ++r) {
        float l = f[r * 4] * f[r * 4] + f[r * 4 + 1] * f[r * 4 + 1] + f[r * 4 + 2] * f[r * 4 + 2];
        if (!(l > 0.8f && l < 1.2f)) return false;
    }
    float dx = f[12] - T[0], dy = f[13] - T[1], dz = f[14] - T[2];
    return dx * dx + dy * dy + dz * dz < 40.0f * 40.0f;
}
static bool PropHit(const float* f, const float* T, bool loose)
{
    float dx = f[12] - T[0], dy = f[13] - T[1], dz = f[14] - T[2];
    if (dx * dx + dy * dy + dz * dz > 0.3f * 0.3f) return true;
    return loose ? f[5] < 0.97f : (U32(f, 47) & 0x800) != 0;   // tipped over / the hit flag
}

// ---- worker: find standing props by position
struct PropFind { float T[3]; unsigned char* addr; int loose; };
static CRITICAL_SECTION g_propLock;
static PropFind g_propReq[256], g_propRes[256];
static int g_propReqN, g_propResN;
static unsigned char* g_propPool[16];   // allocations the game keeps its props in (learnt from exact matches)
static volatile LONG g_propPoolN;
static DWORD g_propLastFull;
static volatile LONG g_propThreadOn, g_propSearches, g_propFound;

static bool InPool(const unsigned char* a)
{
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery(a, &mi, sizeof(mi)) != sizeof(mi)) return false;
    for (int i = 0; i < g_propPoolN; ++i) if (g_propPool[i] == (unsigned char*)mi.AllocationBase) return true;
    return false;
}

static void PropScan(unsigned char* base, size_t size, PropFind* q, int n, bool* found, bool pool)
{
    __try {
        const unsigned* u = (const unsigned*)base;
        size_t cnt = size / 4;
        unsigned lo = 0xFFFFFFFFu, hi = 0;   // quick reject: outside the range of the x values looked for
        for (int k = 0; k < n; ++k) {
            unsigned x = ((const unsigned*)q[k].T)[0];
            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
        for (size_t i = 12; i + 48 < cnt; ++i) {
            if (u[i] - lo > hi - lo) continue;
            for (int k = 0; k < n; ++k) {
                if (found[k]) continue;
                const unsigned* t = (const unsigned*)q[k].T;
                if (u[i] != t[0] || u[i + 1] != t[1] || u[i + 2] != t[2]) continue;
                float f[60];
                unsigned char* a = (unsigned char*)(u + i);
                if (!ReadProp(a, f)) continue;
                if (PropStanding(f, q[k].T)) { q[k].addr = a; q[k].loose = 0; found[k] = true; }
                else if (pool && PropLooseStanding(f, q[k].T)) { q[k].addr = a; q[k].loose = 1; found[k] = true; }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void PropLearnPool(unsigned char* a)
{
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery(a, &mi, sizeof(mi)) != sizeof(mi)) return;
    unsigned char* ab = (unsigned char*)mi.AllocationBase;
    for (int i = 0; i < g_propPoolN; ++i) if (g_propPool[i] == ab) return;
    int slot = g_propPoolN < 16 ? (int)InterlockedIncrement(&g_propPoolN) - 1 : (int)(GetTickCount() % 16);
    g_propPool[slot] = ab;
    Log("props: the game keeps props in the memory allocation at %p", ab);
}

// every committed read/write region of the allocation at ab
static void ScanAllocation(unsigned char* ab, PropFind* q, int n, bool* found)
{
    MEMORY_BASIC_INFORMATION mi;
    unsigned char* a = ab;
    while (VirtualQuery(a, &mi, sizeof(mi)) == sizeof(mi) && mi.AllocationBase == ab) {
        if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(mi.Protect & PAGE_GUARD))
            PropScan((unsigned char*)mi.BaseAddress, mi.RegionSize, q, n, found, true);
        a = (unsigned char*)mi.BaseAddress + mi.RegionSize;
    }
}

static DWORD WINAPI PropThread(void*)
{
    static PropFind q[256];
    for (;;) {
        Sleep(400);
        int n;
        EnterCriticalSection(&g_propLock);
        n = g_propReqN;
        memcpy(q, g_propReq, n * sizeof(PropFind));
        g_propReqN = 0;
        LeaveCriticalSection(&g_propLock);
        if (!n) continue;
        InterlockedIncrement(&g_propSearches);
        bool found[256] = {};
        for (int k = 0; k < n; ++k) { q[k].addr = 0; q[k].loose = 0; }
        // the allocations props were found in before (fast)
        for (int r = 0; r < g_propPoolN; ++r) ScanAllocation(g_propPool[r], q, n, found);
        int left = 0;
        for (int k = 0; k < n; ++k) if (!found[k]) ++left;
        // everything (slow), at most every 15 s: exact matches teach where the pool is
        if (left && GetTickCount() - g_propLastFull > 15000) {
            g_propLastFull = GetTickCount();
            unsigned char* a = 0;
            HMODULE self = 0;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&PropThread, &self);
            MEMORY_BASIC_INFORMATION mi;
            while ((size_t)a < 0xFFFF0000u && VirtualQuery(a, &mi, sizeof(mi)) == sizeof(mi)) {
                unsigned char* next = (unsigned char*)mi.BaseAddress + mi.RegionSize;
                if (mi.State == MEM_COMMIT && mi.Type == MEM_PRIVATE && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
                    !(mi.Protect & PAGE_GUARD) && (HMODULE)mi.AllocationBase != self)
                    PropScan((unsigned char*)mi.BaseAddress, mi.RegionSize, q, n, found, InPool((unsigned char*)mi.BaseAddress));
                if (next <= a) break;
                a = next;
            }
            for (int k = 0; k < n; ++k) if (found[k] && !q[k].loose) PropLearnPool(q[k].addr);
            // with a pool known now, the rest may be loose matches in it
            bool again = false;
            for (int k = 0; k < n; ++k) if (!found[k]) again = true;
            if (again) for (int r = 0; r < g_propPoolN; ++r) ScanAllocation(g_propPool[r], q, n, found);
        }
        EnterCriticalSection(&g_propLock);
        for (int k = 0; k < n; ++k)
            if (found[k] && g_propResN < 256) {
                g_propRes[g_propResN++] = q[k];
                if (!q[k].loose) PropLearnPool(q[k].addr);
                InterlockedIncrement(&g_propFound);
            }
        LeaveCriticalSection(&g_propLock);
    }
}

static void PropRequest(const float* T)
{
    if (!InterlockedExchange(&g_propThreadOn, 1)) {
        InitializeCriticalSection(&g_propLock);
        CloseHandle(CreateThread(0, 0, PropThread, 0, 0, 0));
    }
    EnterCriticalSection(&g_propLock);
    bool dup = false;
    for (int i = 0; i < g_propReqN && !dup; ++i) dup = !memcmp(g_propReq[i].T, T, 12);
    if (!dup && g_propReqN < 256) { memcpy(g_propReq[g_propReqN].T, T, 12); g_propReq[g_propReqN].addr = 0; ++g_propReqN; }
    LeaveCriticalSection(&g_propLock);
}

// ---- every frame (render thread): results in, every tied lamp near the camera checked
static void PropTick()
{
    DWORD now = GetTickCount();
    if (g_propThreadOn) {
        EnterCriticalSection(&g_propLock);
        for (int r = 0; r < g_propResN; ++r)
            for (int i = 0; i < g_mapCount; ++i) {
                MapLamp& L = g_map[i];
                if (!L.hasObj || L.obj[0] != g_propRes[r].T[0] || L.obj[1] != g_propRes[r].T[1] || L.obj[2] != g_propRes[r].T[2]) continue;
                L.propAddr = g_propRes[r].addr;
                L.propLoose = g_propRes[r].loose;
                if (L.propState != PS_LIT) {
                    float f[60];
                    if (L.propLoose && ReadProp(L.propAddr, f))
                        Log("props: lamp %d tied to its prop at %p (standing, by its matrix; flags %08x %08x)", i + 1, L.propAddr, U32(f, 47), U32(f, 55));
                    else Log("props: lamp %d tied to its prop at %p (standing)", i + 1, L.propAddr);
                }
                L.propState = PS_LIT;
            }
        g_propResN = 0;
        LeaveCriticalSection(&g_propLock);
    }
    for (int i = 0; i < g_mapCount; ++i) {
        MapLamp& L = g_map[i];
        if (!L.hasObj) { L.propState = PS_NONE; L.propFade = 1; continue; }
        float farM = g_p[P_LAMP_REACH].v + g_p[P_LAMP_RADIUS].v + 50;
        if (farM < 300) farM = 300;
        if (g_camKnown && Dist2(L.obj, g_cam[3]) > farM * farM) continue;   // far away: left as it is
        float f[60];
        if (L.propAddr) {
            if (ReadProp(L.propAddr, f) && PropAlive(f, L.obj, L.propLoose != 0)) {
                if (!PropHit(f, L.obj, L.propLoose != 0)) {
                    // standing: where the lamp sits in the prop's own axes (it may have been moved in the editor)
                    float d[3] = { L.p[0] - f[12], L.p[1] - f[13], L.p[2] - f[14] };
                    for (int r = 0; r < 3; ++r) L.propLocal[r] = d[0] * f[r * 4] + d[1] * f[r * 4 + 1] + d[2] * f[r * 4 + 2];
                    if (L.propState == PS_KNOCKED || L.propState == PS_GONE) Log("props: lamp %d: its prop stands again", i + 1);
                    L.propState = PS_LIT;
                } else {
                    if (L.propState == PS_LIT) { L.propFlicker = now + 450; Log("props: lamp %d: its prop was knocked down", i + 1); }
                    L.propState = PS_KNOCKED;
                }
                memcpy(L.propM, f, 16 * sizeof(float));
                continue;
            }
            // the instance is gone (streamed out, or a hit prop removed): look for it standing again
            L.propAddr = 0;
            if (L.propState == PS_KNOCKED) L.propState = PS_GONE;
            else if (L.propState != PS_GONE) L.propState = PS_SEARCH;
        }
        if (L.propState == PS_NONE) L.propState = PS_SEARCH;
        if ((LONG)(now - L.propNext) >= 0) { L.propNext = now + 2000; PropRequest(L.obj); }
    }
}

// shown power (0..1) and where the light is, for a tied map lamp
static float PropLight(MapLamp& L, float* pos)
{
    memcpy(pos, L.p, 12);
    if (!L.hasObj) return 1;
    DWORD now = GetTickCount();
    bool follow = g_p[P_KNOCK].v > 0.5f;
    float target = 1;
    if (L.propState == PS_KNOCKED) {
        if (follow) {   // the light falls with the post
            const float* f = L.propM;
            for (int j = 0; j < 3; ++j) pos[j] = f[12 + j] + L.propLocal[0] * f[j] + L.propLocal[1] * f[4 + j] + L.propLocal[2] * f[8 + j];
        } else target = 0;
    } else if (L.propState == PS_GONE) target = follow ? 1.0f : 0.0f;
    L.propFade += (target - L.propFade) * 0.25f;
    float k = L.propFade;
    if ((LONG)(L.propFlicker - now) > 0) k *= ((now / 40) % 3) ? 1.0f : 0.15f;   // the bulb flickers as the post goes down
    return k;
}
