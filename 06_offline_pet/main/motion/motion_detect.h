#pragma once

#include "app_event.h"
#include "imu_mpu6050.h"

/* Turns raw motion samples into discrete actions.
 *
 * The accelerometer cannot tell gravity from acceleration, so the detector
 * keeps its own slow estimate of the gravity vector and works with whatever
 * is left over. That residual is what "still" and "moving" are measured
 * against. */

void motion_detect_init(void);

/* Feed one sample and append up to max_events events. Returns how many. */
int motion_detect_update(const imu_sample_t *sample, app_event_t *events, int max_events);

/* Gravity residual from the most recent sample, in g. Exposed for tuning logs. */
float motion_detect_dynamic(void);
