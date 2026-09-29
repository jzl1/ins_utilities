// ============================================================================
// vehicle_exit_fix.cpp - see vehicle_exit_fix.h for the full rationale.
//
// Summary: IMIC's CBasePlayer::ItemPostFrame omits the
// `pVehicle->ItemPostFrame( this )` dispatch that Valve's SDK contains, which
// makes CBaseServerVehicle::ItemPostFrame (and therefore the engine's own
// `+use` exit branch) dead code. This module restores that dispatch.
// ============================================================================

#include <stdio.h>
#include <string.h>

#include "tier0/dbg.h"
#include "eiface.h"
#include "edict.h"
#include "engine/iserverplugin.h"
#include "server_class.h"
#include "dt_send.h"
#include "iserverentity.h"
#include "const.h"

#include "vehicle_exit_fix.h"

// ---------------------------------------------------------------------------
// Per-platform addresses.
//
// Linux: server_i486.so is ET_DYN (PIE), so these are RVAs added to the live
// module base. Both were read out of the dynamic symbol table:
//   objdump -T server_i486.so | grep ItemPostFrame
//     006b1800  CBaseServerVehicle::ItemPostFrame(CBasePlayer*)
//
// Windows: server.dll has a preferred base of 0x10000000 but a listen server
// loads it elsewhere (the turret code measured 0x0EEC0000), so these are RVAs
// too and must be rebased.
//
// The Windows value was derived from the vtable, not by pattern-hunting for
// the `+use` test. Locating the vtable is unambiguous because IServerVehicle
// inherits IVehicle and the slot layout is fixed by the shipped headers
// (game_shared/IVehicle.h -> slot 7 == ItemPostFrame; dlls/iservervehicle.h
// -> slot 8 == GetVehicleEnt):
//
//   vtable @ VA 0x10208404
//     [7] 0x10164BD0  CBaseServerVehicle::ItemPostFrame
//     [8] 0x10161E90  mov eax,[ecx+0x2C] ; ret   == GetVehicleEnt
//
// Slot 8's body is the cross-check that this is the right vtable: it is the
// engine's own back-pointer accessor, and it reads exactly the offset this
// module uses to validate the IServerVehicle* (VXF_OFFSET_BACKVEHICLE). The
// body of slot 7 then shows the exit branch in full:
//
//   push esi / mov esi,ecx
//   mov ecx,[esi+0x30] / mov eax,[ecx] / mov edx,[eax+4]    ; IsPassengerVisible?
//   call edx
//   test byte ptr [edi+0x768], 0x40                         ; m_afButtonPressed & IN_USE
//   je   ...                                                ; no +use -> return
//   mov ecx,[esi+0x30] / mov eax,[ecx] / mov edx,[eax+0x18] ; CanExitVehicle?
//   call edx / test al,al / je ...
//   mov eax,[esi] / mov edx,[eax+0x3C] / call edx            ; IServerVehicle slot 15
//   mov eax,[edi] / mov edx,[eax+0x430] / call edx           ; CBasePlayer slot 268
//   ret 4
//
// An earlier revision used 0x000207E0, which is NOT this function: it takes a
// stack argument (`push edi; mov 0x8(%esp),%edi`) and contains none of the
// above. Calling it __thiscall would have corrupted the stack on the first
// dispatched frame.
// ---------------------------------------------------------------------------
#ifdef _LINUX
    #define VXF_ITEMPOSTFRAME_RVA   0x006B1800UL
    #define VXF_MODULE_NAME         "server_i486.so"
#else
    #define VXF_ITEMPOSTFRAME_RVA   0x00164BD0UL
    #define VXF_MODULE_NAME         "server.dll"
#endif

// IServerVehicle::ItemPostFrame sits at vtable slot 7 on both builds, because
// IVehicle declares it seventh (game_shared/IVehicle.h) and IServerVehicle
// only appends after it (dlls/iservervehicle.h). Verified against the shipped
// binaries:
//   Linux   CBaseServerVehicle vtable base 0x0076E4A8, typeinfo at [-1]
//           (0x0076EC94 == typeinfo for CFourWheelServerVehicle),
//           slot[7] at byte 0x1C == 0x006B1800.
//   Windows CBaseServerVehicle vtable base 0x10208404, slot[7] == 0x10164BD0,
//           slot[8] == 0x10161E90 (GetVehicleEnt, reads [ecx+0x2C]).
#define VXF_ITEMPOSTFRAME_SLOT  7

// The two member offsets needed to walk from the vehicle entity to its
// IServerVehicle, read out of the engine's OWN accessors rather than guessed.
//
// Linux (measured, server_i486.so):
//   CPropVehicleDriveable::GetServerVehicle @ 0x6ae2fa
//       mov 0x8(%ebp),%eax ; mov 0x5c4(%eax),%eax ; ret
//     -> m_pServerVehicle is at 0x5C4
//   CBaseServerVehicle::GetVehicleEnt @ 0x6ae3b4
//       mov 0x8(%ebp),%eax ; mov 0x2c(%eax),%eax ; ret
//     -> the back-pointer to the owning entity is at 0x2C
//
// Windows (measured, server.dll):
//   CPropVehicleDriveable::GetServerVehicle @ rva 0x00162390
//       mov eax,[ecx+0x5B0] ; ret
//     -> m_pServerVehicle is at 0x5B0. This function has exactly one
//        reference in the image: vtable slot 0 at VA 0x10207864.
//   CBaseServerVehicle::GetVehicleEnt @ rva 0x00161E90
//       mov eax,[ecx+0x2C] ; ret
//     -> the back-pointer to the owning entity is at 0x2C. It sits at slot 8
//        of the IServerVehicle vtable at VA 0x10208404, which is the layout
//        dlls/iservervehicle.h mandates.
// These are DIFFERENT from the Linux values - MSVC and GCC lay these classes
// out differently, which is exactly why the offsets are per-platform instead
// of shared.
//
// AN EARLIER VERSION OF THIS FILE SCANNED FOR THE POINTER and crashed the
// server: a neighbouring field held a pointer to an unrelated object that
// shared vtable slot 7, and calling it dereferenced garbage. Reading the
// engine's own member removes the guess.
#ifdef _LINUX
    #define VXF_OFFSET_SERVERVEHICLE  0x5C4
    #define VXF_OFFSET_BACKVEHICLE    0x2C
#else
    #define VXF_OFFSET_SERVERVEHICLE  0x5B0
    #define VXF_OFFSET_BACKVEHICLE    0x2C
#endif

// ---------------------------------------------------------------------------

static void           **s_vxfModuleBase   = NULL;
static unsigned char   *s_vxfItemPostFrame = NULL;
static bool             s_vxfInstalled    = false;
static bool             s_vxfLogged       = false;
static unsigned long    s_vxfDispatches   = 0;

// Cached per-class lookup state, resolved once on the first frame that sees a
// driveable. Avoids walking the send table every tick.
static ServerClass     *s_vxfVehicleClass = NULL;
static int              s_vxfOffsetDriver = -1;
static int              s_vxfOffsetEnterAnim = -1;
static int              s_vxfOffsetExitAnim  = -1;

// ---------------------------------------------------------------------------
// Entry hold-off.
//
// m_afButtonPressed is a one-tick edge: CBasePlayer::UpdateButtonState
// computes it as `(last ^ now) & now` (baseplayer_shared.cpp:618). The very
// same edge that triggers the ENTER path is therefore still set when this
// dispatch runs on the frame the player climbs in, so the restored exit branch
// reads it, CanExitVehicle() passes (the vehicle is stationary), and the
// player is ejected the instant they sit down.
//
// Valve's own engine never had to care because the press is consumed by
// PlayerUse before ItemPostFrame; IMIC's ItemPostFrame skips the whole item
// block while a vehicle is attached, so the stale edge survives into the
// restored call. Suppress the dispatch for a short window after a player is
// FIRST seen occupying a seat. A deliberate exit is a fresh press, and a
// second press inside this window would be an unusual double-tap.
#define VXF_MAX_SEATS       64
#define VXF_DRIVER_HOLDOFF  0.25f

// Keyed by the (vehicle, driver) PAIR rather than by player index alone: a
// vehicle entindex is recycled after a wreck despawns, and a driver entindex
// is recycled when a player disconnects, so a stale timestamp must not be
// allowed to satisfy a brand new occupancy.
struct VxfSeatStamp
{
    int   vehicle;      // entindex of the driveable, 0 = free slot
    int   driver;       // entindex of the seated player
    float since;        // curtime when this pair was first observed
    bool  seen;         // set every frame the pair is observed
    bool  released;     // hold-off already expired and logged for this pair
};

static VxfSeatStamp    s_vxfSeats[VXF_MAX_SEATS];
static bool            s_vxfHoldoffLogged = false;

// Externs supplied by ins_utilities.cpp.
extern IVEngineServer  *engine;
extern CGlobalVars     *gpGlobals;

// IServerVehicle::ItemPostFrame is __thiscall on Windows (this in ECX, callee
// pops the one stack argument) and plain cdecl on Linux. Declaring it without
// __thiscall makes MSVC emit a redundant `add esp,4` and corrupt the stack -
// the same trap turret_abi.h documents for OnTakeDamage and AcceptInput.
#ifdef _LINUX
typedef void (*VxfItemPostFrameFn)(void *pServerVehicle, void *pPlayer);
#else
typedef void (__thiscall *VxfItemPostFrameFn)(void *pServerVehicle, void *pPlayer);
#endif

// ---------------------------------------------------------------------------
// Module base discovery.
//
// Windows cannot link GetModuleHandle here (the plugin deliberately imports
// only the small Win32 surface in ins_utilities.cpp), so the base is found by
// walking the PE export table the same way the RPG-speed patch does. Linux
// uses dl_iterate_phdr, which needs no symbol from the game module.
// ---------------------------------------------------------------------------
#ifdef _LINUX
#include <link.h>
#include <string.h>

static int VxfPhdrCallback(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    if (!info || !info->dlpi_name) return 0;
    // Match the basename; dlpi_name may be an absolute path.
    const char *name = info->dlpi_name;
    const char *slash = strrchr(name, '/');
    if (slash) name = slash + 1;
    if (strcmp(name, VXF_MODULE_NAME) != 0) return 0;
    *(void ***)data = (void **)info->dlpi_addr;
    return 1;   // stop
}

static void **VxfFindModuleBase()
{
    void **base = NULL;
    dl_iterate_phdr(VxfPhdrCallback, &base);
    return base;
}
#else
extern "C" {
    void *__stdcall GetModuleHandleA(const char *lpModuleName);
}

// Minimal PE walk: read e_lfanew, find the export directory, and return the
// module base. Mirrors GetWindowsTextRange in ins_utilities.cpp.
static void **VxfFindModuleBase()
{
    unsigned char *module = (unsigned char *)GetModuleHandleA(VXF_MODULE_NAME);
    return module ? (void **)module : NULL;
}
#endif

// ---------------------------------------------------------------------------
// Resolve the m_hPlayer send-prop offset once.
//
// m_hPlayer resolves by name on both builds (unlike m_hVehicle on the player),
// so no offset constant is needed.
// ---------------------------------------------------------------------------
static int VxfFindSendPropOffset(SendTable *pTable, const char *pName)
{
    if (!pTable || !pName) return -1;
    for (int i = 0; i < pTable->GetNumProps(); i++)
    {
        SendProp *p = pTable->GetProp(i);
        if (!p) continue;
        if (p->GetName() && strcmp(p->GetName(), pName) == 0)
            return p->GetOffset();
        if (p->GetDataTable())
        {
            int sub = VxfFindSendPropOffset(p->GetDataTable(), pName);
            if (sub >= 0) return p->GetOffset() + sub;
        }
    }
    return -1;
}

// Read the driver index out of a vehicle object. Returns -1 when the seat is
// empty or the offset has not resolved.
static int VxfDriverOf(void *pVehicle)
{
    if (!pVehicle || s_vxfOffsetDriver < 0) return -1;

    unsigned long h = *(unsigned long *)((char *)pVehicle + s_vxfOffsetDriver);
    if (h == INVALID_EHANDLE_INDEX || h == 0) return -1;

    int idx = (int)(h & ENT_ENTRY_MASK);
    if (idx < 1 || idx > gpGlobals->maxClients) return -1;
    return idx;
}

// ---------------------------------------------------------------------------
// Find the IServerVehicle* for a vehicle object.
//
// Reads the engine's own member (CPropVehicleDriveable::m_pServerVehicle at
// VXF_OFFSET_SERVERVEHICLE) instead of scanning the object. An earlier version
// scanned and crashed the server: a neighbouring field held a pointer to an
// unrelated object that happened to share vtable slot 7, and calling through
// it dereferenced garbage.
//
// The member read is still validated two ways before use, so a wrong offset
// degrades to "no dispatch" rather than to a crash:
//   (a) vtable slot 7 of the candidate is the engine's ItemPostFrame, and
//   (b) the candidate's own back-pointer (VXF_OFFSET_BACKVEHICLE) points at
//       the vehicle we started from.
// ---------------------------------------------------------------------------
static void *VxfFindServerVehicle(void *pVehicle)
{
    if (!pVehicle || !s_vxfItemPostFrame) return NULL;

    void *cand = *(void **)((char *)pVehicle + VXF_OFFSET_SERVERVEHICLE);
    if (!cand) return NULL;

    // Reject obvious non-pointers cheaply before dereferencing.
    unsigned long v = (unsigned long)cand;
    if (v < 0x1000UL || (v & 3) != 0) return NULL;

    // (a) the vtable slot must be the engine's own ItemPostFrame.
    void **vtbl = *(void ***)cand;
    if (!vtbl) return NULL;
    if (vtbl[VXF_ITEMPOSTFRAME_SLOT] != (void *)s_vxfItemPostFrame) return NULL;

    // (b) the back-pointer must lead back to this exact vehicle.
    if (*(void **)((char *)cand + VXF_OFFSET_BACKVEHICLE) != pVehicle) return NULL;

    return cand;
}

// ---------------------------------------------------------------------------

bool VehicleExitFixReady()
{
    return s_vxfInstalled && s_vxfItemPostFrame != NULL;
}

unsigned long VehicleExitFixDispatchCount()
{
    return s_vxfDispatches;
}

void VehicleExitFixInstall()
{
    if (s_vxfInstalled) return;

    void **base = VxfFindModuleBase();
    if (!base)
    {
        Msg("[ins_utilities] vehicle_exit_fix: module '%s' not found - dispatch NOT restored\n",
            VXF_MODULE_NAME);
        return;
    }

    s_vxfModuleBase = base;
    s_vxfItemPostFrame = (unsigned char *)base + VXF_ITEMPOSTFRAME_RVA;
    s_vxfInstalled = true;

    Msg("[ins_utilities] vehicle_exit_fix: module=%p ItemPostFrame=%p (rva 0x%X)\n",
        (void *)base, (void *)s_vxfItemPostFrame, (unsigned)VXF_ITEMPOSTFRAME_RVA);
}

void VehicleExitFixRemove()
{
    s_vxfInstalled = false;
    s_vxfItemPostFrame = NULL;
    s_vxfModuleBase = NULL;
    s_vxfLogged = false;
    s_vxfHoldoffLogged = false;
    s_vxfVehicleClass = NULL;
    s_vxfOffsetDriver = -1;
    s_vxfOffsetEnterAnim = -1;
    s_vxfOffsetExitAnim = -1;
    for (int k = 0; k < VXF_MAX_SEATS; k++)
    {
        s_vxfSeats[k].vehicle = 0;
        s_vxfSeats[k].driver  = 0;
        s_vxfSeats[k].since   = 0.0f;
        s_vxfSeats[k].seen    = false;
        s_vxfSeats[k].released = false;
    }
}

// ---------------------------------------------------------------------------
// The per-frame dispatch that IMIC omits.
//
// For every driveable with a seated driver, call the vehicle's own
// ItemPostFrame - the function Valve's SDK calls from CBasePlayer::ItemPostFrame
// and this mod's binary does not.
// ---------------------------------------------------------------------------
void VehicleExitFixFrame()
{
    if (!VehicleExitFixReady()) return;
    if (!engine || !gpGlobals) return;

    int maxEdicts = engine->GetEntityCount();
    int dispatched = 0;
    int occupied = 0;
    int heldOff = 0;

    // Mark every existing seat stamp unseen; anything still unseen when the
    // sweep finishes belongs to a seat that has since emptied or changed
    // hands, and is dropped so the next occupancy starts a fresh window.
    for (int k = 0; k < VXF_MAX_SEATS; k++)
        s_vxfSeats[k].seen = false;

    for (int i = 1; i < maxEdicts; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (!e || e->IsFree()) continue;

        IServerUnknown *unk = e->GetUnknown();
        if (!unk || !unk->GetBaseEntity()) continue;

        IServerNetworkable *net = unk->GetNetworkable();
        ServerClass *sc = net ? net->GetServerClass() : NULL;
        if (!sc) continue;

        // Cache the vehicle class and its driver offset on first sight. A
        // pointer compare against the cached ServerClass keeps the per-frame
        // cost to one comparison per entity instead of a name walk.
        if (!s_vxfVehicleClass)
        {
            // ServerClass::GetName() returns the C++ class name
            // ("CPropVehicleDriveable"), NOT the map-facing classname
            // ("prop_vehicle_driveable"). insbots learned this the hard way and
            // records it in turret.cpp:IsDriveableClass - the design spec used
            // the map classname and silently matched nothing.
            const char *cls = sc->GetName();
            if (!cls || strcmp(cls, "CPropVehicleDriveable") != 0)
                continue;

            int off = VxfFindSendPropOffset(sc->m_pTable, "m_hPlayer");
            if (off < 0)
            {
                Msg("[ins_utilities] vehicle_exit_fix: m_hPlayer not found in the "
                    "driveable send table - dispatch disabled\n");
                s_vxfLogged = true;
                return;
            }
            s_vxfVehicleClass = sc;
            s_vxfOffsetDriver = off;
            s_vxfOffsetEnterAnim = VxfFindSendPropOffset(sc->m_pTable, "m_bEnterAnimOn");
            s_vxfOffsetExitAnim  = VxfFindSendPropOffset(sc->m_pTable, "m_bExitAnimOn");
            Msg("[ins_utilities] vehicle_exit_fix: driveable class resolved, "
                "m_hPlayer offset 0x%X, enterAnim offset 0x%X, exitAnim offset 0x%X\n",
                off, s_vxfOffsetEnterAnim, s_vxfOffsetExitAnim);
        }

        if (sc != s_vxfVehicleClass) continue;

        void *pVehicle = unk->GetBaseEntity();
        int driver = VxfDriverOf(pVehicle);
        if (driver < 1) continue;   // empty seat; its stamp is dropped below

        occupied++;

        edict_t *pe = engine->PEntityOfEntIndex(driver);
        if (!pe || pe->IsFree()) continue;

        // IServerVehicle::ItemPostFrame takes a CBasePlayer*, NOT an edict_t*.
        // The engine reads m_afButtonPressed directly out of the argument
        // (Linux `mov eax,[eax+0x77C]`, Windows `test byte ptr [edi+0x768],40h`),
        // so handing it the edict would test an unrelated field and silently
        // never fire the exit branch. Resolve the real entity pointer.
        IServerUnknown *driverUnk = pe->GetUnknown();
        if (!driverUnk) continue;
        void *pDriver = driverUnk->GetBaseEntity();
        if (!pDriver) continue;

        // Entry hold-off. The press that seated this player is still latched
        // in m_afButtonPressed on the frame the seat becomes occupied; feeding
        // it to the restored exit branch ejects the driver immediately. Track
        // how long this pair has been seen seated and skip the dispatch
        // until the stale edge has certainly expired.
        VxfSeatStamp *stamp = NULL;
        bool created = false;
        for (int k = 0; k < VXF_MAX_SEATS; k++)
        {
            if (s_vxfSeats[k].vehicle == i && s_vxfSeats[k].driver == driver)
            {
                stamp = &s_vxfSeats[k];
                break;
            }
        }
        if (!stamp)
        {
            for (int k = 0; k < VXF_MAX_SEATS; k++)
            {
                if (s_vxfSeats[k].vehicle != 0) continue;
                stamp = &s_vxfSeats[k];
                break;
            }
            if (stamp)
            {
                stamp->vehicle = i;
                stamp->driver  = driver;
                stamp->since   = gpGlobals->curtime;
                stamp->released = false;
                created = true;
            }
        }

        if (stamp)
        {
            stamp->seen = true;
            // curtime restarts at 0 on a level change. Without this guard a
            // seat that was already occupied across the transition would
            // compute a negative age and hold the dispatch off indefinitely.
            if (gpGlobals->curtime < stamp->since)
                stamp->since = gpGlobals->curtime;
            if (created && !s_vxfHoldoffLogged)
            {
                Msg("[ins_utilities] vehicle_exit_fix: driver %d seated in "
                    "driveable %d, holding dispatch for %.2fs (entry press "
                    "still latched)\n", driver, i, VXF_DRIVER_HOLDOFF);
                s_vxfHoldoffLogged = true;
            }
            if (gpGlobals->curtime - stamp->since < VXF_DRIVER_HOLDOFF)
            {
                heldOff++;
                continue;
            }
            if (!stamp->released)
            {
                stamp->released = true;
                Msg("[ins_utilities] vehicle_exit_fix: hold-off expired for "
                    "driver %d in driveable %d after %.3fs - dispatch active\n",
                    driver, i, gpGlobals->curtime - stamp->since);
            }
        }

        // Clear stuck animation flags once the entry hold-off has expired.
        // CBaseServerVehicle::HandlePassengerEntry sets m_bEnterAnimOn = true,
        // but CPropVehicleDriveable never calls HandleEntryExitFinish to clear
        // it because it lacks entry anim sequences and completion checks.
        // Once the hold-off window has elapsed and the driver is seated, clear
        // both flags so CanExitVehicle() returns true on +use and SetupMove()
        // enables driving.
        if (s_vxfOffsetEnterAnim >= 0)
        {
            unsigned char *pEnter = (unsigned char *)pVehicle + s_vxfOffsetEnterAnim;
            if (*pEnter != 0)
                *pEnter = 0;
        }
        if (s_vxfOffsetExitAnim >= 0)
        {
            unsigned char *pExit = (unsigned char *)pVehicle + s_vxfOffsetExitAnim;
            if (*pExit != 0)
                *pExit = 0;
        }

        void *pServerVehicle = VxfFindServerVehicle(pVehicle);
        if (!pServerVehicle)
        {
            // Silent after the first report: an unresolved pointer is a real
            // fault, not a per-frame event, and would otherwise spam the log.
            if (!s_vxfLogged)
            {
                Msg("[ins_utilities] vehicle_exit_fix: no IServerVehicle found on "
                    "driveable %d (driver %d) - dispatch skipped\n", i, driver);
                s_vxfLogged = true;
            }
            continue;
        }

        // Call the engine's own per-frame vehicle handler. This is the call
        // Valve's CBasePlayer::ItemPostFrame makes and IMIC's does not.
        VxfItemPostFrameFn fn = (VxfItemPostFrameFn)s_vxfItemPostFrame;
        fn(pServerVehicle, pDriver);
        dispatched++;
    }

    s_vxfDispatches += (unsigned long)dispatched;

    // Drop stamps whose seat was not observed this frame. This is what makes
    // re-entry start a fresh hold-off window instead of inheriting the
    // original seat time and skipping the guard.
    for (int k = 0; k < VXF_MAX_SEATS; k++)
    {
        if (s_vxfSeats[k].vehicle == 0) continue;
        if (s_vxfSeats[k].seen) continue;
        s_vxfSeats[k].vehicle = 0;
        s_vxfSeats[k].driver  = 0;
        s_vxfSeats[k].since   = 0.0f;
        s_vxfSeats[k].released = false;
    }

    // Report the first frame that actually had a driver, so the log proves the
    // dispatch ran rather than merely that the plugin loaded.
    if (occupied > 0 && !s_vxfLogged)
    {
        Msg("[ins_utilities] vehicle_exit_fix: %d occupied driveable(s), "
            "%d dispatched, %d held off this frame\n",
            occupied, dispatched, heldOff);
        s_vxfLogged = true;
    }
}
