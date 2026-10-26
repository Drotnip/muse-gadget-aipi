#pragma once

#include <stddef.h>
#include <stdint.h>

/* Rooster crow clip for the alarm clock: 16 kHz mono PCM samples, defined
 * in rooster_crow_data.c. */
extern const int16_t rooster_crow_pcm[];
extern const size_t rooster_crow_pcm_frames;
