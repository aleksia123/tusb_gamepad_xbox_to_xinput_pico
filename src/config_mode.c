// config_mode.c - the CDC request/response server. See config_mode.h for why
// this is a separate boot mode, and tools/configurator/PROTOCOL.md for the wire
// format (that document is normative; this file implements it).
//
// DESIGN NOTES
//  * Line-based ASCII commands, hex-encoded binary payloads. Not the most compact
//    choice, but it is trivially debuggable from any serial terminal, survives a
//    dropped byte without desynchronising a length-prefixed framer, and needs no
//    escaping logic on either side. A full profile is ~17 KB = ~35 KB of hex,
//    which crosses full-speed CDC in about a second.
//  * Client-pulled chunking rather than server-pushed streaming. The CDC TX
//    buffer is 256 bytes; a push loop would need flow control and a stall timeout.
//    One request per 64-byte chunk means the transfer is self-pacing and a lost
//    reply is just a retry.
//  * Writes land in the RAM working copy only. Flash is touched exactly once, by
//    an explicit COMMIT, after the client has verified its own CRC - so a cable
//    yank mid-transfer cannot leave a half-written profile on the device.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "tusb.h"
#include "tusb_gamepad.h"

#include "config_mode.h"
#include "profile_store.h"
#include "macro_types.h"

// ── Pin sampling ─────────────────────────────────────────────────────────────
bool config_mode_requested(void)
{
    gpio_init(CONFIG_MODE_PIN);
    gpio_set_dir(CONFIG_MODE_PIN, GPIO_IN);
    gpio_pull_up(CONFIG_MODE_PIN);

    // The internal pull-up drives a ~50 kOhm net; give it time against pin and
    // wire capacitance before sampling, then sample a few times and require
    // agreement. A single read right after enabling the pull-up can catch the
    // line mid-rise and put a normally-booting board into config mode, which
    // would look exactly like "the dongle stopped working".
    sleep_ms(5);
    int low = 0;
    for (int i = 0; i < 8; i++) {
        if (!gpio_get(CONFIG_MODE_PIN)) low++;
        sleep_us(200);
    }
    return low >= 6; // grounded
}

// ── CDC line I/O ─────────────────────────────────────────────────────────────
#define CMD_MAX   512      // 64-byte hex payload + command word + slack
#define CHUNK_MAX  64       // payload bytes per READ/WRITE

static char s_line[CMD_MAX];
static int  s_line_len;

// Blocking-with-pump write. tud_cdc_write() only accepts what fits in the FIFO,
// so we drain by servicing the USB stack between attempts rather than dropping
// the tail. Bounded so a disconnected host cannot wedge the loop forever.
static void cdc_send(const char *str)
{
    uint32_t len = (uint32_t)strlen(str);
    uint32_t sent = 0;
    uint32_t spins = 0;
    while (sent < len && spins < 200000u) {
        uint32_t n = tud_cdc_write(str + sent, len - sent);
        sent += n;
        if (n == 0) spins++;
        tud_cdc_write_flush();
        tud_task();
    }
    tud_cdc_write_flush();
}

static void cdc_sendln(const char *str)
{
    cdc_send(str);
    cdc_send("\r\n");
}

// ── Hex helpers ──────────────────────────────────────────────────────────────
static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Returns bytes decoded, or -1 on a malformed string (odd length or bad digit).
static int hex_decode(const char *src, uint8_t *dst, int dst_cap)
{
    int n = 0;
    while (src[0] && src[0] != '\r' && src[0] != '\n') {
        int hi = hex_nibble(src[0]);
        int lo = hex_nibble(src[1]);
        if (hi < 0 || lo < 0) return -1;
        if (n >= dst_cap) return -1;
        dst[n++] = (uint8_t)((hi << 4) | lo);
        src += 2;
    }
    return n;
}

static void hex_encode(const uint8_t *src, int n, char *dst)
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        dst[i * 2]     = digits[src[i] >> 4];
        dst[i * 2 + 1] = digits[src[i] & 0x0F];
    }
    dst[n * 2] = '\0';
}

// ── Command handling ─────────────────────────────────────────────────────────
// Tokeniser deliberately hand-rolled: strtok_r drags in locale machinery and we
// only ever need "split on single spaces".
static char *next_token(char **cursor)
{
    char *p = *cursor;
    while (*p == ' ') p++;
    if (!*p) { *cursor = p; return NULL; }
    char *start = p;
    while (*p && *p != ' ') p++;
    if (*p) { *p = '\0'; p++; }
    *cursor = p;
    return start;
}

static bool parse_u32(const char *s, uint32_t *out)
{
    if (!s || !*s) return false;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s) return false;
    *out = (uint32_t)v;
    return true;
}

static void handle_command(char *line)
{
    char *cursor = line;
    char *cmd = next_token(&cursor);
    if (!cmd) return;

    // HELLO - identity + geometry. The configurator reads the sizes from here
    // rather than hardcoding them, so a firmware built with different caps is
    // detected instead of silently mis-parsed.
    if (strcmp(cmd, "HELLO") == 0) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "RXCFG %u slots=%u macros=%u profile_bytes=%u macro_bytes=%u "
                 "seq_steps=%u script_steps=%u active=%u",
                 (unsigned)PROFILE_STORE_VERSION,
                 (unsigned)PROFILE_SLOTS,
                 (unsigned)MACROS_PER_PROFILE,
                 (unsigned)PROFILE_WIRE_BYTES,
                 (unsigned)sizeof(macro_definition_t),
                 (unsigned)MACRO_MAX_SEQ_STEPS,
                 (unsigned)MACRO_MAX_SCRIPT_STEPS,
                 (unsigned)profile_store_active_index());
        cdc_sendln(buf);
        return;
    }

    if (strcmp(cmd, "PING") == 0) { cdc_sendln("PONG"); return; }

    // ACTIVE            -> report active slot
    // ACTIVE <n>        -> set it (RAM; COMMIT to persist)
    if (strcmp(cmd, "ACTIVE") == 0) {
        char *arg = next_token(&cursor);
        if (!arg) {
            char buf[32];
            snprintf(buf, sizeof(buf), "ACTIVE %u", (unsigned)profile_store_active_index());
            cdc_sendln(buf);
            return;
        }
        uint32_t slot;
        if (!parse_u32(arg, &slot) || slot >= PROFILE_SLOTS) { cdc_sendln("ERR range"); return; }
        profile_store_ram()->active_slot = slot;
        cdc_sendln("OK");
        return;
    }

    // READ <slot> <offset> <len>
    if (strcmp(cmd, "READ") == 0) {
        char *a = next_token(&cursor);
        char *b = next_token(&cursor);
        char *c = next_token(&cursor);
        uint32_t slot, off, len;
        if (!parse_u32(a, &slot) || !parse_u32(b, &off) || !parse_u32(c, &len)) {
            cdc_sendln("ERR args"); return;
        }
        if (slot >= PROFILE_SLOTS || len == 0 || len > CHUNK_MAX ||
            off > PROFILE_WIRE_BYTES || off + len > PROFILE_WIRE_BYTES) {
            cdc_sendln("ERR range"); return;
        }
        const uint8_t *src = (const uint8_t *)&profile_store_ram()->slots[slot] + off;
        char hex[CHUNK_MAX * 2 + 1];
        hex_encode(src, (int)len, hex);
        char buf[CHUNK_MAX * 2 + 32];
        snprintf(buf, sizeof(buf), "DATA %u %s", (unsigned)off, hex);
        cdc_sendln(buf);
        return;
    }

    // WRITE <slot> <offset> <hex>
    if (strcmp(cmd, "WRITE") == 0) {
        char *a = next_token(&cursor);
        char *b = next_token(&cursor);
        char *payload = next_token(&cursor);
        uint32_t slot, off;
        if (!parse_u32(a, &slot) || !parse_u32(b, &off) || !payload) {
            cdc_sendln("ERR args"); return;
        }
        uint8_t tmp[CHUNK_MAX];
        int n = hex_decode(payload, tmp, sizeof(tmp));
        if (n <= 0) { cdc_sendln("ERR hex"); return; }
        if (slot >= PROFILE_SLOTS || off + (uint32_t)n > PROFILE_WIRE_BYTES) {
            cdc_sendln("ERR range"); return;
        }
        uint8_t *dst = (uint8_t *)&profile_store_ram()->slots[slot] + off;
        memcpy(dst, tmp, (size_t)n);
        char buf[32];
        snprintf(buf, sizeof(buf), "OK %d", n);
        cdc_sendln(buf);
        return;
    }

    // CRC <slot> -> CRC32 over the whole slot as the device currently holds it.
    // The configurator compares this against its own computation before asking
    // for a COMMIT, so a corrupted transfer is caught before it reaches flash.
    if (strcmp(cmd, "CRC") == 0) {
        char *a = next_token(&cursor);
        uint32_t slot;
        if (!parse_u32(a, &slot) || slot >= PROFILE_SLOTS) { cdc_sendln("ERR range"); return; }
        uint32_t crc = profile_store_crc32(&profile_store_ram()->slots[slot], PROFILE_WIRE_BYTES);
        char buf[32];
        snprintf(buf, sizeof(buf), "CRC %08lx", (unsigned long)crc);
        cdc_sendln(buf);
        return;
    }

    // RESET <slot> -> compiled-in defaults (RAM only until COMMIT)
    if (strcmp(cmd, "RESET") == 0) {
        char *a = next_token(&cursor);
        uint32_t slot;
        if (!parse_u32(a, &slot) || slot >= PROFILE_SLOTS) { cdc_sendln("ERR range"); return; }
        profile_store_reset_slot(slot);
        cdc_sendln("OK");
        return;
    }

    // COMMIT -> erase + program + verify. The only command that touches flash.
    if (strcmp(cmd, "COMMIT") == 0) {
        // Drain the TX FIFO before we mask interrupts and kill XIP: anything
        // still queued would not move until after the write, and the host would
        // see a long stall with no explanation.
        tud_cdc_write_flush();
        for (int i = 0; i < 64; i++) tud_task();

        bool ok = profile_store_commit();
        cdc_sendln(ok ? "OK committed" : "ERR flash");
        return;
    }

    cdc_sendln("ERR cmd");
}

// ── Main loop ────────────────────────────────────────────────────────────────
static void config_mode_task(void)
{
    if (!tud_cdc_connected()) return;

    while (tud_cdc_available()) {
        int32_t ch = tud_cdc_read_char();
        if (ch < 0) break;

        if (ch == '\n' || ch == '\r') {
            if (s_line_len > 0) {
                s_line[s_line_len] = '\0';
                s_line_len = 0;
                handle_command(s_line);
            }
            continue;
        }
        if (s_line_len < CMD_MAX - 1) {
            s_line[s_line_len++] = (char)ch;
        } else {
            // Overlong line: drop it wholesale rather than silently truncating,
            // which would let a corrupted WRITE land at the wrong offset.
            s_line_len = 0;
            cdc_sendln("ERR toolong");
        }
    }
}

void config_mode_run(void)
{
    // tusb_gamepad's USBSERIAL mode owns a known-good CDC descriptor set, so we
    // reuse it rather than authoring one. We do NOT call tusb_gamepad_task() -
    // that would run the USBSerialDriver's log-pump, which writes to the same CDC
    // endpoint and sleeps 10 ms per iteration. Only tud_task() is needed.
    init_tusb_gamepad(INPUT_MODE_USBSERIAL);

    s_line_len = 0;

    while (true) {
        tud_task();
        config_mode_task();
    }
}
