/*
 * Airspy R2 / Mini SDR backend for iridium-sniffer
 */

#ifndef __OUR_AIRSPY_H__
#define __OUR_AIRSPY_H__

#include <libairspy/airspy.h>

void airspy_list(void);
struct airspy_device *airspy_setup(void);
int airspy_rx_cb(airspy_transfer *t);

#endif
