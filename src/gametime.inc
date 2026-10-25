// ---------------------------------------------------------------- 1.96b: the game's time of day
// The game steps its clock in one place (NFS13.exe 0x8B4C0A..0x8B4C30): its environment object (esi) holds the time at
// +0x2A84 (seconds of a 86400 s day: 0 midnight, 50000 its midday; HD Effects' F8 sets these) and its speed at +0x2A90;
// the step is stored by "movss [esi+0x2A84], xmm0" at 0x8B4C28. There a jump to ours keeps esi and does the same store
// (HD Effects hooks 0x8B4C0A and 0x8B4C1A, not this one). Only when those 8 bytes are the expected ones (another exe,
// another mod there: no time control). The quick bench (bench.inc) sets the time for its day and night.
static void* volatile g_todObj;           // the environment object (0: the clock not stepped yet)
static void* g_todRet = (void*)0x8B4C30;
static bool g_todHooked;
static __declspec(naked) void TodCave()
{
    __asm {
        mov g_todObj, esi
        movss dword ptr [esi + 0x2A84], xmm0
        jmp dword ptr [g_todRet]
    }
}
// at the device's making (the game's loop not running yet)
static void TodInstall()
{
    static const unsigned char want[8] = { 0xF3, 0x0F, 0x11, 0x86, 0x84, 0x2A, 0x00, 0x00 };
    unsigned char* at = (unsigned char*)0x8B4C28;
    MEMORY_BASIC_INFORMATION mi;
    if (g_todHooked || !VirtualQuery(at, &mi, sizeof(mi)) || mi.State != MEM_COMMIT || !(mi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
        if (!g_todHooked) Log("game time: its code not found (another exe?): the bench keeps the time as it is");
        return;
    }
    if (memcmp(at, want, 8)) {
        Log("game time: its clock's step is not as expected (%02x %02x %02x %02x %02x: another mod there?): the bench keeps the time as it is",
            at[0], at[1], at[2], at[3], at[4]);
        return;
    }
    DWORD old;
    VirtualProtect(at, 8, PAGE_EXECUTE_READWRITE, &old);
    at[5] = at[6] = at[7] = 0x90;
    *(long*)(at + 1) = (long)((unsigned char*)TodCave - (at + 5));
    at[0] = 0xE9;
    VirtualProtect(at, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 8);
    g_todHooked = true;
    Log("game time: its clock hooked (0x8B4C28)");
}
static bool TodKnown() { return g_todHooked && g_todObj; }
static float TodGet() { return TodKnown() ? *(float*)((char*)g_todObj + 0x2A84) : -1.0f; }
static void TodSet(float t)
{
    if (!TodKnown()) return;
    t -= 86400.0f * floorf(t / 86400.0f);
    *(float*)((char*)g_todObj + 0x2A84) = t;
}
