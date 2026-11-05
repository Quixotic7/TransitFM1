/* SPDX-License-Identifier: GPL-3.0-only */
/* FL_JEDEC_OK: both FM-1 flash chips accepted, a near miss refused (firmware/hal/fm1_flash_id.h) */
#include <stdint.h>
#include <stdio.h>
#include "../firmware/hal/fm1_flash_id.h"

int main(void)
{
    static const uint32_t ok[] = {0x856014u, 0xB36014u}, no[] = {0, 0xFFFFFFu, 0x856015u, 0xB36015u, 0x856013u,
                                                                 0xEB6014u, 0xC86014u};
    unsigned i, bad = 0;
    for (i = 0; i < sizeof ok / sizeof ok[0]; i++)
        if (!FL_JEDEC_OK(ok[i])) { printf("flash id %06X refused\n", (unsigned)ok[i]); bad++; }
    for (i = 0; i < sizeof no / sizeof no[0]; i++)
        if (FL_JEDEC_OK(no[i])) { printf("flash id %06X accepted\n", (unsigned)no[i]); bad++; }
    printf(bad ? "FLASH ID TEST FAILED\n" : "flash id test passed (0x856014, 0xB36014)\n");
    return bad != 0;
}
