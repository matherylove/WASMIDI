#ifndef VOICE_GPU_H
#define VOICE_GPU_H

#include "../buildconfig.h"
#include <stdint.h>

#define GPU_MIX_ABI_VERSION 5

#if defined(GPU_MIX_BUILD_DLL)
#define GPUMIX_API EXPORT
#else
#define GPUMIX_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef VOICE_ENV_STATE_DEFINED
#define VOICE_ENV_STATE_DEFINED
typedef enum {
    ENV_ATTACK,
    ENV_DECAY,
    ENV_SUSTAIN,
    ENV_RELEASE,
    ENV_OFF
} VoiceEnvState;
#endif

#ifdef GPUMIX

typedef struct {
    int voice_id;
    const int16_t *sample_data;
    int sample_frames;
    int playback_end;
    int loop_start;
    int loop_end;
    int sample_channels;
    int output_channels;
    int frames;
    float position;
    float pitch;
    float gainL;
    float gainR;
    float env_level;
    int env_state;
    int startup_samples;
    int pending_note_off_samples;
    int note_off;
    int sustain_down;
    int kill_now;
    int use_no_interp;
    float attack_inc;
    float decay_mul;
    float sustain_level;
    float release_alpha;
    int filter_enabled;
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
    float z1_l;
    float z2_l;
    float z1_r;
    float z2_r;
} GpuVoiceRequest;

typedef struct {
    int voice_id;
    float position;
    float env_level;
    int env_state;
    int startup_samples;
    int pending_note_off_samples;
    int note_off;
    int prev_note_off;
    int filter_enabled;
    float z1_l;
    float z2_l;
    float z1_r;
    float z2_r;
} GpuVoiceUpdate;

GPUMIX_API int  gpu_mix_global_init(int max_voices, const AudioConfig *config);
GPUMIX_API void gpu_mix_global_shutdown(void);
GPUMIX_API int  gpu_mix_get_abi_version(void);
GPUMIX_API int  gpu_mix_is_active(void);
GPUMIX_API void gpu_mix_begin(int frames,
                              int channels,
                              float attack_inc,
                              float decay_mul,
                              float sustain_level,
                              float release_alpha,
                              float kill_release_alpha,
                              float silence_threshold,
                              float release_simple_no_interp);
GPUMIX_API int  gpu_mix_queue_voice(const GpuVoiceRequest *req);
GPUMIX_API int  gpu_mix_queue_voices(const GpuVoiceRequest *reqs, int count);
GPUMIX_API int  gpu_mix_dispatch(float *out_buffer,
                                 int frames,
                                 int channels,
                                 const GpuVoiceUpdate **updates_out);

#else

typedef struct {
    int dummy;
} GpuVoiceRequest;

typedef struct {
    int dummy;
} GpuVoiceUpdate;

static inline int gpu_mix_global_init(int max_voices, const AudioConfig *config) { (void)max_voices; (void)config; return 0; }
static inline void gpu_mix_global_shutdown(void) {}
static inline int gpu_mix_get_abi_version(void) { return 0; }
static inline int gpu_mix_is_active(void) { return 0; }
static inline void gpu_mix_begin(int frames,
                                 int channels,
                                 float attack_inc,
                                 float decay_mul,
                                 float sustain_level,
                                 float release_alpha,
                                 float kill_release_alpha,
                                 float silence_threshold,
                                 float release_simple_no_interp) {
    (void)frames; (void)channels; (void)attack_inc; (void)decay_mul;
    (void)sustain_level; (void)release_alpha; (void)kill_release_alpha;
    (void)silence_threshold; (void)release_simple_no_interp;
}
static inline int gpu_mix_queue_voice(const GpuVoiceRequest *req) { (void)req; return 0; }
static inline int gpu_mix_queue_voices(const GpuVoiceRequest *reqs, int count) { (void)reqs; (void)count; return 0; }
static inline int gpu_mix_dispatch(float *out_buffer,
                                   int frames,
                                   int channels,
                                   const GpuVoiceUpdate **updates_out) {
    (void)out_buffer; (void)frames; (void)channels; (void)updates_out; return 0;
}

#endif // GPUMIX

#ifdef __cplusplus
}
#endif

#endif // VOICE_GPU_H
