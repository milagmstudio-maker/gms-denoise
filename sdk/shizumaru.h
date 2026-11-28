/* しずまる / Shizumaru — embedding API (C, usable from C, C++, C#, Rust, ...)
 *
 * The same processing as the plug-in (all 6 modes), without JUCE. One handle per
 * audio stream. Thread rules:
 *   - shz_create / shz_destroy / shz_prepare: any thread except the audio thread
 *     (they may take ~1 s when an AI Deep model loads).
 *   - shz_process, shz_set_*, shz_get_*: from your audio thread (set/get may also be
 *     called from a UI thread; values are atomics).
 * Audio is non-interleaved float, any block size up to the max given to shz_prepare
 * (larger blocks are split internally), processed in place.
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(SHZ_BUILD_DLL)
#define SHZ_API __declspec(dllexport)
#elif defined(_WIN32) && !defined(SHZ_STATIC)
#define SHZ_API __declspec(dllimport)
#elif defined(__GNUC__)
#define SHZ_API __attribute__((visibility("default")))
#else
#define SHZ_API
#endif

typedef struct shz_engine shz_engine;

enum shz_mode {
    SHZ_MODE_ZERO = 0,       /* zero latency */
    SHZ_MODE_LOW = 1,        /* 128 samples lookahead (x rate/48k) */
    SHZ_MODE_HQ = 2,         /* 512 samples lookahead */
    SHZ_MODE_AI = 3,         /* RNNoise, 10 ms */
    SHZ_MODE_AI_DEEP = 4,    /* DeepFilterNet3 low latency, ~20 ms */
    SHZ_MODE_AI_DEEP_HQ = 5  /* DeepFilterNet3, ~40 ms */
};

enum shz_strength { SHZ_GENTLE = 0, SHZ_MEDIUM = 1, SHZ_STRONG = 2 };

typedef struct shz_params {
    float amount;        /* 0..100 %, default 50 */
    float reduction_db;  /* 0..80 dB max attenuation, default 40 (AI Deep: 80 = no limit) */
    float output_db;     /* -12..12 dB, default 0 */
    float hiss_cut;      /* 0..100 %, default 0 (modes 0-3) */
    float gate_db;       /* -90 (off) .. -10 dBFS, default -90 (modes 0-3) */
    int strength;        /* shz_strength, default SHZ_MEDIUM (modes 0-3) */
    int learn;           /* 0/1: learn noise while 1 (modes 0-3) */
    int adaptive;        /* 0/1, default 1 (modes 0-3) */
    int voice_protect;   /* 0/1 "Strong Voice Protect" (AI, AI Deep) */
    int pause_cleanup;   /* 0/1 (AI) */
} shz_params;

/* Default parameters (same as the plug-in's defaults). */
SHZ_API shz_params shz_default_params(void);

/* sample_rate: 44.1k-192k; channels 1..8 (all channels share one analysis). */
SHZ_API shz_engine* shz_create(double sample_rate, int channels, int max_block);
SHZ_API void shz_destroy(shz_engine*);
/* Change rate / channels / block size (not on the audio thread). Resets the audio state. */
SHZ_API void shz_prepare(shz_engine*, double sample_rate, int channels, int max_block);
SHZ_API void shz_reset(shz_engine*);

/* Mode change is crossfaded (20 ms). An AI Deep model loads in the background the first
 * time its mode is used (~0.5 s); meanwhile the input passes delayed by the mode's
 * latency. Call shz_preload() ahead of time (not on the audio thread) to avoid that. */
SHZ_API void shz_set_mode(shz_engine*, int mode);
SHZ_API int shz_get_mode(const shz_engine*);
SHZ_API int shz_preload(shz_engine*, int mode); /* 1 = ready */
SHZ_API void shz_set_params(shz_engine*, const shz_params*);

/* Delay of the output in samples for the current mode (compensate it in your mixer). */
SHZ_API int shz_get_latency(const shz_engine*);
/* Latency a mode will have at a rate, without an engine. */
SHZ_API int shz_latency_for(int mode, double sample_rate);
/* 1 while an AI Deep model is still loading / warming up (output = delayed input). */
SHZ_API int shz_is_loading(const shz_engine*);

/* In place: channels[c][0..n-1], c < channels given to create/prepare. */
SHZ_API void shz_process(shz_engine*, float* const* channels, int n);

/* Gain reduction meter: out[0..bands-1] in dB (<= 0), log-spaced 40 Hz..Nyquist. */
SHZ_API void shz_get_meter(const shz_engine*, float* out, int bands);

#ifdef __cplusplus
}
#endif
