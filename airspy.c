/*
 * Airspy R2 / Mini SDR backend for iridium-sniffer
 *
 * The R2's native 10 MSPS is exactly the rate this project recommends for
 * the full authorized Iridium band (1618.725-1626.5 MHz at 1622 centre), so
 * nothing resamples anywhere: libairspy is asked for FLOAT32_IQ and
 * burst_detect feeds it straight to burst_detector_feed_cf32(). The Mini
 * offers 6 and 3 MSPS for the same job at lower CPU.
 *
 * Not to be confused with libairspyhf, which drives the Airspy HF+ and is a
 * different library with a different API. The R2 and Mini are libairspy.
 */

#include <err.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libairspy/airspy.h>

#include "sdr.h"

extern double samp_rate;
extern double center_freq;
extern char *airspy_serial;
extern sig_atomic_t running;

extern int airspy_lna_gain;
extern int airspy_mixer_gain;
extern int airspy_vga_gain;
extern int airspy_linearity_gain;
extern int airspy_sensitivity_gain;
extern int airspy_packing;
extern int bias_tee;

/* Reported once rather than per callback: at 10 MSPS a stuck consumer would
 * otherwise produce a line every few milliseconds and bury everything else.
 * The distinction this preserves is worth the trouble -- a drop counted here
 * is the USB layer giving up, which is a different problem from the sample
 * queue overflowing downstream, and the two want different fixes. */
static uint64_t airspy_dropped_total = 0;
static int airspy_drop_reported = 0;

void airspy_list(void) {
    uint64_t serials[16];
    int i, n;

    n = airspy_list_devices(serials, 16);
    for (i = 0; i < n; ++i) {
        struct airspy_device *d = NULL;
        char rates[128] = "";

        printf("  airspy-%016" PRIx64 "   ", serials[i]);

        /* The rate list is a property of the board, not of libairspy: R2 is
         * 10/2.5 MSPS and Mini is 6/3, and picking one the board does not
         * have fails at set_samplerate() with nothing to suggest what would
         * have worked. Cheap to open the device and just say. */
        if (airspy_open_sn(&d, serials[i]) == AIRSPY_SUCCESS) {
            uint32_t count = 0, buf[16], j;
            char part[16];
            airspy_get_samplerates(d, &count, 0);
            if (count > 0 && count <= 16 &&
                airspy_get_samplerates(d, buf, count) == AIRSPY_SUCCESS) {
                for (j = 0; j < count; ++j) {
                    snprintf(part, sizeof(part), "%s%.4g",
                             j ? "/" : "", buf[j] / 1e6);
                    strncat(rates, part, sizeof(rates) - strlen(rates) - 1);
                }
            }
            airspy_close(d);
        }
        printf("Airspy%s%s\n", rates[0] ? " " : "", rates);
    }
}

struct airspy_device *airspy_setup(void) {
    int r;
    struct airspy_device *dev = NULL;

    if (airspy_serial != NULL) {
        uint64_t sn = strtoull(airspy_serial, NULL, 16);
        if ((r = airspy_open_sn(&dev, sn)) != AIRSPY_SUCCESS)
            errx(1, "Unable to open Airspy %s: %s", airspy_serial,
                 airspy_error_name(r));
    } else {
        if ((r = airspy_open(&dev)) != AIRSPY_SUCCESS)
            errx(1, "Unable to open Airspy: %s", airspy_error_name(r));
    }

    /* Before set_samplerate, which is the order airspy_rx.c uses and not
     * merely a convention: the sample type selects the conversion path the
     * rate is then applied to. */
    if ((r = airspy_set_sample_type(dev, AIRSPY_SAMPLE_FLOAT32_IQ)) != AIRSPY_SUCCESS)
        errx(1, "Unable to set Airspy sample type: %s", airspy_error_name(r));

    /* set_samplerate takes either an index into the board's list or a value
     * in Hz; a value is passed here so the failure names the rate the user
     * asked for. */
    if ((r = airspy_set_samplerate(dev, (uint32_t)samp_rate)) != AIRSPY_SUCCESS) {
        uint32_t count = 0, buf[16], j;
        fprintf(stderr, "Unable to set Airspy sample rate %.6g MHz: %s\n",
                samp_rate / 1e6, airspy_error_name(r));
        airspy_get_samplerates(dev, &count, 0);
        if (count > 0 && count <= 16 &&
            airspy_get_samplerates(dev, buf, count) == AIRSPY_SUCCESS) {
            fprintf(stderr, "This board supports:");
            for (j = 0; j < count; ++j)
                fprintf(stderr, " %.4g", buf[j] / 1e6);
            fprintf(stderr, " MHz\n");
        }
        exit(1);
    }

    /* 12-bit packed USB transfers. At 10 MSPS this is 30 MB/s instead of
     * 40, which is the difference between comfortable and marginal on a
     * shared controller. The unpacking is libairspy's problem and the
     * samples are identical either way. */
    if (airspy_packing) {
        if ((r = airspy_set_packing(dev, 1)) != AIRSPY_SUCCESS)
            errx(1, "Unable to enable Airspy packing: %s", airspy_error_name(r));
    }

    /* Not optional with an active antenna: an L-band patch with an
     * integrated LNA is powered down this same coax, and without it the
     * receiver is simply deaf rather than obviously broken. */
    if (bias_tee) {
        if ((r = airspy_set_rf_bias(dev, 1)) != AIRSPY_SUCCESS)
            errx(1, "Unable to enable Airspy bias tee: %s", airspy_error_name(r));
    }

    /*
     * Three ways to set gain, and they are mutually exclusive.
     *
     * linearity and sensitivity are presets (0-21) that drive lna, mixer
     * and vga together along two different curves; the individual gains
     * (0-15 each) are the manual alternative. Iridium bursts are ~8 ms and
     * weak, so sensitivity is the sane default -- it favours noise figure,
     * which is what decides whether a burst clears the detector at all.
     *
     * Preferring linearity is a real option at L-band, where a strong
     * nearby emitter can drive the front end into compression and cost more
     * than the noise figure gains. Which one wins here is a measurement,
     * not a default, so all three routes are exposed.
     */
    if (airspy_lna_gain >= 0 || airspy_mixer_gain >= 0 || airspy_vga_gain >= 0) {
        if (airspy_lna_gain >= 0 &&
            (r = airspy_set_lna_gain(dev, airspy_lna_gain)) != AIRSPY_SUCCESS)
            errx(1, "Unable to set Airspy LNA gain: %s", airspy_error_name(r));
        if (airspy_mixer_gain >= 0 &&
            (r = airspy_set_mixer_gain(dev, airspy_mixer_gain)) != AIRSPY_SUCCESS)
            errx(1, "Unable to set Airspy mixer gain: %s", airspy_error_name(r));
        if (airspy_vga_gain >= 0 &&
            (r = airspy_set_vga_gain(dev, airspy_vga_gain)) != AIRSPY_SUCCESS)
            errx(1, "Unable to set Airspy VGA gain: %s", airspy_error_name(r));
    } else if (airspy_linearity_gain >= 0) {
        if ((r = airspy_set_linearity_gain(dev, airspy_linearity_gain)) != AIRSPY_SUCCESS)
            errx(1, "Unable to set Airspy linearity gain: %s", airspy_error_name(r));
    } else {
        if ((r = airspy_set_sensitivity_gain(dev, airspy_sensitivity_gain)) != AIRSPY_SUCCESS)
            errx(1, "Unable to set Airspy sensitivity gain: %s", airspy_error_name(r));
    }

    /* The R820T2 is specified to 1750 MHz and Iridium sits at 1626, inside
     * the range but near the top of it where noise figure is rising. Worth
     * knowing when the front end underperforms: it is the tuner, not a
     * misconfiguration. */
    if ((r = airspy_set_freq(dev, (uint32_t)center_freq)) != AIRSPY_SUCCESS)
        errx(1, "Unable to set Airspy centre frequency: %s", airspy_error_name(r));

    return dev;
}

int airspy_rx_cb(airspy_transfer *t) {
    sample_buf_t *s;
    size_t bytes;

    if (t->dropped_samples > 0) {
        airspy_dropped_total += t->dropped_samples;
        if (!airspy_drop_reported) {
            airspy_drop_reported = 1;
            fprintf(stderr, "airspy: dropping samples in the USB layer "
                    "(%" PRIu64 " so far) -- try --airspy-packing or a lower "
                    "rate; further drops counted silently\n",
                    airspy_dropped_total);
        }
    }

    /* FLOAT32_IQ: sample_count counts IQ pairs, and samples is a float*
     * holding 2 per pair. sample_buf_t::num is likewise pairs, matching
     * hackrf_rx_cb. */
    bytes = (size_t)t->sample_count * 2 * sizeof(float);
    if ((s = malloc(sizeof(*s) + bytes)) == NULL)
        return 0;                       /* drop this block, keep streaming */
    s->format = SAMPLE_FMT_FLOAT;
    s->hw_timestamp_ns = 0;
    s->num = t->sample_count;
    memcpy(s->samples, t->samples, bytes);

    if (running)
        push_samples(s);
    else
        free(s);
    return 0;
}
