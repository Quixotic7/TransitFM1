/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Copyright (C) 2026 ChoralRoot FM-1 contributors (the stub order), TransitFM1 contributors (the foreign order) */
/* TransitFM1's update loader, the portable part (step 2 of the M-UPGRADE update protocol). Felucca's loader as
 * SLOOP carries it (the package CRC, the passes), with ChoralRoot's order for packages that carry its boot stub,
 * and TransitFM1's order for every other package (docs/SAFETY.md).
 *
 * The host serves the package ("logical image") with cmd 0x30 reads (ota.c). The loader:
 *   1. reads the UFW header and finds flash.bin;
 *   2. checks that the package's app area decrypts with THIS chip's key (taken from isd_config.ini in the
 *      device's own flash head), so a package for another key is refused before anything is erased;
 *   3. writes ONLY the app area [0x4000, 0x93000), sector by sector, skipping sectors that are already equal,
 *      and verifies each one; the flash head (SPL, isd_config) is never written. A package whose flash.bin
 *      reaches past 0x93000 is refused before anything is written: its firmware would be committed truncated;
 *   4. checks the CRC16 the package stores for flash.bin against everything it was served (the head too, which
 *      it never writes; a mismatch: up to two more passes, rewriting what differs), asks 0xF0000000 ("success"),
 *      then invalidates the update record (RAM and flash) so the SPL boots the new app, and resets.
 *
 * The order of 3, so that a write cut at any point leaves a unit that the boot stub in sector 0x4000 (ChoralRoot's,
 * firmware/stub) turns into a rescue for as long as possible. One sector of the package is the COMMIT sector: it is
 * read in its place (for the CRC) and held in RAM, and written only after every other sector is written and
 * verified AND the whole package's CRC holds:
 *   - a package WITH the stub's commit marker (TransitFM1's, ChoralRoot's): the commit sector is the marker's
 *     (0x91000). Before the first write anywhere the marker's sector on the flash is erased (the body is
 *     uncommitted: the stub will not boot it), sector 0x4000 is written first and only if it differs (the one
 *     window without a working stub; the same bytes in every build with the same stub, so normally never), then
 *     the body, then the marker. ChoralRoot's order (docs/BOOT-SAFETY.md "As built"), with the commit after the CRC.
 *   - ANY OTHER package (the official firmware, Felucca, SLOOP, Melodee, Bubba Machine, ...: no marker): the commit
 *     sector is 0x4000 itself, the new firmware's directory and entry. The marker's sector on the flash is erased
 *     first as above, then everything from 0x5000 up in address order, then, last, sector 0x4000. Until that last
 *     sector's erase the running stub (TransitFM1's, still in 0x4000) finds no marker and hands over to the staged
 *     loader: every cut but those in the last sector is a rescue. The last sector's erase and its 16 pages are the
 *     one window (about 0.1 s of a 1 to 2 minute write); then the new firmware is whole and runs.
 * Power loss: the SPL does not start the loader from the flash copy of the record on a cold boot (ChoralRoot's
 * dark-unit dump, docs/SAFETY.md); the stub finds no marker and starts the staged loader again, or the ROM's USB boot.
 *
 * Hooks from the platform (loader.c, tests/ldr_test.c):
 *   ldr_fread(off, p, n)  ldr_erase(off)  ldr_prog(off, p, n)   flash, 0 = ok
 *   ldr_record_clear()    forget the RAM update record
 *   ldr_flash_known()     the flash chip is the 1 MiB part the FM-1 has (else nothing is written: the update
 *                         records go and the old firmware starts again)
 *   ldr_progress(done, total)
 * plus the ota.c hooks (frames, time, idle). */
#define LDR_APP_LO 0x4000u
#define LDR_APP_HI 0x93000u
#define LDR_REC_LO 0x93000u                     /* update records live above the app ... */
#define LDR_REC_HI 0xFC000u                     /* ... and below the globals sector */
#define LDR_STUB_SEC 0x4000u                    /* the directory and the boot stub */
#define LDR_MARK_SEC 0x91000u                   /* stub_core.h STUB_MARK_SEC, STUB_MARK_OFF */
#define LDR_MARK_OFF 0x91FE0u

static int ldr_fread(uint32_t off, void *p, uint32_t n);
static int ldr_erase(uint32_t off);
static int ldr_prog(uint32_t off, const void *p, uint32_t n);
static void ldr_record_clear(void);
static int ldr_flash_known(void);
static void ldr_progress(uint32_t done, uint32_t total);

/* SFC cipher: per 32-byte block, key ^ (block offset >> 2) */
static void ldr_sfc(uint8_t *p, uint32_t n, uint32_t base_off, uint32_t key)
{
    uint32_t i, j;
    for (i = 0; i < n; i += 32u) {
        uint32_t k = key ^ ((base_off + i) >> 2);
        for (j = 0; j < 32u && i + j < n; j++) {
            p[i + j] ^= (uint8_t)k;
            k = ((k << 1) ^ (k & 0x8000u ? 0x1021u : 0u)) & 0xFFFFu;
        }
    }
}

/* chip key from the device's flash head: JLFS top entries (ENC 0xFFFF) -> isd_config.ini blob */
static int ldr_chip_key(uint32_t *key)
{
    uint8_t e[32], blob[34];
    uint32_t off, i, sum, k;
    for (off = 32; off < 0x400u; off += 32u) {
        if (ldr_fread(off, e, 32))
            return -1;
        ota_jl_enc(e, 32);
        if (ota_crc16(e + 2, 30, 0) != ota_rd16(e))
            return -2;
        if (!ota_memeq(e + 16, (const uint8_t *)"isd_config.ini", 15)) {
            if (ota_rd16(e + 14))
                return -3;                               /* last entry, not found */
            continue;
        }
        if (ldr_fread(ota_rd32(e + 4), blob, 34) || ota_crc16(blob, 32, 0) != ota_rd16(blob + 32))
            return -4;
        for (i = 0, sum = 0; i < 16u; i++)
            sum += blob[i];
        sum &= 0xFFu;
        sum = sum >= 0xE0u ? 0xAAu : sum <= 0x10u ? 0x55u : sum;
        for (i = 0, k = 0; i < 16u; i++)
            if ((uint32_t)(blob[16 + i] ^ blob[15 - i]) < sum)
                k |= 1u << i;
        *key = k;
        return 0;
    }
    return -3;
}

#define LDR_EVERIFY (-11)                       /* a sector did not verify, twice */
#define LDR_EBADPKG (-12)                       /* flash.bin as served does not match the package's CRC16 */
#define LDR_ENOFIN (-13)                        /* no "success" from the host */
#define LDR_EFLASH (-14)                        /* another flash chip: nothing written, the records dropped */
#define LDR_ENOERASE (-15)                      /* the marker's sector does not erase: nothing else was touched */
#define LDR_ETOOLONG (-16)                      /* flash.bin reaches past the app area: refused, nothing written */

/* the update records (RAM, and flash: a 4K boundary - 256) go: the SPL boots the app */
static void ldr_records_drop(void)
{
    uint32_t s;
    ldr_record_clear();
    for (s = LDR_REC_HI; s > LDR_REC_LO; s -= 0x1000u) {
        uint8_t r[80];
        if (ldr_fread(s - 0x100u, r, 80))
            break;
        if (ota_rd16(r + 6) == 0x5441u && ota_rd16(r) && ota_rd16(r) == ota_crc16(r + 2, 78, 0))
            ldr_erase(s - 0x1000u);
    }
}

/* CRC-32 (zlib's), bit by bit: the marker's own 16 bytes only */
static uint32_t ldr_crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu, i;
    while (n--) {
        c ^= *p++;
        for (i = 0; i < 8u; i++)
            c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

/* does the package carry the stub's commit marker? (the 512 bytes that hold it, decrypted in place) */
static int ldr_pkg_marker(uint32_t fl_off, uint8_t *b, uint32_t key)
{
    const uint8_t *m = b + (LDR_MARK_OFF & 0x1FFu);
    if (ota_read(fl_off + (LDR_MARK_OFF & ~0x1FFu), b, 512))
        return -7;
    ldr_sfc(b, 512, (LDR_MARK_OFF & ~0x1FFu) - LDR_APP_LO, key);
    return ota_rd32(m) == 0x4D425243u && ota_rd32(m + 4) == 1u && ota_rd32(m + 16) == ldr_crc32(m, 16);
}

/* the marker's sector erased, once, before the first write elsewhere: from here to the commit the stub refuses
 * the body. Left alone when it is blank already (a resumed install) */
static int ldr_uncommit(uint8_t *cur, uint32_t *open)
{
    uint32_t i, tries;
    if (*open)
        return 0;
    for (tries = 0; tries < 3u; tries++) {
        if (ldr_fread(LDR_MARK_SEC, cur, 0x1000u))
            return -8;
        for (i = 0; i < 0x1000u && cur[i] == 0xFFu; i++)
            ;
        if (i == 0x1000u) {
            *open = 1;
            return 0;
        }
        if (tries < 2u && ldr_erase(LDR_MARK_SEC))
            return -9;
    }
    return LDR_ENOERASE;
}

/* sector s from sec (the package's, already read): left alone when the flash holds it already, else (the marker's
 * sector uncommitted first, unless s is that sector) erased, written and read back, twice at most. Once the
 * marker's sector itself is written (a foreign package's own bytes there, or the commit) it counts as uncommitted:
 * a later sector must not erase it again */
static int ldr_write(uint32_t s, const uint8_t *sec, uint8_t *cur, uint32_t *open)
{
    uint32_t k, tries;
    int rc;
    if (s == LDR_MARK_SEC)
        *open = 1;
    if (ldr_fread(s, cur, 0x1000u))
        return -8;
    if (ota_memeq(sec, cur, 0x1000u))
        return 0;
    if ((rc = ldr_uncommit(cur, open)) != 0)
        return rc;
    for (tries = 0; tries < 2u; tries++) {
        if (ldr_erase(s))
            return -9;
        for (k = 0; k < 0x1000u; k += 256u)
            if (ldr_prog(s + k, sec + k, 256))
                return -10;
        if (ldr_fread(s, cur, 0x1000u))
            return -8;
        if (ota_memeq(sec, cur, 0x1000u))
            return 0;
    }
    return LDR_EVERIFY;
}

static int ldr_pass(void)
{
    static uint8_t hdr[0x400], sec[0x1000], cur[0x1000], held[0x1000];
    uint32_t i, k, fl_off, fl_size, ota_off, ota_len, key, s, want, crc = 0, done = 0, open = 0, commit;
    int rc, mark;
    /* 1. UFW header + entry list (ota.c) */
    if ((rc = ota_ufw(hdr, &fl_off, &fl_size, &ota_off, &ota_len, &want)) != 0)
        return rc;
    if (!fl_off || fl_size < LDR_APP_HI)
        return -3;
    if (fl_size > LDR_APP_HI)                            /* more than this loader writes: a truncated firmware would */
        return LDR_ETOOLONG;                             /* be committed (ota.c ota_stage refuses it first) */
    /* 2. the package's app area must decrypt with this chip's key */
    if ((rc = ldr_chip_key(&key)) != 0)
        return -40 + rc;
    if (ota_read(fl_off + LDR_APP_LO, sec, 32))
        return -5;
    ldr_sfc(sec, 32, 0, key);
    if (ota_crc16(sec + 2, 30, 0) != ota_rd16(sec))
        return -6;                                       /* package for another chip key */
    if (!ldr_flash_known()) {                            /* the firmware could not keep its data there, and could */
        ldr_records_drop();                              /* not be updated again: keep the firmware there is */
        return LDR_EFLASH;
    }
    if ((mark = ldr_pkg_marker(fl_off, sec, key)) < 0)
        return mark;
    commit = mark ? LDR_MARK_SEC : LDR_STUB_SEC;         /* the sector written last, after the CRC holds */
    /* the head of flash.bin is never written, but it is part of the package's CRC */
    for (s = 0; s < LDR_APP_LO; s += 512u) {
        if (ota_read(fl_off + s, sec, 512))
            return -7;
        crc = ota_crc16(sec, 512, crc);
    }
    /* 3. app area, sector by sector, in address order but for the commit sector */
    for (s = LDR_APP_LO; s < LDR_APP_HI; s += 0x1000u) {
        uint8_t *dst = s == commit ? held : sec;
        for (k = 0; k < 0x1000u; k += 512u)
            if (ota_read(fl_off + s + k, dst + k, 512))
                return -7;
        crc = ota_crc16(dst, 0x1000u, crc);
        if (s != commit && (rc = ldr_write(s, sec, cur, &open)) != 0)
            return rc;
        done += 0x1000u;
        ldr_progress(done, LDR_APP_HI - LDR_APP_LO);
    }
    /* 4. the whole package arrived as it was built: only now the commit sector */
    if (crc != want)
        return LDR_EBADPKG;
    if ((rc = ldr_write(commit, held, cur, &open)) != 0)
        return rc;
    for (i = 0; i < 4u; i++)
        if (!ota_read(0xF0000000u, sec, 8))
            break;
    if (i == 4u)
        return LDR_ENOFIN;
    ldr_records_drop();
    return 0;
}

/* a pass that does not add up (a byte damaged on the way) is run again: the host answers any read, so it just
 * serves the image once more and the sectors that differ are rewritten; the commit sector is never written by a
 * pass that does not add up. A package that is damaged itself fails every pass and the record stays */
static int ldr_session(void)
{
    int rc = LDR_EBADPKG;
    uint32_t pass;
    for (pass = 0; pass < 3u && rc == LDR_EBADPKG; pass++)
        rc = ldr_pass();
    return rc;
}
