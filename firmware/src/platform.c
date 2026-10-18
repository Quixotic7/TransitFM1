/* SPDX-License-Identifier: GPL-3.0-only */
/* TransitFM1's platform: Dinghy's (Felucca's, as SLOOP carries it), with the sound taken out and ChoralRoot's
 * boot stub in front of it (docs/SAFETY.md).
 *
 *   boot stub           firmware/stub, before any of this runs: the commit marker and the body's CRC, OCT- at
 *                       power-on, the hand-over to the staged update loader, the ROM's USB boot
 *   boot + boot guard   a firmware that crashes at boot falls back to the USB rescue, then to the ROM
 *   USB rescue          only the updater (the boot guard's second crash)
 *   updater             the M-UPGRADE SysEx path: the installers and the hub reach the FM-1 as the official V15
 *   our own loader      staged at 0xE0000 at every boot if it is not there (ota_own_staged, ota_restage): the one
 *                       the stub hands over to, and the one that writes every package (firmware/loader)
 *   keys                OCT-, OCT+ (hal/fm1_input.h)
 *   screen              240 x 240, lcd_fill, draw_text_box with FONT_S (16 px) / FONT_L (32 px) (gfx.c)
 *   crash screen        the PC of a fault, then a reboot
 *
 * The app (transit.c) includes this file first and supplies:
 *   static void app_main(void)                        never returns; calls platform_poll() often (every few ms)
 */
enum { BTN_OCT_DOWN = 0, BTN_OCT_UP = 1, BTN_FX = 2, BTN_SEL = 3, BTN_ENV = 4, BTN_LFO = 5, BTN_EDIT = 6,
       BTN_GLO = 7, BTN_HOME = 8, BTN_SAVE = 9, BTN_ARP = 10, BTN_SEQ = 11, BTN_PLAY = 12, BTN_REC = 13 };
#define KEY_LED(k) (14u + (k))

#include <stdint.h>
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_input.h"
#include "fm1_timer.h"
#include "fm1_lcd_hw.h"
#include "fm1_xip.h"
#include "bootguard.h"

#ifndef APP_NAME
#define APP_NAME "TRANSIT"
#endif
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_015"                   /* the official V15's identity (tools/build.py): see ota.c */
#endif
#ifndef APP_VER
#define APP_VER "0.0"
#endif

bootguard_t bootguard __attribute__((section(".noinit")));
static volatile uint32_t fm1_ms;                /* milliseconds since power-on (TIMER5) */

#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#include "libc.c"
#include "lcd.c"
#define CV_MAX (240u * 64u)                     /* the biggest text box drawn at once */
#include "gfx.c"

#define FELUCCA_OTA 1
#define FELUCCA_CDC 0
#define FELUCCA_UAC 0                           /* no sound, so no USB audio: the smallest USB device that updates */
#define FELUCCA_UART 0
#include "usb.c"

/* ---- flash: read anywhere; erase / program only the update's staging area */
#define FL_RANGE_OK(off, n) FL_IN(off, n, FL_OTA_LO, FL_OTA_HI)
#include "fm1_flash.h"
static uint8_t flash_ok;
static int st_read(uint32_t off, void *dst, uint32_t n)
{
    uint8_t *d = dst;
    while (n) {
        uint32_t k = n > 256u ? 256u : n, f = irq_save();
        int rc = FL_FAR(fl_read_ram)(off, d, k);
        irq_restore(f);
        if (rc)
            return rc;
        off += k;
        d += k;
        n -= k;
    }
    return 0;
}

static uint8_t recovery_active;
static uint8_t lcd_ready;                       /* lcd_init done (fm1_fault: a fault before it brings the panel up) */
#include "transit_loader.h"                     /* build/gen: our ota.bin (tools/build.py) */
#define OTA_OWN_LDR TRANSIT_LDR
#define OTA_OWN_LEN ((uint32_t)sizeof TRANSIT_LDR)
#include "ota.c"
static void recovery_poll(void);
static uint32_t ota_now_ms(void) { return fm1_ms; }
static void ota_idle(void)
{
    fm1_wdt_feed();
    if (recovery_active) recovery_poll();
}
static int ota_in_area(uint32_t off, uint32_t n) { return FL_IN(off, n, OTA_AREA, OTA_AREA + OTA_AREA_LEN); }
static int ota_erase(uint32_t off)
{
    uint32_t took;
    if (!ota_in_area(off, 0x1000u) || (off & 0xFFFu))
        return -8;
    return fl_erase4k(off, &took);
}
/* the flash driver reads the source while the flash is out of XIP (hal/fm1_flash.h fl_prog_ram): a source in XIP
 * (code, a const array) would fault, so it is refused here, whoever calls (ota.c copies to RAM first) */
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!ota_in_area(off, n))
        return -8;
    if (fm1_in_xip(p, n))
        return -9;
    return fl_write(off, p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n) { return st_read(off, p, n); }
static void ota_show(uint32_t step, int32_t code)
{
    static const char *const STEP[] = {"", "PACKAGE", "", "LOADER", "CONFIRM", "RESTART"};
    if (recovery_active) return;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 92, 240, &FONT_S, "UPDATE", C_WHITE, 1);
    if (step < 9u)
        draw_text_box(0, 124, 240, &FONT_S, STEP[step < 6u ? step : 0], C_WHITE, 1);
    else
        draw_text_box(0, 124, 240, &FONT_S, code == 1 ? "DRY RUN OK" : "FAILED", C_WHITE, 1);
}
static void ota_commit(const uint8_t *parm)
{
    bootguard_clear(&bootguard);
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);
}
#include "recovery.c"

/* ---- the timer: 10 kHz key scan, milliseconds, USB at 2 kHz */
void fm1_timer5_irq(void)
{
    static uint32_t sub, last, acc;
    uint32_t t0 = fm1_ticks();
    fm1_timer5_ack();
    fm1_input_tick();
    acc += t0 - last;
    last = t0;
    while (acc >= 1000u * FM1_TICKS_PER_US) {
        acc -= 1000u * FM1_TICKS_PER_US;
        fm1_ms++;
    }
    if (sub % 5u == 0u)
        usb_poll();
    if (++sub == 10u)
        sub = 0;
}
extern void isr_timer5(void);

static void app_main(void);

/* MIDI in: nothing here plays it, so it is dropped. The USB stack queues every channel and real-time message in
 * midi_in_q and stops taking packets (EP1 OUT NAKed, usb.c ep1_take) while the ring has less than EP1_ROOM free:
 * undrained, a DAW's clock or notes fill it in about 40 events and the installer's SysEx can no longer arrive (0.1:
 * Dinghy's audio ISR had drained it, and went with the sound). SysEx takes another path (sysex_byte, the ota frame) */
static void midi_drain(void) { mi_r = mi_w; }

/* ---- the main loop's duties: watchdog, MIDI in, USB, the installer's update request */
static void platform_poll(void)
{
    fm1_wdt_feed();
    midi_drain();
    usb_retry(fm1_ms);
    ota_service();
    if (usb.ota_req) {
        usb.ota_req = 0;
        if (flash_ok)
            ota_session();                      /* returns only if nothing was committed */
    }
    if (usb.uboot_req) {
        lcd_fill(0, 0, 240, 240, C_BLACK);
        draw_text_box(0, 110, 240, &FONT_S, "UBOOT (USB)", C_WHITE, 1);
        usb_detach();
        fm1_delay_ms(30);
        bootguard.pending = 0;
        fm1_enter_uboot();
    }
    if (fm1_ms >= 30000u && bootguard.pending)
        bootguard_clear(&bootguard);            /* 30 s up: this firmware boots */
}

static void fm1_fault(const fm1_crash_t *c)
{
    char b[12];
    uint32_t i, t0, v[3] = {c->vec, c->pc, c->rets};
    if (!lcd_ready) {
        lcd_ready = 1;                          /* (before: a fault inside lcd_init does not come back here) */
        lcd_init();
    }
    lcd_fill(0, 0, 240, 240, RGB(160, 0, 0));
    draw_text_box(0, 20, 240, &FONT_S, APP_NAME " CRASH", C_WHITE, 1);
    for (i = 0; i < 3u; i++) {
        uint32_t k;
        for (k = 0; k < 8u; k++) b[k] = "0123456789ABCDEF"[(v[i] >> (28u - 4u * k)) & 15u];
        b[8] = 0;
        draw_text_box(10, 60 + i * 20, 220, &FONT_S, b, C_WHITE, 0);
    }
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 8000u * 1000u * FM1_TICKS_PER_US)
        fm1_wdt_feed();
    fm1_reboot();
}

/* what the boot found in the staging area (the screen shows it): 0 our loader was there, 1 staged now, < 0 it could
 * not be staged (the boot stub's rescue then ends in the ROM's USB boot) */
static int loader_state;

void fm1_cstart(void)
{
    extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
    extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];
    uint32_t *s, *d, p3, boot_mode;
    fm1_time_init();
    fm1_reset_reason();
    p3 = fm1_boot.p3_rst;
    fm1_wdt_arm(0x0D);
    if ((p3 & 1u) && !(p3 & (4u | 0x40u)))
        bootguard_clear(&bootguard);
    boot_mode = bootguard_begin(&bootguard);
    if (boot_mode == BOOT_ROM)
        fm1_enter_uboot();
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++) *d = 0;
    for (d = _pool_start; d < _pool_end; d++) *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++) *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++) *d = *s;
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    fm1_boot.p3_rst = (uint8_t)p3;
    if (boot_mode == BOOT_RECOVERY)
        recovery_main();                        /* the boot guard's second crash: the USB rescue */
    flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;
    lcd_init();                                 /* first: anything that faults from here on shows its crash screen */
    lcd_ready = 1;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    if (flash_ok) {
        draw_text_box(0, 110, 240, &FONT_S, "STAGING THE LOADER", C_WHITE, 1);
        lcd_sync();
        loader_state = ota_boot_stage();        /* what an update left goes; our whole loader stays, or is staged */
        lcd_fill(0, 0, 240, 240, C_BLACK);
    } else {
        loader_state = -2;
    }
    fm1_input_init();
    usb_start();
    fm1_timer5_start(isr_timer5, 4);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    app_main();
    for (;;)
        platform_poll();
}
