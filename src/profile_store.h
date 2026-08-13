// profile_store.h - on-flash profile storage: 4 slots of (filter config + macro
// list), an active-slot index, and a CRC32 over the lot.
//
// WHY IT LIVES IN FLASH AND NOT IN THE FIRMWARE IMAGE
// pad_config.h explains why there was never a live config channel: Windows'
// xusb22.sys claims the device's single XInput interface exclusively, so nothing
// in a browser can talk to the board while it is acting as a pad, and bolting a
// second interface onto tusb_gamepad's fixed XInput descriptor risks breaking
// enumeration outright. That reasoning still holds for the *running* pad - so
// configuration moved to a separate boot mode instead (see config_mode.h), and
// the settings it writes need somewhere persistent to land. Hence this store.
//
// SAFETY RULES BAKED IN HERE
//   * Never brick a fresh board. Blank flash reads as 0xFF, which fails both the
//     magic check and the CRC, so profile_store_init() silently falls back to
//     g_pad_config's compiled-in defaults and an empty macro list. The pad works
//     out of the box with zero flash writes ever having happened.
//   * Never half-write. Writes go through a RAM staging copy; the flash sectors
//     are only erased and programmed once a full profile has been received and
//     its CRC verified. A cable yank mid-transfer leaves the old image intact.
//   * Never write flash while the pad is live. flash_range_erase() disables XIP
//     and would fault core 1 mid-execution. All writes happen in config mode,
//     where core 1 is never launched and the XInput device is never brought up.
//
// The struct layout below is ALSO the wire format the browser configurator
// speaks. tools/configurator/PROTOCOL.md documents these offsets; the static
// asserts at the end of macro_types.h and this file lock them down.
#ifndef PROFILE_STORE_H
#define PROFILE_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/flash.h"
#include "macro_types.h"
#include "pad_config.h"

#define PROFILE_SLOTS         4
#define PROFILE_NAME_LEN     16

// 'RX' + 'M' + version nibble. Bump PROFILE_STORE_VERSION on ANY layout change
// (adding a macro field, changing a cap) - an old image then fails validation and
// is replaced by defaults instead of being misread field-by-field.
#define PROFILE_STORE_MAGIC   0x314D5852u  // "RXM1" little-endian
#define PROFILE_STORE_VERSION 1u

// One profile slot: the Phase-3 filter knobs (the same pad_config_t hid_app.c
// already consumes, so nothing downstream had to change) plus the macro list.
typedef struct {
    char        name[PROFILE_NAME_LEN];
    pad_config_t filters;
    uint8_t     _pad[2];              // pad_config_t is 10 bytes; realign to 4
    int32_t     macro_count;
    macro_definition_t macros[MACROS_PER_PROFILE];
} profile_t;

// The whole flash image. crc32 covers everything AFTER the crc32 field, so the
// header itself participates (version and active_slot are protected too).
typedef struct {
    uint32_t  magic;
    uint32_t  version;
    uint32_t  crc32;
    uint32_t  active_slot;
    profile_t slots[PROFILE_SLOTS];
} profile_store_t;

// ── Flash geometry ───────────────────────────────────────────────────────────
// PICO_BOARD is "none" in this project's CMakeLists, so the SDK does not always
// define PICO_FLASH_SIZE_BYTES. 2 MB is the safe floor: every RP2350 module in
// circulation has at least that, and if the fitted part is larger we simply are
// not using the very last sectors, which costs nothing. The firmware image is a
// couple hundred KB, so the reserved region is never anywhere near it.
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2u * 1024u * 1024u)
#endif

#define PROFILE_STORE_SECTORS 32u   // 128 KB reserved - ~1.8x the current image
#define PROFILE_STORE_BYTES   (PROFILE_STORE_SECTORS * FLASH_SECTOR_SIZE)
#define PROFILE_STORE_OFFSET  (PICO_FLASH_SIZE_BYTES - PROFILE_STORE_BYTES)

// ── API ──────────────────────────────────────────────────────────────────────

// Reads flash, validates magic/version/CRC, and populates the RAM working copy.
// Returns true if a valid image was found, false if defaults were substituted.
// Call once, early, before macro_engine_load().
bool profile_store_init(void);

// The RAM working copy. Always valid after profile_store_init(); mutate it and
// call profile_store_commit() to persist.
profile_store_t *profile_store_ram(void);

// Convenience accessors for the currently-active slot.
profile_t *profile_store_active(void);
uint32_t   profile_store_active_index(void);

// Sets the active slot (clamped) and rebinds the macro engine to it. RAM-only -
// call profile_store_commit() if the choice should survive a power cycle.
void profile_store_set_active(uint32_t slot);

// Recomputes the CRC over the RAM copy and writes it to flash. MUST NOT be
// called while core 1 is running (see the safety rules above) - i.e. config mode
// only. Returns true if the readback verified.
bool profile_store_commit(void);

// Resets one slot to compiled-in defaults (g_pad_config filters, no macros).
void profile_store_reset_slot(uint32_t slot);

// CRC32 (IEEE 802.3, reflected, init 0xFFFFFFFF, final xor) - exposed because
// the configurator computes the same value over the same bytes to verify a
// transfer before the device commits it.
uint32_t profile_store_crc32(const void *data, uint32_t len);

// Bytes of one profile_t - the transfer unit the config protocol chunks over CDC.
#define PROFILE_WIRE_BYTES ((uint32_t)sizeof(profile_t))

_Static_assert(sizeof(profile_store_t) <= PROFILE_STORE_BYTES,
               "profile store no longer fits its reserved flash region - raise "
               "PROFILE_STORE_SECTORS or lower MACROS_PER_PROFILE");
_Static_assert(sizeof(pad_config_t) == 10, "pad_config_t grew; fix profile_t padding");
// The transfer unit the configurator chunks. Locked because PROTOCOL.md's offset
// table is written against this exact number.
_Static_assert(sizeof(profile_t) == 17632, "profile_t layout changed - bump "
               "PROFILE_STORE_VERSION and update tools/configurator/PROTOCOL.md");

#endif // PROFILE_STORE_H
