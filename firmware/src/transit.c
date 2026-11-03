/* SPDX-License-Identifier: GPL-3.0-only */
/* TransitFM1: the firmware an FM-1 stops at between two others. It makes no sound and keeps nothing; it says what
 * it is and waits for the next install. What makes it worth the stop is underneath (docs/SAFETY.md):
 *
 *   - it answers every installer as the official V15 (FM-1_015, the port "FM-1"), so the next firmware's own
 *     installer writes over it as over the stock firmware, whatever it guards against;
 *   - ChoralRoot's boot stub sits in front of it, and its own update loader is always staged: the install of the
 *     NEXT firmware is written by that loader, with the new firmware's first sector last. A cable pulled or a
 *     battery that dies at any point before that last sector leaves an FM-1 that comes back in update mode (three
 *     short flashes, "ota-FM-1"): run the installer again. OCT- held at power-on gives the same update mode.
 *   - getting here is as safe as the firmware you come from allows, and from ChoralRoot (or from TransitFM1 itself)
 *     it is safe all the way: the same stub, so the write never touches sector 0x4000.
 *
 * platform.c does all of it; this file only draws the screen. */
#include "platform.c"

#define C_ACC RGB(120, 200, 255)                /* the transit sign's blue */

static void app_main(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 18, 240, &FONT_L, APP_NAME, C_ACC, 1);
    draw_text_box(0, 52, 240, &FONT_S, "FM-1 in between  v" APP_VER, C_OFF, 1);
    draw_text_box(0, 92, 240, &FONT_S, "READY FOR THE NEXT", C_WHITE, 1);
    draw_text_box(0, 112, 240, &FONT_S, "FIRMWARE: INSTALL IT", C_WHITE, 1);
    draw_text_box(0, 132, 240, &FONT_S, "WITH ITS OWN INSTALLER", C_WHITE, 1);
    if (loader_state >= 0) {
        draw_text_box(0, 172, 240, &FONT_S, "CUT OFF? SWITCH ON AGAIN", C_OFF, 1);
        draw_text_box(0, 192, 240, &FONT_S, "AND INSTALL AGAIN", C_OFF, 1);
    } else {                                    /* nothing to hand over to: the stub's rescue would end in UBOOT */
        draw_text_box(0, 172, 240, &FONT_S, loader_state == -2 ? "UNKNOWN FLASH CHIP" : "LOADER NOT STAGED", C_BAD, 1);
        draw_text_box(0, 192, 240, &FONT_S, "UPDATES ARE REFUSED", C_BAD, 1);
    }
    draw_text_box(0, 216, 240, &FONT_S, "AS V15 (FM-1_015)", C_OFF, 1);
    lcd_sync();
    for (;;)
        platform_poll();
}
