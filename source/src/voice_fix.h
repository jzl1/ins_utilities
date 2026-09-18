#ifndef INS_UTILITIES_VOICE_FIX_H
#define INS_UTILITIES_VOICE_FIX_H

// Fix for the CLC_VoiceData / SVC_VoiceData stack overflow in the Source 2006
// engine (CGameClient::ProcessVoiceData -> bf_read::ReadBits with an unbounded,
// wire-supplied bit count).
//
// The vulnerable call is repointed at a run-time-built thunk that clamps the
// bit count to the handler's stack buffer before tail-jumping into ReadBits.
// The thunk clamps TWO things, and both are required:
//
//   1. ReadBits(dst, nBits)                          -> the stack overflow
//   2. SV_BroadcastVoiceData(..., (nBits+7)/8, dst)  -> re-reads the same
//      field and copies that many bytes back out of the 4096-byte buffer
//
// Clamping only (1) still lets (2) over-read the stack buffer - on Windows that
// ends in a /GS fail-fast (0xC0000409).  The handler re-reads the message field
// for (2), so the thunk writes the clamped value back into it.  The field sits
// immediately in front of the bf_read the handler consumes:
//
//   Windows server handler : bf_read = msg+0x14, length = msg+0x10 (= this-4)
//   Windows client handler : bf_read = msg+0x18, length = msg+0x14 (= this-4)
//   Linux   server handler : bf_read = msg+0x14, length = msg+0x10 (= arg0-4)
//
// Calling convention
//   Windows: thiscall - `this` (the bf_read) in ECX, nBits = [esp+8].
//            ECX must survive the thunk: it IS ReadBits' `this` pointer.
//   Linux:   cdecl   - arg0/`this` on the stack at [esp+4], nBits = [esp+0Ch].
// Only eax/ecx/edx are touched (caller-saved in both ABIs), so nothing has to
// be preserved across the tail jump.
//
// Header-only and engine-free so the byte builder and the call-site guard can
// be unit-tested on the host without the HL2SDK.

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if defined(_WIN32)
#  define VF_BITS_OFFSET 8
#else
#  define VF_BITS_OFFSET 12
#endif

static const uint32_t VF_MAX_BITS = 0x8000u;   /* 4096 bytes = the voice buffer */

/* A byte range to inspect.  `p` points at the first byte, `va` is that byte's
 * runtime virtual address, `len` is the range length.  The guard works purely
 * from these three fields, which is what lets the host test run it over a file
 * image (va = 0, p = file contents, len = file size). */
struct VfCode {
    const unsigned char *p;
    uintptr_t va;
    size_t len;
};

/* One repointed `call bf_read::ReadBits`: where it is, the 5 bytes that were
 * there, and the 5 bytes that replace them. */
#define VF_MAX_SITES 4
struct VfSite {
    uintptr_t va;
    unsigned char orig[5];
    unsigned char patched[5];
};

static inline void VfPut32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
    p[2] = (unsigned char)((v >> 16) & 0xffu);
    p[3] = (unsigned char)((v >> 24) & 0xffu);
}

/** Build the thunk at `b` (which must be the address it will run from - the
 *  tail jump is a rel32) and return its length in bytes.
 *
 *   mov  edx,[esp+nBits]      ; the wire bit count
 *   cmp  edx,0x8000
 *   jbe  done
 *   mov  [lastBits],edx       ; remember what was asked for (for the log)
 *   mov  edx,0x8000
 *   inc  [clampCount]
 *   mov  [bf_read-4],edx      ; msg->m_nLength = clamped  (see above)
 * done:
 *   mov  [esp+nBits],edx
 *   jmp  bf_read::ReadBits
 */
static inline size_t VfBuildThunk(unsigned char *b, uintptr_t readBits,
                                  uintptr_t clampCount, uintptr_t lastBits)
{
    size_t n = 0;
    size_t jbe_at, done_at;

    b[n++] = 0x8B; b[n++] = 0x54; b[n++] = 0x24; b[n++] = (unsigned char)VF_BITS_OFFSET;
    b[n++] = 0x81; b[n++] = 0xFA; VfPut32(&b[n], VF_MAX_BITS); n += 4;
    jbe_at = n; b[n++] = 0x76; b[n++] = 0x00;
    b[n++] = 0x89; b[n++] = 0x15; VfPut32(&b[n], (uint32_t)lastBits); n += 4;
    b[n++] = 0xBA; VfPut32(&b[n], VF_MAX_BITS); n += 4;
    b[n++] = 0xFF; b[n++] = 0x05; VfPut32(&b[n], (uint32_t)clampCount); n += 4;
#if defined(_WIN32)
    b[n++] = 0x89; b[n++] = 0x51; b[n++] = 0xFC;                      /* mov [ecx-4],edx */
#else
    b[n++] = 0x8B; b[n++] = 0x44; b[n++] = 0x24; b[n++] = 0x04;       /* mov eax,[esp+4] */
    b[n++] = 0x89; b[n++] = 0x50; b[n++] = 0xFC;                      /* mov [eax-4],edx */
#endif
    done_at = n;
    b[jbe_at + 1] = (unsigned char)(done_at - (jbe_at + 2));

    b[n++] = 0x89; b[n++] = 0x54; b[n++] = 0x24; b[n++] = (unsigned char)VF_BITS_OFFSET;
    b[n++] = 0xE9;
    VfPut32(&b[n], (uint32_t)(readBits - ((uintptr_t)&b[n + 4]))); n += 4;

    return n;
}

/** Verify a thunk that is already sitting at `self`.
 *
 *  Everything except the tail rel32 is compared byte for byte against what the
 *  builder emits for this (readBits, clampCount, lastBits) tuple; the rel32
 *  cannot be compared because it is address dependent, so it is decoded and
 *  checked to land exactly on ReadBits from `self`.  Deliberately absolute
 *  instead of a hand-written list of offsets: a copy built for a different
 *  address, a stray byte or a partial write are all caught. */
static inline int VfCheckThunk(const unsigned char *b, size_t len, uintptr_t self,
                               uintptr_t readBits, uintptr_t clampCount,
                               uintptr_t lastBits)
{
    unsigned char expect[64];
    size_t cmp_len;
    int32_t rel;
    size_t built;

    if (!b || len < 20 || len > sizeof(expect)) return 0;
    if (b[len - 5] != 0xE9) return 0;

    built = VfBuildThunk(expect, readBits, clampCount, lastBits);
    if (built != len) return 0;

    cmp_len = built - 4;
    if (memcmp(b, expect, cmp_len) != 0) return 0;

    memcpy(&rel, b + len - 4, sizeof(rel));
    if ((uintptr_t)((int64_t)self + (int64_t)len + rel) != readBits) return 0;
    return 1;
}

// Implemented in voice_fix.cpp.  The plugin object in ins_utilities.cpp calls
// these; nothing here touches engine memory at include time.
bool VoiceFixInstall(void);
void VoiceFixRemove(void);
void VoiceFixUpdate(bool enabled);
bool VoiceFixIsActive(void);

#endif /* INS_UTILITIES_VOICE_FIX_H */
