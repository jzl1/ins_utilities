// ============================================================================
// vehicle_exit_fix.h - restores the vehicle per-frame dispatch IMIC omits.
//
// THE DEFECT (measured, both shipped binaries)
// -------------------------------------------
// Valve's HL2/Episode 1 CBasePlayer::ItemPostFrame dispatches to the vehicle
// the player is riding:
//
//     if ( GetVehicle() )
//         pVehicle->ItemPostFrame( this );      // baseplayer_shared.cpp:158-175
//
// Insurgency's shipped server.dll / server_i486.so do NOT contain that call.
// Verified by disassembly:
//   * CBasePlayer::ItemPostFrame (Linux 0x0049CC38) never touches the
//     m_hVehicle member (offset 0x7EC) anywhere in its 0x18F-byte body.
//   * CBaseServerVehicle::ItemPostFrame (Linux 0x006B1800) is referenced only
//     from its own vtable entries - no instruction in either binary calls it.
//
// CBaseServerVehicle::ItemPostFrame is where the engine's own `+use` exit
// branch lives:
//
//     if ( player->m_afButtonPressed & IN_USE )
//         if ( GetDrivableVehicle()->CanExitVehicle( player ) )
//             HandlePassengerExit( player );
//
// With the dispatch gone that branch is dead code, so a seated driver can
// never leave - the press survives the frame (measured: m_afButtonPressed
// retains IN_USE through PostThink) and simply has no reader. This is a
// mod-level engine defect: it affects every driveable in IMIC, not just the
// Humvee. Restoring the call also revives m_flMinimumSpeedToEnterExit, the
// exit animations, and the driver-ejection path.
//
// WHAT THIS DOES
// --------------
// Calls IServerVehicle::ItemPostFrame once per tick for every occupied
// driveable, reproducing the missing dispatch.
//
// HOW THE POINTERS ARE OBTAINED
// -----------------------------
//   * The driver comes from the vehicle's own m_hPlayer SEND prop, which
//     resolves by name on both builds, so CBasePlayer::m_hVehicle - which is
//     NOT a send prop on this build and whose raw offset differs between the
//     MSVC and GCC layouts - is never needed.
//   * The IServerVehicle* comes from CPropVehicleDriveable::m_pServerVehicle,
//     read at a per-platform offset taken from the engine's OWN accessor
//     (GetServerVehicle), not from a guess.
//
// The member read is validated TWO ways before the pointer is ever called, so
// a wrong offset degrades to "no dispatch" instead of to a crash:
//   (a) vtable slot 7 of the candidate must be the engine's own
//       CBaseServerVehicle::ItemPostFrame address, and
//   (b) the candidate's back-pointer must point back at the vehicle the walk
//       started from.
//
// An earlier revision SCANNED the vehicle object for any pointer passing check
// (a) alone. That crashed the live server on the first dispatched frame: a
// neighbouring field held a pointer to an unrelated object that happened to
// share vtable slot 7, so the call went into garbage. Check (b) is what makes
// the read safe.
//
// ENTRY HOLD-OFF
// --------------
// m_afButtonPressed is a one-tick edge, and the very press that seats a player
// is still latched when the restored dispatch first runs, so the exit branch
// would read it and eject the driver immediately. The dispatch is therefore
// suppressed for a short window after a (vehicle, driver) pair is first seen
// seated. A deliberate exit is a fresh press and is unaffected.
//
// STUCK ANIMATION FLAGS
// ---------------------
// CBaseServerVehicle::HandlePassengerEntry sets m_bEnterAnimOn = true when a
// player boards. In Valve's derived vehicles (jeep, airboat), think calls
// HandleEntryExitFinish() when the entry animation completes to clear the flag.
// CPropVehicleDriveable has no entry anim sequences and never calls
// HandleEntryExitFinish(), latching m_bEnterAnimOn permanently to 1.
// Because CanExitVehicle() requires !m_bEnterAnimOn && !m_bExitAnimOn, exit is
// permanently blocked. Once the hold-off expires and the driver is seated,
// this module clears both flags, allowing exit and driving to succeed.
// ============================================================================
#pragma once

// Install: resolve the module base and the ItemPostFrame address. Safe to call
// more than once. Logs and refuses if the address does not validate.
void VehicleExitFixInstall();

// Uninstall: drop all cached state. Does not patch any engine code, so there
// is nothing to restore.
void VehicleExitFixRemove();

// Per-frame entry point. Call once per server frame while simulating.
void VehicleExitFixFrame();

// True once the ItemPostFrame address has been resolved and validated.
bool VehicleExitFixReady();

// Number of successful dispatches since load - for the status command.
unsigned long VehicleExitFixDispatchCount();
