// ============================================================================
// cfgvar.h - lightweight settings that are NOT registered with the engine.
//
// The engine's console autocomplete on this old build crashes when it walks a
// large number of plugin-registered ConVars (and leaves dangling entries after
// plugin_unload). So instead of ~30 ConVars we keep the tunables as CfgVars -
// plain name/value holders that live only in the plugin and are read/written
// through our single "insbot" console command. CfgVar mimics ConVar's getter
// surface so existing call sites (insbot_fov.GetFloat(), etc.) don't change.
// ============================================================================
#pragma once

class CfgVar
{
public:
    CfgVar(const char *name, const char *defaultValue, const char *help);

    const char *Name() const { return m_name; }
    const char *Help() const { return m_help; }

    const char *GetString() const { return m_value; }
    float       GetFloat() const;
    int         GetInt() const;
    bool        GetBool() const { return GetFloat() != 0.0f; }

    void        SetValue(const char *v);

    CfgVar     *m_next;
    static CfgVar *s_head;

private:
    const char *m_name;
    const char *m_help;
    char        m_value[256];
};

// find a CfgVar by name (case-insensitive), or null
CfgVar *CfgFind(const char *name);
