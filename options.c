/*
 * Command-line option parsing
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * Modifications Copyright (c) 2026 Mike Brown
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Command-line option parsing for iridium-sniffer
 */

#include <err.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tk_reassembler.h"
#include <unistd.h>

#include "aircraft_db.h"
#include "sdr.h"
#include "sigmf.h"
#include "simd_kernels.h"

#ifdef HAVE_HACKRF
#include "hackrf.h"
#endif
#ifdef HAVE_BLADERF
#include "bladerf.h"
#endif
#ifdef HAVE_UHD
#include "usrp.h"
#endif
#ifdef HAVE_SOAPYSDR
#include "soapysdr.h"
#endif
#ifdef HAVE_SDRPLAY
#include "sdrplay.h"
#endif

typedef enum {
    FMT_CI8 = 0,
    FMT_CI16,
    FMT_CF32,
} iq_format_t;

extern double samp_rate;
extern double center_freq;
extern int verbose;
extern int live;
extern char *file_info;
extern uint64_t file_start_ns;
extern double threshold_db;
extern iq_format_t iq_format;
extern FILE *in_file;

extern char *serial;
#ifdef HAVE_BLADERF
extern int bladerf_num;
#endif
#ifdef HAVE_UHD
extern char *usrp_serial;
#endif
#ifdef HAVE_SOAPYSDR
extern int soapy_num;
extern char *soapy_args;
#define SOAPY_SETTINGS_MAX 8
extern char *soapy_setting_keys[SOAPY_SETTINGS_MAX];
extern char *soapy_setting_vals[SOAPY_SETTINGS_MAX];
extern int soapy_setting_count;
#define SOAPY_GAINS_MAX 8
extern char *soapy_gain_elem_names[SOAPY_GAINS_MAX];
extern double soapy_gain_elem_vals[SOAPY_GAINS_MAX];
extern int soapy_gain_elem_count;
#endif
#ifdef HAVE_SDRPLAY
extern char *sdrplay_serial;
#endif

extern int hackrf_lna_gain;
extern int hackrf_vga_gain;
extern int hackrf_amp_enable;
extern int bladerf_gain_val;
extern int usrp_gain_val;
extern double soapy_gain_val;
extern int sdrplay_gain_val;
extern int bias_tee;
extern int use_gpu;
extern int simd_mode;
extern int use_chase;
extern char *save_bursts_dir;
extern int web_enabled;
extern int web_port;
extern int archive_enabled;
extern char *archive_dir;
extern int gsmtap_enabled;
extern char *gsmtap_host;
extern int gsmtap_port;
extern int diagnostic_mode;
extern int use_gardner;
extern float uw_reject_threshold;
extern int parsed_mode;
extern char *replay_raw_path;
extern int reassemble_mode;
extern int parse_harder;
extern int parse_uwec;
extern char *messages_spec[4];
extern int n_messages_spec;
extern int position_enabled;
extern double position_height;
extern int acars_enabled;
extern int acars_json;
extern char *station_id;
#define ACARS_UDP_MAX 4
extern char *acars_udp_hosts[ACARS_UDP_MAX];
extern int acars_udp_ports[ACARS_UDP_MAX];
extern int acars_udp_count;
#define FEED_MAX 4
extern char *feed_hosts[FEED_MAX];
extern int feed_ports[FEED_MAX];
extern int feed_is_tcp[FEED_MAX];
extern int feed_count;
extern int zmq_enabled;
extern char *zmq_endpoint;
extern int zmq_sub_enabled;
extern char *zmq_sub_endpoint;
extern int vita49_enabled;
extern char *vita49_endpoint;
extern int basestation_enabled;
extern int basestation_beam;
extern char *basestation_endpoint;
extern int num_downmix_workers;
extern char *aircraft_db_path;
extern int samp_rate_explicit;
extern int center_freq_explicit;
extern int iq_format_explicit;
extern int clock_source;
extern int time_source;

static void usage(int exitcode) {
    fprintf(stderr,
"Usage: iridium-sniffer <-f FILE | -i IFACE> [options]\n"
"Standalone Iridium satellite burst detector and demodulator.\n"
"Outputs iridium-toolkit compatible RAW format to stdout.\n"
"\n"
"Input (one required):\n"
"    -f, --file=FILE         read IQ samples from file\n"
"    -l, --live              capture live from SDR (implied by -i)\n"
"    --format=FMT            IQ file format: ci8 (default), ci16, cf32\n"
"\n"
"SDR options:\n"
"    -i, --interface=IFACE   SDR to use (see --list for available devices):\n"
"                             soapy-N (by index) or soapy:driver=X,serial=Y (by args)\n"
"                             hackrf-SERIAL, bladerfN, usrp-PRODUCT-SERIAL\n"
"                             sdrplay-SERIAL (native SDRplay API)\n"
"    -c, --center-freq=HZ    center frequency in Hz (default: 1622000000)\n"
"    -r, --sample-rate=HZ    sample rate in Hz (default: 10000000)\n"
"    -B, --bias-tee           enable bias tee power\n"
"    --clock-source=SRC       clock reference: internal (default), external, gpsdo\n"
"    --time-source=SRC        time/PPS reference: internal (default), external, gpsdo\n"
"\n"
"Gain options:\n"
"    --hackrf-lna=GAIN       HackRF LNA gain in dB (default: 40)\n"
"    --hackrf-vga=GAIN       HackRF VGA gain in dB (default: 20)\n"
"    --hackrf-amp            enable HackRF RF amplifier\n"
"    --bladerf-gain=GAIN     BladeRF gain in dB (default: 40)\n"
"    --usrp-gain=GAIN        USRP gain in dB (default: 40)\n"
"    --soapy-gain=GAIN       SoapySDR gain in dB (default: 30)\n"
"    --sdrplay-gain=GAIN    SDRplay IF gain reduction 20-59, disables AGC (default: AGC on)\n"
"    --soapy-gain-element=NAME:VAL  set SoapySDR per-element gain (repeatable)\n"
"                             e.g. LNA:10, MIX:9, VGA:10 (Airspy R2)\n"
"                             skips aggregate --soapy-gain when any element is set\n"
"                             use -v to list available gain elements for your device\n"
"    --soapy-setting=K:V    SoapySDR device setting (repeatable)\n"
"                             e.g. bitpack:true (Airspy), biastee_rx:true (bladeRF)\n"
"\n"
"Detection options:\n"
"    -d, --threshold=DB      burst detection threshold in dB (default: 16.0)\n"
"    --no-gpu                disable GPU acceleration (use CPU FFTW)\n"
"    --simd=MODE             SIMD kernel selection: auto (default), avx2, sse42, neon, scalar\n"
"    --no-simd               alias for --simd=scalar\n"
"    --workers=N             downmix worker threads (1-32, default: auto from CPU cores)\n"
"    --chase[=N]             enable Chase soft-decision BCH decoder (experimental)\n"
"                             N = flip-bits count, 0-7 (default 5 = 31 combos).\n"
"                             0 or omitting --chase disables (default).\n"
"                             Generates per-bit LLR scores; tries 2^N-1 flip\n"
"                             combinations on BCH-rejected blocks.  IDA/ACARS\n"
"                             path gated by CRC-16; IRA/IBC have no payload CRC.\n"
"                             Off by default.\n"
"\n"
"Web map:\n"
"    --web[=PORT]            enable live web map (default port: 8888)\n"
"    --archive[=DIR]         append JSONL archive of frame-type rollups and\n"
"                             decoded messages (default dir: ./archive)\n"
"    --position[=HEIGHT_M]   estimate receiver position from Doppler shift\n"
"                             optional height aiding in meters (implies --web)\n"
"\n"
"GSMTAP:\n"
"    --gsmtap[=HOST:PORT]    send IDA frames as GSMTAP/LAPDm via UDP\n"
"                             (default: 127.0.0.1:4729, for Wireshark)\n"
"\n"
"Output options:\n"
"    --file-info=STR         file info string for output (default: auto)\n"
"    --file-start=TIME       time of the file's first sample: Unix seconds, ISO 8601\n"
"                             UTC, or 'sigmf' (captures[0].core:datetime of the\n"
"                             .sigmf-meta, also <name>.sigmf-meta beside any file).\n"
"                             Frame times then follow the recording's clock instead\n"
"                             of the wall clock at start (default). With --zmq-sub:\n"
"                             the time of the first sample received.\n"
"    --save-bursts=DIR       save IQ samples of decoded bursts to directory\n"
"    --diagnostic            setup verification mode (suppresses RAW output)\n"
"    --no-gardner           disable Gardner timing recovery (enabled by default)\n"
"    --uw-reject=T          drop UW-fail bursts with sync score < T as false\n"
"                           positives (default 0.70; 0 disables). Does not\n"
"                           affect decoding, only the UW-fail count.\n"
"    --parsed               output parsed IDA lines (pipe to reassembler.py)\n"
"    --parsed=full          output every frame as iridium-parser.py prints it\n"
"                           (native port; see docs/NATIVE_PARSER_PLAN.md)\n"
"    --reassemble=MODE[,ARG,...]  reassemble frames as iridium-toolkit's\n"
"                           reassembler.py -m MODE -a ARG,... does (MODE: ida,\n"
"                           sbd, acars; ARG: json, showerrs, nopings, perfect)\n"
"                           and print that instead of frames (native port)\n"
"    --messages=MODE[,ARG,...]  alongside the frames (repeatable, up to 4):\n"
"                           reassemble as --reassemble does and print each\n"
"                           message as \"RSM: MODE N T1,...,TN | <line>\", with\n"
"                           the times of its N IDA frames (libacars: json only)\n"
"    --parse-harder         for --parsed=full and --reassemble: parse as\n"
"                           iridium-parser.py --harder does (recovers frames\n"
"                           with bit errors in their headers)\n"
"    --parse-uw-ec          likewise iridium-parser.py --uw-ec (accepts an\n"
"                           access code with up to 3 symbol errors)\n"
"    --replay-raw=FILE      read RAW: lines (this program's output) instead of\n"
"                           samples and run only the output stage on them\n"
"                           (tests; hard bits only, as iridium-toolkit gets)\n"
"    --acars               decode and display ACARS messages from IDA\n"
"    --acars-json          output ACARS as JSON (compatible with acars.py)\n"
"    --acars-udp=HOST:PORT stream ACARS JSON via UDP (repeatable, max 4)\n"
"    --feed[=PROTO://HOST:PORT] feed aggregator (iridium-toolkit JSON format)\n"
"                             udp://HOST:PORT for acarshub (e.g. udp://127.0.0.1:5558)\n"
"                             tcp://HOST:PORT for airframes.io direct\n"
"                             bare --feed defaults to tcp://feed.airframes.io:5590\n"
"                             repeatable (max 4, mix udp:// and tcp://)\n"
"    --station=ID          station identifier for ACARS JSON output\n"
#ifdef HAVE_ZMQ
"    --zmq[=ENDPOINT]     publish output via ZMQ PUB socket for multi-consumer\n"
"                             (default: tcp://*:7006, compatible with iridium-toolkit)\n"
"    --zmq-sub[=ENDPOINT]  receive IQ samples via ZMQ SUB socket\n"
"                             (default: tcp://127.0.0.1:5555, use with -f and -r)\n"
#endif
"    --vita49[=IP:PORT]    receive IQ via VITA 49 (VRT) UDP packets\n"
"                             (default: 0.0.0.0:4991, auto-detects -r/-c/format\n"
"                             from VRT context packets if not specified)\n"
"\n"
"  BaseStation output:\n"
"    --basestation[=PORT]     SBS server on PORT (default 30003, tools connect in)\n"
"    --basestation=HOST:PORT  SBS push to remote host (auto-reconnect)\n"
"    --basestation-beam       include beam-estimated positions in SBS feed\n"
"                             (default: only GPS-quality positions are sent)\n"
"    --aircraft-db=PATH       aircraft database CSV (default: ~/.iridium-sniffer/aircraft.csv)\n"
"    --update-db              download/update aircraft database and exit\n"
"\n"
"    -v, --verbose           verbose output to stderr\n"
"    -h, --help              show this help\n"
"    --list                  list available SDR interfaces\n"
"\n"
"The output format is compatible with iridium-toolkit. Pipe to iridium-parser.py:\n"
"    iridium-sniffer -l | python3 iridium-toolkit/iridium-parser.py\n"
    );
    exit(exitcode);
}

static void list_interfaces(void) {
    printf("Available SDR interfaces (-i VALUE):\n");
#ifdef HAVE_HACKRF
    hackrf_list();
#endif
#ifdef HAVE_BLADERF
    bladerf_list();
#endif
#ifdef HAVE_UHD
    usrp_list();
#endif
#ifdef HAVE_SDRPLAY
    sdrplay_list();
#endif
#ifdef HAVE_SOAPYSDR
    soapy_list();
#endif
    fflush(stdout);
    /* Use _exit to skip atexit/destructor handlers -- SoapySDR's SDRplay
     * module conflicts with the native API during library teardown. */
    _exit(0);
}

void parse_options(int argc, char **argv) {
    int ch;
    int format_explicit = 0;
    int file_start_from_sigmf = 0;
    const char *in_filename = NULL;

    enum {
        OPT_HACKRF_LNA = 0x100,
        OPT_HACKRF_VGA,
        OPT_HACKRF_AMP,
        OPT_BLADERF_GAIN,
        OPT_USRP_GAIN,
        OPT_SOAPY_GAIN,
        OPT_FILE_INFO,
        OPT_FILE_START,
        OPT_FORMAT,
        OPT_LIST,
        OPT_NO_GPU,
        OPT_NO_SIMD,
        OPT_SIMD,
        OPT_CHASE,
        OPT_WEB,
        OPT_ARCHIVE,
        OPT_GSMTAP,
        OPT_SAVE_BURSTS,
        OPT_DIAGNOSTIC,
        OPT_GARDNER,
        OPT_NO_GARDNER,
        OPT_UW_REJECT,
        OPT_PARSED,
        OPT_REPLAY_RAW,
        OPT_REASSEMBLE,
        OPT_PARSE_HARDER,
        OPT_PARSE_UWEC,
        OPT_MESSAGES,
        OPT_POSITION,
        OPT_ACARS,
        OPT_ACARS_JSON,
        OPT_ACARS_UDP,
        OPT_FEED,
        OPT_STATION,
        OPT_SOAPY_SETTING,
        OPT_SOAPY_GAIN_ELEM,
        OPT_ZMQ,
        OPT_ZMQ_SUB,
        OPT_CLOCK_SOURCE,
        OPT_TIME_SOURCE,
        OPT_SDRPLAY_GAIN,
        OPT_VITA49,
        OPT_BASESTATION,
        OPT_BASESTATION_BEAM,
        OPT_AIRCRAFT_DB,
        OPT_UPDATE_DB,
        OPT_WORKERS,
    };

    static const struct option longopts[] = {
        { "file",           required_argument, NULL, 'f' },
        { "live",           no_argument,       NULL, 'l' },
        { "interface",      required_argument, NULL, 'i' },
        { "center-freq",    required_argument, NULL, 'c' },
        { "sample-rate",    required_argument, NULL, 'r' },
        { "bias-tee",       no_argument,       NULL, 'B' },
        { "threshold",      required_argument, NULL, 'd' },
        { "file-info",      required_argument, NULL, OPT_FILE_INFO },
        { "file-start",     required_argument, NULL, OPT_FILE_START },
        { "format",         required_argument, NULL, OPT_FORMAT },
        { "verbose",        no_argument,       NULL, 'v' },
        { "help",           no_argument,       NULL, 'h' },
        { "list",           no_argument,       NULL, OPT_LIST },
        { "hackrf-lna",     required_argument, NULL, OPT_HACKRF_LNA },
        { "hackrf-vga",     required_argument, NULL, OPT_HACKRF_VGA },
        { "hackrf-amp",     no_argument,       NULL, OPT_HACKRF_AMP },
        { "bladerf-gain",   required_argument, NULL, OPT_BLADERF_GAIN },
        { "usrp-gain",      required_argument, NULL, OPT_USRP_GAIN },
        { "soapy-gain",     required_argument, NULL, OPT_SOAPY_GAIN },
        { "no-gpu",         no_argument,       NULL, OPT_NO_GPU },
        { "no-simd",        no_argument,       NULL, OPT_NO_SIMD },
        { "simd",           required_argument, NULL, OPT_SIMD },
        { "chase",          optional_argument, NULL, OPT_CHASE },
        { "web",            optional_argument, NULL, OPT_WEB },
        { "archive",        optional_argument, NULL, OPT_ARCHIVE },
        { "gsmtap",         optional_argument, NULL, OPT_GSMTAP },
        { "save-bursts",    required_argument, NULL, OPT_SAVE_BURSTS },
        { "diagnostic",     no_argument,       NULL, OPT_DIAGNOSTIC },
        { "gardner",        no_argument,       NULL, OPT_GARDNER },
        { "no-gardner",     no_argument,       NULL, OPT_NO_GARDNER },
        { "uw-reject",      required_argument, NULL, OPT_UW_REJECT },
        { "parsed",         optional_argument, NULL, OPT_PARSED },
        { "replay-raw",     required_argument, NULL, OPT_REPLAY_RAW },
        { "reassemble",     required_argument, NULL, OPT_REASSEMBLE },
        { "parse-harder",   no_argument,       NULL, OPT_PARSE_HARDER },
        { "parse-uw-ec",    no_argument,       NULL, OPT_PARSE_UWEC },
        { "messages",       required_argument, NULL, OPT_MESSAGES },
        { "position",       optional_argument, NULL, OPT_POSITION },
        { "acars",          no_argument,       NULL, OPT_ACARS },
        { "acars-json",     no_argument,       NULL, OPT_ACARS_JSON },
        { "acars-udp",      required_argument, NULL, OPT_ACARS_UDP },
        { "feed",           optional_argument, NULL, OPT_FEED },
        { "station",        required_argument, NULL, OPT_STATION },
        { "soapy-setting",  required_argument, NULL, OPT_SOAPY_SETTING },
        { "soapy-gain-element", required_argument, NULL, OPT_SOAPY_GAIN_ELEM },
        { "zmq",            optional_argument, NULL, OPT_ZMQ },
        { "zmq-sub",        optional_argument, NULL, OPT_ZMQ_SUB },
        { "clock-source",   required_argument, NULL, OPT_CLOCK_SOURCE },
        { "time-source",    required_argument, NULL, OPT_TIME_SOURCE },
        { "sdrplay-gain",   required_argument, NULL, OPT_SDRPLAY_GAIN },
        { "vita49",         optional_argument, NULL, OPT_VITA49 },
        { "basestation",    optional_argument, NULL, OPT_BASESTATION },
        { "basestation-beam", no_argument,     NULL, OPT_BASESTATION_BEAM },
        { "aircraft-db",    required_argument, NULL, OPT_AIRCRAFT_DB },
        { "update-db",      no_argument,       NULL, OPT_UPDATE_DB },
        { "workers",        required_argument, NULL, OPT_WORKERS },
        { NULL,             0,                 NULL, 0 }
    };

    while ((ch = getopt_long(argc, argv, "f:li:c:r:Bd:vh", longopts, NULL)) != -1) {
        switch (ch) {
            case 'f':
                in_file = fopen(optarg, "rb");
                if (in_file == NULL)
                    err(1, "Cannot open input file '%s'", optarg);
                in_filename = optarg;
                break;

            case 'l':
                live = 1;
                break;

            case 'i':
#ifdef HAVE_HACKRF
                if (strstr(optarg, "hackrf-") == optarg) {
                    serial = strdup(optarg + 7);
                    break;
                }
#endif
#ifdef HAVE_BLADERF
                if (strstr(optarg, "bladerf") == optarg) {
                    bladerf_num = atoi(optarg + 7);
                    break;
                }
#endif
#ifdef HAVE_UHD
                if (strstr(optarg, "usrp-") == optarg) {
                    usrp_serial = strdup(usrp_get_serial(optarg));
                    break;
                }
#endif
#ifdef HAVE_SOAPYSDR
                if (strstr(optarg, "soapy:") == optarg) {
                    soapy_args = strdup(optarg + 6);
                    break;
                }
                if (strstr(optarg, "soapy-") == optarg) {
                    soapy_num = atoi(optarg + 6);
                    break;
                }
#endif
#ifdef HAVE_SDRPLAY
                if (strstr(optarg, "sdrplay-") == optarg) {
                    sdrplay_serial = strdup(optarg + 8);
                    break;
                }
#endif
                errx(1, "Unknown SDR interface: %s", optarg);
                break;

            case 'c':
                center_freq = atof(optarg);
                center_freq_explicit = 1;
                break;

            case 'r':
                samp_rate = atof(optarg);
                samp_rate_explicit = 1;
                break;

            case 'B':
                bias_tee = 1;
                break;

            case 'd':
                threshold_db = atof(optarg);
                break;

            case 'v':
                verbose = 1;
                break;

            case OPT_FILE_INFO:
                file_info = strdup(optarg);
                break;

            case OPT_FILE_START:
                if (strcmp(optarg, "sigmf") == 0) {
                    file_start_from_sigmf = 1;
                } else if (strchr(optarg, 'T')) {
                    if (sigmf_parse_datetime(optarg, &file_start_ns) != 0)
                        errx(1, "--file-start: cannot parse '%s' (ISO 8601 UTC, e.g. "
                             "2026-09-27T10:22:59.128Z)", optarg);
                } else {
                    /* integer and fraction apart: a double can't hold Unix ns exactly */
                    char *end;
                    unsigned long long sec = strtoull(optarg, &end, 10);
                    uint64_t ns = 0, scale = 100000000ULL;
                    if (*end == '.')
                        for (end++; *end >= '0' && *end <= '9'; end++, scale /= 10)
                            ns += (uint64_t)(*end - '0') * scale;
                    if (*end != '\0' || sec == 0)
                        errx(1, "--file-start: expected Unix seconds, ISO 8601 or 'sigmf', got '%s'", optarg);
                    file_start_ns = (uint64_t)sec * 1000000000ULL + ns;
                }
                break;

            case OPT_FORMAT:
                format_explicit = 1;
                iq_format_explicit = 1;
                if (strcmp(optarg, "ci8") == 0)
                    iq_format = FMT_CI8;
                else if (strcmp(optarg, "ci16") == 0)
                    iq_format = FMT_CI16;
                else if (strcmp(optarg, "cf32") == 0)
                    iq_format = FMT_CF32;
                else
                    errx(1, "Unknown format '%s'. Use ci8, ci16, or cf32.", optarg);
                break;

            case OPT_LIST:
                list_interfaces();
                break;

            case OPT_HACKRF_LNA:  hackrf_lna_gain  = atoi(optarg); break;
            case OPT_HACKRF_VGA:  hackrf_vga_gain  = atoi(optarg); break;
            case OPT_HACKRF_AMP:  hackrf_amp_enable = 1;           break;
            case OPT_BLADERF_GAIN: bladerf_gain_val = atoi(optarg); break;
            case OPT_USRP_GAIN:   usrp_gain_val    = atoi(optarg); break;
            case OPT_SOAPY_GAIN:  soapy_gain_val   = atof(optarg); break;
            case OPT_SDRPLAY_GAIN: sdrplay_gain_val = atoi(optarg); break;
            case OPT_NO_GPU:      use_gpu = 0;                       break;
            case OPT_NO_SIMD:     simd_mode = SIMD_SCALAR;           break;
            case OPT_SIMD:
                if (strcmp(optarg, "auto") == 0)
                    simd_mode = SIMD_AUTO;
                else if (strcmp(optarg, "avx2") == 0)
                    simd_mode = SIMD_AVX2;
                else if (strcmp(optarg, "sse42") == 0 || strcmp(optarg, "sse4.2") == 0)
                    simd_mode = SIMD_SSE42;
                else if (strcmp(optarg, "neon") == 0)
                    simd_mode = SIMD_NEON;
                else if (strcmp(optarg, "scalar") == 0 || strcmp(optarg, "none") == 0)
                    simd_mode = SIMD_SCALAR;
                else
                    errx(1, "Unknown --simd mode '%s'. Use auto, avx2, sse42, neon, or scalar.",
                         optarg);
                break;
            case OPT_CHASE:
                use_chase = optarg ? atoi(optarg) : 0;
                if (use_chase < 0 || use_chase > 7)
                    errx(1, "--chase flip-bits must be 0-7 (got %d); "
                         "0=disabled (default), k=1-7 enables soft-decision BCH on IDA only", use_chase);
                break;
            case OPT_WEB:
                web_enabled = 1;
                if (optarg) web_port = atoi(optarg);
                break;

            case OPT_ARCHIVE:
                archive_enabled = 1;
                if (optarg) archive_dir = strdup(optarg);
                break;

            case OPT_GSMTAP:
                gsmtap_enabled = 1;
                if (optarg) {
                    char *colon = strrchr(optarg, ':');
                    if (colon) {
                        *colon = '\0';
                        gsmtap_host = strdup(optarg);
                        gsmtap_port = atoi(colon + 1);
                    } else {
                        gsmtap_host = strdup(optarg);
                    }
                }
                break;

            case OPT_SAVE_BURSTS:
                save_bursts_dir = strdup(optarg);
                break;

            case OPT_DIAGNOSTIC:
                diagnostic_mode = 1;
                break;

            case OPT_GARDNER:
                use_gardner = 1;
                break;

            case OPT_NO_GARDNER:
                use_gardner = 0;
                break;

            case OPT_UW_REJECT:
                uw_reject_threshold = (float)atof(optarg);
                if (uw_reject_threshold < 0.0f) uw_reject_threshold = 0.0f;
                if (uw_reject_threshold > 1.0f) uw_reject_threshold = 1.0f;
                break;

            case OPT_MESSAGES: {
                if (n_messages_spec >= 4)
                    errx(1, "--messages: at most 4");
                char *spec = strdup(optarg), *save = NULL;
                char *tok = strtok_r(spec, ",", &save);
                tkr_mode_t m = tok ? tkr_mode_from_name(tok) : TKR_OFF;
                if (m == TKR_OFF)
                    errx(1, "--messages: ida, sbd, acars or libacars[,ARG...] (got '%s')", optarg);
                int json = 0;
                while ((tok = strtok_r(NULL, ",", &save))) {
                    if (strcmp(tok, "json") && strcmp(tok, "showerrs") && strcmp(tok, "nopings") && strcmp(tok, "perfect"))
                        errx(1, "--messages: unknown option '%s' (json, showerrs, nopings, perfect)", tok);
                    if (!strcmp(tok, "json")) json = 1;
                }
                if (m == TKR_LIBACARS && !json)
                    errx(1, "--messages=libacars needs json (its text spans lines)");
                free(spec);
                messages_spec[n_messages_spec++] = strdup(optarg);
                break;
            }

            case OPT_PARSE_HARDER:
                parse_harder = 1;
                break;

            case OPT_PARSE_UWEC:
                parse_uwec = 1;
                break;

            case OPT_REASSEMBLE: {
                /* MODE[,ARG...]: the reassembler.py -m MODE -a ARG,... */
                char *spec = strdup(optarg), *save = NULL;
                char *tok = strtok_r(spec, ",", &save);
                reassemble_mode = tok ? (int)tkr_mode_from_name(tok) : 0;
                if (!reassemble_mode)
                    errx(1, "--reassemble: ida, sbd, acars or libacars[,json|showerrs|nopings|perfect] (got '%s')", optarg);
                while ((tok = strtok_r(NULL, ",", &save)))
                    if (tkr_set_arg(tok) != 0)
                        errx(1, "--reassemble: unknown option '%s' (json, showerrs, nopings, perfect)", tok);
                free(spec);
                break;
            }

            case OPT_REPLAY_RAW:
                replay_raw_path = strdup(optarg);
                break;

            case OPT_PARSED:
                if (optarg == NULL)
                    parsed_mode = 1;
                else if (strcmp(optarg, "full") == 0)
                    parsed_mode = 2;
                else
                    errx(1, "--parsed takes no value, or =full (got '%s')", optarg);
                break;

            case OPT_POSITION:
                position_enabled = 1;
                web_enabled = 1;  /* position implies web map */
                if (optarg) {
                    position_height = atof(optarg);
                    if (position_height < 0 || position_height > 9000)
                        errx(1, "--position height must be 0-9000 m (got %.0f)",
                             position_height);
                }
                break;

            case OPT_ACARS:
                acars_enabled = 1;
                break;

            case OPT_ACARS_JSON:
                acars_enabled = 1;
                acars_json = 1;
                break;

            case OPT_ACARS_UDP:
                acars_enabled = 1;
                if (acars_udp_count >= ACARS_UDP_MAX)
                    errx(1, "Too many --acars-udp endpoints (max %d)",
                         ACARS_UDP_MAX);
                {
                    char *colon = strrchr(optarg, ':');
                    if (!colon)
                        errx(1, "--acars-udp requires HOST:PORT (e.g. 127.0.0.1:5555)");
                    *colon = '\0';
                    int port = atoi(colon + 1);
                    if (port <= 0 || port > 65535)
                        errx(1, "Invalid UDP port: %s", colon + 1);
                    acars_udp_hosts[acars_udp_count] = strdup(optarg);
                    acars_udp_ports[acars_udp_count] = port;
                    acars_udp_count++;
                }
                break;

            case OPT_FEED:
                acars_enabled = 1;
                if (feed_count >= FEED_MAX)
                    errx(1, "Too many --feed endpoints (max %d)", FEED_MAX);
                if (!optarg) {
                    /* bare --feed defaults to airframes.io TCP */
                    feed_hosts[feed_count] = strdup("feed.airframes.io");
                    feed_ports[feed_count] = 5590;
                    feed_is_tcp[feed_count] = 1;
                    feed_count++;
                } else if (strncmp(optarg, "udp://", 6) == 0) {
                    char *addr = optarg + 6;
                    char *colon = strrchr(addr, ':');
                    if (!colon)
                        errx(1, "--feed udp:// requires HOST:PORT "
                             "(e.g. udp://127.0.0.1:5558)");
                    *colon = '\0';
                    int port = atoi(colon + 1);
                    if (port <= 0 || port > 65535)
                        errx(1, "Invalid feed port: %s", colon + 1);
                    feed_hosts[feed_count] = strdup(addr);
                    feed_ports[feed_count] = port;
                    feed_is_tcp[feed_count] = 0;
                    feed_count++;
                } else if (strncmp(optarg, "tcp://", 6) == 0) {
                    char *addr = optarg + 6;
                    char *colon = strrchr(addr, ':');
                    if (!colon)
                        errx(1, "--feed tcp:// requires HOST:PORT "
                             "(e.g. tcp://feed.airframes.io:5590)");
                    *colon = '\0';
                    int port = atoi(colon + 1);
                    if (port <= 0 || port > 65535)
                        errx(1, "Invalid feed port: %s", colon + 1);
                    feed_hosts[feed_count] = strdup(addr);
                    feed_ports[feed_count] = port;
                    feed_is_tcp[feed_count] = 1;
                    feed_count++;
                } else {
                    errx(1, "--feed requires udp:// or tcp:// prefix "
                         "(e.g. --feed=udp://127.0.0.1:5558 or "
                         "--feed=tcp://feed.airframes.io:5590)");
                }
                break;

            case OPT_STATION:
                station_id = strdup(optarg);
                break;

            case OPT_ZMQ:
#ifdef HAVE_ZMQ
                zmq_enabled = 1;
                if (optarg)
                    zmq_endpoint = strdup(optarg);
#else
                errx(1, "--zmq requires ZMQ support (install libzmq3-dev and rebuild)");
#endif
                break;

            case OPT_ZMQ_SUB:
#ifdef HAVE_ZMQ
                zmq_sub_enabled = 1;
                if (optarg)
                    zmq_sub_endpoint = strdup(optarg);
#else
                errx(1, "--zmq-sub requires ZMQ support (install libzmq3-dev and rebuild)");
#endif
                break;

            case OPT_VITA49:
                vita49_enabled = 1;
                if (optarg)
                    vita49_endpoint = strdup(optarg);
                break;

            case OPT_BASESTATION:
                basestation_enabled = 1;
                if (optarg)
                    basestation_endpoint = strdup(optarg);
                break;

            case OPT_BASESTATION_BEAM:
                basestation_beam = 1;
                break;

            case OPT_AIRCRAFT_DB:
                aircraft_db_path = strdup(optarg);
                break;

            case OPT_UPDATE_DB: {
                int ret = aircraft_db_update();
                exit(ret == 0 ? 0 : 1);
            }

            case OPT_WORKERS: {
                int w = atoi(optarg);
                if (w < 1 || w > 32)
                    errx(1, "--workers must be 1-32");
                num_downmix_workers = w;
                break;
            }

            case OPT_SOAPY_SETTING:
#ifdef HAVE_SOAPYSDR
                if (soapy_setting_count >= SOAPY_SETTINGS_MAX)
                    errx(1, "Too many --soapy-setting options (max %d)",
                         SOAPY_SETTINGS_MAX);
                {
                    char *colon = strchr(optarg, ':');
                    if (!colon)
                        errx(1, "--soapy-setting requires KEY:VALUE "
                             "(e.g. bitpack:true)");
                    *colon = '\0';
                    soapy_setting_keys[soapy_setting_count] = strdup(optarg);
                    soapy_setting_vals[soapy_setting_count] = strdup(colon + 1);
                    soapy_setting_count++;
                }
#else
                errx(1, "--soapy-setting requires SoapySDR support");
#endif
                break;

            case OPT_SOAPY_GAIN_ELEM:
#ifdef HAVE_SOAPYSDR
                if (soapy_gain_elem_count >= SOAPY_GAINS_MAX)
                    errx(1, "Too many --soapy-gain-element options (max %d)",
                         SOAPY_GAINS_MAX);
                {
                    char *colon = strchr(optarg, ':');
                    if (!colon)
                        errx(1, "--soapy-gain-element requires NAME:VALUE "
                             "(e.g. LNA:10)");
                    *colon = '\0';
                    char *endptr;
                    double val = strtod(colon + 1, &endptr);
                    if (endptr == colon + 1 || *endptr != '\0')
                        errx(1, "--soapy-gain-element invalid value for '%s': %s",
                             optarg, colon + 1);
                    soapy_gain_elem_names[soapy_gain_elem_count] = strdup(optarg);
                    soapy_gain_elem_vals[soapy_gain_elem_count] = val;
                    soapy_gain_elem_count++;
                }
#else
                errx(1, "--soapy-gain-element requires SoapySDR support");
#endif
                break;

            case OPT_CLOCK_SOURCE:
                if (strcmp(optarg, "internal") == 0)
                    clock_source = CLOCK_SRC_INTERNAL;
                else if (strcmp(optarg, "external") == 0)
                    clock_source = CLOCK_SRC_EXTERNAL;
                else if (strcmp(optarg, "gpsdo") == 0)
                    clock_source = CLOCK_SRC_GPSDO;
                else
                    errx(1, "Unknown clock source '%s'. "
                         "Use internal, external, or gpsdo.", optarg);
                break;

            case OPT_TIME_SOURCE:
                if (strcmp(optarg, "internal") == 0)
                    time_source = CLOCK_SRC_INTERNAL;
                else if (strcmp(optarg, "external") == 0)
                    time_source = CLOCK_SRC_EXTERNAL;
                else if (strcmp(optarg, "gpsdo") == 0)
                    time_source = CLOCK_SRC_GPSDO;
                else
                    errx(1, "Unknown time source '%s'. "
                         "Use internal, external, or gpsdo.", optarg);
                break;

            case 'h':
                usage(0);
                break;

            case '?':
            default:
                usage(1);
                break;
        }
    }

    /* -i implies live capture */
    if (serial
#ifdef HAVE_BLADERF
        || bladerf_num >= 0
#endif
#ifdef HAVE_UHD
        || usrp_serial
#endif
#ifdef HAVE_SOAPYSDR
        || soapy_num >= 0 || soapy_args
#endif
#ifdef HAVE_SDRPLAY
        || sdrplay_serial
#endif
    )
        live = 1;

    if (!live && in_file == NULL && !zmq_sub_enabled && !vita49_enabled && !replay_raw_path)
        usage(1);

    if (live && in_file != NULL)
        errx(1, "Cannot use both --live and --file");

    if (replay_raw_path && (live || in_file != NULL || zmq_sub_enabled || vita49_enabled))
        errx(1, "--replay-raw replaces the input: no --file, --live, --zmq-sub or VITA 49 with it");

    if (zmq_sub_enabled && (live || in_file != NULL))
        errx(1, "Cannot use --zmq-sub with --live or --file");

    if (vita49_enabled && (live || in_file != NULL))
        errx(1, "Cannot use --vita49 with --live or --file");

    if (vita49_enabled && zmq_sub_enabled)
        errx(1, "Cannot use --vita49 with --zmq-sub");

    /* Auto-detect format from file extension if not explicitly specified */
    if (in_filename && !format_explicit) {
        const char *ext = strrchr(in_filename, '.');
        if (ext) {
            if (strcmp(ext, ".cf32") == 0 || strcmp(ext, ".fc32") == 0 ||
                strcmp(ext, ".cfile") == 0)
                iq_format = FMT_CF32;
            else if (strcmp(ext, ".ci16") == 0 || strcmp(ext, ".cs16") == 0 ||
                     strcmp(ext, ".sc16") == 0)
                iq_format = FMT_CI16;
            /* .ci8 and other extensions keep the ci8 default */
        }
    }

    /* SigMF metadata auto-configuration for file input.
     * If -f points to a .sigmf-data or .sigmf-meta file, read the companion
     * .sigmf-meta and auto-apply sample rate, center frequency, and format
     * for any values not explicitly set on the command line. */
    if (in_filename) {
        const char *ext = strrchr(in_filename, '.');
        char meta_path[PATH_MAX];
        int have_sigmf = 0;

        if (ext && strcmp(ext, ".sigmf-meta") == 0) {
            /* User passed the meta file -- switch to the data file for -f */
            snprintf(meta_path, sizeof(meta_path), "%s", in_filename);
            size_t base_len = (size_t)(ext - in_filename);
            char data_path[PATH_MAX];
            snprintf(data_path, sizeof(data_path), "%.*s.sigmf-data",
                     (int)base_len, in_filename);
            /* Reopen as data file (in_file currently points to meta) */
            fclose(in_file);
            in_file = fopen(data_path, "rb");
            if (in_file == NULL)
                err(1, "Cannot open SigMF data file '%s'", data_path);
            have_sigmf = 1;
        } else if (ext && strcmp(ext, ".sigmf-data") == 0) {
            /* User passed the data file -- look for companion meta */
            size_t base_len = (size_t)(ext - in_filename);
            snprintf(meta_path, sizeof(meta_path), "%.*s.sigmf-meta",
                     (int)base_len, in_filename);
            have_sigmf = 1;
        }

        if (have_sigmf) {
            double sigmf_sr = -1, sigmf_freq = -1;
            int sigmf_fmt = -1;

            if (sigmf_read_meta(meta_path, &sigmf_sr, &sigmf_freq,
                                &sigmf_fmt) == 0) {
                if (sigmf_fmt >= 0 && !format_explicit) {
                    iq_format = (iq_format_t)sigmf_fmt;
                    const char *fmt_names[] = { "ci8", "ci16", "cf32" };
                    fprintf(stderr, "sigmf: auto-detected format=%s\n",
                            fmt_names[iq_format]);
                }
                if (sigmf_sr > 0 && !samp_rate_explicit) {
                    samp_rate = sigmf_sr;
                    fprintf(stderr, "sigmf: auto-detected sample_rate=%.0f Hz\n",
                            sigmf_sr);
                }
                if (sigmf_freq > 0 && !center_freq_explicit) {
                    center_freq = sigmf_freq;
                    fprintf(stderr, "sigmf: auto-detected center_freq=%.0f Hz\n",
                            sigmf_freq);
                }
            }
        }
    }

    /* --file-start=sigmf: the recording's own start time, from the .sigmf-meta
     * named by -f or sitting beside the data file as <name>.sigmf-meta */
    if (file_start_from_sigmf) {
        if (!in_filename)
            errx(1, "--file-start=sigmf needs a file (-f)");
        char meta[PATH_MAX];
        const char *ext = strrchr(in_filename, '.');
        const char *slash = strrchr(in_filename, '/');
        size_t base_len = (ext && (!slash || ext > slash)) ? (size_t)(ext - in_filename)
                                                           : strlen(in_filename);
        if (ext && strcmp(ext, ".sigmf-meta") == 0)
            snprintf(meta, sizeof(meta), "%s", in_filename);
        else
            snprintf(meta, sizeof(meta), "%.*s.sigmf-meta", (int)base_len, in_filename);
        if (sigmf_read_datetime(meta, &file_start_ns) != 0)
            errx(1, "--file-start=sigmf: no captures[0].core:datetime in %s", meta);
        fprintf(stderr, "sigmf: file starts at %s (core:datetime)\n", meta);
    }

    /* Skip validation if VITA 49 will auto-detect from context packets */
    if (!vita49_enabled || samp_rate_explicit) {
        if (samp_rate <= 0)
            errx(1, "Invalid sample rate: %.0f", samp_rate);
    }

    if (!vita49_enabled || center_freq_explicit) {
        if (center_freq <= 0)
            errx(1, "Invalid center frequency: %.0f", center_freq);
    }
}
