#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "tier0/dbg.h"
#include "interface.h"
#include "engine/iserverplugin.h"
#include "eiface.h"
#include "edict.h"
#include "dlls/iplayerinfo.h"
#include "server_class.h"
#include "dt_send.h"
#include "convar.h"
#include "icvar.h"
#include "tier1.h"
#include "cfgvar.h"
#include "iserverentity.h"
#include "datamap.h"
#include "igameevents.h"
#include "linux_offsets.h"
#include "voice_fix.h"
#include "variant_t.h"
#include "engine/IEngineTrace.h"

#define INSUTIL_VERSION_NUM "0.5.12"
#ifdef BUILD_TIMESTAMP
#define INSUTIL_VERSION "0.5.12 (" BUILD_TIMESTAMP ")"
#else
#define INSUTIL_VERSION "0.5.12 (" __DATE__ " " __TIME__ ")"
#endif

#ifdef _LINUX
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>
#include <link.h>
#include <elf.h>

static bool SetMemProtect(void *addr, size_t size, int prot)
{
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) pageSize = 4096;
    uintptr_t start = (uintptr_t)addr;
    uintptr_t base = start & ~((uintptr_t)pageSize - 1);
    size_t span = ((start - base) + size + (size_t)pageSize - 1) & ~((size_t)pageSize - 1);
    return mprotect((void *)base, span, prot) == 0;
}

static bool MakeWritable(void *addr, size_t size, unsigned long *oldProtect)
{
    (void)oldProtect;
    return SetMemProtect(addr, size, PROT_READ | PROT_WRITE);
}

static bool RestoreProtection(void *addr, size_t size, unsigned long oldProtect, bool code)
{
    (void)oldProtect;
    return SetMemProtect(addr, size, code ? (PROT_READ | PROT_EXEC) : PROT_READ);
}
#else
// Forward decls instead of <windows.h> to avoid macro conflicts
extern "C" {
    unsigned long __stdcall VirtualProtect(void *lpAddress, unsigned long dwSize, unsigned long flNewProtect, unsigned long *lpflOldProtect);
    void *__stdcall GetModuleHandleA(const char *lpModuleName);
}
#define PAGE_READWRITE 4
#define PAGE_EXECUTE_READWRITE 0x40

static bool MakeWritable(void *addr, size_t size, unsigned long *oldProtect)
{
    return VirtualProtect(addr, (unsigned long)size, PAGE_READWRITE, oldProtect) != 0;
}

static bool RestoreProtection(void *addr, size_t size, unsigned long oldProtect, bool code)
{
    (void)code;
    return VirtualProtect(addr, (unsigned long)size, oldProtect, NULL) != 0;
}
#endif

#ifndef _LINUX
static unsigned char *FindWindowsPatternInRange(const unsigned char *start,
                                                size_t size,
                                                const unsigned char *pattern,
                                                const char *mask,
                                                size_t patternSize)
{
    if (!start || !pattern || !mask || patternSize == 0 || size < patternSize)
        return NULL;

    for (size_t i = 0; i <= size - patternSize; i++)
    {
        bool match = true;
        for (size_t j = 0; j < patternSize; j++)
        {
            if (mask[j] != '?' && start[i + j] != pattern[j])
            {
                match = false;
                break;
            }
        }
        if (match)
            return (unsigned char *)(start + i);
    }
    return NULL;
}

// Locate a signature in the loaded server.dll .text section without depending
// on the Windows SDK headers. The plugin is built with /nodefaultlib and only
// imports the small Win32 surface declared above.
static bool GetWindowsTextRange(unsigned char **textStart, size_t *textSize)
{
    unsigned char *module = (unsigned char *)GetModuleHandleA("server.dll");
    if (!module || !textStart || !textSize)
        return false;

    unsigned long peOffset = *(unsigned long *)(module + 0x3C);
    unsigned char *nt = module + peOffset;
    if (*(unsigned long *)nt != 0x00004550UL) // PE\0\0
        return false;

    unsigned short sectionCount = *(unsigned short *)(nt + 0x06);
    unsigned short optionalHeaderSize = *(unsigned short *)(nt + 0x14);
    unsigned char *section = nt + 0x18 + optionalHeaderSize;

    for (unsigned short i = 0; i < sectionCount; i++, section += 40)
    {
        if (memcmp(section, ".text", 5) != 0)
            continue;

        unsigned long virtualSize = *(unsigned long *)(section + 0x08);
        unsigned long virtualAddress = *(unsigned long *)(section + 0x0C);
        unsigned long rawSize = *(unsigned long *)(section + 0x10);
        unsigned long scanSize = virtualSize > rawSize ? virtualSize : rawSize;
        if (scanSize == 0)
            return false;

        *textStart = module + virtualAddress;
        *textSize = scanSize;
        return true;
    }
    return false;
}

static unsigned char *FindWindowsTextPattern(const unsigned char *pattern,
                                             const char *mask,
                                             size_t patternSize)
{
    unsigned char *textStart = NULL;
    size_t textSize = 0;
    if (!GetWindowsTextRange(&textStart, &textSize))
        return NULL;

    return FindWindowsPatternInRange(textStart, textSize,
                                     pattern, mask, patternSize);
}

// Older Windows server.dll builds do not contain the CreateGameRulesObject
// sequence used by newer builds. Their mode accessors all begin with the same
// verified g_pGameRules/m_iMode access. Require several matches and require
// every match to agree on the global address before accepting this fallback.
static void **FindWindowsGameRulesGlobalFromModeAccessors()
{
    static const unsigned char pattern[] = {
        0xA1, 0x00, 0x00, 0x00, 0x00,
        0x83, 0xB8, 0xD8, 0x00, 0x00, 0x00, 0x02,
        0x74, 0x00
    };
    static const char mask[] = "x????xxxxxxxx?";

    unsigned char *textStart = NULL;
    size_t textSize = 0;
    if (!GetWindowsTextRange(&textStart, &textSize))
        return NULL;

    unsigned char *cursor = textStart;
    size_t remaining = textSize;
    unsigned long candidate = 0;
    int matches = 0;
    while (remaining >= sizeof(pattern))
    {
        unsigned char *hit = FindWindowsPatternInRange(cursor, remaining,
                                                        pattern, mask,
                                                        sizeof(pattern));
        if (!hit)
            break;

        unsigned long globalAddress = *(unsigned long *)(hit + 1);
        if (!candidate)
            candidate = globalAddress;
        if (globalAddress != candidate)
            return NULL;

        matches++;
        size_t consumed = (size_t)(hit - cursor) + 1;
        cursor = hit + 1;
        remaining -= consumed;
    }

    if (matches < 4 || !candidate)
        return NULL;
    return (void **)candidate;
}
#endif


class CBaseEntity : public IServerEntity
{
public:
    virtual ServerClass* GetServerClass() = 0;
    virtual char const* GetClassName() = 0;
    virtual int YouForgotToImplementOrDeclareServerClass() = 0;
    virtual datamap_t *GetDataDescMap(void) = 0;
};

static typedescription_t *FindDataMapField(datamap_t *pMap, const char *fieldName)
{
    while (pMap)
    {
        for (int i = 0; i < pMap->dataNumFields; i++)
        {
            if (pMap->dataDesc[i].fieldName && strcmp(pMap->dataDesc[i].fieldName, fieldName) == 0)
            {
                return &pMap->dataDesc[i];
            }
        }
        pMap = pMap->baseMap;
    }
    return NULL;
}

extern IServerPluginHelpers *helpers;
extern CGlobalVars          *gpGlobals;
extern IVEngineServer       *engine;
extern ICvar                *g_pCVar;
IPlayerInfoManager          *playerinfomanager = 0;
IEngineTrace                *enginetrace = 0;

CfgVar ins_instant_respawn("instant_respawn", "0", "Toggle instant respawns (1=on, 0=off)");
CfgVar ins_cleanup_weapons("cleanup_weapons", "0", "Automatically remove dropped weapons/items");
CfgVar ins_cleanup_ragdolls("cleanup_ragdolls", "0", "Automatically remove server-side ragdolls");
CfgVar ins_cleanup_blood("cleanup_blood", "0", "Automatically clear blood and bullet decals for all players");
CfgVar ins_cleanup_interval("cleanup_interval", "10.0", "How often to clean the battlefield (seconds)");
CfgVar ins_force_map_switch("force_map_switch", "0", "Forcefully switch to a random map from mapcycle.txt after X seconds");
CfgVar ins_sudden_death_map_switch("sudden_death_map_switch", "0", "Force the next map after X seconds of actual sudden death");
CfgVar ins_force_map_switch_idle("force_map_switch_idle", "0", "Force the next map after X seconds of empty server (0 players and 0 bots)");
CfgVar ins_keep_doors_open("keep_doors_open", "1", "Keep doors opened by players or bots open until explicitly closed via +use (bump-closing disabled; 1=on, 0=off)");
CfgVar ins_mute_killfeed("mute_killfeed", "0", "Mute built-in kill messages (KIA, TK) via vtable hook (1=on, 0=off)");
CfgVar ins_rpg_speed("rpg_speed", "0", "RPG projectile speed in units/sec (0=leave the game's normal speed unchanged)");
CfgVar ins_door_auto_open("door_auto_open", "1", "Auto-open doors on proximity: 0=off, 1=bots only, 2=bots and human players");
CfgVar ins_door_auto_open_radius("door_auto_open_radius", "120", "Distance in Source units to trigger door auto-open");
CfgVar ins_door_speed("door_speed", "200", "Door movement speed in degrees/sec or units/sec (0=use map default)");
CfgVar ins_weapon_swap("weapon_swap", "1", "Aim + +use (F key) weapon drop and swap: 0=off, 1=players only, 2=players and bots");
CfgVar ins_weapon_swap_range("weapon_swap_range", "100", "Maximum distance in Source units to target a dropped weapon for swap");
CfgVar ins_door_smoothness("door_smoothness", "1", "Maximize door opening/closing animation smoothness: 1=on, 0=off");
CfgVar ins_fix_strncpy_null("fix_strncpy_null", "1", "Guard V_strncpy against a NULL source (prevents the GetNetworkIDString crash); 1=on, 0=off");
CfgVar ins_voice_fix("voice_fix", "1", "Clamp CLC/SVC_VoiceData length to the engine's 4096-byte voice buffer (fixes the remote stack overflow RCE); 1=on, 0=off");

int FindSendPropOffset(SendTable *pTable, const char *pPropName);
#ifdef _LINUX
static int s_offset_m_hOwner = INSUTIL_LINUX_M_H_WEAPON_OWNER;
#else
// Audited CBaseCombatWeapon::m_hOwner offset in Windows server.dll (SendTable DT_BaseCombatWeapon, 0x0454 / 1108 decimal).
static int s_offset_m_hOwner = 0x0454;
#endif
static int s_offset_m_CollisionGroup = -1;
static int s_offset_m_flSimulationTime = -1;
static int s_offset_m_flAnimTime = -1;

static bool s_bDoorsUpdated = false;
static IGameEventManager2 *s_pEventManager = NULL;
#ifdef _LINUX
typedef bool (*FireEventFn)(void *mgr, IGameEvent *event, bool bDontBroadcast);
static FireEventFn s_OrigFireEvent = NULL;

static bool FireEvent_Hook(void *mgr, IGameEvent *event, bool bDontBroadcast)
{
    if (s_OrigFireEvent && event && ins_mute_killfeed.GetBool())
    {
        const char *name = event->GetName();
        if (name && !strcmp(name, "player_death"))
            bDontBroadcast = true;
    }
    return s_OrigFireEvent ? s_OrigFireEvent(mgr, event, bDontBroadcast) : false;
}
#else
typedef bool (__fastcall *FireEventFn)(void *mgr, void *edxUnused, IGameEvent *event, bool bDontBroadcast);
static FireEventFn s_OrigFireEvent = NULL;

static bool __fastcall FireEvent_Hook(void *mgr, void *edxUnused, IGameEvent *event, bool bDontBroadcast)
{
    if (s_OrigFireEvent && event && ins_mute_killfeed.GetBool())
    {
        const char *name = event->GetName();
        if (name && !strcmp(name, "player_death"))
            bDontBroadcast = true;
    }
    return s_OrigFireEvent ? s_OrigFireEvent(mgr, edxUnused, event, bDontBroadcast) : false;
}
#endif

static void InstallFireEventHook()
{
    if (s_pEventManager && !s_OrigFireEvent)
    {
        void **vtable = *(void ***)s_pEventManager;
#ifdef _LINUX
        const int FIRE_EVENT_SLOT = INSUTIL_LINUX_FIRE_EVENT_SLOT;
        Dl_info info;
        memset(&info, 0, sizeof(info));
        bool resolved = dladdr(vtable[FIRE_EVENT_SLOT], &info) && info.dli_fbase
            && info.dli_fname;
        bool directEngineTarget = resolved
            && strstr(info.dli_fname, "engine_i686.so")
            && info.dli_sname && strstr(info.dli_sname, "FireEvent")
            && (uintptr_t)vtable[FIRE_EVENT_SLOT] - (uintptr_t)info.dli_fbase
                == INSUTIL_LINUX_FIRE_EVENT_RVA;
        // SourceMod's SourceHook may already own this exact vtable slot by the
        // time a runtime config enables mute_killfeed. Chain its current
        // trampoline, then restore that same pointer on plugin unload.
        bool sourceModTarget = resolved && strstr(info.dli_fname, "sourcemod.2.ep1.so");
        if (!directEngineTarget && !sourceModTarget)
        {
            Msg("[ins_utilities] FireEvent hook unavailable: Linux vtable signature mismatch "
                "target=%p module=%s symbol=%s rva=0x%X\n",
                vtable[FIRE_EVENT_SLOT], info.dli_fname ? info.dli_fname : "<none>",
                info.dli_sname ? info.dli_sname : "<none>",
                info.dli_fbase ? (unsigned int)((uintptr_t)vtable[FIRE_EVENT_SLOT]
                    - (uintptr_t)info.dli_fbase) : 0);
            return;
        }
        if (sourceModTarget)
            Msg("[ins_utilities] FireEvent slot is SourceMod-hooked; chaining current trampoline\n");
#else
        const int FIRE_EVENT_SLOT = 7;
#endif
        s_OrigFireEvent = (FireEventFn)vtable[FIRE_EVENT_SLOT];
        unsigned long oldProtect = 0;
        if (!MakeWritable(&vtable[FIRE_EVENT_SLOT], sizeof(void*), &oldProtect))
        {
            s_OrigFireEvent = NULL;
            Msg("[ins_utilities] FireEvent hook unavailable: vtable page is protected\n");
            return;
        }
        vtable[FIRE_EVENT_SLOT] = (void*)FireEvent_Hook;
        RestoreProtection(&vtable[FIRE_EVENT_SLOT], sizeof(void*), oldProtect, false);
        Msg("[ins_utilities] Installed FireEvent vtable hook (mute_killfeed ready)\n");
    }
}

static void UninstallFireEventHook()
{
    if (s_pEventManager && s_OrigFireEvent)
    {
        void **vtable = *(void ***)s_pEventManager;
        unsigned long oldProtect = 0;
#ifdef _LINUX
        const int FIRE_EVENT_SLOT = INSUTIL_LINUX_FIRE_EVENT_SLOT;
#else
        const int FIRE_EVENT_SLOT = 7;
#endif
        if (vtable[FIRE_EVENT_SLOT] == (void*)FireEvent_Hook
            && MakeWritable(&vtable[FIRE_EVENT_SLOT], sizeof(void*), &oldProtect))
        {
            vtable[FIRE_EVENT_SLOT] = (void*)s_OrigFireEvent;
            RestoreProtection(&vtable[FIRE_EVENT_SLOT], sizeof(void*), oldProtect, false);
        }
        s_OrigFireEvent = NULL;
    }
}


static const int SF_DOOR_NO_AUTO_RETURN = 32;
static const int SF_DOOR_PUSE   = 256;
static const int SF_DOOR_PTOUCH = 1024;

static bool LooksLikeDoorSpawnFlags(int val)
{
    if (val < 0 || val >= 0x10000) return false;
    if ((val & ~0xFFFF) != 0) return false;
    if ((val & (SF_DOOR_PUSE | SF_DOOR_PTOUCH | SF_DOOR_NO_AUTO_RETURN)) == 0) return false;
    return true;
}

static int KnownDoorSpawnFlagsOffset(const char *ecn)
{
    if (!ecn) return -1;
    if (!_stricmp(ecn, "func_door_rotating")) return 140;
    if (!_stricmp(ecn, "func_door")) return 652;
    return -1;
}

void ApplyKeepDoorsOpen()
{
    if (!engine || !ins_keep_doors_open.GetBool() || !gpGlobals)
        return;

    // Door GetDataDescMap virtuals return NULL in this game. Linux therefore
    // uses exact offsets independently verified from owning functions and the
    // exported datamap objects. No broad object-memory scan is performed.

    int modifiedCount = 0;
    int scannedCount = 0;
    int totalEnts = engine->GetEntityCount();

    for (int i = gpGlobals->maxClients + 1; i <= totalEnts; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (!e || e->IsFree()) continue;

        const char *ecn = e->GetClassName();
        if (!ecn) continue;
        bool isDoor = false;
        if (ecn[0]=='f' && ecn[1]=='u' && ecn[2]=='n' && ecn[3]=='c' && ecn[4]=='_')
        {
            if (!strncmp(ecn+5, "door", 4)) isDoor = true;
        }
        else if (ecn[0]=='p' && ecn[1]=='r' && ecn[2]=='o' && ecn[3]=='p' && ecn[4]=='_')
        {
            if (!strncmp(ecn+5, "door", 4)) isDoor = true;
        }
        if (!isDoor) continue;

        scannedCount++;
        IServerUnknown *unk = e->GetUnknown();
        if (!unk) continue;
        CBaseEntity *base = unk->GetBaseEntity();
        if (!base) continue;

        bool changed = false;
#ifdef _LINUX
        int *pFlags = (int *)((char *)base + INSUTIL_LINUX_M_SPAWNFLAGS);
        if (*pFlags >= 0 && (*pFlags & ~0xFFFF) == 0)
        {
            if (!(*pFlags & SF_DOOR_NO_AUTO_RETURN))
            {
                *pFlags |= SF_DOOR_NO_AUTO_RETURN;
                changed = true;
            }
            if (*pFlags & SF_DOOR_PTOUCH)
            {
                *pFlags &= ~SF_DOOR_PTOUCH;
                changed = true;
            }
            if (!(*pFlags & SF_DOOR_PUSE))
            {
                *pFlags |= SF_DOOR_PUSE;
                changed = true;
            }
        }
        else
        {
            Msg("[ins_utilities] WARNING: refusing unexpected %s spawnflags value %d\n",
                ecn, *pFlags);
        }

        if (!_stricmp(ecn, "func_door") || !_stricmp(ecn, "func_door_rotating"))
        {
            float *pWait = (float *)((char *)base + INSUTIL_LINUX_M_FL_WAIT);
            if (*pWait == *pWait && *pWait > -1000000.0f && *pWait < 1000000.0f)
            {
                if (*pWait != -1.0f)
                {
                    *pWait = -1.0f;
                    changed = true;
                }
            }
            else
            {
                Msg("[ins_utilities] WARNING: refusing unexpected %s m_flWait value\n", ecn);
            }
        }
        else if (!_stricmp(ecn, "prop_door_rotating"))
        {
            float *pDelay = (float *)((char *)base + INSUTIL_LINUX_M_FL_AUTO_RETURN_DELAY);
            if (*pDelay == *pDelay && *pDelay > -1000000.0f && *pDelay < 1000000.0f)
            {
                if (*pDelay != -1.0f)
                {
                    *pDelay = -1.0f;
                    changed = true;
                }
            }
            else
            {
                Msg("[ins_utilities] WARNING: refusing unexpected %s auto-return value\n", ecn);
            }
        }
#else
        // Preferred path: datamap m_spawnflags when the class exposes one.
        datamap_t *dmap = base->GetDataDescMap();
        typedescription_t *tdFlags = dmap ? FindDataMapField(dmap, "m_spawnflags") : NULL;
        if (tdFlags && tdFlags->fieldType == FIELD_INTEGER)
        {
            int *pFlags = (int *)((char *)base + tdFlags->fieldOffset[TD_OFFSET_NORMAL]);
            if (!(*pFlags & SF_DOOR_NO_AUTO_RETURN))
            {
                *pFlags |= SF_DOOR_NO_AUTO_RETURN;
                changed = true;
            }
            if (*pFlags & SF_DOOR_PTOUCH)
            {
                *pFlags &= ~SF_DOOR_PTOUCH;
                changed = true;
            }
            if (!(*pFlags & SF_DOOR_PUSE))
            {
                *pFlags |= SF_DOOR_PUSE;
                changed = true;
            }
        }
        else
        {
            // Fixed-offset fallback for this game's door classes. Reads exactly
            // one int at the known offset and validates it before writing.
            int off = KnownDoorSpawnFlagsOffset(ecn);
            if (off >= 0)
            {
                char *raw = (char *)base;
                int *pFlags = (int *)(raw + off);
                if (LooksLikeDoorSpawnFlags(*pFlags))
                {
                    if (!(*pFlags & SF_DOOR_NO_AUTO_RETURN))
                    {
                        *pFlags |= SF_DOOR_NO_AUTO_RETURN;
                        changed = true;
                    }
                    if (*pFlags & SF_DOOR_PTOUCH)
                    {
                        *pFlags &= ~SF_DOOR_PTOUCH;
                        changed = true;
                    }
                    if (!(*pFlags & SF_DOOR_PUSE))
                    {
                        *pFlags |= SF_DOOR_PUSE;
                        changed = true;
                    }
                }
            }
        }

        // Bonus: disable auto-close timer fields where the datamap is available.
        if (dmap)
        {
            typedescription_t *tdReturn = FindDataMapField(dmap, "m_flAutoReturnDelay");
            if (tdReturn && tdReturn->fieldType == FIELD_FLOAT)
            {
                float *pVal = (float *)((char *)base + tdReturn->fieldOffset[TD_OFFSET_NORMAL]);
                if (*pVal != -1.0f)
                {
                    *pVal = -1.0f;
                    changed = true;
                }
            }
            typedescription_t *tdWait = FindDataMapField(dmap, "m_flWait");
            if (tdWait && tdWait->fieldType == FIELD_FLOAT)
            {
                float *pVal = (float *)((char *)base + tdWait->fieldOffset[TD_OFFSET_NORMAL]);
                if (*pVal != -1.0f)
                {
                    *pVal = -1.0f;
                    changed = true;
                }
            }
        }
#endif

        if (changed)
            modifiedCount++;
    }

    if (scannedCount > 0 && modifiedCount > 0)
    {
        Msg("[ins_utilities] Modified %d door(s) to stay open\n", modifiedCount);
    }
    s_bDoorsUpdated = true;
}
static bool FireEntityInput(CBaseEntity *pEnt, const char *szInput, CBaseEntity *pActivator, CBaseEntity *pCaller, variant_t val)
{
    if (!pEnt) return false;
#ifdef _LINUX
    typedef bool (*AcceptInputFn_Lin)(void *pThis, const char *szInputName, CBaseEntity *pActivator, CBaseEntity *pCaller, variant_t Value, int outputID);
    static AcceptInputFn_Lin s_pfnAcceptInput = NULL;
    static bool s_bLookedUp = false;
    if (!s_bLookedUp)
    {
        s_bLookedUp = true;
        s_pfnAcceptInput = (AcceptInputFn_Lin)dlsym(RTLD_DEFAULT, "_ZN11CBaseEntity11AcceptInputEPKcPS_S2_9variant_ti");
    }
    if (s_pfnAcceptInput)
        return s_pfnAcceptInput(pEnt, szInput, pActivator, pCaller, val, 0);
    void **vtable = *(void ***)pEnt;
    if (!vtable) return false;
    AcceptInputFn_Lin pfnVtable = (AcceptInputFn_Lin)vtable[35];
    return pfnVtable ? pfnVtable(pEnt, szInput, pActivator, pCaller, val, 0) : false;
#else
    typedef bool (__thiscall *AcceptInputFn_Win)(void *pThis, const char *szInputName, CBaseEntity *pActivator, CBaseEntity *pCaller, variant_t Value, int outputID);
    void **vtable = *(void ***)pEnt;
    if (!vtable) return false;
    AcceptInputFn_Win pfnAcceptInput = (AcceptInputFn_Win)vtable[34];
    return pfnAcceptInput ? pfnAcceptInput(pEnt, szInput, pActivator, pCaller, val, 0) : false;
#endif
}

static bool IsDoorClosedOrClosing(CBaseEntity *base, const char *ecn)
{
    if (!base || !ecn) return false;
    if (!_stricmp(ecn, "func_door") || !_stricmp(ecn, "func_door_rotating"))
    {
#ifdef _LINUX
        int state = *(int *)((char *)base + 0x344);
#else
        int state = *(int *)((char *)base + 0x330);
#endif
        // TS_AT_TOP = 0 (open), TS_AT_BOTTOM = 1 (closed), TS_GOING_UP = 2 (opening), TS_GOING_DOWN = 3 (closing)
        return (state == 1 || state == 3);
    }
    else if (!_stricmp(ecn, "prop_door_rotating"))
    {
#ifdef _LINUX
        int state = *(int *)((char *)base + 0x5F4);
#else
        int state = *(int *)((char *)base + 0x5E0);
#endif
        // DOOR_STATE_CLOSED = 0, DOOR_STATE_OPENING = 1, DOOR_STATE_OPEN = 2, DOOR_STATE_CLOSING = 3, DOOR_STATE_AJAR = 4
        return (state == 0 || state == 3 || state == 4);
    }
    return false;
}

void ApplyDoorSpeed()
{
    float targetSpeed = ins_door_speed.GetFloat();
    if (targetSpeed <= 0.0f || !engine || !gpGlobals)
        return;

    int totalEnts = engine->GetEntityCount();
    for (int i = gpGlobals->maxClients + 1; i <= totalEnts; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (!e || e->IsFree()) continue;

        const char *ecn = e->GetClassName();
        if (!ecn) continue;
        bool isDoor = (!_stricmp(ecn, "func_door") || !_stricmp(ecn, "func_door_rotating") || !_stricmp(ecn, "prop_door_rotating"));
        if (!isDoor) continue;

        IServerUnknown *unk = e->GetUnknown();
        if (!unk) continue;
        CBaseEntity *base = unk->GetBaseEntity();
        if (!base) continue;

        variant_t val;
        val.SetFloat(targetSpeed);
        FireEntityInput(base, "SetSpeed", NULL, NULL, val);

#ifndef _LINUX
        datamap_t *dmap = base->GetDataDescMap();
        typedescription_t *tdSpeed = dmap ? FindDataMapField(dmap, "m_flSpeed") : NULL;
        if (!tdSpeed && dmap) tdSpeed = FindDataMapField(dmap, "speed");
        if (tdSpeed && tdSpeed->fieldType == FIELD_FLOAT)
        {
            float *pSpeed = (float *)((char *)base + tdSpeed->fieldOffset[TD_OFFSET_NORMAL]);
            *pSpeed = targetSpeed;
        }
#endif
    }
}

static bool IsDoorMoving(CBaseEntity *base, const char *ecn)
{
    if (!base || !ecn) return false;
    if (!_stricmp(ecn, "func_door") || !_stricmp(ecn, "func_door_rotating"))
    {
#ifdef _LINUX
        int state = *(int *)((char *)base + 0x344);
#else
        int state = *(int *)((char *)base + 0x330);
#endif
        // TS_GOING_UP = 2 (opening), TS_GOING_DOWN = 3 (closing)
        return (state == 2 || state == 3);
    }
    else if (!_stricmp(ecn, "prop_door_rotating"))
    {
#ifdef _LINUX
        int state = *(int *)((char *)base + 0x5F4);
#else
        int state = *(int *)((char *)base + 0x5E0);
#endif
        // DOOR_STATE_OPENING = 1, DOOR_STATE_CLOSING = 3
        return (state == 1 || state == 3);
    }
    return false;
}

void UpdateDoorSmoothness()
{
    if (!ins_door_smoothness.GetBool() || !engine || !gpGlobals)
        return;

    int totalEnts = engine->GetEntityCount();
    for (int i = gpGlobals->maxClients + 1; i <= totalEnts; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (!e || e->IsFree()) continue;

        const char *ecn = e->GetClassName();
        if (!ecn) continue;
        bool isDoor = (!_stricmp(ecn, "func_door") || !_stricmp(ecn, "func_door_rotating") || !_stricmp(ecn, "prop_door_rotating"));
        if (!isDoor) continue;

        IServerUnknown *unk = e->GetUnknown();
        if (!unk) continue;
        CBaseEntity *base = unk->GetBaseEntity();
        if (!base) continue;

        if (s_offset_m_CollisionGroup < 0 || s_offset_m_flSimulationTime < 0 || s_offset_m_flAnimTime < 0)
        {
            IServerNetworkable *net = unk->GetNetworkable();
            ServerClass *sc = net ? net->GetServerClass() : NULL;
            if (sc && sc->m_pTable)
            {
                if (s_offset_m_CollisionGroup < 0)
                    s_offset_m_CollisionGroup = FindSendPropOffset(sc->m_pTable, "m_CollisionGroup");
                if (s_offset_m_flSimulationTime < 0)
                    s_offset_m_flSimulationTime = FindSendPropOffset(sc->m_pTable, "m_flSimulationTime");
                if (s_offset_m_flAnimTime < 0)
                    s_offset_m_flAnimTime = FindSendPropOffset(sc->m_pTable, "m_flAnimTime");
            }
        }

        bool moving = IsDoorMoving(base, ecn);
        if (moving)
        {
            float now = gpGlobals->curtime;

            // 1. Stamp simulation time (brush doors -> LATCH_SIMULATION_VAR)
            if (s_offset_m_flSimulationTime > 0)
            {
                *(float *)((char *)base + s_offset_m_flSimulationTime) = now;
            }

            // 2. Stamp animation time (model prop doors -> LATCH_ANIMATION_VAR)
            if (s_offset_m_flAnimTime > 0 && !_stricmp(ecn, "prop_door_rotating"))
            {
                *(float *)((char *)base + s_offset_m_flAnimTime) = now;
            }

            // 3. Force network snapshot serialization on every tick while moving
            e->m_fStateFlags |= FL_EDICT_CHANGED;

            // 4. Set non-friction passable collision while in motion
            if (s_offset_m_CollisionGroup > 0)
            {
                int *pGroup = (int *)((char *)base + s_offset_m_CollisionGroup);
                if (*pGroup != COLLISION_GROUP_PASSABLE_DOOR)
                {
                    *pGroup = COLLISION_GROUP_PASSABLE_DOOR;
                    e->m_fStateFlags |= FL_EDICT_CHANGED | FL_FULL_EDICT_CHANGED;
                }
            }
        }
        else
        {
            if (s_offset_m_CollisionGroup > 0)
            {
                int *pGroup = (int *)((char *)base + s_offset_m_CollisionGroup);
                if (*pGroup == COLLISION_GROUP_PASSABLE_DOOR)
                {
                    *pGroup = COLLISION_GROUP_NONE;
                    e->m_fStateFlags |= FL_EDICT_CHANGED | FL_FULL_EDICT_CHANGED;
                }
            }
        }
    }
}

static float s_flLastProximityCheckTime = 0.0f;
static float s_doorCooldown[2048] = { 0.0f };
static Vector s_playerLastPos[65];
static float s_playerLastTime[65] = { 0.0f };

void UpdateDoorProximityOpen()
{
    int autoOpenMode = ins_door_auto_open.GetInt();
    if (autoOpenMode <= 0 || !engine || !gpGlobals || !playerinfomanager || !enginetrace)
        return;

    float now = gpGlobals->curtime;
    if (now - s_flLastProximityCheckTime < 0.10f)
        return;
    s_flLastProximityCheckTime = now;

    float radius = ins_door_auto_open_radius.GetFloat();
    if (radius <= 0.0f) radius = 120.0f;

    int totalEnts = engine->GetEntityCount();
    if (totalEnts >= 2048) totalEnts = 2047;

    for (int i = gpGlobals->maxClients + 1; i <= totalEnts; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (!e || e->IsFree()) continue;

        const char *ecn = e->GetClassName();
        if (!ecn) continue;
        bool isDoor = (!_stricmp(ecn, "func_door") || !_stricmp(ecn, "func_door_rotating") || !_stricmp(ecn, "prop_door_rotating"));
        if (!isDoor) continue;

        IServerUnknown *unk = e->GetUnknown();
        if (!unk) continue;
        CBaseEntity *base = unk->GetBaseEntity();
        if (!base) continue;

        if (now < s_doorCooldown[i]) continue;
        if (!IsDoorClosedOrClosing(base, ecn)) continue;

        ICollideable *c = unk->GetCollideable();
        if (!c) continue;
        Vector worldMins, worldMaxs;
        c->WorldSpaceSurroundingBounds(&worldMins, &worldMaxs);
        Vector doorCenter = (worldMins + worldMaxs) * 0.5f;

        QAngle angles = c->GetCollisionAngles();
        float radYaw = angles.y * (3.14159265f / 180.0f);
        Vector fwd(cosf(radYaw), sinf(radYaw), 0.0f);
        Vector right(-sinf(radYaw), cosf(radYaw), 0.0f);
        fwd.z = 0.0f;
        float fwdLen = sqrtf(fwd.x * fwd.x + fwd.y * fwd.y);
        if (fwdLen > 0.001f) { fwd.x /= fwdLen; fwd.y /= fwdLen; }
        else { fwd = Vector(1.0f, 0.0f, 0.0f); }

        right.z = 0.0f;
        float rightLen = sqrtf(right.x * right.x + right.y * right.y);
        if (rightLen > 0.001f) { right.x /= rightLen; right.y /= rightLen; }
        else { right = Vector(0.0f, 1.0f, 0.0f); }

        Vector obbMins = c->OBBMins();
        Vector obbMaxs = c->OBBMaxs();
        float dimX = fabsf(obbMaxs.x - obbMins.x);
        float dimY = fabsf(obbMaxs.y - obbMins.y);
        float halfWidth = (dimX > dimY ? dimX : dimY) * 0.5f;
        if (halfWidth < 24.0f) halfWidth = 24.0f;
        float maxLateral = halfWidth + 48.0f;

        for (int client = 1; client <= gpGlobals->maxClients; client++)
        {
            edict_t *pe = engine->PEntityOfEntIndex(client);
            if (!pe || pe->IsFree()) continue;
            IPlayerInfo *info = playerinfomanager->GetPlayerInfo(pe);
            if (!info || !info->IsConnected() || info->IsDead() || info->GetTeamIndex() <= 0) continue;

            if (autoOpenMode == 1 && !info->IsFakeClient()) continue;

            Vector playerPos = info->GetAbsOrigin();
            Vector playerWaist = playerPos + Vector(0.0f, 0.0f, 36.0f);

            Vector vel(0.0f, 0.0f, 0.0f);
            float dt = now - s_playerLastTime[client];
            if (dt > 0.001f && dt < 1.0f)
                vel = (playerPos - s_playerLastPos[client]) * (1.0f / dt);
            s_playerLastPos[client] = playerPos;
            s_playerLastTime[client] = now;

            if (fabsf(playerWaist.z - doorCenter.z) > 72.0f) continue;

            Vector d = playerPos - doorCenter;
            float depth = d.x * fwd.x + d.y * fwd.y;
            float lateral = d.x * right.x + d.y * right.y;

            if (fabsf(depth) > radius) continue;
            if (fabsf(lateral) > maxLateral) continue;

            float dist2D = sqrtf(depth * depth + lateral * lateral);
            if (dist2D > radius) continue;

            float speedSq = vel.x * vel.x + vel.y * vel.y;
            if (speedSq >= 40.0f * 40.0f)
            {
                float velToCenter = -(vel.x * d.x + vel.y * d.y);
                float velAlongDepth = -(vel.x * fwd.x + vel.y * fwd.y) * (depth >= 0.0f ? 1.0f : -1.0f);
                if (velToCenter <= 0.0f && velAlongDepth <= 0.0f) continue;
            }

            Ray_t ray; trace_t tr;
            CTraceFilterWorldAndPropsOnly filter;
            ray.Init(playerWaist, doorCenter);
            enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &filter, &tr);
            if (tr.fraction < 0.95f && tr.m_pEnt != (IHandleEntity *)unk)
                continue;

            IServerUnknown *playerUnk = pe->GetUnknown();
            CBaseEntity *playerBase = playerUnk ? playerUnk->GetBaseEntity() : NULL;
            variant_t empty;
            FireEntityInput(base, "Use", playerBase, playerBase, empty);
            s_doorCooldown[i] = now + 1.0f;
            break;
        }
    }
}

class CTraceFilterSkipPlayer : public CTraceFilter
{
public:
    CTraceFilterSkipPlayer(IHandleEntity *pPlayer) : m_pPlayer(pPlayer) {}
    virtual bool ShouldHitEntity(IHandleEntity *pServerEntity, int contentsMask)
    {
        return (pServerEntity != m_pPlayer);
    }
private:
    IHandleEntity *m_pPlayer;
};

static IHandleEntity *EdictHandleEntity(edict_t *e)
{
    if (!e) return NULL;
    IServerUnknown *unk = e->GetUnknown();
    return (IHandleEntity *)unk;
}

static int GetWeaponCategory(const char *wcn)
{
    if (!wcn) return -1;
    if (!strcmp(wcn, "weapon_m9") || !strcmp(wcn, "weapon_makarov") || !strcmp(wcn, "weapon_1911"))
        return 1; // Sidearm
    if (!strcmp(wcn, "weapon_m67") || !strcmp(wcn, "weapon_rgd5"))
        return 2; // Frag grenade
    if (!strcmp(wcn, "weapon_m18"))
        return 3; // Smoke grenade
    if (!strncmp(wcn, "weapon_", 7))
        return 0; // Primary weapon
    return -1;
}

static bool IsSameWeaponCategory(const char *wcn1, const char *wcn2)
{
    int c1 = GetWeaponCategory(wcn1);
    int c2 = GetWeaponCategory(wcn2);
    return (c1 >= 0 && c1 == c2);
}

static void DropPlayerWeapon(CBaseEntity *pPlayer, CBaseEntity *pWeapon, const Vector *pVel)
{
    if (!pPlayer || !pWeapon) return;
#ifdef _LINUX
    typedef void (*WeaponDropFn_Lin)(CBaseEntity *pPlayer, CBaseEntity *pWeapon, bool b1, bool b2, const Vector *pVel);
    static WeaponDropFn_Lin s_pfnDrop = NULL;
    static bool s_bDropLookedUp = false;
    if (!s_bDropLookedUp)
    {
        s_bDropLookedUp = true;
        s_pfnDrop = (WeaponDropFn_Lin)dlsym(RTLD_DEFAULT, "_ZN11CBasePlayer11Weapon_DropEP17CBaseCombatWeaponbbPK6Vector");
    }
    if (s_pfnDrop)
    {
        s_pfnDrop(pPlayer, pWeapon, true, false, pVel);
        return;
    }
#endif
    if (s_offset_m_hOwner > 0)
    {
        *(unsigned long *)((char *)pWeapon + s_offset_m_hOwner) = 0xFFFFFFFF;
    }
}

static void EquipPlayerWeapon(CBaseEntity *pPlayer, CBaseEntity *pWeapon)
{
    if (!pPlayer || !pWeapon) return;
#ifdef _LINUX
    typedef int (*BumpWeaponFn_Lin)(CBaseEntity *pPlayer, CBaseEntity *pWeapon, bool bTouch);
    static BumpWeaponFn_Lin s_pfnBump = NULL;
    static bool s_bBumpLookedUp = false;
    if (!s_bBumpLookedUp)
    {
        s_bBumpLookedUp = true;
        s_pfnBump = (BumpWeaponFn_Lin)dlsym(RTLD_DEFAULT, "_ZN10CINSPlayer10BumpWeaponEP17CBaseCombatWeaponb");
    }
    if (s_pfnBump)
    {
        s_pfnBump(pPlayer, pWeapon, false);
        return;
    }
#endif
    if (s_offset_m_hOwner > 0 && engine && playerinfomanager)
    {
        IServerUnknown *punk = (IServerUnknown *)pPlayer;
        IServerNetworkable *pnet = punk ? punk->GetNetworkable() : NULL;
        edict_t *pedict = pnet ? pnet->GetEdict() : NULL;
        if (pedict)
        {
            int pidx = engine->IndexOfEdict(pedict);
            *(unsigned long *)((char *)pWeapon + s_offset_m_hOwner) = (unsigned long)pidx;
        }
    }
}

static int s_lastButtons[65] = { 0 };

void UpdateWeaponDropAndSwap()
{
    int swapMode = ins_weapon_swap.GetInt();
    if (swapMode <= 0 || !engine || !gpGlobals || !playerinfomanager || !enginetrace)
        return;

    float range = ins_weapon_swap_range.GetFloat();
    if (range <= 0.0f) range = 100.0f;

    int totalEnts = engine->GetEntityCount();

    for (int client = 1; client <= gpGlobals->maxClients; client++)
    {
        edict_t *pe = engine->PEntityOfEntIndex(client);
        if (!pe || pe->IsFree()) continue;
        IPlayerInfo *info = playerinfomanager->GetPlayerInfo(pe);
        if (!info || !info->IsConnected() || info->IsDead() || info->GetTeamIndex() <= 0)
        {
            s_lastButtons[client] = 0;
            continue;
        }

        if (swapMode == 1 && info->IsFakeClient()) continue;

        CBotCmd cmd = info->GetLastUserCommand();
        int curButtons = cmd.buttons;
        bool usePressed = ((curButtons & (32 | 64)) && !(s_lastButtons[client] & (32 | 64)));
        s_lastButtons[client] = curButtons;

        if (!usePressed) continue;

        Vector playerPos = info->GetAbsOrigin();
        Vector eyePos = playerPos + Vector(0.0f, 0.0f, (curButtons & 2) ? 36.0f : 62.0f);

        float radPitch = cmd.viewangles.x * (3.14159265f / 180.0f);
        float radYaw   = cmd.viewangles.y * (3.14159265f / 180.0f);
        Vector fwd(cosf(radPitch) * cosf(radYaw), cosf(radPitch) * sinf(radYaw), -sinf(radPitch));

        CBaseEntity *bestTargetBase = NULL;
        const char *bestTargetCn = NULL;
        float bestScore = 0.82f; // ~35 deg FOV cone around crosshair

        for (int w = gpGlobals->maxClients + 1; w <= totalEnts; w++)
        {
            edict_t *we = engine->PEntityOfEntIndex(w);
            if (!we || we->IsFree()) continue;
            const char *wcn = we->GetClassName();
            if (!wcn || strncmp(wcn, "weapon_", 7)) continue;
            IServerUnknown *wunk = we->GetUnknown();
            if (!wunk) continue;
            CBaseEntity *wbase = wunk->GetBaseEntity();
            if (!wbase) continue;

            if (s_offset_m_hOwner > 0)
            {
                unsigned long owner = *(unsigned long *)((char *)wbase + s_offset_m_hOwner);
                if (owner != 0xFFFFFFFF) continue;
            }

            ICollideable *c = wunk->GetCollideable();
            if (!c) continue;
            Vector wmins, wmaxs;
            c->WorldSpaceSurroundingBounds(&wmins, &wmaxs);
            Vector wcenter = (wmins + wmaxs) * 0.5f;

            Vector toWpn = wcenter - eyePos;
            float dist = sqrtf(toWpn.x * toWpn.x + toWpn.y * toWpn.y + toWpn.z * toWpn.z);
            if (dist <= 0.001f || dist > range) continue;

            toWpn.x /= dist; toWpn.y /= dist; toWpn.z /= dist;
            float dot = fwd.x * toWpn.x + fwd.y * toWpn.y + fwd.z * toWpn.z;
            if (dot < bestScore) continue;

            Ray_t ray; trace_t tr;
            CTraceFilterWorldAndPropsOnly filter;
            ray.Init(eyePos, wcenter);
            enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &filter, &tr);
            if (tr.fraction < 0.95f && tr.m_pEnt != (IHandleEntity *)wunk)
                continue;

            bestScore = dot;
            bestTargetBase = wbase;
            bestTargetCn = wcn;
        }

        if (!bestTargetBase || !bestTargetCn) continue;

        CBaseEntity *pOldWeapon = NULL;
        for (int w = gpGlobals->maxClients + 1; w <= totalEnts; w++)
        {
            edict_t *we = engine->PEntityOfEntIndex(w);
            if (!we || we->IsFree()) continue;
            const char *wcn = we->GetClassName();
            if (!wcn || strncmp(wcn, "weapon_", 7)) continue;
            IServerUnknown *wunk = we->GetUnknown();
            if (!wunk) continue;
            CBaseEntity *wbase = wunk->GetBaseEntity();
            if (!wbase) continue;

            if (s_offset_m_hOwner > 0)
            {
                unsigned long owner = *(unsigned long *)((char *)wbase + s_offset_m_hOwner);
                if (owner == (unsigned long)client)
                {
                    if (IsSameWeaponCategory(wcn, bestTargetCn))
                    {
                        pOldWeapon = wbase;
                        break;
                    }
                }
            }
        }

        IServerUnknown *playerUnk = pe->GetUnknown();
        CBaseEntity *playerBase = playerUnk ? playerUnk->GetBaseEntity() : NULL;
        if (!playerBase) continue;

        Vector dropVel = fwd * 120.0f;
        if (pOldWeapon)
        {
            DropPlayerWeapon(playerBase, pOldWeapon, &dropVel);
        }

        EquipPlayerWeapon(playerBase, bestTargetBase);
    }
}

static float s_flLastCleanupTime = 0.0f;
static float s_flMapStartTime = 0.0f;
static float s_rpgInternalLaunchSpeed = 3000.0f;
static float s_rpgInternalThrust = 1000.0f;
static unsigned long *s_rpgSpeedOperand = NULL;
static unsigned long *s_rpgThrustOperand = NULL;
static unsigned long s_rpgOriginalSpeedOperand = 0;
static unsigned long s_rpgOriginalThrustOperand = 0;

// CBaseRocketMissile::Setup applies a hard-coded 3000.0f launch speed and stores
// 1000.0f as continuous thrust. Patch only those two verified instructions;
// other users of the same numeric constants remain untouched.
#ifdef _LINUX
struct InsModuleInfo
{
    const char *needle;
    uintptr_t base;
    const Elf32_Phdr *phdrs;
    int phnum;
};

static int InsModuleCallback(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    InsModuleInfo *m = (InsModuleInfo *)data;
    if (!info->dlpi_name || !m->needle) return 0;
    if (strstr(info->dlpi_name, m->needle))
    {
        m->base = info->dlpi_addr;
        m->phdrs = (const Elf32_Phdr *)info->dlpi_phdr;
        m->phnum = info->dlpi_phnum;
        return 1;
    }
    return 0;
}

static unsigned char *FindLinuxRpgSignature(const unsigned char *pattern,
                                            int patternSize,
                                            unsigned int expectedRva,
                                            const char *label)
{
    InsModuleInfo m = {0};
    m.needle = "server_i486.so";
    dl_iterate_phdr(InsModuleCallback, &m);
    if (!m.phdrs) return NULL;

    unsigned char *match = NULL;
    int matches = 0;
    for (int p = 0; p < m.phnum; ++p)
    {
        const Elf32_Phdr *segment = &m.phdrs[p];
        if (segment->p_type != PT_LOAD || !(segment->p_flags & PF_X)) continue;
        unsigned char *begin = (unsigned char *)(m.base + segment->p_vaddr);
        unsigned char *end = begin + segment->p_memsz;
        for (unsigned char *at = begin; at + patternSize <= end; ++at)
        {
            if (memcmp(at, pattern, patternSize)) continue;
            match = at;
            ++matches;
        }
    }
    unsigned int actualRva = match ? (unsigned int)((uintptr_t)match - m.base) : 0;
    if (matches != 1 || actualRva != expectedRva)
    {
        Msg("[ins_utilities] RPG %s signature mismatch: matches=%d rva=0x%X expected=0x%X\n",
            label, matches, actualRva, expectedRva);
        return NULL;
    }
    return match;
}

static unsigned long InsUtilFloatBits(float value)
{
    unsigned long bits = 0;
    memcpy(&bits, &value, sizeof(value));
    return bits;
}

static bool PatchLinuxRpgImmediates(unsigned long speed, unsigned long thrust)
{
    if (!s_rpgSpeedOperand || !s_rpgThrustOperand) return false;
    unsigned char *a = (unsigned char *)s_rpgSpeedOperand;
    unsigned char *b = (unsigned char *)s_rpgThrustOperand;
    unsigned char *begin = a < b ? a : b;
    unsigned char *end = (a > b ? a : b) + sizeof(unsigned long);
    unsigned long oldProtect = 0;
    if (!MakeWritable(begin, (size_t)(end - begin), &oldProtect))
    {
        Msg("[ins_utilities] RPG speed control unavailable: instruction page is protected\n");
        return false;
    }
    memcpy(s_rpgSpeedOperand, &speed, sizeof(speed));
    memcpy(s_rpgThrustOperand, &thrust, sizeof(thrust));
    __builtin___clear_cache((char *)begin, (char *)end);
    if (!RestoreProtection(begin, (size_t)(end - begin), oldProtect, true))
    {
        Msg("[ins_utilities] WARNING: could not restore RPG instruction-page protection\n");
        return false;
    }
    return true;
}
#endif

static bool InstallRpgSpeedPatch()
{
    unsigned char *speedInstruction = 0;
    unsigned char *thrustInstruction = 0;
#ifdef _LINUX
    static const unsigned char speedPattern[] = {
        0x8D,0x55,0xC8,0xB8,0x00,0x80,0x3B,0x45,0x89,0x44,
        0x24,0x08,0x8B,0x45,0x08,0x05,0x6C,0x04,0x00,0x00
    };
    static const unsigned char thrustPattern[] = {
        0x8B,0x55,0x08,0xB8,0x00,0x00,0x7A,0x44,0x89,0x82,
        0x98,0x04,0x00,0x00
    };
    speedInstruction = FindLinuxRpgSignature(speedPattern, sizeof(speedPattern),
        INSUTIL_LINUX_RPG_SPEED_SIGNATURE_RVA, "speed");
    thrustInstruction = FindLinuxRpgSignature(thrustPattern, sizeof(thrustPattern),
        INSUTIL_LINUX_RPG_THRUST_SIGNATURE_RVA, "thrust");
    if (!speedInstruction || !thrustInstruction)
    {
        Msg("[ins_utilities] RPG speed control unavailable: launch signature did not match in server_i486.so\n");
        return false;
    }
    s_rpgSpeedOperand = (unsigned long *)(speedInstruction + 4);
    s_rpgThrustOperand = (unsigned long *)(thrustInstruction + 4);
    s_rpgOriginalSpeedOperand = *s_rpgSpeedOperand;
    s_rpgOriginalThrustOperand = *s_rpgThrustOperand;
    if (s_rpgOriginalSpeedOperand != InsUtilFloatBits(3000.0f)
        || s_rpgOriginalThrustOperand != InsUtilFloatBits(1000.0f))
    {
        s_rpgSpeedOperand = NULL;
        s_rpgThrustOperand = NULL;
        Msg("[ins_utilities] RPG speed control unavailable: immediate values did not match\n");
        return false;
    }
    Msg("[ins_utilities] RPG speed control ready: Linux immediates 0x%X/0x%X verified\n",
        INSUTIL_LINUX_RPG_SPEED_IMMEDIATE_RVA,
        INSUTIL_LINUX_RPG_THRUST_IMMEDIATE_RVA);
    return true;
#else
    unsigned char *serverBase = (unsigned char *)GetModuleHandleA("server.dll");
    if (!serverBase)
    {
        Msg("[ins_utilities] RPG speed control unavailable: server.dll was not found\n");
        return false;
    }

    speedInstruction = serverBase + 0x1007E1;
    thrustInstruction = serverBase + 0x10082B;
    unsigned long expectedSpeed = (unsigned long)(serverBase + 0x1C33EC);
    unsigned long expectedThrust = (unsigned long)(serverBase + 0x1ABD74);
    unsigned long currentSpeed = *(unsigned long *)(speedInstruction + 2);
    unsigned long currentThrust = *(unsigned long *)(thrustInstruction + 2);
    if (speedInstruction[0] != 0xD9 || speedInstruction[1] != 0x05 || currentSpeed != expectedSpeed ||
        thrustInstruction[0] != 0xD9 || thrustInstruction[1] != 0x05 || currentThrust != expectedThrust)
    {
        Msg("[ins_utilities] RPG speed control unavailable: launch signature did not match\n");
        return false;
    }
    s_rpgSpeedOperand = (unsigned long *)(speedInstruction + 2);
    s_rpgThrustOperand = (unsigned long *)(thrustInstruction + 2);
    s_rpgOriginalSpeedOperand = *s_rpgSpeedOperand;
    s_rpgOriginalThrustOperand = *s_rpgThrustOperand;
    unsigned long oldProtect = 0;
    if (!MakeWritable(s_rpgSpeedOperand, sizeof(*s_rpgSpeedOperand), &oldProtect))
    {
        s_rpgSpeedOperand = NULL;
        s_rpgThrustOperand = NULL;
        Msg("[ins_utilities] RPG speed control unavailable: launch instruction is protected\n");
        return false;
    }
    *s_rpgSpeedOperand = (unsigned long)&s_rpgInternalLaunchSpeed;
    RestoreProtection(s_rpgSpeedOperand, sizeof(*s_rpgSpeedOperand), oldProtect, true);

    if (!MakeWritable(s_rpgThrustOperand, sizeof(*s_rpgThrustOperand), &oldProtect))
    {
        if (MakeWritable(s_rpgSpeedOperand, sizeof(*s_rpgSpeedOperand), &oldProtect))
        {
            *s_rpgSpeedOperand = s_rpgOriginalSpeedOperand;
            RestoreProtection(s_rpgSpeedOperand, sizeof(*s_rpgSpeedOperand), oldProtect, true);
        }
        s_rpgSpeedOperand = NULL;
        s_rpgThrustOperand = NULL;
        Msg("[ins_utilities] RPG speed control unavailable: thrust instruction is protected\n");
        return false;
    }
    *s_rpgThrustOperand = (unsigned long)&s_rpgInternalThrust;
    RestoreProtection(s_rpgThrustOperand, sizeof(*s_rpgThrustOperand), oldProtect, true);
    Msg("[ins_utilities] RPG speed control ready: physics launch and thrust paths patched\n");
    return true;
#endif
}

static void UninstallRpgSpeedPatch()
{
    if (!s_rpgSpeedOperand) return;
#ifdef _LINUX
    PatchLinuxRpgImmediates(s_rpgOriginalSpeedOperand, s_rpgOriginalThrustOperand);
#else
    unsigned long oldProtect = 0;
    if (MakeWritable(s_rpgSpeedOperand, sizeof(*s_rpgSpeedOperand), &oldProtect))
    {
        *s_rpgSpeedOperand = s_rpgOriginalSpeedOperand;
        RestoreProtection(s_rpgSpeedOperand, sizeof(*s_rpgSpeedOperand), oldProtect, true);
    }
    if (s_rpgThrustOperand && MakeWritable(s_rpgThrustOperand, sizeof(*s_rpgThrustOperand), &oldProtect))
    {
        *s_rpgThrustOperand = s_rpgOriginalThrustOperand;
        RestoreProtection(s_rpgThrustOperand, sizeof(*s_rpgThrustOperand), oldProtect, true);
    }
#endif
    s_rpgSpeedOperand = NULL;
    s_rpgThrustOperand = NULL;
}

static void UpdateRpgLaunchSpeed()
{
    float requestedSpeed = ins_rpg_speed.GetFloat();
    // VPhysics clamps the stock 3000 u/s request to about 2000 u/s. That is a
    // ceiling, not a scale factor, so configured speeds must pass through 1:1.
    s_rpgInternalLaunchSpeed = requestedSpeed > 0.0f ? requestedSpeed : 3000.0f;
    // Custom speeds are absolute rather than an initial velocity followed by
    // acceleration. Stock mode preserves the game's continuous rocket thrust.
    s_rpgInternalThrust = requestedSpeed > 0.0f ? 0.0f : 1000.0f;
#ifdef _LINUX
    if (s_rpgSpeedOperand && s_rpgThrustOperand)
    {
        unsigned long speed = InsUtilFloatBits(s_rpgInternalLaunchSpeed);
        unsigned long thrust = InsUtilFloatBits(s_rpgInternalThrust);
        if (*s_rpgSpeedOperand != speed || *s_rpgThrustOperand != thrust)
        {
            if (PatchLinuxRpgImmediates(speed, thrust))
                Msg("[ins_utilities] RPG Linux immediates applied: speed=%.1f thrust=%.1f\n",
                    s_rpgInternalLaunchSpeed, s_rpgInternalThrust);
        }
    }
#endif
}

// ============================================================================
// V_strncpy NULL-source guard
//
// CBasePlayer::GetNetworkIDString() calls V_strncpy(dest, id, 64) where `id`
// is whatever engine->GetPlayerNetworkIDString() returned. That engine call
// returns NULL by design when the edict is no longer resolvable among sv's
// clients (CVEngineServer::GetPlayerNetworkIDString has explicit NULL exits),
// and neither the caller nor V_strncpy checks. glibc's strncpy then reads from
// NULL and the server dies inside CEventLog::PrintPlayerEvent while logging
// player_death.
//
// V_strncpy lives in bin/vstdlib_i486.so and is reached by 167 relocations
// from server_i486.so, so replacing that one body protects every caller
// (server, engine, SourceMod and insbots alike). The replacement reimplements
// the shipping semantics exactly (hl2sdk-episode1/tier1/strtools.cpp:581-592)
// plus a NULL guard.
// ============================================================================
// V_strncpy sits on a shared .text page, so the page must keep PROT_EXEC while
// it is writable: srcds is multithreaded and other functions on the same page
// may run during the patch window. The existing MakeWritable() deliberately
// sets PROT_READ|PROT_WRITE only (correct for the RPG *data* operand patch),
// so this block needs its own exec-capable pair.
#ifdef _LINUX
static bool MakeWritableExec(void *addr, size_t size)
{
    return SetMemProtect(addr, size, PROT_READ | PROT_WRITE | PROT_EXEC);
}
#endif

static void *s_vstrncpyTarget = NULL;
static unsigned char s_vstrncpyOriginal[INSUTIL_V_STRNCPY_PATCH_LEN];
static bool s_vstrncpyPatched = false;
static unsigned int s_vstrncpyGuardedCalls = 0;

// cdecl, matching the target ABI (args on the stack, caller cleans up).
extern "C" void InsUtilVStrncpySafe(char *pDest, const char *pSrc, int maxLen)
{
    if (pSrc == NULL)
    {
        pSrc = "";
        ++s_vstrncpyGuardedCalls;
        if (s_vstrncpyGuardedCalls <= 5 || (s_vstrncpyGuardedCalls % 100) == 0)
            Msg("[ins_utilities] V_strncpy NULL source guarded (hit #%u)\n",
                s_vstrncpyGuardedCalls);
    }
    if (maxLen <= 0) return;
    strncpy(pDest, pSrc, (size_t)maxLen);
    pDest[maxLen - 1] = '\0';
}

#ifdef _LINUX
static void *FindVStrncpyTarget()
{
    InsModuleInfo m = {0};
    m.needle = "vstdlib_i486.so";
    dl_iterate_phdr(InsModuleCallback, &m);
    if (!m.phdrs) return NULL;
    return (void *)(m.base + INSUTIL_LINUX_V_STRNCPY_RVA);
}

static bool InstallVStrncpyGuard()
{
    if (s_vstrncpyPatched) return true;

    void *target = FindVStrncpyTarget();
    if (!target)
    {
        Msg("[ins_utilities] V_strncpy guard unavailable: vstdlib_i486.so not mapped\n");
        return false;
    }

    static const unsigned char expected[] = INSUTIL_V_STRNCPY_PROLOGUE;
    if (memcmp(target, expected, sizeof(expected)) != 0)
    {
        Msg("[ins_utilities] V_strncpy guard unavailable: prologue mismatch at %p\n", target);
        return false;
    }

    memcpy(s_vstrncpyOriginal, target, INSUTIL_V_STRNCPY_PATCH_LEN);

    // push imm32 ; ret   -> transfers to InsUtilVStrncpySafe without touching
    // any register (eax/ecx/edx/esi/edi/ebp are all preserved), so this is safe
    // to place at a function entry. Control never returns into the old body.
    unsigned char patch[INSUTIL_V_STRNCPY_PATCH_LEN];
    patch[0] = 0x68;
    unsigned long dest = (unsigned long)(uintptr_t)&InsUtilVStrncpySafe;
    memcpy(&patch[1], &dest, sizeof(dest));
    patch[5] = 0xC3;

    if (!MakeWritableExec(target, sizeof(patch)))
    {
        Msg("[ins_utilities] V_strncpy guard unavailable: .text page is protected\n");
        return false;
    }
    memcpy(target, patch, sizeof(patch));
    __builtin___clear_cache((char *)target, (char *)target + sizeof(patch));
    if (!RestoreProtection(target, sizeof(patch), 0 /* ignored on Linux */, true))
    {
        Msg("[ins_utilities] WARNING: could not restore V_strncpy .text page protection\n");
    }

    s_vstrncpyTarget = target;
    s_vstrncpyPatched = true;
    Msg("[ins_utilities] V_strncpy NULL guard installed (rva=0x%X target=%p)\n",
        INSUTIL_LINUX_V_STRNCPY_RVA, target);
    return true;
}

static void UninstallVStrncpyGuard()
{
    if (!s_vstrncpyPatched || !s_vstrncpyTarget) return;
    if (MakeWritableExec(s_vstrncpyTarget, INSUTIL_V_STRNCPY_PATCH_LEN))
    {
        memcpy(s_vstrncpyTarget, s_vstrncpyOriginal, INSUTIL_V_STRNCPY_PATCH_LEN);
        __builtin___clear_cache((char *)s_vstrncpyTarget,
                                (char *)s_vstrncpyTarget + INSUTIL_V_STRNCPY_PATCH_LEN);
        RestoreProtection(s_vstrncpyTarget, INSUTIL_V_STRNCPY_PATCH_LEN, 0 /* ignored on Linux */, true);
    }
    else
    {
        Msg("[ins_utilities] WARNING: could not restore V_strncpy (page protected)\n");
    }
    s_vstrncpyTarget = NULL;
    s_vstrncpyPatched = false;
}

// Mirrors UpdateRpgLaunchSpeed(): reconciled once per frame so the cvar can be
// toggled at runtime. Configs execute after Load(), so the cvar test must not
// live in Load().
static void UpdateVStrncpyGuard()
{
    if (ins_fix_strncpy_null.GetBool())
    {
        if (!s_vstrncpyPatched) InstallVStrncpyGuard();
    }
    else if (s_vstrncpyPatched)
    {
        UninstallVStrncpyGuard();
        Msg("[ins_utilities] V_strncpy NULL guard removed (fix_strncpy_null 0)\n");
    }
}
#else
// Windows: server.dll does not import V_strncpy at all. Its imports are
// KERNEL32/USER32/tier0/vstdlib{GetCVarIF, CUniformRandomStream::ctor,
// RandomFloat, KeyValuesSystem, RandomSeed, RandomInt}; the literal strings
// "V_strncpy", "Q_strncpy" and "strncpy" are absent. The helper is statically
// linked and/or inlined there, so there is no single shared target address and
// nothing to patch. Do not patch blind.
static bool InstallVStrncpyGuard()
{
    Msg("[ins_utilities] V_strncpy NULL guard unavailable on this platform "
        "(V_strncpy is statically linked into server.dll)\n");
    return false;
}

static void UninstallVStrncpyGuard() {}
static void UpdateVStrncpyGuard() {}
#endif
void Cmd_InsUtil()
{
    int argc = engine->Cmd_Argc();
    if (argc < 2)
    {
        Msg("[ins_utilities] Available commands:\n");
        Msg("  ins_util list           list all settings\n");
        Msg("  ins_util set <name> <v> change a setting\n");
        Msg("  ins_util get <name>     show one setting\n");
        Msg("  ins_util version        show plugin version\n");
        Msg("  ins_util version_raw    show raw plugin version string\n");
        return;
    }
    
    const char *sub = engine->Cmd_Argv(1);
    if (!_stricmp(sub, "list"))
    {
        for (CfgVar *c = CfgVar::s_head; c; c = c->m_next)
            Msg("[ins_utilities] %s = %s  (%s)\n", c->Name(), c->GetString(), c->Help());
        return;
    }
    if (!_stricmp(sub, "get"))
    {
        CfgVar *c = (argc > 2) ? CfgFind(engine->Cmd_Argv(2)) : 0;
        if (c) Msg("[ins_utilities] %s = %s\n", c->Name(), c->GetString());
        else   Msg("[ins_utilities] unknown setting\n");
        return;
    }
    if (!_stricmp(sub, "version"))
    {
        Msg("[ins_utilities] version %s\n", INSUTIL_VERSION);
        return;
    }
    if (!_stricmp(sub, "version_raw"))
    {
        Msg("%s\n", INSUTIL_VERSION);
        return;
    }
    if (!_stricmp(sub, "set"))
    {
        if (argc < 4) { Msg("[ins_utilities] usage: ins_util set <name> <value>\n"); return; }
        CfgVar *c = CfgFind(engine->Cmd_Argv(2));
        if (!c) { Msg("[ins_utilities] unknown setting '%s'\n", engine->Cmd_Argv(2)); return; }
        c->SetValue(engine->Cmd_Argv(3));
        Msg("[ins_utilities] %s = %s\n", c->Name(), c->GetString());
        if (!_stricmp(c->Name(), "keep_doors_open") && c->GetBool())
        {
            s_bDoorsUpdated = false;
        }
        if (!_stricmp(c->Name(), "rpg_speed"))
            UpdateRpgLaunchSpeed();
        if (!_stricmp(c->Name(), "door_speed"))
            ApplyDoorSpeed();
        if (!_stricmp(c->Name(), "mute_killfeed"))
        {
            if (c->GetBool())
                InstallFireEventHook();
            else
        UninstallFireEventHook();
        }
        return;
    }
}
static ConCommand ins_util_cmd("ins_util", Cmd_InsUtil, "Insurgency Utilities settings");

static int s_offset_flStartReinforcementTime = -1;
static int s_offset_bReinforcement = -1;
static int s_offset_iReinforcementsLeft = -1;
static int s_offset_iTeamNum = -1;

static float s_flPlayerDieTime[65] = {0};
static float s_flNextReinforcementResolve = 0.0f;

int FindSendPropOffset(SendTable *pTable, const char *pPropName)
{
    if (!pTable) return -1;
    for (int i = 0; i < pTable->GetNumProps(); i++)
    {
        SendProp *pProp = pTable->GetProp(i);
        if (strcmp(pProp->GetName(), pPropName) == 0)
            return pProp->GetOffset();
        
        if (pProp->GetDataTable())
        {
            int offset = FindSendPropOffset(pProp->GetDataTable(), pPropName);
            if (offset >= 0)
                return pProp->GetOffset() + offset;
        }
    }
    return -1;
}

static bool ReinforcementOffsetsReady()
{
    return s_offset_flStartReinforcementTime > 0 && s_offset_iTeamNum > 0;
}

// LevelInit runs before CPlayTeam exists on the Windows server. Before v0.5.3,
// a non-simulating GameFrame accidentally called LevelInit again and eventually
// found the team sendtable. Removing that state-reset side effect exposed the
// ordering bug: every Windows offset stayed at -1, so instant_respawn never ran.
// Resolve explicitly at ServerActivate and retry while the feature is enabled.
static bool ResolveReinforcementOffsets()
{
    if (ReinforcementOffsetsReady())
        return true;
    if (!engine)
        return false;

    for (int i = 1; i <= engine->GetEntityCount(); i++)
    {
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (!e || e->IsFree()) continue;
        IServerUnknown *unk = e->GetUnknown();
        if (!unk) continue;
        IServerNetworkable *net = unk->GetNetworkable();
        if (!net) continue;

        ServerClass *sc = net->GetServerClass();
        if (!sc || (strcmp(sc->GetName(), "CPlayTeam") != 0
                    && strcmp(sc->GetName(), "CINSTeam") != 0))
            continue;

        int start = FindSendPropOffset(sc->m_pTable, "m_flStartReinforcementTime");
#ifdef _LINUX
        int reinforce = FindSendPropOffset(sc->m_pTable, "m_bEmergencyReinforcement");
#else
        int reinforce = FindSendPropOffset(sc->m_pTable, "m_bReinforcement");
        if (reinforce < 0)
            reinforce = FindSendPropOffset(sc->m_pTable, "m_bEmergencyReinforcement");
#endif
        int left = FindSendPropOffset(sc->m_pTable, "m_iReinforcementsLeft");
        int team = FindSendPropOffset(sc->m_pTable, "m_iTeamNum");

#ifdef _LINUX
        if (start <= 0) start = INSUTIL_LINUX_M_FL_START_REINFORCEMENT_TIME;
        if (reinforce <= 0) reinforce = INSUTIL_LINUX_M_B_EMERGENCY_REINFORCEMENT;
        if (left <= 0) left = INSUTIL_LINUX_M_I_REINFORCEMENTS_LEFT;
        if (team <= 0) team = INSUTIL_LINUX_M_I_TEAM_NUM;
#endif

        if (start <= 0 || team <= 0)
            continue;

        s_offset_flStartReinforcementTime = start;
        s_offset_bReinforcement = reinforce;
        s_offset_iReinforcementsLeft = left;
        s_offset_iTeamNum = team;
        Msg("[ins_utilities] Found CPlayTeam offsets: StartTime=%d, bReinforce=%d, Left=%d, TeamNum=%d\n",
            start, reinforce, left, team);
        return true;
    }
    return false;
}

static bool ResolveWeaponOwnerOffset()
{
    if (!engine)
        return false;
    for (int i = 1; i <= engine->GetEntityCount(); i++)
    {
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (!e || e->IsFree()) continue;
        IServerUnknown *unk = e->GetUnknown();
        if (!unk) continue;
        IServerNetworkable *net = unk->GetNetworkable();
        if (!net) continue;
        ServerClass *sc = net->GetServerClass();
        if (!sc) continue;
        const char *name = sc->GetName();
        if (name && (!strncmp(name, "CWeapon", 7) || !strncmp(name, "CINSWeapon", 10) || strstr(name, "CombatWeapon")))
        {
            int off = FindSendPropOffset(sc->m_pTable, "m_hOwner");
            if (off > 0)
            {
                s_offset_m_hOwner = off;
                return true;
            }
        }
    }
    return false;
}

static bool s_bMapSwitchIssued = false;
static char s_szCurrentMap[64] = {0};
static void **s_pGameRulesGlobal = NULL;
static bool s_bSuddenDeathResolutionFailed = false;
static bool s_bSuddenDeathWarningLogged = false;
static bool s_bSuddenDeathActive = false;
static float s_flSuddenDeathStartTime = -1.0f;

static void SuddenDeathWarning(const char *reason)
{
    if (s_bSuddenDeathWarningLogged)
        return;
    s_bSuddenDeathWarningLogged = true;
    Msg("[ins_utilities] WARNING: sudden-death failsafe unavailable: %s\n",
        reason ? reason : "unknown reason");
}

static bool ResolveGameRulesGlobal()
{
    if (s_pGameRulesGlobal)
        return true;
    if (s_bSuddenDeathResolutionFailed)
        return false;

#ifdef _LINUX
    s_pGameRulesGlobal = (void **)dlsym(RTLD_DEFAULT, "g_pGameRules");
    if (!s_pGameRulesGlobal)
    {
        s_bSuddenDeathResolutionFailed = true;
        SuddenDeathWarning("g_pGameRules is not exported by server_i486.so");
        return false;
    }
#else
    // CreateGameRulesObject begins by loading g_pGameRules. This signature is
    // from the newer supported Source 2006 Insurgency server.dll build; the
    // four immediate bytes are the address of the global CINSRules pointer.
    static const unsigned char signature[] = {
        0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x85, 0xC9,
        0x74, 0x00, 0x8B, 0x01, 0x6A, 0x01, 0xFF, 0x50
    };
    static const char mask[] = "xx????xxx?xxxxxx";
    unsigned char *match = FindWindowsTextPattern(signature, mask, sizeof(signature));
    if (match)
    {
        unsigned long globalAddress = *(unsigned long *)(match + 2);
        if (globalAddress)
            s_pGameRulesGlobal = (void **)globalAddress;
    }
    else
    {
        s_pGameRulesGlobal = FindWindowsGameRulesGlobalFromModeAccessors();
        if (!s_pGameRulesGlobal)
        {
            s_bSuddenDeathResolutionFailed = true;
            SuddenDeathWarning("verified game-rules signatures did not match server.dll");
            return false;
        }
        Msg("[ins_utilities] sudden-death resolver using legacy Windows mode accessors\n");
    }

    if (!s_pGameRulesGlobal)
    {
        s_bSuddenDeathResolutionFailed = true;
        SuddenDeathWarning("CreateGameRulesObject returned a null g_pGameRules address");
        return false;
    }
#endif

    Msg("[ins_utilities] sudden-death state resolver ready\n");
    return true;
}

static bool ReadSuddenDeathState(bool *available)
{
    if (available)
        *available = false;

    if (!ResolveGameRulesGlobal())
        return false;

    void *rules = s_pGameRulesGlobal ? *s_pGameRulesGlobal : NULL;
    if (!rules)
        return false; // GameRules may not exist during early level startup.

    int modeIndex = *(int *)((char *)rules + INSUTIL_RUNNING_MODE_INDEX_OFFSET);
    if (modeIndex != 2)
    {
        if (available)
            *available = true;
        return false; // In INS:MIC (CINSRules::IsModeRunning / RunningMode), mode 2 is the active running mode that supports round extension. Modes 0 and 1 have no CRunningMode instance.
    }

    void *mode = *(void **)((char *)rules + INSUTIL_RUNNING_MODE_TABLE_OFFSET
                            + 2 * (int)sizeof(void *));
    if (!mode)
        return false; // Transient while the game is constructing the mode.

    if (available)
        *available = true;
    return *(unsigned char *)((char *)mode + INSUTIL_RUNNING_MODE_EXTENDED_OFFSET) != 0;
}

static bool IsSafeMapToken(const char *map)
{
    if (!map || !*map)
        return false;

    for (const char *p = map; *p; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (c <= ' ' || c == '"' || c == ';' || c == '\\')
            return false;
    }
    return true;
}

static bool IsUsableMap(const char *map, bool avoidCurrent)
{
    if (!IsSafeMapToken(map))
        return false;
    if (avoidCurrent && s_szCurrentMap[0] && !_stricmp(map, s_szCurrentMap))
        return false;
    return !engine || engine->IsMapValid(map) != 0;
}

static int LoadMapCycle(char maps[64][64])
{
    FILE *f = fopen("insurgency/mapcycle.txt", "r");
    if (!f)
    {
        Msg("[ins_utilities] Error: Could not open insurgency/mapcycle.txt\n");
        return 0;
    }

    int count = 0;
    char line[128];
    while (count < 64 && fgets(line, sizeof(line), f))
    {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        size_t length = strlen(p);
        while (length > 0 && (p[length - 1] == '\n' || p[length - 1] == '\r'
            || p[length - 1] == ' ' || p[length - 1] == '\t'))
            p[--length] = '\0';

        if (!*p || p[0] == '/' || p[0] == '#')
            continue;

        char *comment = strstr(p, "//");
        if (comment)
        {
            *comment = '\0';
            length = strlen(p);
            while (length > 0 && (p[length - 1] == ' ' || p[length - 1] == '\t'))
                p[--length] = '\0';
        }

        if (length == 0 || length >= sizeof(maps[0]) || !IsSafeMapToken(p))
            continue;

        strncpy(maps[count], p, sizeof(maps[count]) - 1);
        maps[count][sizeof(maps[count]) - 1] = '\0';
        count++;
    }
    fclose(f);
    return count;
}

static bool SelectRandomMapFromCycle(char *selected, size_t selectedSize)
{
    if (!selected || selectedSize == 0)
        return false;

    char maps[64][64];
    int count = LoadMapCycle(maps);
    if (count <= 0)
        return false;

    int valid[64];
    int nonCurrent[64];
    int validCount = 0;
    int nonCurrentCount = 0;
    for (int i = 0; i < count; i++)
    {
        if (!IsUsableMap(maps[i], false))
            continue;
        valid[validCount++] = i;
        if (!s_szCurrentMap[0] || _stricmp(maps[i], s_szCurrentMap) != 0)
            nonCurrent[nonCurrentCount++] = i;
    }

    if (validCount == 0)
    {
        Msg("[ins_utilities] No valid maps were found in insurgency/mapcycle.txt\n");
        return false;
    }

    srand((unsigned int)time(NULL));
    int chosenIndex = nonCurrentCount > 0
        ? nonCurrent[rand() % nonCurrentCount]
        : valid[rand() % validCount];
    strncpy(selected, maps[chosenIndex], selectedSize - 1);
    selected[selectedSize - 1] = '\0';
    return true;
}

static bool GetMapChooserNextMap(char *selected, size_t selectedSize)
{
    if (!selected || selectedSize == 0 || !g_pCVar)
        return false;

    ConVar *nextMap = g_pCVar->FindVar("sm_nextmap");
    if (!nextMap)
        return false;

    const char *value = nextMap->GetString();
    if (!IsUsableMap(value, true))
        return false;

    strncpy(selected, value, selectedSize - 1);
    selected[selectedSize - 1] = '\0';
    return true;
}

static bool IssueMapChange(const char *map, const char *reason)
{
    if (!engine || !IsUsableMap(map, false))
        return false;

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "changelevel %s\n", map);
    engine->ServerCommand(cmd);
    Msg("[ins_utilities] %s: changing to %s\n",
        reason ? reason : "Map switch", map);
    return true;
}

static bool SwitchToRandomMap(const char *reason)
{
    char selected[64];
    if (!SelectRandomMapFromCycle(selected, sizeof(selected)))
        return false;
    return IssueMapChange(selected, reason);
}

static bool SwitchToMapChooserOrRandomMap(const char *reason)
{
    char selected[64];
    if (GetMapChooserNextMap(selected, sizeof(selected)))
    {
        Msg("[ins_utilities] map switch choice: using MapChooser sm_nextmap (%s)\n",
            selected);
        return IssueMapChange(selected, reason ? reason : "Map switch (MapChooser)");
    }

    if (SelectRandomMapFromCycle(selected, sizeof(selected)))
    {
        Msg("[ins_utilities] map switch choice: MapChooser unavailable or stale; using random mapcycle entry (%s)\n",
            selected);
        return IssueMapChange(selected, reason ? reason : "Map switch (random mapcycle fallback)");
    }

    Msg("[ins_utilities] map switch requested, but no usable next map was found\n");
    return false;
}

static bool SwitchToMapChooserOrRandomMap()
{
    return SwitchToMapChooserOrRandomMap("Sudden-death limit reached (MapChooser)");
}

static int CountTotalClients()
{
    if (!engine || !playerinfomanager || !gpGlobals)
        return 0;
    int count = 0;
    for (int i = 1; i <= gpGlobals->maxClients; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (!e || e->IsFree()) continue;
        IPlayerInfo *pInfo = playerinfomanager->GetPlayerInfo(e);
        if (pInfo && pInfo->IsConnected())
            count++;
    }
    return count;
}

static bool s_bIdleTimerActive = false;
static double s_flIdleStartTime = -1.0;

static void ResetIdleTimer()
{
    s_bIdleTimerActive = false;
    s_flIdleStartTime = -1.0;
}

static void UpdateIdleFailsafe()
{
    float limit = ins_force_map_switch_idle.GetFloat();
    if (limit <= 0.0f)
    {
        ResetIdleTimer();
        return;
    }

    int clientCount = CountTotalClients();
    if (clientCount > 0)
    {
        if (s_bIdleTimerActive)
            Msg("[ins_utilities] client active (%d present); idle map switch timer reset\n", clientCount);
        ResetIdleTimer();
        return;
    }

    // 0 players and 0 bots on server
    double now = Plat_FloatTime();
    if (!s_bIdleTimerActive || s_flIdleStartTime < 0.0)
    {
        s_bIdleTimerActive = true;
        s_flIdleStartTime = now;
        Msg("[ins_utilities] empty server detected (0 players/bots); idle map switch timer started at %.1f seconds\n",
            limit);
    }

    if (!s_bMapSwitchIssued && s_flIdleStartTime >= 0.0 && (now - s_flIdleStartTime) >= (double)limit)
    {
        s_bMapSwitchIssued = true;
        Msg("[ins_utilities] idle limit reached (%.1f seconds empty)\n", limit);
        SwitchToMapChooserOrRandomMap("Idle limit reached (empty server)");
    }
}

static void ResetSuddenDeathTimer()
{
    s_bSuddenDeathActive = false;
    s_flSuddenDeathStartTime = -1.0f;
}

static void UpdateSuddenDeathFailsafe()
{
    float limit = ins_sudden_death_map_switch.GetFloat();
    if (limit <= 0.0f || !gpGlobals)
    {
        ResetSuddenDeathTimer();
        return;
    }

    bool stateAvailable = false;
    bool suddenDeath = ReadSuddenDeathState(&stateAvailable);
    if (!stateAvailable)
    {
        ResetSuddenDeathTimer();
        return;
    }

    if (!suddenDeath)
    {
        if (s_bSuddenDeathActive)
            Msg("[ins_utilities] sudden-death state ended; timer reset\n");
        ResetSuddenDeathTimer();
        return;
    }

    if (!s_bSuddenDeathActive)
    {
        s_bSuddenDeathActive = true;
        s_flSuddenDeathStartTime = gpGlobals->curtime;
        Msg("[ins_utilities] sudden death entered; map switch timer started at %.1f seconds\n",
            limit);
    }

    if (!s_bMapSwitchIssued && s_flSuddenDeathStartTime >= 0.0f
        && gpGlobals->curtime - s_flSuddenDeathStartTime >= limit)
    {
        s_bMapSwitchIssued = true;
        SwitchToMapChooserOrRandomMap();
    }
}

void SwitchToRandomMap()
{
    SwitchToRandomMap("Failsafe timer reached");
}

class CInsUtilitiesPlugin : public IServerPluginCallbacks
{
public:
    virtual bool Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn gameServerFactory)
    {
        engine = (IVEngineServer *)interfaceFactory(INTERFACEVERSION_VENGINESERVER, NULL);
        helpers = (IServerPluginHelpers *)interfaceFactory(INTERFACEVERSION_ISERVERPLUGINHELPERS, NULL);
        g_pCVar = (ICvar *)interfaceFactory(VENGINE_CVAR_INTERFACE_VERSION, NULL);
        
        enginetrace = (IEngineTrace *)interfaceFactory(INTERFACEVERSION_ENGINETRACE_SERVER, NULL);
        playerinfomanager = (IPlayerInfoManager *)gameServerFactory(INTERFACEVERSION_PLAYERINFOMANAGER, NULL);
        
        if (playerinfomanager) 
            gpGlobals = playerinfomanager->GetGlobalVars();

        if (g_pCVar)
        {
            ins_util_cmd.AddFlags(FCVAR_PLUGIN);
            g_pCVar->RegisterConCommandBase(&ins_util_cmd);
        }

        s_pEventManager = (IGameEventManager2 *)interfaceFactory(INTERFACEVERSION_GAMEEVENTSMANAGER2, NULL);
        // Only touch the engine vtable when the feature is actually enabled.
        // Configs execute after Load, so normally the hook installs when the
        // user runs "ins_util set mute_killfeed 1".
        if (s_pEventManager && ins_mute_killfeed.GetBool())
            InstallFireEventHook();
        InstallRpgSpeedPatch();
        InstallVStrncpyGuard();
        // The voice-read clamp is a security fix and defaults on; unlike the
        // V_strncpy guard it also resolves on Windows (engine.dll is patched by
        // signature there).  A build whose handler shape does not match is
        // refused with a log line rather than patched blind.
        if (ins_voice_fix.GetBool())
            VoiceFixInstall();
#ifdef _LINUX
        Msg("[ins_utilities] Linux offsets: team=0x%X start=0x%X emergency=0x%X left=0x%X owner=0x%X\n",
            s_offset_iTeamNum, s_offset_flStartReinforcementTime,
            s_offset_bReinforcement, s_offset_iReinforcementsLeft,
            s_offset_m_hOwner);
        Msg("[ins_utilities] Linux door offsets: spawnflags=0x%X wait=0x%X auto_return=0x%X; FireEvent slot=%d rva=0x%X\n",
            INSUTIL_LINUX_M_SPAWNFLAGS, INSUTIL_LINUX_M_FL_WAIT,
            INSUTIL_LINUX_M_FL_AUTO_RETURN_DELAY,
            INSUTIL_LINUX_FIRE_EVENT_SLOT, INSUTIL_LINUX_FIRE_EVENT_RVA);
#else
        Msg("[ins_utilities] Windows offsets: team=0x%X start=0x%X emergency=0x%X left=0x%X owner=0x%X (%d)\n",
            s_offset_iTeamNum, s_offset_flStartReinforcementTime,
            s_offset_bReinforcement, s_offset_iReinforcementsLeft,
            s_offset_m_hOwner, s_offset_m_hOwner);
#endif
        Msg("[ins_utilities] v%s Loaded!\n", INSUTIL_VERSION);
        return true;
    }

    virtual void Unload(void) 
    { 
        UninstallRpgSpeedPatch();
        UninstallVStrncpyGuard();
        VoiceFixRemove();
        UninstallFireEventHook();
        if (g_pCVar) g_pCVar->UnlinkVariables(FCVAR_PLUGIN);
        Msg("[ins_utilities] Unloaded!\n"); 
    }
    virtual void Pause(void) {}
    virtual void UnPause(void) {}
    virtual const char *GetPluginDescription(void) { return "Insurgency Utilities " INSUTIL_VERSION; }
    
    virtual void LevelInit(char const *pMapName) 
    {
        for (int i = 0; i < 65; i++) s_flPlayerDieTime[i] = 0.0f;
        s_flLastCleanupTime = 0.0f;
        s_flMapStartTime = 0.0f;
        s_flLastProximityCheckTime = 0.0f;
        memset(s_doorCooldown, 0, sizeof(s_doorCooldown));
        memset(s_playerLastTime, 0, sizeof(s_playerLastTime));
        s_bDoorsUpdated = false;
        s_bMapSwitchIssued = false;
        s_flNextReinforcementResolve = 0.0f;
        ResetSuddenDeathTimer();
        ResetIdleTimer();
        if (pMapName)
        {
            strncpy(s_szCurrentMap, pMapName, sizeof(s_szCurrentMap) - 1);
            s_szCurrentMap[sizeof(s_szCurrentMap) - 1] = '\0';
        }
        else
        {
            s_szCurrentMap[0] = '\0';
        }
        
        ResolveReinforcementOffsets();
    }
    
    virtual void ServerActivate(edict_t *pEdictList, int edictCount, int clientMax)
    {
        (void)pEdictList; (void)edictCount; (void)clientMax;
        // All map entities exist here. Applying once at activation also works
        // on an empty/frozen server where simulation time is not advancing.
        ResolveReinforcementOffsets();
        ResolveWeaponOwnerOffset();
        if (ins_keep_doors_open.GetBool())
            ApplyKeepDoorsOpen();
        if (ins_door_speed.GetFloat() > 0.0f)
            ApplyDoorSpeed();
    }
    
    virtual void GameFrame(bool simulating)
    {
        UpdateIdleFailsafe();
        UpdateDoorProximityOpen();
        UpdateDoorSmoothness();
        UpdateWeaponDropAndSwap();

        if (simulating && gpGlobals)
        {
            if (s_flMapStartTime == 0.0f) 
                s_flMapStartTime = gpGlobals->curtime;
                
            float switchTime = ins_force_map_switch.GetFloat();
            if (switchTime > 0.0f && s_flMapStartTime > 0.0f
                && (gpGlobals->curtime - s_flMapStartTime) >= switchTime)
            {
                s_flMapStartTime = -1.0f; // Prevent re-triggering
                if (!s_bMapSwitchIssued)
                {
                    s_bMapSwitchIssued = true;
                    SwitchToRandomMap();
                }
            }

            UpdateSuddenDeathFailsafe();
            
            if (ins_keep_doors_open.GetBool() && !s_bDoorsUpdated && s_flMapStartTime > 0.0f && (gpGlobals->curtime - s_flMapStartTime) >= 1.0f)
            {
                ApplyKeepDoorsOpen();
            }

            UpdateRpgLaunchSpeed();
            UpdateVStrncpyGuard();
            VoiceFixUpdate(ins_voice_fix.GetBool());

            bool cleanWeapons = ins_cleanup_weapons.GetBool();
            bool cleanRagdolls = ins_cleanup_ragdolls.GetBool();
            bool cleanBlood = ins_cleanup_blood.GetBool();
            bool keepDoors = ins_keep_doors_open.GetBool();
            
            if ((cleanWeapons || cleanRagdolls || cleanBlood || keepDoors) && gpGlobals->curtime - s_flLastCleanupTime >= ins_cleanup_interval.GetFloat())
            {
                s_flLastCleanupTime = gpGlobals->curtime;
                
                if (keepDoors)
                {
                    ApplyKeepDoorsOpen();
                }

                if (cleanBlood)
                {
                    for (int i = 1; i <= gpGlobals->maxClients; i++)
                    {
                        edict_t *e = engine->PEntityOfEntIndex(i);
                        if (e && !e->IsFree())
                        {
                            IPlayerInfo *pInfo = playerinfomanager->GetPlayerInfo(e);
                            if (pInfo && pInfo->IsConnected() && !pInfo->IsFakeClient())
                            {
                                engine->ClientCommand(e, "r_cleardecals\n");
                            }
                        }
                    }
                }
                
                if (cleanWeapons || cleanRagdolls)
                {
                    int cleanedWeaponsCount = 0;
                    int cleanedRagdollsCount = 0;
                    int totalPool = engine->GetEntityCount();
                    int activeCount = 0;
                    int weaponCount = 0;
                    int otherCount = 0;
                    char sampleOther[128] = "";

                    // Build lookup of weapons currently carried by living players/bots
                    bool isCarriedByLivingPlayer[2048] = { false };
                    if (cleanWeapons && playerinfomanager)
                    {
#ifdef _LINUX
                        // Audited in Linux server_i486.so SendTable DT_BaseCombatCharacter:
                        // 0x0A30 is m_flNextAttack (float), 0x0A38 is m_hMyWeapons (7 handles).
                        int offset_mywep = 0x0A38;
#else
                        // Audited in Windows server.dll SendTable DT_BaseCombatCharacter:
                        // 0x0A24 is m_hMyWeapons (7 handles).
                        int offset_mywep = 0x0A24;
#endif
                        for (int client = 1; client <= gpGlobals->maxClients; client++)
                        {
                            edict_t *pe = engine->PEntityOfEntIndex(client);
                            if (!pe || pe->IsFree()) continue;
                            IPlayerInfo *info = playerinfomanager->GetPlayerInfo(pe);
                            if (!info || !info->IsConnected() || info->IsDead()) continue;

                            IServerUnknown *punk = pe->GetUnknown();
                            if (!punk) continue;
                            CBaseEntity *pPlayer = punk->GetBaseEntity();
                            if (!pPlayer) continue;

                            // INS:MIC player inventory holds up to 7 weapons (0x1C bytes).
                            // Limit scan to 7 slots so we do not read adjacent member variables.
                            for (int w = 0; w < 7; w++)
                            {
                                unsigned long hWpn = *(unsigned long *)((char *)pPlayer + offset_mywep + w * 4);
                                int wpnIdx = hWpn & 0xFFF;
                                if (wpnIdx > gpGlobals->maxClients && wpnIdx < 2048)
                                {
                                    isCarriedByLivingPlayer[wpnIdx] = true;
                                }
                            }
                        }
                    }

                    for (int i = 1; i <= totalPool; i++)
                    {
                        edict_t *e = engine->PEntityOfEntIndex(i);
                        if (!e || e->IsFree()) continue;
                        activeCount++;

                        const char *wcn = e->GetClassName();
                        if (wcn && strncmp(wcn, "weapon_", 7) == 0)
                            weaponCount++;
                        else
                        {
                            otherCount++;
                            if (!sampleOther[0] && i > gpGlobals->maxClients && wcn)
                                strncpy(sampleOther, wcn, sizeof(sampleOther) - 1);
                        }

                        if (i <= gpGlobals->maxClients) continue;

                        IServerUnknown *unk = e->GetUnknown();
                        if (!unk) continue;
                        IServerNetworkable *net = unk->GetNetworkable();
                        if (!net) continue;
                        ServerClass *sc = net->GetServerClass();
                        if (!sc) continue;
                        
                        const char *cname = sc->GetName();
                        if (cleanRagdolls && strstr(cname, "Ragdoll"))
                        {
                            engine->RemoveEdict(e);
                            cleanedRagdollsCount++;
                            continue;
                        }
                        
                        bool isWeapon = false;
                        if (cleanWeapons)
                        {
                            if (wcn && strncmp(wcn, "weapon_", 7) == 0)
                            {
                                isWeapon = true;
                            }
                            else if (cname && (strncmp(cname, "CWeapon", 7) == 0 || strstr(cname, "Weapon") || strstr(cname, "Item")))
                            {
                                if (strcmp(cname, "CINSWeaponCache") != 0)
                                    isWeapon = true;
                            }
                        }

                        if (isWeapon)
                        {
                            if (!isCarriedByLivingPlayer[i])
                            {
                                engine->RemoveEdict(e);
                                cleanedWeaponsCount++;
                            }
                        }
                    }
                    if (cleanedWeaponsCount > 0 || cleanedRagdollsCount > 0 || activeCount > 1000)
                    {
                        int currentActive = activeCount - cleanedWeaponsCount - cleanedRagdollsCount;
                        int survivingWeapons = weaponCount - cleanedWeaponsCount;
                        Msg("[ins_utilities] Battlefield sweep: removed %d dropped weapon(s), %d ragdoll(s) [Active: %d, Free: %d, HighWater: %d/2048] (Census: %d live wpns, %d other [%s])\n",
                            cleanedWeaponsCount, cleanedRagdollsCount, currentActive, 2048 - currentActive, totalPool,
                            survivingWeapons, otherCount, sampleOther);
                    }
                }
            }

            if (ins_instant_respawn.GetBool() && !ReinforcementOffsetsReady()
                && gpGlobals->curtime >= s_flNextReinforcementResolve)
            {
                s_flNextReinforcementResolve = gpGlobals->curtime + 1.0f;
                ResolveReinforcementOffsets();
            }

            if (ins_instant_respawn.GetBool() && ReinforcementOffsetsReady() && engine)
            {
                bool bForceWave[4] = {false, false, false, false};
                
                for (int i = 1; i <= gpGlobals->maxClients; i++)
                {
                    edict_t *e = engine->PEntityOfEntIndex(i);
                    if (!e || e->IsFree()) 
                    {
                        s_flPlayerDieTime[i] = 0.0f;
                        continue;
                    }
                    
                    IPlayerInfo *pInfo = playerinfomanager->GetPlayerInfo(e);
                    if (pInfo && pInfo->IsConnected())
                    {
                        int team = pInfo->GetTeamIndex();
                        if (team > 0 && team < 4)
                        {
                            if (pInfo->IsDead())
                            {
                                if (s_flPlayerDieTime[i] == 0.0f)
                                {
                                    s_flPlayerDieTime[i] = gpGlobals->curtime;
                                }
                                else if (gpGlobals->curtime - s_flPlayerDieTime[i] >= 3.0f)
                                {
                                    bForceWave[team] = true;
                                }
                            }
                            else
                            {
                                s_flPlayerDieTime[i] = 0.0f;
                            }
                        }
                    }
                    else
                    {
                        s_flPlayerDieTime[i] = 0.0f;
                    }
                }
                
                for (int i = 1; i <= engine->GetEntityCount(); i++)
                {
                    edict_t *e = engine->PEntityOfEntIndex(i);
                    if (!e || e->IsFree()) continue;
                    
                    IServerUnknown *unk = e->GetUnknown();
                    if (!unk) continue;
                    
                    IServerNetworkable *net = unk->GetNetworkable();
                    if (!net) continue;
                    
                    ServerClass *sc = net->GetServerClass();
                    if (!sc) continue;
                    
                    if (strcmp(sc->GetName(), "CPlayTeam") == 0 || strcmp(sc->GetName(), "CINSTeam") == 0)
                    {
                        CBaseEntity *teamEnt = unk->GetBaseEntity();
                        if (teamEnt && s_offset_iTeamNum > 0)
                        {
                            char *base = (char *)teamEnt;
                            int teamNum = *(int *)(base + s_offset_iTeamNum);
                            if (teamNum > 0 && teamNum < 4 && bForceWave[teamNum])
                            {
                                *(float *)(base + s_offset_flStartReinforcementTime) = gpGlobals->curtime - 9999.0f;
                                if (s_offset_bReinforcement > 0)
                                    *(bool *)(base + s_offset_bReinforcement) = true;
                                if (s_offset_iReinforcementsLeft > 0)
                                    *(int *)(base + s_offset_iReinforcementsLeft) = 99;
                            }
                        }
                    }
                }
            }
        }
    }
    
    virtual void LevelShutdown(void) {}
    virtual void ClientActive(edict_t *pEntity) {}
    virtual void ClientDisconnect(edict_t *pEntity) {
        int idx = engine->IndexOfEdict(pEntity);
        if (idx >= 0 && idx < 65) s_flPlayerDieTime[idx] = 0.0f;
    }
    virtual void ClientPutInServer(edict_t *pEntity, char const *playername) {}
    virtual void SetCommandClient(int index) {}
    virtual void ClientSettingsChanged(edict_t *pEdict) {}
    virtual PLUGIN_RESULT ClientConnect(bool *bAllowConnect, edict_t *pEntity, const char *pszName, const char *pszAddress, char *reject, int maxrejectlen) { return PLUGIN_CONTINUE; }
    virtual PLUGIN_RESULT ClientCommand(edict_t *pEntity) { return PLUGIN_CONTINUE; }
    virtual PLUGIN_RESULT NetworkIDValidated(const char *pszUserName, const char *pszNetworkID) { return PLUGIN_CONTINUE; }
};

CInsUtilitiesPlugin g_UtilitiesPlugin;

#ifdef _LINUX
extern "C" __attribute__((visibility("default"))) void *CreateInterface(const char *pName, int *pReturnCode)
#else
extern "C" __declspec(dllexport) void *CreateInterface(const char *pName, int *pReturnCode)
#endif
{
    if (strcmp(pName, INTERFACEVERSION_ISERVERPLUGINCALLBACKS) == 0)
    {
        if (pReturnCode) *pReturnCode = 0;
        return &g_UtilitiesPlugin;
    }
    if (pReturnCode) *pReturnCode = 1;
    return NULL;
}

// Globals used by the shims
IServerPluginHelpers *helpers = 0;
CGlobalVars          *gpGlobals = 0;
IVEngineServer       *engine = 0;
ICvar                *g_pCVar = 0;
