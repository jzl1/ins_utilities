// ============================================================================
// crtshim.h - minimal MSVC CRT declarations for msvcrt.dll
//
// This build deliberately avoids the Visual C++ runtime and Windows SDK.
// Everything below resolves against msvcrt.dll (present on every Windows
// system) via an import library generated with llvm-dlltool.
// ============================================================================
#pragma once
#ifdef _MSC_VER
#ifndef _SIZE_T_DEFINED
typedef unsigned int size_t;
#define _SIZE_T_DEFINED
#endif
typedef char* va_list_shim;
#else
typedef __SIZE_TYPE__ size_t;
typedef __builtin_va_list va_list_shim;
#endif

#ifndef FLT_MAX
#define FLT_MAX 3.402823466e+38F
#endif
#ifndef FLT_MIN
#define FLT_MIN 1.175494351e-38F
#endif
#ifndef FLT_EPSILON
#define FLT_EPSILON 1.192092896e-07F
#endif

#ifdef _MSC_VER
#ifndef _alloca
extern "C" void* __cdecl _alloca(size_t);
#endif
#else
#ifndef _alloca
#define _alloca(s) __builtin_alloca(s)
#endif
#endif

#ifndef NULL
#ifdef __cplusplus
#define NULL 0
#else
#define NULL ((void*)0)
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- memory / string --------------------------------------------------------
void*  __cdecl malloc(size_t);
void*  __cdecl calloc(size_t, size_t);
void*  __cdecl realloc(void*, size_t);
void   __cdecl free(void*);
void*  __cdecl memcpy(void*, const void*, size_t);
void*  __cdecl memmove(void*, const void*, size_t);
void*  __cdecl memset(void*, int, size_t);
int    __cdecl memcmp(const void*, const void*, size_t);
size_t __cdecl strlen(const char*);
char*  __cdecl strcpy(char*, const char*);
char*  __cdecl strncpy(char*, const char*, size_t);
char*  __cdecl strcat(char*, const char*);
char*  __cdecl strncat(char*, const char*, size_t);
int    __cdecl strcmp(const char*, const char*);
int    __cdecl strncmp(const char*, const char*, size_t);
int    __cdecl _stricmp(const char*, const char*);
int    __cdecl _strnicmp(const char*, const char*, size_t);
char*  __cdecl strstr(const char*, const char*);
char*  __cdecl strchr(const char*, int);
char*  __cdecl strrchr(const char*, int);
char*  __cdecl _strdup(const char*);
char*  __cdecl strtok(char*, const char*);
long   __cdecl strtol(const char*, char**, int);
double __cdecl strtod(const char*, char**);
double __cdecl atof(const char*);
int    __cdecl atoi(const char*);
int    __cdecl tolower(int);
int    __cdecl toupper(int);
int    __cdecl isspace(int);
int    __cdecl isdigit(int);
int    __cdecl isalpha(int);
int    __cdecl isalnum(int);

#define strcmpi _stricmp
#define stricmp _stricmp
#define strnicmp _strnicmp

// ---- stdio ------------------------------------------------------------------
typedef struct _iobuf FILE;   // opaque; only ever handed back to msvcrt
FILE*  __cdecl fopen(const char*, const char*);
int    __cdecl fclose(FILE*);
size_t __cdecl fread(void*, size_t, size_t, FILE*);
size_t __cdecl fwrite(const void*, size_t, size_t, FILE*);
int    __cdecl fseek(FILE*, long, int);
long   __cdecl ftell(FILE*);
int    __cdecl fflush(FILE*);
int    __cdecl fprintf(FILE*, const char*, ...);
char*  __cdecl fgets(char*, int, FILE*);
int    __cdecl remove(const char*);
int    __cdecl printf(const char*, ...);
int    __cdecl sprintf(char*, const char*, ...);
int    __cdecl _snprintf(char*, size_t, const char*, ...);
int    __cdecl vsprintf(char*, const char*, va_list_shim);
int    __cdecl _vsnprintf(char*, size_t, const char*, va_list_shim);
int    __cdecl sscanf(const char*, const char*, ...);
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#ifndef EOF
#define EOF (-1)
#endif
#ifndef BUFSIZ
#define BUFSIZ 512
#endif

// old msvcrt has no snprintf/vsnprintf under the C99 names
#define snprintf  _snprintf
#define vsnprintf _vsnprintf

// ---- math (double versions exist in msvcrt; float wrappers in crt_stub) -----
double __cdecl sin(double);
double __cdecl cos(double);
double __cdecl tan(double);
double __cdecl asin(double);
double __cdecl acos(double);
double __cdecl atan(double);
double __cdecl atan2(double, double);
double __cdecl sqrt(double);
double __cdecl pow(double, double);
double __cdecl exp(double);
double __cdecl log(double);
double __cdecl log10(double);
double __cdecl floor(double);
double __cdecl ceil(double);
double __cdecl fabs(double);
double __cdecl fmod(double, double);

float  __cdecl sinf(float);
float  __cdecl cosf(float);
float  __cdecl tanf(float);
float  __cdecl asinf(float);
float  __cdecl acosf(float);
float  __cdecl atanf(float);
float  __cdecl atan2f(float, float);
float  __cdecl sqrtf(float);
float  __cdecl powf(float, float);
float  __cdecl fmodf(float, float);
float  __cdecl floorf(float);
float  __cdecl ceilf(float);
float  __cdecl fabsf(float);
float  __cdecl logf(float);
float  __cdecl expf(float);

// ---- wide char / case helpers ----------------------------------------------
#ifdef __cplusplus
size_t  __cdecl wcslen(const wchar_t*);
wchar_t* __cdecl wcscpy(wchar_t*, const wchar_t*);
int     __cdecl wcscmp(const wchar_t*, const wchar_t*);
int     __cdecl _wcsicmp(const wchar_t*, const wchar_t*);
wchar_t* __cdecl wcsstr(const wchar_t*, const wchar_t*);
#endif
char*  __cdecl _strupr(char*);
char*  __cdecl _strlwr(char*);
#define strupr _strupr
#define strlwr _strlwr

// ---- misc -------------------------------------------------------------------
void   __cdecl abort(void);
void   __cdecl exit(int);
int    __cdecl rand(void);
void   __cdecl srand(unsigned int);
#define RAND_MAX 0x7fff
typedef long time_t32;
long   __cdecl time(long*);

int    __cdecl atexit(void (__cdecl*)(void));

#ifdef __cplusplus
}
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// aligned allocation (implemented in crt_stub.cpp; old msvcrt lacks these)
#ifdef __cplusplus
extern "C" {
#endif
void* __cdecl _aligned_malloc(size_t size, size_t align);
void  __cdecl _aligned_free(void* p);
#ifdef __cplusplus
}
#endif
