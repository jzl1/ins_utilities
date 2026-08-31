#ifndef INS_UTILITIES_LINUX_OFFSETS_H
#define INS_UTILITIES_LINUX_OFFSETS_H

// The Insurgency game-rules object stores the active mode index and mode
// pointers at these offsets on the supported Windows and Linux game builds.
// CRunningMode::RoundExtended() sets the byte at +0x3C. These are read-only
// offsets used by the sudden-death failsafe; no game memory is modified.
enum InsUtilitiesRunningModeOffset
{
    INSUTIL_RUNNING_MODE_INDEX_OFFSET        = 0x00D8,
    INSUTIL_RUNNING_MODE_TABLE_OFFSET        = 0x00E0,
    INSUTIL_RUNNING_MODE_EXTENDED_OFFSET     = 0x003C
};

// Verified against the 32-bit Linux INS:MIC server binaries:
//   insurgency/bin/server_i486.so
//   SHA-256 7afe49221a6f15d583261d9f019b706d88160ad67e6ca1141f5990f7152ffe70
//   bin/engine_i686.so (the launcher-selected engine, despite the i486 game DLL)
//   SHA-256 bdd6250284924ec8fb7cc264c14fb6e796d53444489d6d771d1a6378cf1a03cf
//
// Static owning-function/vtable disassembly and a read-only runtime audit both
// independently confirmed these values. Keep them Linux-only.
#ifdef _LINUX
enum InsUtilitiesLinuxOffset
{
    INSUTIL_LINUX_FIRE_EVENT_SLOT                 = 8,
    INSUTIL_LINUX_FIRE_EVENT_RVA                  = 0x001CD920,

    INSUTIL_LINUX_M_I_TEAM_NUM                    = 0x0348,
    INSUTIL_LINUX_M_FL_START_REINFORCEMENT_TIME   = 0x03BC,
    INSUTIL_LINUX_M_B_EMERGENCY_REINFORCEMENT     = 0x03C0,
    INSUTIL_LINUX_M_I_REINFORCEMENTS_LEFT         = 0x03C8,
    INSUTIL_LINUX_M_H_WEAPON_OWNER                = 0x0468,

    INSUTIL_LINUX_M_SPAWNFLAGS                    = 0x00A0,
    INSUTIL_LINUX_M_FL_WAIT                       = 0x034C,
    INSUTIL_LINUX_M_FL_AUTO_RETURN_DELAY          = 0x05D8,

    INSUTIL_LINUX_RPG_SPEED_SIGNATURE_RVA         = 0x006DB0E3,
    INSUTIL_LINUX_RPG_SPEED_IMMEDIATE_RVA         = 0x006DB0E7,
    INSUTIL_LINUX_RPG_THRUST_SIGNATURE_RVA        = 0x006DB196,
    INSUTIL_LINUX_RPG_THRUST_IMMEDIATE_RVA        = 0x006DB19A
};

#define INSUTIL_LINUX_SERVER_SHA256 \
    "7afe49221a6f15d583261d9f019b706d88160ad67e6ca1141f5990f7152ffe70"
#define INSUTIL_LINUX_ENGINE_I686_SHA256 \
    "bdd6250284924ec8fb7cc264c14fb6e796d53444489d6d771d1a6378cf1a03cf"
#endif

#endif
