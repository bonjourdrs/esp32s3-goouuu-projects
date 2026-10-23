#pragma once

#include "driver/gpio.h"

/* ML307R Cat.1 module on the GOOUUU baseboard.
 *
 * Measured 2026-10-21: the schematic nets are named from the module side, so
 * the module's RXD sits on G12 and its TXD on G11. The ESP therefore has to
 * transmit on GPIO12 and receive on GPIO11. The opposite order gets no reply. */

#define PIN_ML307_TX GPIO_NUM_12
#define PIN_ML307_RX GPIO_NUM_11

/* The component probes 115200 / 921600 / 460800 / ... on its own; this is the
 * rate it settles on afterwards. */
#define ML307_BAUD 115200

/* Long enough for ~5 baud-rate rounds per pin order. Without a response the
 * component would otherwise retry forever (timeout -1). */
#define ML307_DETECT_TIMEOUT_MS 6000

/* First attach after boot can take 15-30 s on a live network, so give it 60 s.
 * Without a SIM it still returns ErrorInsertPin almost immediately. */
#define ML307_ATTACH_TIMEOUT_MS 60000
