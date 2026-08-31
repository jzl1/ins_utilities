#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <link.h>

#include "tier0/dbg.h"
#include "interface.h"
#include "engine/iserverplugin.h"
#include "eiface.h"
#include "edict.h"
#include "iserverentity.h"
#include "iserverunknown.h"
#include "iservernetworkable.h"
#include "dlls/iplayerinfo.h"
#include "server_class.h"
#include "dt_send.h"
#include "datamap.h"
#include "igameevents.h"

static IVEngineServer *s_engine;
static IServerGameDLL *s_server;
static IGameEventManager2 *s_events;

class CAuditBaseEntity : public IServerEntity
{
public:
    virtual ServerClass *GetServerClass() = 0;
    virtual char const *GetClassName() = 0;
    virtual int YouForgotToImplementOrDeclareServerClass() = 0;
    virtual datamap_t *GetDataDescMap() = 0;
};

static const char *const kProps[] = {
    "m_flStartReinforcementTime",
    "m_bEmergencyReinforcement",
    "m_bReinforcement",
    "m_iReinforcementsLeft",
    "m_iTeamNum",
    "m_hOwner",
    "m_hOwnerEntity",
    "m_spawnflags",
    0
};

static bool Wanted(const char *name)
{
    if (!name) return false;
    for (int i = 0; kProps[i]; ++i)
        if (!strcmp(name, kProps[i])) return true;
    return false;
}

static void DumpProps(const char *serverClass, SendTable *table,
                      int parentOffset, int depth)
{
    if (!table || depth > 32) return;
    for (int i = 0; i < table->GetNumProps(); ++i)
    {
        SendProp *prop = table->GetProp(i);
        if (!prop) continue;
        const int absolute = parentOffset + prop->GetOffset();
        if (Wanted(prop->GetName()))
            Msg("[insutil-audit] sendprop class=%s table=%s prop=%s offset=%d (0x%X) depth=%d\n",
                serverClass ? serverClass : "?",
                table->GetName() ? table->GetName() : "?",
                prop->GetName(), absolute, absolute, depth);
        if (prop->GetDataTable())
            DumpProps(serverClass, prop->GetDataTable(), absolute, depth + 1);
    }
}

static bool DoorClass(const char *name)
{
    return name && (!strcmp(name, "func_door")
        || !strcmp(name, "func_door_rotating")
        || !strcmp(name, "prop_door_rotating"));
}

static bool WantedDatamapField(const char *name)
{
    return name && (!strcmp(name, "m_spawnflags")
        || !strcmp(name, "m_flWait")
        || !strcmp(name, "m_flAutoReturnDelay"));
}

static void DumpDoorDatamap(const char *entityClass, datamap_t *map)
{
    int depth = 0;
    for (; map && depth < 32; map = map->baseMap, ++depth)
    {
        Msg("[insutil-audit] datamap entity=%s map=%s depth=%d fields=%d\n",
            entityClass, map->dataClassName ? map->dataClassName : "?",
            depth, map->dataNumFields);
        for (int i = 0; i < map->dataNumFields; ++i)
        {
            typedescription_t *field = &map->dataDesc[i];
            if (WantedDatamapField(field->fieldName))
                Msg("[insutil-audit] datafield entity=%s map=%s field=%s type=%d offset=%d (0x%X)\n",
                    entityClass, map->dataClassName ? map->dataClassName : "?",
                    field->fieldName, (int)field->fieldType,
                    field->fieldOffset[TD_OFFSET_NORMAL],
                    field->fieldOffset[TD_OFFSET_NORMAL]);
        }
    }
}

struct LoadedPath
{
    const char *needle;
    const char *path;
};

static int FindLoadedPath(struct dl_phdr_info *info, size_t, void *opaque)
{
    LoadedPath *search = (LoadedPath *)opaque;
    if (info->dlpi_name && strstr(info->dlpi_name, search->needle))
    {
        search->path = info->dlpi_name;
        return 1;
    }
    return 0;
}

static void DumpExportedDatamaps()
{
    struct ExportedMap { const char *name; const char *symbol; };
    static const ExportedMap maps[] = {
        { "CBaseDoor", "_ZN9CBaseDoor9m_DataMapE" },
        { "CRotDoor", "_ZN8CRotDoor9m_DataMapE" },
        { "CBasePropDoor", "_ZN13CBasePropDoor9m_DataMapE" },
        { "CPropDoorRotating", "_ZN17CPropDoorRotating9m_DataMapE" },
        { 0, 0 }
    };
    LoadedPath server = { "server_i486.so", 0 };
    dl_iterate_phdr(FindLoadedPath, &server);
    void *handle = server.path ? dlopen(server.path, RTLD_NOW | RTLD_NOLOAD) : 0;
    Msg("[insutil-audit] server_path=%s handle=%p\n",
        server.path ? server.path : "?", handle);
    for (int i = 0; maps[i].name; ++i)
    {
        datamap_t *map = handle ? (datamap_t *)dlsym(handle, maps[i].symbol) : 0;
        Msg("[insutil-audit] exported_datamap class=%s address=%p\n",
            maps[i].name, map);
        if (map) DumpDoorDatamap(maps[i].name, map);
    }
    if (handle) dlclose(handle);
}

static void DumpDoors()
{
    if (!s_engine) return;
    int found = 0;
    for (int i = 1; i <= s_engine->GetEntityCount(); ++i)
    {
        edict_t *edict = s_engine->PEntityOfEntIndex(i);
        if (!edict || edict->IsFree() || !DoorClass(edict->GetClassName())) continue;
        IServerUnknown *unknown = edict->GetUnknown();
        CAuditBaseEntity *entity = unknown
            ? (CAuditBaseEntity *)unknown->GetBaseEntity() : 0;
        datamap_t *map = entity ? entity->GetDataDescMap() : 0;
        Msg("[insutil-audit] door index=%d class=%s entity=%p datamap=%p\n",
            i, edict->GetClassName(), entity, map);
        if (map) DumpDoorDatamap(edict->GetClassName(), map);
        ++found;
    }
    Msg("[insutil-audit] doors=%d\n", found);
}

static void DumpEventVtable()
{
    if (!s_events) return;
    void **vtable = *(void ***)s_events;
    for (int slot = 0; slot < 12; ++slot)
    {
        Dl_info info;
        memset(&info, 0, sizeof(info));
        dladdr(vtable[slot], &info);
        uintptr_t rva = info.dli_fbase
            ? (uintptr_t)vtable[slot] - (uintptr_t)info.dli_fbase : 0;
        Msg("[insutil-audit] event_slot=%d address=%p rva=0x%X symbol=%s\n",
            slot, vtable[slot], (unsigned int)rva,
            info.dli_sname ? info.dli_sname : "?");
    }
}

struct ModuleScan
{
    const char *needle;
    uintptr_t base;
    const Elf32_Phdr *phdrs;
    int phnum;
};

static int FindModule(struct dl_phdr_info *info, size_t, void *opaque)
{
    ModuleScan *scan = (ModuleScan *)opaque;
    if (info->dlpi_name && strstr(info->dlpi_name, scan->needle))
    {
        scan->base = info->dlpi_addr;
        scan->phdrs = (const Elf32_Phdr *)info->dlpi_phdr;
        scan->phnum = info->dlpi_phnum;
        return 1;
    }
    return 0;
}

static void ScanPattern(const ModuleScan &module, const char *label,
                        const unsigned char *pattern, int length,
                        int immediateOffset)
{
    int matches = 0;
    for (int p = 0; p < module.phnum; ++p)
    {
        const Elf32_Phdr &segment = module.phdrs[p];
        if (segment.p_type != PT_LOAD || !(segment.p_flags & PF_X)) continue;
        const unsigned char *begin = (const unsigned char *)(module.base + segment.p_vaddr);
        const unsigned char *end = begin + segment.p_memsz;
        for (const unsigned char *at = begin; at + length <= end; ++at)
        {
            if (memcmp(at, pattern, length)) continue;
            uint32_t raw = 0;
            float value = 0.0f;
            memcpy(&raw, at + immediateOffset, sizeof(raw));
            memcpy(&value, &raw, sizeof(value));
            Msg("[insutil-audit] rpg_%s match_rva=0x%X immediate_rva=0x%X raw=0x%08X value=%.1f\n",
                label, (unsigned int)((uintptr_t)at - module.base),
                (unsigned int)((uintptr_t)(at + immediateOffset) - module.base),
                raw, value);
            ++matches;
        }
    }
    Msg("[insutil-audit] rpg_%s matches=%d\n", label, matches);
}

static void DumpRpgSignatures()
{
    static const unsigned char speed[] = {
        0x8D,0x55,0xC8,0xB8,0x00,0x80,0x3B,0x45,0x89,0x44,
        0x24,0x08,0x8B,0x45,0x08,0x05,0x6C,0x04,0x00,0x00
    };
    static const unsigned char thrust[] = {
        0x8B,0x55,0x08,0xB8,0x00,0x00,0x7A,0x44,0x89,0x82,
        0x98,0x04,0x00,0x00
    };
    ModuleScan module = { "server_i486.so", 0, 0, 0 };
    dl_iterate_phdr(FindModule, &module);
    Msg("[insutil-audit] server_base=%p phnum=%d\n", (void *)module.base, module.phnum);
    if (!module.phdrs) return;
    ScanPattern(module, "speed", speed, sizeof(speed), 4);
    ScanPattern(module, "thrust", thrust, sizeof(thrust), 4);
}

class CInsUtilAuditPlugin : public IServerPluginCallbacks
{
public:
    virtual bool Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn gameFactory)
    {
        s_engine = (IVEngineServer *)interfaceFactory(INTERFACEVERSION_VENGINESERVER, 0);
        s_events = (IGameEventManager2 *)interfaceFactory(INTERFACEVERSION_GAMEEVENTSMANAGER2, 0);
        s_server = (IServerGameDLL *)gameFactory("ServerGameDLL006", 0);
        if (!s_server) s_server = (IServerGameDLL *)gameFactory("ServerGameDLL005", 0);
        if (!s_server) s_server = (IServerGameDLL *)gameFactory("ServerGameDLL004", 0);
        if (!s_server) s_server = (IServerGameDLL *)gameFactory("ServerGameDLL003", 0);
        if (!s_engine || !s_server || !s_events)
        {
            Msg("[insutil-audit] ERROR interfaces engine=%p server=%p events=%p\n",
                s_engine, s_server, s_events);
            return false;
        }
        Msg("[insutil-audit] BEGIN\n");
        for (ServerClass *sc = s_server->GetAllServerClasses(); sc; sc = sc->m_pNext)
            DumpProps(sc->GetName(), sc->m_pTable, 0, 0);
        DumpExportedDatamaps();
        DumpEventVtable();
        DumpRpgSignatures();
        return true;
    }
    virtual void Unload() { Msg("[insutil-audit] END\n"); }
    virtual void Pause() {}
    virtual void UnPause() {}
    virtual const char *GetPluginDescription() { return "Ins Utilities read-only Linux offset audit"; }
    virtual void LevelInit(char const *) {}
    virtual void ServerActivate(edict_t *, int, int) { DumpDoors(); }
    virtual void GameFrame(bool) {}
    virtual void LevelShutdown() {}
    virtual void ClientActive(edict_t *) {}
    virtual void ClientDisconnect(edict_t *) {}
    virtual void ClientPutInServer(edict_t *, char const *) {}
    virtual void SetCommandClient(int) {}
    virtual void ClientSettingsChanged(edict_t *) {}
    virtual PLUGIN_RESULT ClientConnect(bool *, edict_t *, const char *, const char *, char *, int) { return PLUGIN_CONTINUE; }
    virtual PLUGIN_RESULT ClientCommand(edict_t *) { return PLUGIN_CONTINUE; }
    virtual PLUGIN_RESULT NetworkIDValidated(const char *, const char *) { return PLUGIN_CONTINUE; }
};

static CInsUtilAuditPlugin s_plugin;

extern "C" __attribute__((visibility("default")))
void *CreateInterface(const char *name, int *returnCode)
{
    if (name && !strcmp(name, INTERFACEVERSION_ISERVERPLUGINCALLBACKS))
    {
        if (returnCode) *returnCode = 0;
        return &s_plugin;
    }
    if (returnCode) *returnCode = 1;
    return 0;
}
