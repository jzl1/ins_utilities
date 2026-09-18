#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "voice_fix.h"

// ---------------------------------------------------------------------------
// call-site guard
// ---------------------------------------------------------------------------
// The thunk writes the clamped value back into the 4 bytes in front of the
// bf_read (= msg->m_nLength), because the handler reads that same field again
// to size the broadcast.  Writing to the `this-4` of some *other* ReadBits
// caller would corrupt live engine memory, so a candidate is only patched if
// the surrounding code proves the field is really that length:
//
//   (a) the nBits handed to ReadBits was loaded from [reg+disp-4]:
//         mov eax,[reg+disp-4] ... lea <r32>,[reg+disp] (the bf_read) ...
//         call ReadBits
//   (b) the handler turns that same field into a byte count somewhere:
//         mov r,[reg+disp-4] ... sar r,3          (the (n+7)/8 idiom)
//
// The bf_read pointer is recognised as the last `lea r32,[base+disp8]` before
// the call that does not use esp as its base - which handles both ABIs: on
// Windows it is `lea ecx,[reg+disp]` right before the thiscall, on Linux it is
// `lea ecx,[reg+disp]` followed by `mov [esp],ecx`.
//
// Both shipped voice handlers satisfy this - server CGameClient::ProcessVoiceData
// (field at msg+0x10, bf_read at msg+0x14) and client CClientState::ProcessVoiceData
// (field at msg+0x14, bf_read at msg+0x18).  Anything else is refused.
// ---------------------------------------------------------------------------
static const size_t VF_GUARD_BACK = 48;   /* how far the nBits load may sit back */
static const size_t VF_GUARD_FWD  = 48;   /* how far the byte conversion may sit  */

/* Map a VA to an offset inside the scanned region. */
static bool VfCodeOff(const VfCode *c, uintptr_t va, size_t *off)
{
    if (va < c->va || va - c->va >= c->len) return false;
    *off = (size_t)(va - c->va);
    return true;
}

bool VfSiteGuard(const VfCode *c, uintptr_t site, char *why, size_t whyLen)
{
    size_t off, k, field, lea_off = 0;
    unsigned reg = 0, disp = 0;
    bool have_lea = false, ok_a = false, ok_b = false;

    if (!VfCodeOff(c, site - 24, &off)) {
        snprintf(why, whyLen, "call site sits too close to the start of the scanned region");
        return false;
    }

    /* the last `lea r32,[base+disp8]` with base != esp before the call is the
     * bf_read the handler is about to consume */
    for (k = 0; k + 3 <= 24; k++) {
        const unsigned char *b = c->p + off + k;
        unsigned m;
        if (b[0] != 0x8D) continue;
        m = b[1];
        if ((m & 0xC0) != 0x40) continue;       /* mod == 01 -> disp8 form */
        if ((m & 0x07) == 4) continue;          /* [esp+disp8] needs a SIB byte */
        reg = m & 0x07u;
        disp = b[2];
        lea_off = off + k;
        have_lea = true;                        /* keep the last (closest) one */
    }
    if (!have_lea) {
        snprintf(why, whyLen, "no 'lea r32,[reg+disp8]' feeding ReadBits was found before the call");
        return false;
    }
    if (disp < 4) {
        snprintf(why, whyLen, "bf_read is at [reg+0x%02X], nothing in front of it", disp);
        return false;
    }
    field = (size_t)disp - 4;

    /* (a) the nBits that gets handed to ReadBits came out of [reg+field] */
    {
        size_t start = (lea_off > VF_GUARD_BACK) ? lea_off - VF_GUARD_BACK : 0;
        for (k = start; k + 3 <= lea_off; k++) {
            if (c->p[k] == 0x8B && c->p[k + 1] == (unsigned char)(0x40u | reg) &&
                c->p[k + 2] == (unsigned char)field) {
                ok_a = true;
                break;
            }
        }
    }

    /* (b) some load of the same field is turned into a byte count */
    {
        size_t start = (site - c->va > VF_GUARD_BACK + 20) ? (size_t)(site - c->va) - (VF_GUARD_BACK + 20) : 0;
        size_t end   = (size_t)(site - c->va) + 5 + VF_GUARD_FWD + 5;
        if (end > c->len) end = c->len;
        for (k = start; k + 3 <= end; k++) {
            if (c->p[k] != 0x8B) continue;
            if ((c->p[k + 1] & 0xC7u) != (0x40u | reg)) continue;   /* any destination */
            if (c->p[k + 2] != (unsigned char)field) continue;
            size_t lim = (k + 24 <= end) ? 24 : (end - k);
            for (size_t j = 0; j + 3 <= lim; j++) {
                if (c->p[k + j] == 0xC1 && (c->p[k + j + 1] & 0xF8u) == 0xF8 &&
                    c->p[k + j + 2] == 0x03) {                      /* sar r,3 */
                    ok_b = true;
                    break;
                }
            }
            if (ok_b) break;
        }
    }

    if (!ok_a || !ok_b) {
        snprintf(why, whyLen,
                 "(a) nBits loaded from [reg+0x%02X]: %s; (b) that field converted "
                 "to bytes ((n+7)/8): %s",
                 (unsigned)field, ok_a ? "yes" : "no", ok_b ? "yes" : "no");
        return false;
    }
    snprintf(why, whyLen,
             "length field at [bf_read-0x4] (= [reg+0x%02X], bits, (n+7)/8 => bytes)",
             (unsigned)field);
    return true;
}

// ---------------------------------------------------------------------------
// engine discovery
// ---------------------------------------------------------------------------
// ins_utilities' existing Windows helpers walk server.dll; the voice bug lives
// in the *engine* module, so this resolver walks the engine instead.  The
// candidate list is deliberately wider than this game: IMIC, Garry's Mod 9 and
// Source SDK Base 2006 ship the identical engine binary, and NEOTOKYO ships a
// build whose voice code is byte-identical (see
// rce/evidence/engine_family_comparison.txt).
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

static const IMAGE_SECTION_HEADER *g_vfSec = NULL;
static int       g_vfNSec = 0;
static uintptr_t g_vfEngineBase = 0;
static uintptr_t g_vfReadBits = 0;
static char      g_vfEngineName[128];

bool VfResolveEngine(char *engineName, size_t nameLen)
{
    static const char *names[] = { "engine.dll", "engine_srv.dll", "engine2.dll" };
    HMODULE h = NULL;
    for (int i = 0; i < 3 && !h; i++) {
        h = GetModuleHandleA(names[i]);
        if (h) snprintf(g_vfEngineName, sizeof(g_vfEngineName), "%s", names[i]);
    }
    if (!h) {
        snprintf(engineName, nameLen, "engine module is not loaded in this process");
        return false;
    }
    g_vfEngineBase = (uintptr_t)h;

    const unsigned char *b = (const unsigned char *)h;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)h;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        snprintf(engineName, nameLen, "%s is not a valid PE image", g_vfEngineName);
        return false;
    }
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(b + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        snprintf(engineName, nameLen, "%s has no PE signature", g_vfEngineName);
        return false;
    }
    g_vfSec = IMAGE_FIRST_SECTION(nt);
    g_vfNSec = nt->FileHeader.NumberOfSections;
    snprintf(engineName, nameLen, "%s", g_vfEngineName);
    return true;
}

// Audited 19-byte entry of bf_read::ReadBits on the 2009-12-04 engine build.
static uintptr_t VfFindReadBits(void)
{
    static const unsigned char sig[] = {
        0x51, 0x8B, 0x44, 0x24, 0x08, 0xA8, 0x03, 0x53, 0x55,
        0x8B, 0x6C, 0x24, 0x14, 0x56, 0x8B, 0xD8, 0x57, 0x8B, 0xF1
    };
    for (int s = 0; s < g_vfNSec; s++) {
        if (!(g_vfSec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const unsigned char *p = (const unsigned char *)(g_vfEngineBase + g_vfSec[s].VirtualAddress);
        size_t n = g_vfSec[s].Misc.VirtualSize ? g_vfSec[s].Misc.VirtualSize : g_vfSec[s].SizeOfRawData;
        if (n < sizeof(sig)) continue;
        for (size_t i = 0; i + sizeof(sig) <= n; i++) {
            if (memcmp(p + i, sig, sizeof(sig)) == 0) {
                return g_vfEngineBase + g_vfSec[s].VirtualAddress + i;
            }
        }
    }
    return 0;
}

int VfResolveSites(VfSite *sites, int maxSites, uintptr_t readBits, char *why, size_t whyLen)
{
    int found = 0;
    g_vfReadBits = readBits;
    for (int s = 0; s < g_vfNSec && found < maxSites; s++) {
        if (!(g_vfSec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const unsigned char *p = (const unsigned char *)(g_vfEngineBase + g_vfSec[s].VirtualAddress);
        size_t n = g_vfSec[s].Misc.VirtualSize ? g_vfSec[s].Misc.VirtualSize : g_vfSec[s].SizeOfRawData;
        for (size_t i = 0; i + 16 <= n && found < maxSites; i++) {
            if (p[i] != 0x50) continue;                                  // push eax
            if (p[i + 1] != 0x8D || p[i + 2] != 0x4C) continue;          // lea ecx,[esp+X]
            if (p[i + 5] != 0x51) continue;                              // push ecx
            if (p[i + 6] != 0x8D || (p[i + 7] & 0xF8) != 0x48) continue; // lea ecx,[reg+disp8]
            for (size_t j = i + 8; j <= i + 12 && j + 5 <= n; j++) {
                if (p[j] != 0xE8) continue;
                int32_t rel;
                memcpy(&rel, p + j + 1, sizeof(rel));
                uintptr_t site = g_vfEngineBase + g_vfSec[s].VirtualAddress + j;
                if ((uintptr_t)((int64_t)site + 5 + rel) == readBits) {
                    VfCode code;
                    char local[192];
                    code.p = p;
                    code.va = g_vfEngineBase + g_vfSec[s].VirtualAddress;
                    code.len = n;
                    if (VfSiteGuard(&code, site, local, sizeof(local))) {
                        sites[found].va = site;
                        found++;
                        snprintf(why, whyLen, "candidate 0x%08X accepted: %s",
                                 (unsigned)site, local);
                    } else {
                        snprintf(why, whyLen, "candidate 0x%08X REJECTED: %s",
                                 (unsigned)site, local);
                    }
                }
                break;
            }
        }
    }
    return found;
}

uintptr_t VfReadBitsAddress(void)
{
    if (!g_vfReadBits) g_vfReadBits = VfFindReadBits();
    return g_vfReadBits;
}

#else  /* ------------------------------------------------------------- Linux */
#  include <dlfcn.h>
#  include <link.h>

static uintptr_t g_vfReadBits = 0;
static VfSite    g_vfPendingSites[VF_MAX_SITES];
static char      g_vfEngineName[128];

static uintptr_t VfFindCallTo(uintptr_t fn, size_t len, uintptr_t target)
{
    const unsigned char *p = (const unsigned char *)fn;
    for (size_t i = 0; i + 5 <= len; i++) {
        if (p[i] != 0xE8) continue;
        int32_t rel;
        memcpy(&rel, p + i + 1, sizeof(rel));
        if ((uintptr_t)((int64_t)fn + (int64_t)i + 5 + rel) == target) {
            return fn + i;
        }
    }
    return 0;
}

struct VfFindCtx { int found; };

static int VfFindCallback(struct dl_phdr_info *info, size_t, void *data)
{
    VfFindCtx *ctx = (VfFindCtx *)data;
    const char *name = info->dlpi_name;
    if (!name || !*name) return 0;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (strncmp(base, "engine_", 7) != 0 || !strstr(base, ".so")) return 0;

    void *h = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
    if (!h) return 0;
    void *rb = dlsym(h, "_ZN7bf_read8ReadBitsEPvi");
    void *vf = dlsym(h, "_ZN11CGameClient16ProcessVoiceDataEP13CLC_VoiceData");
    if (!rb || !vf) {
        dlclose(h);
        return 0;
    }

    uintptr_t site = VfFindCallTo((uintptr_t)vf, 256, (uintptr_t)rb);
    if (site) {
        VfCode code;
        char local[192];
        code.p = (const unsigned char *)vf;
        code.va = (uintptr_t)vf;
        code.len = 256;
        if (VfSiteGuard(&code, site, local, sizeof(local))) {
            snprintf(g_vfEngineName, sizeof(g_vfEngineName), "%s", base);
            g_vfReadBits = (uintptr_t)rb;
            g_vfPendingSites[0].va = site;
            ctx->found = 1;
            dlclose(h);
            return 1;
        }
        dlclose(h);
        return 0;
    }
    dlclose(h);
    return 0;
}

bool VfResolveEngine(char *engineName, size_t nameLen)
{
    VfFindCtx ctx;
    ctx.found = 0;
    dl_iterate_phdr(VfFindCallback, &ctx);
    if (!ctx.found) {
        snprintf(engineName, nameLen, "no engine_i*.so with the expected symbols/handler found");
        return false;
    }
    snprintf(engineName, nameLen, "%s", g_vfEngineName);
    return true;
}

int VfResolveSites(VfSite *sites, int maxSites, uintptr_t readBits, char *why, size_t whyLen)
{
    (void)maxSites;
    if (!g_vfPendingSites[0].va) {
        snprintf(why, whyLen, "no call site located during engine discovery");
        return 0;
    }
    sites[0] = g_vfPendingSites[0];
    snprintf(why, whyLen, "call site 0x%08X accepted during engine discovery",
             (unsigned)g_vfPendingSites[0].va);
    return 1;
}

uintptr_t VfReadBitsAddress(void) { return g_vfReadBits; }
#endif

// ---------------------------------------------------------------------------
// the patch
// ---------------------------------------------------------------------------
// Excluded from the host unit test build (VOICE_FIX_UNIT_TEST), which compiles
// this file without the HL2SDK on the include path.
#ifndef VOICE_FIX_UNIT_TEST

#include <stdlib.h>
#include "tier0/dbg.h"
#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <unistd.h>
#  include <errno.h>
#endif

// g_vfEngineBase / g_vfEngineName / g_vfReadBits already exist from the
// resolver above - do NOT redefine them here (one translation unit).
static VfSite   g_vfSites[VF_MAX_SITES];
static int      g_vfSiteCount = 0;
static void    *g_vfThunk = NULL;
static size_t   g_vfThunkSize = 0;
static int      g_vfActive = 0;

static volatile uint32_t g_vfClampCount = 0;
static volatile uint32_t g_vfLastBits = 0;
static uint32_t g_vfReported = 0;

void VoiceFixRemove(void);

static bool VfRel32Ok(uintptr_t from, uintptr_t target)
{
    int64_t d = (int64_t)target - (int64_t)(from + 5);
    return d >= -2147483648LL && d <= 2147483647LL;
}

static bool VfPatchCode(void *dst, const void *src, size_t len)
{
#if defined(_WIN32)
    DWORD oldProt = 0;
    if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    memcpy(dst, src, len);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    VirtualProtect(dst, len, oldProt, &oldProt);
    return true;
#else
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    uintptr_t start = (uintptr_t)dst & ~(uintptr_t)(ps - 1);
    uintptr_t end = ((uintptr_t)dst + len + ps - 1) & ~(uintptr_t)(ps - 1);
    if (mprotect((void *)start, end - start, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return false;
    memcpy(dst, src, len);
    __builtin___clear_cache((char *)dst, (char *)dst + len);
    mprotect((void *)start, end - start, PROT_READ | PROT_EXEC);
    return true;
#endif
}

// Executable scratch memory within +-2GB of both `anchor` (the call site) and
// bf_read::ReadBits - both jumps are rel32.
static void *VfAllocExecNear(uintptr_t anchor, size_t size)
{
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    for (uintptr_t delta = 0x100000; delta < 0x40000000u; delta += 0x1000000u) {
        for (int dir = 0; dir < 2; dir++) {
            uintptr_t cand = dir ? (anchor + delta) : (anchor - delta);
            cand &= ~(gran - 1);
            if (cand < 0x10000) continue;
            if (!VfRel32Ok(cand, VfReadBitsAddress()) || !VfRel32Ok(anchor, cand)) continue;
            void *p = VirtualAlloc((LPVOID)cand, size, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    }
    return NULL;
#else
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    for (uintptr_t delta = 4u << 20; delta < 0x40000000u; delta += (16u << 20)) {
        for (int dir = 0; dir < 2; dir++) {
            uintptr_t cand = dir ? (anchor + delta) : (anchor - delta);
            cand &= ~(uintptr_t)(ps - 1);
            if (cand < (uintptr_t)ps) continue;
            if (!VfRel32Ok(cand, VfReadBitsAddress()) || !VfRel32Ok(anchor, cand)) continue;
            void *p;
#ifdef MAP_FIXED_NOREPLACE
            p = mmap((void *)cand, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p != MAP_FAILED && (uintptr_t)p == cand) return p;
            if (p != MAP_FAILED) munmap(p, size);
#endif
            p = mmap((void *)cand, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p != MAP_FAILED && VfRel32Ok((uintptr_t)p, VfReadBitsAddress())
                && VfRel32Ok(anchor, (uintptr_t)p)) {
                return p;
            }
            if (p != MAP_FAILED) munmap(p, size);
        }
    }
    return NULL;
#endif
}

static void VfFreeExec(void *p, size_t size)
{
#if defined(_WIN32)
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
#endif
}

// The thunk is written while the page is writable; drop write permission (and
// on Linux gain execute permission) before anything can jump into it.
static bool VfProtectExec(void *p, size_t size)
{
#if defined(_WIN32)
    DWORD oldProt = 0;
    return VirtualProtect(p, size, PAGE_EXECUTE_READ, &oldProt) != 0;
#else
    return mprotect(p, size, PROT_READ | PROT_EXEC) == 0;
#endif
}

bool VoiceFixInstall(void)
{
    if (g_vfActive) return true;

    char engineName[128] = {0};
    if (!VfResolveEngine(engineName, sizeof(engineName))) {
        Msg("[ins_utilities] voice_fix unavailable: %s\n", engineName);
        return false;
    }
    Msg("[ins_utilities] voice_fix engine: %s\n", engineName);

    uintptr_t readBits = VfReadBitsAddress();
    if (!readBits) {
        Msg("[ins_utilities] voice_fix unavailable: could not locate bf_read::ReadBits\n");
        return false;
    }

    char why[192] = {0};
    g_vfSiteCount = VfResolveSites(g_vfSites, VF_MAX_SITES, readBits, why, sizeof(why));
    if (g_vfSiteCount <= 0) {
        Msg("[ins_utilities] voice_fix unavailable: %s\n",
            why[0] ? why : "no voice call site found");
        return false;
    }

    for (int i = 0; i < g_vfSiteCount; i++) {
        memcpy(g_vfSites[i].orig, (const void *)g_vfSites[i].va, sizeof(g_vfSites[i].orig));
        Msg("[ins_utilities] voice_fix call site 0x%08X orig %02X %02X %02X %02X %02X\n",
            (unsigned)g_vfSites[i].va,
            g_vfSites[i].orig[0], g_vfSites[i].orig[1], g_vfSites[i].orig[2],
            g_vfSites[i].orig[3], g_vfSites[i].orig[4]);
    }

#if defined(_WIN32)
    size_t page = 4096;
#else
    long ps = sysconf(_SC_PAGESIZE);
    size_t page = (ps > 0) ? (size_t)ps : 4096;
#endif
    g_vfThunk = VfAllocExecNear(g_vfSites[0].va, page);
    if (!g_vfThunk) {
        Msg("[ins_utilities] voice_fix unavailable: cannot reserve executable memory within +-2GB of the engine\n");
        return false;
    }
    g_vfThunkSize = page;

    size_t len = VfBuildThunk((unsigned char *)g_vfThunk, readBits,
                              (uintptr_t)&g_vfClampCount, (uintptr_t)&g_vfLastBits);
    if (!VfCheckThunk((const unsigned char *)g_vfThunk, len, (uintptr_t)g_vfThunk, readBits,
                      (uintptr_t)&g_vfClampCount, (uintptr_t)&g_vfLastBits)) {
        Msg("[ins_utilities] voice_fix unavailable: generated thunk did not pass its self-check\n");
        VfFreeExec(g_vfThunk, g_vfThunkSize);
        g_vfThunk = NULL;
        return false;
    }
    if (!VfProtectExec(g_vfThunk, g_vfThunkSize)) {
        Msg("[ins_utilities] voice_fix unavailable: cannot make the thunk executable\n");
        VfFreeExec(g_vfThunk, g_vfThunkSize);
        g_vfThunk = NULL;
        return false;
    }
    Msg("[ins_utilities] voice_fix thunk 0x%08X (self-check OK, RX, clamps > 0x%X bits)\n",
        (unsigned)(uintptr_t)g_vfThunk, (unsigned)VF_MAX_BITS);

    for (int i = 0; i < g_vfSiteCount; i++) {
        int32_t rel = (int32_t)((int64_t)(uintptr_t)g_vfThunk - (int64_t)(g_vfSites[i].va + 5));
        g_vfSites[i].patched[0] = 0xE8;
        memcpy(g_vfSites[i].patched + 1, &rel, sizeof(rel));
        if (!VfPatchCode((void *)g_vfSites[i].va, g_vfSites[i].patched, 5)) {
            Msg("[ins_utilities] voice_fix FAILED: cannot write the call site at 0x%08X\n",
                (unsigned)g_vfSites[i].va);
            VoiceFixRemove();
            return false;
        }
        unsigned char check[5];
        memcpy(check, (const void *)g_vfSites[i].va, sizeof(check));
        if (memcmp(check, g_vfSites[i].patched, sizeof(check)) != 0) {
            Msg("[ins_utilities] voice_fix FAILED: read-back of the patched call site differs\n");
            VoiceFixRemove();
            return false;
        }
    }

    g_vfActive = 1;
    Msg("[ins_utilities] voice_fix active: oversized voice reads clamped to 0x%X bits (%d call site%s)\n",
        (unsigned)VF_MAX_BITS, g_vfSiteCount, g_vfSiteCount == 1 ? "" : "s");
    return true;
}

void VoiceFixRemove(void)
{
    for (int i = 0; i < g_vfSiteCount; i++) {
        if (g_vfSites[i].va) {
            if (VfPatchCode((void *)g_vfSites[i].va, g_vfSites[i].orig, sizeof(g_vfSites[i].orig))) {
                Msg("[ins_utilities] voice_fix call site 0x%08X restored\n",
                    (unsigned)g_vfSites[i].va);
            } else {
                Msg("[ins_utilities] voice_fix WARNING: could not restore call site 0x%08X\n",
                    (unsigned)g_vfSites[i].va);
            }
            g_vfSites[i].va = 0;
        }
    }
    if (g_vfThunk) {
        VfFreeExec(g_vfThunk, g_vfThunkSize);
        g_vfThunk = NULL;
    }
    g_vfSiteCount = 0;
    g_vfActive = 0;
}

bool VoiceFixIsActive(void) { return g_vfActive != 0; }

// Mirrors UpdateVStrncpyGuard(): reconciled once per frame so the cvar can be
// toggled at runtime.  Configs execute after Load(), so the cvar test must not
// live in Load().
void VoiceFixUpdate(bool enabled)
{
    if (enabled) {
        if (!g_vfActive) VoiceFixInstall();
    } else if (g_vfActive) {
        VoiceFixRemove();
        Msg("[ins_utilities] voice_fix removed (voice_fix 0)\n");
    }

    // The thunk only touches two counters; report from here so the hot path
    // does no logging, allocation or syscalls.
    uint32_t count = g_vfClampCount;
    if (count != g_vfReported) {
        Msg("[ins_utilities] voice_fix blocked %u oversized voice read%s "
            "(last request: 0x%X bits -> clamped to 0x%X)\n",
            (unsigned)(count - g_vfReported), (count - g_vfReported) == 1 ? "" : "s",
            (unsigned)g_vfLastBits, (unsigned)VF_MAX_BITS);
        g_vfReported = count;
    }
}

#endif /* !VOICE_FIX_UNIT_TEST */
