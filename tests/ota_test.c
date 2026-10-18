/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments; ChoralRoot FM-1 contributors; TransitFM1 */
/* Host test of firmware/src/ota.c, TransitFM1's step 1: a fake M-UPGRADE serves a real .fwsc over the SysEx
 * protocol (pack7, cmd 0x30 reads, 0xE0000000 -> "success") and a simulated NOR plays the flash.
 *   cc -o ota_test tests/ota_test.c && ./ota_test build/transit.fwsc [FOREIGN.fwsc]
 * TransitFM1's own loader is the one in its package (tools/build.py puts the same ota.bin into the app). Step 1
 * stages THAT loader (never the package's), keeps it across sessions and failures, restages it when it is missing or
 * damaged; the boot cleanup keeps it (the boot stub's check, firmware/stub/stub_core.h) and removes anything else.
 * FOREIGN (e.g. M-VAVE's FM-1.fwsc): a package with another loader is installed through ours all the same. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FELUCCA_ID "FM-1_015"
#define FELUCCA_OTA_DRYRUN 0
#define T_AREA 0xE0000u
#define T_PARM_ID "ota-FM-1_015"

static uint8_t nor[0x100000];
static uint8_t *logical;
static size_t logical_len;
static uint8_t rx[1024];                 /* one pending host frame (7-bit bytes) */
static uint32_t rx_len, rx_full;
static uint32_t now_ms, requests, ident_replies, mute_after = 0xFFFFFFFFu, bad_erase;
static int committed;
static uint8_t committed_parm[112];
static int32_t last_code;
static uint8_t *corrupt_at;              /* flip a served byte */
static uint8_t own_ldr[0x5000];          /* our ota.bin, from the package (as the app carries it) */
static uint32_t own_len, erases;

static uint32_t pack7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 8;
        while (nb >= 7) { out[o++] = acc & 0x7F; acc >>= 7; nb -= 7; }
    }
    if (nb) out[o++] = acc & 0x7F;
    return o;
}

static uint32_t unpack7h(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 7;
        if (nb >= 8) { out[o++] = (uint8_t)acc; acc >>= 8; nb -= 8; }
    }
    return o;
}

static void host_reply(const uint8_t *msg, uint32_t n)       /* queue msg as a device-bound frame */
{
    rx_len = pack7(msg, n, rx);
    rx_full = 1;
}

/* ---- hooks ---- */
static int ota_wire_send(const uint8_t *p, uint32_t n)
{
    uint8_t u[64] = {0}, m[600];
    uint32_t d, addr, len, i, s = 0;
    if (p[0] != 0xF0 || p[n - 1] != 0xF7) { printf("bad wire framing\n"); exit(1); }
    d = unpack7h(p + 1, n - 2, u);
    if (u[2] == 0x11) { ident_replies++; return 0; }
    if (d != 15 || u[2] != 0x30) { printf("unexpected device msg cmd %02x len %u\n", u[2], d); exit(1); }
    addr = u[7] | u[8] << 8 | u[9] << 16 | (uint32_t)u[10] << 24;
    len = u[11] | u[12] << 8 | u[13] << 16;
    if (++requests > mute_after)
        return 0;                                         /* host gone */
    memcpy(m, "\x00\x59\x30", 3);
    m[3] = (uint8_t)(len + 8); m[4] = (uint8_t)((len + 8) >> 8); m[5] = 0;
    m[6] = 0;
    memcpy(m + 7, &addr, 4);
    m[11] = (uint8_t)len; m[12] = (uint8_t)(len >> 8); m[13] = 0;
    if (addr == 0xE0000000u) {
        memset(m + 14, 0, len);
        memcpy(m + 14, "success", 8);
    } else {
        if (addr + len > logical_len) { printf("read past the package %#x\n", addr); exit(1); }
        memcpy(m + 14, logical + addr, len);
        if (corrupt_at && corrupt_at >= logical + addr && corrupt_at < logical + addr + len)
            m[14 + (corrupt_at - (logical + addr))] ^= 0x01;
    }
    for (i = 6; i < 14 + len; i++) s += m[i];
    m[14 + len] = (uint8_t)~s;
    host_reply(m, 15 + len);
    return 0;
}
static int ota_frame_get(const uint8_t **p, uint32_t *n)
{
    if (!rx_full) return 0;
    *p = rx; *n = rx_len;
    return 1;
}
static void ota_frame_done(void) { rx_full = 0; }
static uint32_t ota_now_ms(void) { return now_ms; }
static void ota_idle(void) { now_ms += 1; }
static int ota_erase(uint32_t off)
{
    if (off < T_AREA || off >= T_AREA + 0x5000u || (off & 0xFFFu)) { bad_erase++; return -8; }
    erases++;
    memset(nor + off, 0xFF, 4096);
    return 0;
}
static int ota_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    if (off < T_AREA || off + n > T_AREA + 0x5000u) { bad_erase++; return -8; }
    for (i = 0; i < n; i++) nor[off + i] &= s[i];
    return 0;
}
static int ota_fread(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static void ota_show(uint32_t step, int32_t code) { if (step == 9) last_code = code; }
static void ota_commit(const uint8_t *parm) { committed = 1; memcpy(committed_parm, parm, 112); }

#define OTA_OWN_LDR own_ldr
#define OTA_OWN_LEN own_len
#include "../firmware/src/ota.c"
#include "../firmware/stub/stub_core.h"            /* the boot stub reads what this step leaves */

static uint32_t ota_entry(const uint8_t *lg, uint32_t *len)   /* ota.bin's offset (and length) in a logical image */
{
    uint8_t h[0x400];
    uint32_t i, n;
    memcpy(h, lg, sizeof h);
    ota_jl_enc(h, 0x40);
    n = ota_rd16(h + 8);
    for (i = 0; i < n; i++) {
        uint8_t *e = h + 0x40 + i * 0x50u;
        ota_jl_enc(e, 0x50);
        if (ota_rd16(e) == 100) { *len = ota_rd32(e + 12); return ota_rd32(e + 8); }
    }
    return 0;
}

static uint8_t *load_logical(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *raw = malloc(0x200000), *lg;
    size_t n, i;
    if (!f) { perror(path); exit(2); }
    n = fread(raw, 1, 0x200000, f);
    fclose(f);
    lg = malloc(n);
    for (i = 0; i < 20; i++) memcpy(lg + i * 47, raw + i * 48, 47);   /* drop the marker bytes */
    memcpy(lg + 20 * 47, raw + 20 * 48, n - 20 * 48);
    *len = n - 20;
    free(raw);
    return lg;
}

static int own_in_area(void)                        /* the area holds our loader: the head, then ota.bin's body */
{
    return ota_staged() == own_len && !memcmp(nor + T_AREA + 32u, own_ldr + 32u, own_len - 32u) &&
           stub_loader_check(nor + T_AREA) == STUB_LD_OK;
}

static int check(const char *what, int ok)
{
    printf("%-52s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

static int area_erased(void)
{
    uint32_t i;
    for (i = T_AREA; i < T_AREA + 0x5000u; i++) if (nor[i] != 0xFF) return 0;
    return 1;
}

static void reset_dev(void)
{
    memset(nor, 0xFF, sizeof nor);
    memcpy(nor, logical + 0x400, 0x4000);                 /* device flash head = package flash.bin head */
    rx_full = 0; requests = 0; ident_replies = 0; committed = 0; last_code = 0; erases = 0;
    mute_after = 0xFFFFFFFFu; corrupt_at = NULL; bad_erase = 0;
}

static int session(void) { rx_full = 0; requests = 0; committed = 0; erases = 0; return ota_session(); }

int main(int argc, char **argv)
{
    uint8_t *own_pkg, *foreign = NULL, keep[0x5000];
    size_t own_pkg_len, foreign_len = 0;
    uint32_t off, len, k, t, agree = 1, oks = 0;
    int bad = 0, rc;
    if (argc < 2) { printf("usage: ota_test TRANSIT.fwsc [FOREIGN.fwsc]\n"); return 2; }
    own_pkg = load_logical(argv[1], &own_pkg_len);
    off = ota_entry(own_pkg, &len);
    if (!off || len > sizeof own_ldr) { printf("no ota.bin in %s\n", argv[1]); return 2; }
    memcpy(own_ldr, own_pkg + off, len);
    own_len = len;
    if (argc > 2) foreign = load_logical(argv[2], &foreign_len);
    logical = own_pkg; logical_len = own_pkg_len;

    /* handshake outside a session */
    reset_dev();
    {
        uint8_t m[7] = {0, 0x59, 0x11, 0, 0, 0, 0xFF};
        host_reply(m, 7);
        ota_service();
        bad += check("handshake answered", ident_replies == 1);
        bad += check("identity frame is " FELUCCA_ID " (the official V15's)",
                     ota_msg[2] == 0x11 && !memcmp(ota_msg + 6, FELUCCA_ID, sizeof FELUCCA_ID - 1));
    }

    /* a unit with nothing staged (a raw write, a Transporter restore): step 1 stages our own loader */
    reset_dev();
    rc = session();
    printf("  %u requests\n", requests);
    bad += check("nothing staged: our loader staged, the session commits", rc == 0 && committed && own_in_area());
    bad += check("the package's header and the confirmation only", requests >= 3 && requests <= 6);
    {
        uint8_t *p = committed_parm, parm[STUB_PARM_LEN];
        bad += check("record at the area + 0x4F00 == RAM record", !memcmp(nor + T_AREA + 0x4F00u, p, 112));
        bad += check("record CRC / type / magic / ota_addr",
                     ota_rd16(p) == ota_crc16(p + 2, 78, 0) && ota_rd16(p + 2) == 0x5A0Du &&
                     ota_rd16(p + 6) == 0x5441u && ota_rd32(p + 72) == T_AREA);
        bad += check("record: the identity " T_PARM_ID ", zeros behind",
                     !memcmp(p + 8, T_PARM_ID, sizeof T_PARM_ID - 1u) && !p[8 + sizeof T_PARM_ID - 1u] && !p[0x27]);
        stub_record(parm);
        bad += check("stub: its record == the committed one (its rescue is this hand-over)", !memcmp(parm, p, 112));
        bad += check("nothing written outside the staging area", bad_erase == 0);
        bad += check("our loader ends below the record's sector", own_len <= 0x4000u);
    }
    memcpy(keep, nor + T_AREA, sizeof keep);
    /* the boot after it (the loader never ran): the loader stays, the record goes */
    bad += check("boot: our loader kept, the record's sector erased (rc 2)",
                 ota_boot_cleanup() == 2 && own_in_area() && nor[T_AREA + 0x4F00u] == 0xFF && bad_erase == 0);
    bad += check("second boot: nothing to do, the loader still ours", ota_boot_cleanup() == 0 && own_in_area());
    /* the next install, our loader kept: nothing erased but the record's sector */
    rc = session();
    bad += check("loader kept: the session commits, only the record's sector erased",
                 rc == 0 && committed && own_in_area() && erases == 1u && !memcmp(nor + T_AREA, keep, 0x4000));
    ota_boot_cleanup();
    if (foreign) {
        uint32_t flen;
        logical = foreign; logical_len = foreign_len;
        ota_entry(foreign, &flen);
        rc = session();
        bad += check("a foreign package (its own loader in it): commits, our loader staged",
                     rc == 0 && committed && own_in_area() && erases == 1u);
        bad += check(".. its loader is not ours and was never staged",
                     flen != own_len || memcmp(nor + T_AREA + 32u, foreign + ota_entry(foreign, &flen) + 32u, 64) != 0);
        logical = own_pkg; logical_len = own_pkg_len;
        ota_boot_cleanup();
    }
    /* failures leave our loader as it is */
    mute_after = 2;
    rc = session();
    mute_after = 0xFFFFFFFFu;
    bad += check("host gone after two reads: no commit, our loader still there", rc < 0 && !committed && own_in_area());
    {
        uint32_t total;
        rc = session();
        total = requests;
        ota_boot_cleanup();
        mute_after = total - 1;                            /* everything but the confirmation */
        rc = session();
        mute_after = 0xFFFFFFFFu;
        bad += check("no 'success': -13, no commit, our loader still there", rc == -13 && !committed && own_in_area());
    }
    corrupt_at = logical + 0x20 + 3;                       /* the UFW header's entry list */
    rc = session();
    corrupt_at = NULL;
    bad += check("a damaged package header: refused, our loader still there", rc < 0 && !committed && own_in_area());
    /* a damaged staged loader is replaced by our copy */
    nor[T_AREA + 0x200u] ^= 0x10;
    bad += check("one bit of the staged loader flipped: not ours byte for byte", !ota_own_staged());
    rc = session();
    bad += check(".. the next session restages it and commits", rc == 0 && committed && own_in_area());
    ota_boot_cleanup();
    /* the boot cleanup: what is not our whole loader goes, and the app then restages it */
    memcpy(nor + T_AREA, keep, sizeof keep);
    memset(nor + T_AREA + 0x4000u, 0xFF, 0x1000u);
    memset(nor + T_AREA, 0xFF, 32);
    bad += check("stub: no head (staging not finished) -> refused", stub_loader_check(nor + T_AREA) == STUB_LD_HEAD);
    bad += check("boot: a staging without its head and no record: left (rc 0), not ours",
                 ota_boot_cleanup() == 0 && !ota_own_staged());
    bad += check(".. so the boot restages it (ota_restage)", ota_restage() == 0 && own_in_area());
    memcpy(nor + T_AREA, keep, sizeof keep);
    memset(nor + T_AREA + 0x4000u, 0xFF, 0x1000u);
    memset(nor + T_AREA + own_len / 2u, 0xFF, 0x4000u - own_len / 2u);
    bad += check("stub: erased from half way on -> refused", stub_loader_check(nor + T_AREA) == STUB_LD_BLOCKS);
    bad += check("boot: a torn loader is erased", ota_boot_cleanup() == 1 && area_erased());
    memcpy(nor + T_AREA + 0x4F00u, committed_parm, 112);
    bad += check("boot: a record alone is erased", ota_boot_cleanup() == 1 && area_erased());
    memcpy(nor + T_AREA, keep, sizeof keep);
    memset(nor + T_AREA + 0x4000u, 0xFF, 0x1000u);
    nor[T_AREA + 0x4Cu] = 'X';
    bad += check("boot: not a Felucca-family loader is erased", ota_boot_cleanup() == 1 && area_erased());
    bad += check(".. restaged", ota_restage() == 0 && own_in_area());
    /* the two checks in step: the same answer on damaged copies of the staged loader */
    memcpy(keep, nor + T_AREA, sizeof keep);
    srand(1);
    for (t = 0; t < 20000u; t++) {
        memcpy(nor + T_AREA, keep, sizeof keep);
        for (k = 0; k < 1u + (uint32_t)rand() % 3u; k++) {
            uint32_t at = (uint32_t)rand() % (t & 1u ? 0x90u : own_len + 64u);
            if (rand() & 3) nor[T_AREA + at] ^= (uint8_t)(1u << (rand() & 7));
            else memset(nor + T_AREA + at, 0xFF, 0x5000u - at > 600u ? 600u : 0x5000u - at);
        }
        k = stub_loader_check(nor + T_AREA) == STUB_LD_OK;
        oks += k;
        agree &= k == (ota_staged() != 0);
    }
    printf("  %u of 20000 damaged copies still pass both checks (block contents are not checked)\n", oks);
    bad += check("ota_staged == stub_loader_check on 20000 damaged copies", agree && oks > 100u && oks < 19000u);

    printf("%s\n", bad ? "OTA TEST FAILED" : "ota test passed");
    return bad != 0;
}
