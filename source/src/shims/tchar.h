#pragma once
#include "crtshim.h"
typedef char tchar;
typedef char TCHAR;
#ifndef _T
#define _T(x) x
#endif
#define _vsntprintf _vsnprintf
#define _sntprintf  _snprintf
#define _tcslen strlen
#define _tcscpy strcpy
#define _tcsstr strstr
#define _stprintf sprintf
