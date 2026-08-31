// ============================================================================
// sdk_glue.cpp - implementations for SDK symbols we don't pull from libs
//
// 1) mathlib's Fast* function pointers (normally set up by MathLib_Init in
//    mathlib.lib, which we don't link to keep CRT deps minimal)
// 2) tier1 V_* string functions (the headers in this SDK branch use V_ names,
//    but the era-correct vstdlib.dll exports the old extern-C Q_ names, so we
//    implement them directly on the CRT)
// ============================================================================
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>

#include "tier1/strtools.h"   // ensure our definitions match the declared signatures

// ---- mathlib fast-math pointers ---------------------------------------------
static float _glue_sqrtf(float x)  { return sqrtf(x); }
static float _glue_rsqrt(float x)  { return x > 0.0f ? 1.0f / sqrtf(x) : 3.4e38f; }
static void  _glue_sincos(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
static float _glue_cosf(float x)   { return cosf(x); }

extern "C" {
float (*pfSqrt)(float x) = _glue_sqrtf;
float (*pfRSqrt)(float x) = _glue_rsqrt;
float (*pfRSqrtFast)(float x) = _glue_rsqrt;
void  (*pfFastSinCos)(float x, float *s, float *c) = _glue_sincos;
float (*pfFastCos)(float x) = _glue_cosf;
}

// ---- tier1 V_* string functions ---------------------------------------------
// Only the ones the link actually needs; grown as required.
void V_strncpy(char *pDest, const char *pSrc, int maxLen)
{
    if (maxLen <= 0) return;
    strncpy(pDest, pSrc, maxLen);
    pDest[maxLen - 1] = 0;
}

int V_snprintf(char *pDest, int maxLen, const char *pFormat, ...)
{
    if (maxLen <= 0) return 0;
    va_list ap;
    va_start(ap, pFormat);
    int n = _vsnprintf(pDest, maxLen, pFormat, ap);
    va_end(ap);
    if (n < 0 || n >= maxLen) { pDest[maxLen - 1] = 0; n = (int)strlen(pDest); }
    return n;
}

int V_vsnprintf(char *pDest, int maxLen, const char *pFormat, va_list ap)
{
    if (maxLen <= 0) return 0;
    int n = _vsnprintf(pDest, maxLen, pFormat, ap);
    if (n < 0 || n >= maxLen) { pDest[maxLen - 1] = 0; n = (int)strlen(pDest); }
    return n;
}

int V_strcasecmp(const char *s1, const char *s2) { return _stricmp(s1, s2); }
int V_strncasecmp(const char *s1, const char *s2, int n) { return _strnicmp(s1, s2, n); }
int V_strnicmp(const char *s1, const char *s2, int n) { return _strnicmp(s1, s2, n); }
int V_strncmp(const char *s1, const char *s2, int n) { return strncmp(s1, s2, n); }
char *V_strncat(char *pDest, const char *pSrc, size_t destSize, int maxToCopy)
{
    size_t len = strlen(pDest);
    size_t room = destSize - len - 1;
    size_t copy = strlen(pSrc);
    if (maxToCopy >= 0 && (size_t)maxToCopy < copy) copy = maxToCopy;
    if (copy > room) copy = room;
    memcpy(pDest + len, pSrc, copy);
    pDest[len + copy] = 0;
    return pDest;
}
float V_atof(const char *s) { return (float)atof(s); }
int V_atoi(const char *s) { return atoi(s); }
