#pragma once
#include "crtshim.h"
void* __cdecl operator new(size_t);
void* __cdecl operator new[](size_t);
void  __cdecl operator delete(void*);
void  __cdecl operator delete[](void*);
inline void* __cdecl operator new(size_t, void* p) { return p; }
inline void* __cdecl operator new[](size_t, void* p) { return p; }
inline void  __cdecl operator delete(void*, void*) {}
inline void  __cdecl operator delete[](void*, void*) {}
