/*
 * cpu_pool.c -- a CPU thread pool behind the GPU plugin interface.
 *
 * burst_detect.c dlopen()s libiridium-sniffer-gpu.so and calls three
 * functions on it: create, process, destroy. Nothing in that contract is
 * actually GPU-specific -- it hands over a batch of frames and wants
 * windowed, transformed, fftshifted magnitudes back, with the burst state
 * machine staying on the caller's thread. So the same seam that exists for
 * OpenCL/Vulkan will carry a thread pool, and iridium-sniffer needs no
 * change at all: the binary that ships is the binary that runs.
 *
 * Why this seam and not --workers: the downmix workers sit AFTER burst
 * detection and were measured flat from 1 to 8 threads. Detection itself
 * is one thread, and window+FFT+magnitude is 79% of it and perfectly
 * per-frame independent.
 *
 * fftwf_execute_dft() is used rather than fftwf_execute(): it is the
 * documented thread-safe form, taking the buffers as arguments so one
 * plan can be shared across threads that each own their scratch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <complex.h>
#include <fftw3.h>
#include "simd_kernels.h"
#include "burst_fft.h"

#define GPU_API __attribute__((visibility("default")))
#define MAX_THREADS 16

typedef struct gpu_burst_fft gpu_burst_fft_t;

struct gpu_burst_fft {
    int fft_size, batch_size, nthreads;
    float *window;
    fftwf_plan plan;
    fftwf_complex *in[MAX_THREADS], *out[MAX_THREADS];

    /* per-dispatch work description */
    const float *input;
    float *output;
    int batch_count;

    pthread_t th[MAX_THREADS];
    pthread_mutex_t lock;
    pthread_cond_t start_cv, done_cv;
    int generation, done_count, shutdown;
};

/* Frames are handed out by stride so each thread walks the batch with a
 * fixed offset -- no shared cursor, no atomics. */
static void run_slice(gpu_burst_fft_t *g, int tid)
{
    for (int f = tid; f < g->batch_count; f += g->nthreads) {
        const float complex *src =
            (const float complex *)(g->input + (size_t)f * g->fft_size * 2);
        simd_window_cf(src, g->window, (float complex *)g->in[tid], g->fft_size);
        fftwf_execute_dft(g->plan, g->in[tid], g->out[tid]);
        simd_fftshift_mag((const float complex *)g->out[tid],
                          g->output + (size_t)f * g->fft_size, g->fft_size);
    }
}

static void *worker(void *arg)
{
    gpu_burst_fft_t *g = *(gpu_burst_fft_t **)arg;
    int tid = (int)(long)(((void **)arg)[1]);
    free(arg);
    int seen = 0;
    for (;;) {
        pthread_mutex_lock(&g->lock);
        while (g->generation == seen && !g->shutdown)
            pthread_cond_wait(&g->start_cv, &g->lock);
        if (g->shutdown) { pthread_mutex_unlock(&g->lock); return NULL; }
        seen = g->generation;
        pthread_mutex_unlock(&g->lock);

        run_slice(g, tid);

        pthread_mutex_lock(&g->lock);
        if (++g->done_count == g->nthreads - 1)
            pthread_cond_signal(&g->done_cv);
        pthread_mutex_unlock(&g->lock);
    }
}

GPU_API gpu_burst_fft_t *gpu_burst_fft_create(int fft_size, int batch_size,
                                              const float *window)
{
    gpu_burst_fft_t *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->fft_size = fft_size;
    g->batch_size = batch_size;

    const char *e = getenv("IRIDIUM_POOL_THREADS");
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    g->nthreads = e ? atoi(e) : (int)(ncpu > 0 ? ncpu : 1);
    if (g->nthreads < 1) g->nthreads = 1;
    if (g->nthreads > MAX_THREADS) g->nthreads = MAX_THREADS;
    if (g->nthreads > batch_size) g->nthreads = batch_size;

    simd_init(0);

    g->window = fftwf_alloc_real(fft_size);
    memcpy(g->window, window, sizeof(float) * fft_size);

    for (int i = 0; i < g->nthreads; i++) {
        g->in[i]  = fftwf_alloc_complex(fft_size);
        g->out[i] = fftwf_alloc_complex(fft_size);
        if (!g->in[i] || !g->out[i]) return NULL;
    }
    /* One plan, shared; executed via fftwf_execute_dft with per-thread
     * buffers. Same alignment for every buffer because they all come from
     * fftwf_alloc_complex. */
    g->plan = fftwf_plan_dft_1d(fft_size, g->in[0], g->out[0],
                                FFTW_FORWARD, FFTW_MEASURE);
    if (!g->plan) return NULL;

    pthread_mutex_init(&g->lock, NULL);
    pthread_cond_init(&g->start_cv, NULL);
    pthread_cond_init(&g->done_cv, NULL);

    for (int i = 1; i < g->nthreads; i++) {
        void **arg = malloc(2 * sizeof(void *));
        arg[0] = g; arg[1] = (void *)(long)i;
        pthread_create(&g->th[i], NULL, worker, arg);
    }

    fprintf(stderr, "burst_detect: CPU FFT pool, %d threads of %ld cores "
            "(%d-point, batch %d)\n", g->nthreads, ncpu, fft_size, batch_size);
    return g;
}

GPU_API int gpu_burst_fft_process(gpu_burst_fft_t *g, const float *input,
                                  float *output, int batch_count)
{
    if (batch_count <= 0 || batch_count > g->batch_size) return -1;
    g->input = input; g->output = output; g->batch_count = batch_count;

    if (g->nthreads == 1) { run_slice(g, 0); return 0; }

    pthread_mutex_lock(&g->lock);
    g->done_count = 0;
    g->generation++;
    pthread_cond_broadcast(&g->start_cv);
    pthread_mutex_unlock(&g->lock);

    run_slice(g, 0);   /* the calling thread takes slice 0 */

    pthread_mutex_lock(&g->lock);
    while (g->done_count < g->nthreads - 1)
        pthread_cond_wait(&g->done_cv, &g->lock);
    pthread_mutex_unlock(&g->lock);
    return 0;
}

GPU_API void gpu_burst_fft_destroy(gpu_burst_fft_t *g)
{
    if (!g) return;
    pthread_mutex_lock(&g->lock);
    g->shutdown = 1;
    pthread_cond_broadcast(&g->start_cv);
    pthread_mutex_unlock(&g->lock);
    for (int i = 1; i < g->nthreads; i++)
        pthread_join(g->th[i], NULL);
    for (int i = 0; i < g->nthreads; i++) {
        fftwf_free(g->in[i]); fftwf_free(g->out[i]);
    }
    fftwf_free(g->window);
    fftwf_destroy_plan(g->plan);
    free(g);
}
