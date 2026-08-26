#ifndef CST816_H
#define CST816_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool pressed;
    int16_t x;
    int16_t y;
} cst816_point_t;

esp_err_t cst816_init(void);

/* Reads the current contact. Returns false only on a bus error, which the
   caller treats as "no touch this frame" rather than as a fault: a bedside
   clock does not stop telling the time because I2C glitched. */
bool cst816_read(cst816_point_t *out);

#endif
