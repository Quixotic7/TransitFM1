/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FM1_FLASH_ID_H
#define FM1_FLASH_ID_H
/* The FM-1's 1 MiB SPI NOR, by its JEDEC id (command 0x9F): Puya's 0x856014, or 0xB36014 (another maker, the same
 * type and capacity bytes 60 14) in the FM-1s with the logo on the front, late 2026 (ChoralRoot 0.29.1, HISTORY
 * 3y222 there; RemoteFM1 0.3.3). The driver (fm1_flash.h) uses generic SPI NOR commands only. On any other id the
 * app, its USB rescue and the update loader write nothing (tests/flash_id_test.c) */
#define FL_JEDEC_OK(id) ((id) == 0x856014u || (id) == 0xB36014u)
#endif
