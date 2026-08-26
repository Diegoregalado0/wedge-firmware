#ifndef WEDGE_RM67162_DRIVER_H
#define WEDGE_RM67162_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define RM67162_W 536
#define RM67162_H 240

/* Rows per transfer. esp_lcd's SPI backend never sets SPI_TRANS_DMA_USE_PSRAM,
   so a frame buffer in PSRAM makes the driver try to bounce the whole 257 kB
   through internal RAM, which cannot be allocated on a part with 185 kB of it.
   The frame goes across in bands out of a buffer that is internal to begin
   with. 24 rows is 25 kB, small enough to sit in DRAM twice over. */
#define RM67162_BAND 24

esp_err_t rm67162_init(void);

/* Panel emission level, 0 to 255. */
void rm67162_set_brightness(uint8_t level);

void rm67162_sleep(bool on);

/* Push rows [y0, y0 + rows) from an internal, DMA-capable buffer holding just
   that band. Blocks until the transfer completes. Byte-swaps the buffer in
   place to the wire order the panel wants, so the caller's copy is left with
   its bytes transposed afterward; harmless here since it is about to be
   overwritten by the next band's conversion anyway. */
void rm67162_blit_rows(uint16_t *pixels, int y0, int rows);

#endif
