// profile_store.c - see profile_store.h for the design rationale and safety rules.
#include <string.h>
#include <stddef.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#include "profile_store.h"
#include "macro_engine.h"

// The RAM working copy. ~70 KB of the RP2350's 520 KB SRAM - deliberately held
// in full rather than paged from flash, because the macro engine dereferences
// macro definitions on every tick and XIP reads would be at the mercy of the
// flash cache exactly when latency matters most.
static profile_store_t s_store;

// XIP-mapped view of the reserved region. Reading flash through XIP_BASE is a
// plain memory read - no driver call, no cache flush - which is why the read path
// needs none of the write path's ceremony.
static const profile_store_t *flash_image(void)
{
    return (const profile_store_t *)(XIP_BASE + PROFILE_STORE_OFFSET);
}

// ── CRC32 (IEEE, reflected) ──────────────────────────────────────────────────
// Bitwise rather than table-driven: this runs a handful of times per session
// (init and commit), never per tick, and a 1 KB table is not worth the flash.
uint32_t profile_store_crc32(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

// CRC covers everything after the crc32 field, so version and active_slot are
// protected too - a corrupted active_slot could otherwise index out of range.
#define CRC_SKIP_BYTES (offsetof(profile_store_t, active_slot))

static uint32_t compute_crc(const profile_store_t *st)
{
    return profile_store_crc32((const uint8_t *)st + CRC_SKIP_BYTES,
                               (uint32_t)(sizeof(*st) - CRC_SKIP_BYTES));
}

void profile_store_reset_slot(uint32_t slot)
{
    if (slot >= PROFILE_SLOTS) return;
    profile_t *p = &s_store.slots[slot];
    memset(p, 0, sizeof(*p));
    // Filters fall back to the compiled-in factory profile, which is the tuned
    // Rainbow 2 Pro setup documented in pad_config.h - so a reset slot behaves
    // exactly like the firmware did before this feature existed.
    p->filters = g_pad_config;
    p->macro_count = 0;
    p->name[0] = 'P';
    p->name[1] = (char)('1' + slot);
    p->name[2] = '\0';
}

static void load_defaults(void)
{
    memset(&s_store, 0, sizeof(s_store));
    s_store.magic       = PROFILE_STORE_MAGIC;
    s_store.version     = PROFILE_STORE_VERSION;
    s_store.active_slot = 0;
    for (uint32_t i = 0; i < PROFILE_SLOTS; i++)
        profile_store_reset_slot(i);
    s_store.crc32 = compute_crc(&s_store);
}

bool profile_store_init(void)
{
    const profile_store_t *img = flash_image();

    // Three independent gates. Blank flash (all 0xFF) fails the first two; a
    // partially-written or bit-rotted image fails the third. Any failure is a
    // silent fall back to defaults - never a hang, never a refusal to boot.
    bool valid = img->magic   == PROFILE_STORE_MAGIC &&
                 img->version == PROFILE_STORE_VERSION;
    if (valid) {
        memcpy(&s_store, img, sizeof(s_store));
        valid = (compute_crc(&s_store) == s_store.crc32);
    }

    if (!valid) {
        load_defaults();
        return false;
    }

    if (s_store.active_slot >= PROFILE_SLOTS) s_store.active_slot = 0;
    return true;
}

profile_store_t *profile_store_ram(void) { return &s_store; }

profile_t *profile_store_active(void) { return &s_store.slots[s_store.active_slot]; }

uint32_t profile_store_active_index(void) { return s_store.active_slot; }

void profile_store_set_active(uint32_t slot)
{
    if (slot >= PROFILE_SLOTS) return;
    s_store.active_slot = slot;
    profile_t *p = &s_store.slots[slot];
    int n = p->macro_count;
    if (n < 0) n = 0;
    if (n > MACROS_PER_PROFILE) n = MACROS_PER_PROFILE;
    // Rebinding resets every runtime state machine, which is the correct
    // behaviour on a profile switch: a half-finished burst or a latched toggle
    // from the old profile must not bleed into the new one.
    macro_engine_load(p->macros, n);
}

bool profile_store_commit(void)
{
    s_store.magic   = PROFILE_STORE_MAGIC;
    s_store.version = PROFILE_STORE_VERSION;
    s_store.crc32   = compute_crc(&s_store);

    uint32_t len = (uint32_t)sizeof(s_store);
    uint32_t erase_len = (len + FLASH_SECTOR_SIZE - 1u) & ~(FLASH_SECTOR_SIZE - 1u);
    if (erase_len > PROFILE_STORE_BYTES) return false; // static-asserted, belt and braces

    // flash_range_program insists on a whole number of FLASH_PAGE_SIZE pages, but
    // sizeof(profile_store_t) is whatever the struct happens to be. Programming
    // the rounded-up length straight from &s_store would read past the end of the
    // object, so the bulk goes out page-aligned and the ragged tail is assembled
    // in a 256-byte scratch page. (Padding the struct instead would tie its size
    // to the page size and break the wire format the configurator parses.)
    uint32_t bulk = len & ~((uint32_t)FLASH_PAGE_SIZE - 1u);
    uint32_t tail = len - bulk;
    static uint8_t tail_page[FLASH_PAGE_SIZE];
    if (tail) {
        memset(tail_page, 0xFF, sizeof(tail_page)); // 0xFF = erased state
        memcpy(tail_page, (const uint8_t *)&s_store + bulk, tail);
    }

    // Interrupts off across erase+program: XIP is disabled for the duration, so
    // any ISR that runs from flash would fault. TinyUSB on this port is polled
    // from the main loop (CFG_TUSB_OS == OPT_OS_NONE) but the USB IRQ handler is
    // still resident, hence the hard mask rather than a mere critical section.
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(PROFILE_STORE_OFFSET, erase_len);
    if (bulk) flash_range_program(PROFILE_STORE_OFFSET, (const uint8_t *)&s_store, bulk);
    if (tail) flash_range_program(PROFILE_STORE_OFFSET + bulk, tail_page, FLASH_PAGE_SIZE);
    restore_interrupts(ints);

    // Verify by reading back through XIP. A mismatch means the sector did not
    // take; the caller reports the failure rather than pretending it saved.
    return memcmp(flash_image(), &s_store, sizeof(s_store)) == 0;
}
