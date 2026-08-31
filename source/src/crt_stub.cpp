// ============================================================================
// crt_stub.cpp - tiny CRT replacement so the DLL needs only msvcrt.dll
// ============================================================================
#include "crtshim.h"

// ---- C++ new/delete map straight onto the msvcrt heap ----------------------
// (This is the known-good allocator from v0.5.0. A previous attempt to share
//  the engine heap via tier0's g_pMemAlloc export broke loading because INS:MIC's
//  tier0.dll does not export that symbol - reverted.)
void* __cdecl operator new(size_t n)                { return malloc(n ? n : 1); }
void* __cdecl operator new[](size_t n)              { return malloc(n ? n : 1); }
void  __cdecl operator delete(void* p)              { if (p) free(p); }
void  __cdecl operator delete[](void* p)            { if (p) free(p); }
void  __cdecl operator delete(void* p, size_t)      { if (p) free(p); }
void  __cdecl operator delete[](void* p, size_t)    { if (p) free(p); }
extern "C" int __cdecl _purecall(void)              { return 0; }

extern "C" {
__declspec(naked) float __cdecl cosf(float x)       { __asm fld dword ptr [esp+4] __asm fcos __asm ret }
__declspec(naked) float __cdecl sinf(float x)       { __asm fld dword ptr [esp+4] __asm fsin __asm ret }
__declspec(naked) float __cdecl sqrtf(float x)      { __asm fld dword ptr [esp+4] __asm fsqrt __asm ret }
__declspec(naked) float __cdecl fabsf(float x)      { __asm fld dword ptr [esp+4] __asm fabs __asm ret }
__declspec(naked) float __cdecl atan2f(float y, float x) { __asm fld dword ptr [esp+4] __asm fld dword ptr [esp+8] __asm fpatan __asm ret }
__declspec(naked) float __cdecl atanf(float x)      { __asm fld dword ptr [esp+4] __asm fld1 __asm fpatan __asm ret }

float __cdecl fmodf(float x, float y)               { return (float)fmod((double)x, (double)y); }
float __cdecl acosf(float x)                        { return (float)acos((double)x); }
float __cdecl asinf(float x)                        { return (float)asin((double)x); }
float __cdecl tanf(float x)                         { return (float)tan((double)x); }
float __cdecl powf(float x, float y)                { return (float)pow((double)x, (double)y); }
float __cdecl expf(float x)                         { return (float)exp((double)x); }
float __cdecl logf(float x)                         { return (float)log((double)x); }
float __cdecl floorf(float x)                       { return (float)floor((double)x); }
float __cdecl ceilf(float x)                        { return (float)ceil((double)x); }
}

// MSVC ABI expects this marker when floating point is used with /nodefaultlib
extern "C" int _fltused = 1;

// Dynamic initializers register destructors through atexit; our DLL lives for
// the whole server session, so tearing down globals at exit is unnecessary
// (and unsafe after unload). Swallow the registrations.
extern "C" int __cdecl _swallow_atexit(void (__cdecl*)(void)) { return 0; }
// note: crtshim.h declares atexit; alias it here
extern "C" int __cdecl atexit(void (__cdecl* f)(void)) { return _swallow_atexit(f); }

// ---- static constructor table ----------------------------------------------
// clang-cl drops pointers to dynamic initializers into .CRT$XCU. We provide
// the bracketing sections and walk the table ourselves on DLL attach.
typedef void(__cdecl* _PVFV)(void);
#pragma section(".CRT$XCA", read)
#pragma section(".CRT$XCZ", read)
__declspec(allocate(".CRT$XCA")) static _PVFV __xc_a[] = { 0 };
__declspec(allocate(".CRT$XCZ")) static _PVFV __xc_z[] = { 0 };
#pragma comment(linker, "/merge:.CRT=.rdata")

static void RunStaticConstructors()
{
    for (_PVFV* f = __xc_a; f < __xc_z; ++f)
        if (*f) (*f)();
}

// ---- DLL entry --------------------------------------------------------------
#define DLL_PROCESS_ATTACH 1
extern "C" int __stdcall _DllMainCRTStartup(void* hinst, unsigned long reason, void* reserved)
{
    (void)hinst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        RunStaticConstructors();
    return 1;
}

// ---- __chkstk: stack probe helper (needed by alloca in SDK code) ------------
// Faithful port of the classic MSVC chkstk.asm for x86.
extern "C" __declspec(naked) void _chkstk(void)
{
    __asm {
        push    ecx
        cmp     eax, 1000h
        lea     ecx, [esp + 8]         // original esp (above saved ecx + ret addr)
        jb      lastpage
probepages:
        sub     ecx, 1000h
        sub     eax, 1000h
        test    dword ptr [ecx], eax   // touch the page
        cmp     eax, 1000h
        jae     probepages
lastpage:
        sub     ecx, eax
        mov     eax, esp               // eax -> saved ecx / ret addr
        test    dword ptr [ecx], eax   // touch final page
        mov     esp, ecx               // commit new stack pointer
        mov     ecx, dword ptr [eax]   // restore ecx
        mov     eax, dword ptr [eax + 4] // fetch return address
        push    eax
        ret
    }
}

extern "C" __declspec(naked) void _alloca_probe_16(void)
{
    __asm {
        push    ecx
        lea     ecx, [esp + 8]
        sub     ecx, eax
        and     ecx, 0FFFFFFF0h
        mov     eax, esp
        test    dword ptr [ecx], eax
        mov     esp, ecx
        mov     ecx, dword ptr [eax]
        mov     eax, dword ptr [eax + 4]
        push    eax
        ret
    }
}
extern "C" __declspec(naked) void __alloca_probe_16(void)
{
    __asm jmp _alloca_probe_16
}

// ---- aligned malloc (msvcrt.dll predates _aligned_malloc) -------------------
extern "C" void* __cdecl _aligned_malloc(size_t size, size_t align)
{
    if (align < sizeof(void*)) align = sizeof(void*);
    unsigned char* raw = (unsigned char*)malloc(size + align + sizeof(void*));
    if (!raw) return 0;
    unsigned char* user = (unsigned char*)(((size_t)(raw + sizeof(void*) + align - 1)) & ~(align - 1));
    ((void**)user)[-1] = raw;
    return user;
}
extern "C" void __cdecl _aligned_free(void* p)
{
    if (p) free(((void**)p)[-1]);
}
