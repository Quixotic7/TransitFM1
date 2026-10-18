/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Copyright (C) 2026 ChoralRoot FM-1 contributors (the kept loader), TransitFM1 contributors (step 1) */
/* M-UPGRADE update entry, TransitFM1's.
 *
 * The updater talks SysEx (F0 pack7(00 59 cmd len24 body chk) F7):
 *   cmd 0x11  handshake -> we answer our package identity (FELUCCA_ID: FM-1_015, as the official V15, so that
 *             every installer that guards against other firmwares writes over TransitFM1 as over the stock one)
 *   F0 22 24 35 7F F7  upgrade -> ota_session(): pull the package's header with cmd 0x30 read requests and check
 *     it, make sure OUR OWN update loader is staged at 0xE0000 (not the package's: docs/SAFETY.md), ask
 *     0xE0000000 ("success"), then write the UPDATA_PARM record (flash 0xE4F00 + RAM 0x01C7FD88) and reset. The
 *     SPL runs our loader (firmware/loader), which installs the package in the order that keeps the boot stub's
 *     rescue for all of the write but its last sector.
 * Nothing is committed before the host's "success"; a failure leaves our staged loader as it is (the boot stub's
 * rescue needs it) and TransitFM1 carries on.
 *
 * The staging area's life (ChoralRoot's rule, docs/BOOT-SAFETY.md "As built" there; docs/SAFETY.md here): the app,
 * at every boot (ota_boot_cleanup), KEEPS a staged loader that is ours and whole (ota_staged: the boot stub's
 * check) and removes only a record left beside it, and erases anything else; then (ota_own_staged, ota_restage)
 * stages its own copy of the loader (OTA_OWN_LDR, built into the app by tools/build.py) if what is staged is not
 * that copy. The kept loader is what the boot stub hands over to: after an interrupted write, and when OCT- is held
 * at power-on.
 *
 * The firmware (platform.c) and the host tests supply these hooks:
 *   ota_wire_send(p, n)        one complete F0..F7 message to the host
 *   ota_frame_get(&p, &n)      next received SysEx (7-bit bytes between F0/F7), 0 if none
 *   ota_frame_done()           release it
 *   ota_now_ms(), ota_idle()   time; called while waiting (watchdog, USB)
 *   ota_erase(off) ota_prog(off, p, n) ota_fread(off, p, n)   flash, 0 = ok
 *   ota_show(step, code)       progress / result for the screen
 *   ota_commit(parm)           write RAM record + reset (never returns on hardware)
 * and, in the app (not the loader), OTA_OWN_LDR / OTA_OWN_LEN: our ota.bin (outer head, inner head, blocks).
 */
#define OTA_AREA 0x000E0000u                     /* staged loader, 5 x 4 KiB */
#define OTA_AREA_LEN 0x5000u
#define OTA_RES (OTA_AREA + OTA_AREA_LEN - 0x100u)  /* 0xE4F00 = 4K boundary - 256: SPL scan */
#define OTA_LDR_HCRC 0xEBAAu                     /* official update loader (usb_hid_ota.bin) */
#define OTA_LDR_DCRC 0x5881u
#define OTA_LDR_LEN 0x4DE1u
#define OTA_FRAME_MAX 640u                       /* 512-B response = 605 wire bytes */

static int ota_wire_send(const uint8_t *p, uint32_t n);
static int ota_frame_get(const uint8_t **p, uint32_t *n);
static void ota_frame_done(void);
static uint32_t ota_now_ms(void);
static void ota_idle(void);
static int ota_erase(uint32_t off);
static int ota_prog(uint32_t off, const void *p, uint32_t n);
static int ota_fread(uint32_t off, void *p, uint32_t n);
static void ota_show(uint32_t step, int32_t code);
static void ota_commit(const uint8_t *parm);

static uint32_t ota_rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t ota_rd24(const uint8_t *p) { return ota_rd16(p) | (uint32_t)p[2] << 16; }
static uint32_t ota_rd32(const uint8_t *p) { return ota_rd24(p) | (uint32_t)p[3] << 24; }
static void ota_wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void ota_wr32(uint8_t *p, uint32_t v) { ota_wr16(p, v); ota_wr16(p + 2, v >> 16); }

static uint32_t ota_crc16(const uint8_t *p, uint32_t n, uint32_t c)   /* poly 0x1021, init 0 */
{
    uint32_t i;
    while (n--) {
        c ^= (uint32_t)*p++ << 8;
        for (i = 0; i < 8u; i++)
            c = c & 0x8000u ? (c << 1) ^ 0x1021u : c << 1;
        c &= 0xFFFFu;
    }
    return c;
}

static void ota_jl_enc(uint8_t *p, uint32_t n)   /* UFW header cipher, key 0xFFFF */
{
    uint32_t k = 0xFFFFu;
    while (n--) {
        *p++ ^= (uint8_t)k;
        k = ((k << 1) ^ (k & 0x8000u ? 0x1021u : 0u)) & 0xFFFFu;
    }
}

/* 8-to-7, LSB first */
static uint32_t ota_pack7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 8u;
        while (nb >= 7u) {
            out[o++] = (uint8_t)(acc & 0x7Fu);
            acc >>= 7;
            nb -= 7u;
        }
    }
    if (nb)
        out[o++] = (uint8_t)(acc & 0x7Fu);
    return o;
}

static uint32_t ota_unpack7(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t max)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)(*in++ & 0x7Fu) << nb;
        nb += 7u;
        if (nb >= 8u) {
            if (o < max)
                out[o] = (uint8_t)acc;
            o++;
            acc >>= 8;
            nb -= 8u;
        }
    }
    return o;
}

static uint8_t ota_msg[8 + 32], ota_wire[2 + 48];
static uint8_t ota_dec[OTA_FRAME_MAX];           /* decoded host message */

static int ota_send_msg(uint32_t cmd, const uint8_t *body, uint32_t n)   /* n <= 32 */
{
    uint32_t i, s = 0, w;
    ota_msg[0] = 0;
    ota_msg[1] = 0x59;
    ota_msg[2] = (uint8_t)cmd;
    ota_msg[3] = (uint8_t)n;
    ota_msg[4] = 0;
    ota_msg[5] = 0;
    for (i = 0; i < n; i++) {
        ota_msg[6 + i] = body[i];
        s += body[i];
    }
    ota_msg[6 + n] = (uint8_t)~s;
    ota_wire[0] = 0xF0;
    w = 1u + ota_pack7(ota_msg, 7u + n, ota_wire + 1);
    ota_wire[w++] = 0xF7;
    return ota_wire_send(ota_wire, w);
}

static void ota_reply_identity(void)
{
    static const char ID[] = FELUCCA_ID;         /* the app, its rescue mode and the loader each answer their own */
    uint8_t id[27];
    uint32_t i;
    for (i = 0; i < sizeof id; i++)
        id[i] = i < sizeof ID - 1u ? (uint8_t)ID[i] : 0;
    ota_send_msg(0x11, id, sizeof id);
}

/* decode the pending frame; returns its decoded length (0 = not ours) */
static uint32_t ota_take(void)
{
    const uint8_t *p;
    uint32_t n, d, s = 0, i;
    if (!ota_frame_get(&p, &n))
        return 0;
    d = ota_unpack7(p, n, ota_dec, sizeof ota_dec);
    ota_frame_done();
    if (d < 7u || d > sizeof ota_dec || ota_dec[0] != 0 || ota_dec[1] != 0x59 || ota_rd24(ota_dec + 3) + 7u != d)
        return 0;
    for (i = 6; i + 1u < d; i++)
        s += ota_dec[i];
    if ((uint8_t)~s != ota_dec[d - 1u])
        return 0;
    return d;
}

/* main loop, outside a session: answer handshakes */
static void ota_service(void)
{
    uint32_t d = ota_take();
    if (d == 7u && ota_dec[2] == 0x11)
        ota_reply_identity();
}

/* one read request (3 tries x 5 s); data -> dst */
static uint32_t ota_deadline;                    /* 0 = none; else ota_now_ms() limit of the session */

static int ota_read(uint32_t addr, uint8_t *dst, uint32_t len)
{
    uint8_t q[8];
    uint32_t tries, t, d, i;
    if (ota_deadline && (int32_t)(ota_now_ms() - ota_deadline) > 0)
        return -1;                                  /* the whole session took too long: give up */
    q[0] = 0;
    ota_wr32(q + 1, addr);
    q[5] = (uint8_t)len;
    q[6] = (uint8_t)(len >> 8);
    q[7] = 0;
    for (tries = 0; tries < 3u; tries++) {
        if (ota_send_msg(0x30, q, 8))
            return -1;
        for (t = ota_now_ms(); ota_now_ms() - t < 5000u;) {
            ota_idle();
            d = ota_take();
            if (!d)
                continue;
            if (d == 7u && ota_dec[2] == 0x11) {
                ota_reply_identity();
                continue;
            }
            if (d == len + 15u && ota_dec[2] == 0x30 && ota_rd24(ota_dec + 3) == len + 8u &&
                ota_rd32(ota_dec + 7) == addr && ota_rd24(ota_dec + 11) == len) {
                for (i = 0; i < len; i++)
                    dst[i] = ota_dec[14 + i];
                return 0;
            }
        }
    }
    return -1;
}

static int ota_memeq(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    while (n--)
        if (*a++ != *b++)
            return 0;
    return 1;
}

/* JLFS head "LOADER.BIN" in front of the staged loader (vm_update_defrag), written last */
static int ota_stage_head(uint32_t len)
{
    static const char NAME[16] = "LOADER.BIN";
    uint8_t h[32];
    uint32_t i;
    for (i = 0; i < 32u; i++)
        h[i] = 0;
    ota_wr16(h + 2, 0xFFFFu);
    ota_wr32(h + 4, OTA_AREA);
    ota_wr32(h + 8, len + 32u);
    h[12] = 0x41u;
    h[13] = 0xFFu;
    ota_wr16(h + 14, 0xFFFFu);
    for (i = 0; i < 16u; i++)
        h[16 + i] = (uint8_t)NAME[i];
    ota_wr16(h, ota_crc16(h + 2, 30, 0));
    return ota_prog(OTA_AREA, h, 32);
}

/* UFW header + entry list (hdr: 1 KiB) from the host -> offset / length of
 * flash.bin (entry type 0) and ota.bin (type 100), 0 if absent, and the CRC16 the
 * package stores for flash.bin (the loader checks what it was served against it);
 * -1 read, -2 CRC */
static int ota_ufw(uint8_t *hdr, uint32_t *fl_off, uint32_t *fl_len, uint32_t *ota_off, uint32_t *ota_len,
                   uint32_t *fl_crc)
{
    uint32_t i, nent;
    *fl_off = *fl_len = *ota_off = *ota_len = *fl_crc = 0;
    if (ota_read(0, hdr, 512) || ota_read(512, hdr + 512, 512))
        return -1;
    ota_jl_enc(hdr, 0x40);
    nent = ota_rd16(hdr + 8);
    if (ota_crc16(hdr + 2, 0x3E, 0) != ota_rd16(hdr) || nent == 0 || nent > 11u ||
        ota_crc16(hdr + 0x40, nent * 0x50u, 0) != ota_rd16(hdr + 2))
        return -2;
    for (i = 0; i < nent; i++) {
        uint8_t *e = hdr + 0x40 + i * 0x50u;
        ota_jl_enc(e, 0x50);
        if (ota_rd16(e) == 0) {                             /* flash.bin */
            *fl_crc = ota_rd16(e + 4);
            *fl_off = ota_rd32(e + 8);
            *fl_len = ota_rd32(e + 12);
        }
        if (ota_rd16(e) == 100) {                           /* ota.bin */
            *ota_off = ota_rd32(e + 8);
            *ota_len = ota_rd32(e + 12);
        }
    }
    return 0;
}

/* The staged loader is ours and whole: the boot stub's check (firmware/stub/stub_core.h stub_loader_check, which
 * reads the area through XIP; here through ota_fread in small pieces, no 20 KiB buffer). The two are kept in step by
 * tests/ota_test.c (the same answer on the staged loader and on thousands of damaged copies). Returns the staged
 * length from OTA_AREA (the head, the inner head, the blocks), 0 if the stub would refuse it. (ChoralRoot's.) */
static uint32_t ota_staged(void)
{
    uint8_t h[0x88], q[8];
    uint32_t len, isize, p, end, total = 0, i, own = 0;
    if (ota_fread(OTA_AREA, h, sizeof h))
        return 0;
    if (ota_crc16(h + 2, 30, 0) != ota_rd16(h) || ota_rd32(h + 4) != OTA_AREA || h[12] != 0x41u ||
        !ota_memeq(h + 16, (const uint8_t *)"LOADER.BIN", 11))
        return 0;
    len = ota_rd32(h + 8) - 32u;
    if (len < 32u + 8u || len > OTA_AREA_LEN - 0x200u)
        return 0;
    isize = ota_rd32(h + 0x20 + 4);
    if (ota_crc16(h + 0x22, 30, 0) != ota_rd16(h + 0x20) || ota_rd32(h + 0x20 + 8) != 0x01C0A800u ||
        !isize || isize > 0x14000u)
        return 0;
    for (i = 0x48; i + 16u <= 0x48u + 64u && !own; i++)
        own = (uint32_t)ota_memeq(h + i, (const uint8_t *)"FELUCCA-LOADER-1", 16);
    if (!own)
        return 0;
    for (p = 0x40, end = 0x20u + len; p + 8u <= end;) {
        uint32_t clen, dlen;
        if (ota_fread(OTA_AREA + p, q, 8))
            return 0;
        clen = ota_rd32(q);
        dlen = ota_rd32(q + 4);
        if (!clen || clen > 4096u || !dlen || dlen > 4096u || clen > end - p - 8u)
            return 0;
        p += 8u + clen;
        total += dlen;
        if (dlen != 4096u && p != end)
            return 0;
    }
    return p == end && total == isize ? end : 0;
}

#ifdef OTA_OWN_LDR
/* what is staged is our own copy, byte for byte (the head ota_stage_head writes, then ota.bin from its inner head) */
static int ota_own_staged(void)
{
    uint8_t b[256];
    uint32_t len = OTA_OWN_LEN - 32u, i, n;
    if (ota_staged() != 32u + len)
        return 0;
    for (i = 0; i < len; i += n) {
        n = len - i > sizeof b ? sizeof b : len - i;
        if (ota_fread(OTA_AREA + 32u + i, b, n) || !ota_memeq(b, OTA_OWN_LDR + 32u + i, n))
            return 0;
    }
    return 1;
}

/* stage our own loader: erase, body, read back against its CRC, head last (a cut leaves no head: nothing staged,
 * restaged at the next boot). 0 = staged.
 * OTA_OWN_LDR is a const array, so in XIP flash on the device: the flash driver sends a page's bytes while the flash
 * is out of XIP (hal/fm1_flash.h fl_prog_ram), so a source there faults. Each piece is copied to RAM first (0.1 did
 * not: its first restage faulted, at the boot after a hop from the official V15, which stages elsewhere) */
static int ota_restage(void)
{
    static uint8_t b[256];
    uint32_t len = OTA_OWN_LEN - 32u, c = ota_rd16(OTA_OWN_LDR + 2), crc = 0, i, n, k;
    if (OTA_OWN_LEN < 32u + 40u || len > OTA_AREA_LEN - 0x1000u)
        return -20;
    for (i = 0; i < OTA_AREA_LEN; i += 0x1000u)
        if (ota_erase(OTA_AREA + i))
            return -21;
    for (i = 0; i < len; i += n) {
        n = len - i > sizeof b ? sizeof b : len - i;
        for (k = 0; k < n; k++)
            b[k] = OTA_OWN_LDR[32u + i + k];
        if (ota_prog(OTA_AREA + 32u + i, b, n))
            return -22;
    }
    for (i = 0; i < len; i += n) {
        n = len - i > sizeof b ? sizeof b : len - i;
        if (ota_fread(OTA_AREA + 32u + i, b, n))
            return -23;
        crc = ota_crc16(b, n, crc);
    }
    if (crc != c || ota_stage_head(len))
        return -24;
    return ota_own_staged() ? 0 : -25;
}

/* the boot's part (platform.c, after the screen is up): what an update left goes (ota_boot_cleanup), then our own
 * loader is staged if what is staged is not it, byte for byte. 0 it was there, 1 staged now, < 0 not staged (the
 * boot stub's rescue would then end in UBOOT; step 1 tries again) */
static int ota_boot_cleanup(void);
static int ota_boot_stage(void)
{
    ota_boot_cleanup();
    if (ota_own_staged())
        return 0;
    return ota_restage() == 0 ? 1 : -1;
}

/* step 1, TransitFM1's: the package is well formed and has a flash.bin the size of the app area; our own loader is
 * staged (it writes any package: firmware/loader/ldr_core.c); the host confirms. The package's own loader is never
 * staged: ours keeps the boot stub's rescue alive through the write. 0 = host said success */
static int ota_stage(void)
{
    static uint8_t hdr[0x400], b[512];
    uint32_t i, ota_off, ota_len, fl_off, fl_len, fl_crc;
    int rc;
    ota_show(1, 0);
    if ((rc = ota_ufw(hdr, &fl_off, &fl_len, &ota_off, &ota_len, &fl_crc)) != 0)
        return rc;
    if (!fl_off || fl_len < 0x93000u)
        return -3;
    /* a flash.bin longer than the app area (Baud Girl's grows past 0x93000 when its app outgrows the slot): our
     * loader writes the app area only, so it would leave a truncated firmware with its sector 0x4000 committed: a
     * dark unit (0.1.2 did that, 2026-10-17). Refused before anything is staged; such a firmware is installed from
     * the official V15, whose updater stages the package's own loader */
    if (fl_len > 0x93000u)
        return -4;
    ota_show(3, 0);
    if (!ota_own_staged() && (rc = ota_restage()) != 0)
        return rc;
    ota_show(4, 0);
    for (i = 0; i < 4u; i++)
        if (!ota_read(0xE0000000u, b, 8) && ota_memeq(b, (const uint8_t *)"success", 8))
            return 0;
    return -13;
}
#endif

static void ota_unstage(void)
{
    uint32_t i;
    for (i = 0; i < OTA_AREA_LEN; i += 0x1000u)
        ota_erase(OTA_AREA + i);
}

#ifdef OTA_OWN_LDR
/* returns only when nothing was committed (refused, failed, or dry run) */
static int ota_session(void)
{
    static uint8_t parm[112];
    uint8_t back[112];
    uint32_t i;
    int rc;
    ota_deadline = ota_now_ms() + 120000u;          /* step 1 normally takes ~5 s; a stalled host gives up after 2 min */
    rc = ota_stage();
    if (rc) {                                       /* (our staged loader stays: the boot stub's rescue needs it) */
        ota_show(9, rc);
        return rc;
    }
    /* 5./7. UPDATA_PARM (update_mode_api_v2 layout), flash copy first */
    for (i = 0; i < sizeof parm; i++)
        parm[i] = 0;
    ota_wr16(parm + 2, 0x5A0Du);
    ota_wr16(parm + 4, 0x5A01u);
    ota_wr16(parm + 6, 0x5441u);
    for (i = 0; i < 12u; i++)
        parm[8 + i] = (uint8_t)"ota-FM-1_015"[i];    /* the loader's own USB identity */
    ota_wr32(parm + 72, OTA_AREA);
    ota_wr16(parm, ota_crc16(parm + 2, 78, 0));
#if FELUCCA_OTA_DRYRUN
    ota_show(9, 1);                                 /* 1 = dry run complete */
    return 1;
#endif
#ifndef FELUCCA_OTA_RAMONLY
#define FELUCCA_OTA_RAMONLY 0                       /* 1: RAM record only, no power-loss resume (a power
                                                     * cycle leaves a broken loader behind) */
#endif
    /* the record's sector (0xE4000: no byte of our loader, which tools/build.py keeps below it) is erased first:
     * the loader kept from the install before stays staged, and a record a cut session left there would not take
     * another write over it */
    if (!FELUCCA_OTA_RAMONLY && (ota_erase(OTA_AREA + OTA_AREA_LEN - 0x1000u) ||
        ota_prog(OTA_RES, parm, sizeof parm) || ota_fread(OTA_RES, back, sizeof back) ||
        !ota_memeq(back, parm, sizeof parm))) {
        ota_erase(OTA_AREA + OTA_AREA_LEN - 0x1000u);  /* the record's sector only (no loader byte there) */
        ota_show(9, -14);
        return -14;
    }
    ota_show(5, 0);
    ota_commit(parm);
    return 0;
}

#endif

/* boot: what an update left in the staging area (we are running, so the update finished or never started). A loader
 * of ours that is whole stays: it is the one the boot stub starts when OCT- is held at power-on or the app's marker
 * is gone (firmware/stub). A staged loader alone starts nothing: the SPL runs it only from the RAM record after a
 * warm reset (every install; the stub's hand-over), not from the flash copy of the record on a cold boot. The flash
 * copy of the record is removed if one is still there (the loader erases it when its write is done), but only when
 * its sector holds no loader byte (ours ends in the second sector; tools/build.py stops a loader that would reach
 * the fifth). Anything else in the area (another loader, a torn staging) is erased. (ChoralRoot's.)
 * Returns 0 nothing to do, 1 the area erased, 2 the loader kept and the record's sector erased, -1 a read error. */
static int ota_boot_cleanup(void)
{
    uint8_t a[4], r[8];
    uint32_t i, used = 0, rec = 0, end;
    if (ota_fread(OTA_AREA, a, 4) || ota_fread(OTA_RES, r, 8))
        return -1;
    for (i = 0; i < 4u; i++)
        used |= a[i] != 0xFFu;
    for (i = 0; i < 8u; i++)
        rec |= r[i] != 0xFFu;
    end = ota_staged();
    if (end) {
        if (!rec || end > OTA_AREA_LEN - 0x1000u)
            return 0;
        ota_erase(OTA_AREA + OTA_AREA_LEN - 0x1000u);
        return 2;
    }
    if (used | rec)
        ota_unstage();
    return (int)(used | rec);
}
