// ============================================================================
// cfgvar.cpp - see cfgvar.h
// ============================================================================
#include <string.h>
#include <stdlib.h>

#include "cfgvar.h"

CfgVar *CfgVar::s_head = 0;

CfgVar::CfgVar(const char *name, const char *defaultValue, const char *help)
    : m_next(0), m_name(name), m_help(help ? help : "")
{
    m_value[0] = 0;
    if (defaultValue)
    {
        strncpy(m_value, defaultValue, sizeof(m_value) - 1);
        m_value[sizeof(m_value) - 1] = 0;
    }
    // link into the global list (plugin-local; never touches the engine)
    m_next = s_head;
    s_head = this;
}

float CfgVar::GetFloat() const
{
    return (float)atof(m_value);
}

int CfgVar::GetInt() const
{
    return atoi(m_value);
}

void CfgVar::SetValue(const char *v)
{
    if (!v) v = "";
    strncpy(m_value, v, sizeof(m_value) - 1);
    m_value[sizeof(m_value) - 1] = 0;
}

CfgVar *CfgFind(const char *name)
{
    if (!name) return 0;
    for (CfgVar *c = CfgVar::s_head; c; c = c->m_next)
        if (_stricmp(c->Name(), name) == 0)
            return c;
    return 0;
}
