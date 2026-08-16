#ifndef OPENHPSDR_H
#define OPENHPSDR_H

#include <stdint.h>
#include <stdbool.h>
#include "lwip/udp.h"
#include "si5351.h"

#define HPSDR_PORT              1024
#define HPSDR_PACKET_SIZE       1032
#define HPSDR_SYNC_WORD         0xEFFE
#define HPSDR_EP6_IQ_DATA       0x01
#define HPSDR_DISCOVERY_RESP    0x02

// Callback function type when host changes frequency or sample rate
typedef void (*hpsdr_freq_callback_t)(uint32_t freq_hz);
typedef void (*hpsdr_rate_callback_t)(uint32_t rate_hz);

void openhpsdr_init(hpsdr_freq_callback_t on_freq, hpsdr_rate_callback_t on_rate);
void openhpsdr_task(void);

// Push raw 32-bit words (I/Q stereo pairs from PCM1808) into OpenHPSDR Protocol 1 packets
void openhpsdr_push_samples(const uint32_t *samples, uint32_t count);

bool openhpsdr_is_active(void);

#endif // OPENHPSDR_H
