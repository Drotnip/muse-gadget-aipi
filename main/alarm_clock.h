#pragma once

/* Starts the rooster crow alarm task. Safe to call once; later calls are
 * no-ops. The task guards itself: it only crows on boards with audio
 * hardware, only when the alarm is on, and only once the clock is synced. */
void alarm_clock_start(void);
