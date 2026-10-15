#ifndef VOICE_H
#define VOICE_H

#include "../Parser/sfz_parser.h"
#include "../buildconfig.h"
#include <stdint.h>

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

typedef struct {
    // ---- CACHE LINE 0 (bytes 0-63): hot render fields ----
    // Every field here is read or written in the per-sample inner loop.
    float           position;               // +0
    float           env_level;              // +4
    int             env_state;              // +8   VoiceEnvState
    int             note_off_received;      // +12
    int             pending_note_off_samples; // +16  -1 = none
    int             startup_samples;        // +20
    volatile int    kill_now;               // +24
    int             dying;                  // +28
    float           base_gain;              // +32
    float           pitch_multiplier;       // +36
    float           portamento_offset;      // semitone offset sliding toward 0
    float           portamento_step;        // semitones advanced per output sample
    int             owner_channel;          // +40
    int             offed;                  // +44
    float           vor_gain_scale;         // +48
    float           vor_gain_target;        // +52
    float           z1_l;                   // +56
    float           z2_l;                   // +60
    // ---- CACHE LINE 1 (bytes 64-127): hot render fields cont. ----
    float           z1_r;                   // +64
    float           z2_r;                   // +68
    // remaining 56 bytes: warm fields read once per block for setup
    float           velocity;               // +72
    float           region_pan_l;           // +76
    float           region_pan_r;           // +80
    int             region_filter_enabled;  // +84
    float           region_cutoff_hz;       // +88
    float           region_resonance_q;     // +92
    float           region_b0;             // +96
    float           region_b1;             // +100
    float           region_b2;             // +104
    float           region_a1;             // +108
    float           region_a2;             // +112
    float           env_attack_inc;         // +116
    float           env_decay_dec;          // +120
    float           env_sustain_level;      // +124
    // ---- CACHE LINE 2 (bytes 128-191): warm + cold ----
    float           env_release_alpha;      // +128
    float           env_release_seconds;    // unscaled per-note release, CC72 applied at render
    int             sample_start;           // +132
    int             sample_end;             // +136
    int             loop_mode;              // +140
    int             loop_start;             // +144
    int             loop_end;              // +148
    int             group_id;              // +152
    int             key;                    // +156
    sfz_region*     region;                 // +160  (8 bytes, needs 8-align — pad if needed)
    // ---- CACHE LINE 3 (bytes 192-255): cold management fields ----
    uint32_t        voice_age;              // +192 (region* ends at 168, pad to 192 via below)
    float           release_start_level;
    unsigned int    raw_midi_msg;
    float           original_volume;
    int             owner_worker;
    int             next_by_key;
    int             prev_by_key;
    int             in_key_queue;
    unsigned int    key_order_id;
    int             next_free;
    int             attenuation;
    int             next_active;
    int             prev_active;
    volatile LONG   in_release_pool;
    #ifdef VOR
    LONGLONG        vor_start_frame;
    int             vor_stack_count;
    int             vor_pending_note_offs;
    int             vor_effective_vel;
    uint16_t        vor_token;
    uint16_t        vor_token_padding;
    float           vor_base_gain_unit;
    float           vor_original_volume_unit;
    #endif
} voice;

typedef struct {
    int active_voices;
    int free_voices;
    long steals;
    long rebalanced; /* free voices workers returned to the global pool (cumulative) */
    long drops;      /* note-ons lost: no free voice anywhere, nothing stealable (cumulative) */
} VoiceStats;

VoiceStats GetVoiceStats(void);
int voice_get_worker_count(void);
int voice_get_worker_thread_failures(void);
int voice_get_detected_cores(void);
int voice_get_path_fast(void);
void voice_refresh_all_region_caches(void);
void voice_prewarm_note_caches(void);
void voice_set_debug_metrics(int enabled);
int voice_get_alloc_us(void);
int voice_get_cc_count(int cc);
int voice_get_cc_us(int cc);
int voice_get_notes_started(void);
int voice_get_voices_recycled(void);
int voice_get_workers_participating(void);
int voice_get_render_queue_size(void);
int voice_get_miss_interp(void);
int voice_get_miss_loop(void);
int voice_get_miss_filter(void);
int voice_get_miss_other(void);
int voice_get_path_scalar(void);
int voice_get_worker_busy_us(void);
void voice_set_admit_budget_us(int us);
int voice_get_admit_budget_us(void);
int voice_get_admit_limited_notes(void);
void voice_set_admit_key_cap(int worker, int cap);
int voice_get_admit_key_cap(int worker);
int voice_get_admit_key_max(int worker);
int voice_get_admit_work_us(int worker);
int voice_get_path_simd_voice(void);
void voice_init(AudioConfig* config);
void voice_init_with_count(long voice_count, AudioConfig* config);
void voice_request_stop(void);
void voice_note_on(int key, int velocity);
void voice_note_off(int key);
void voice_note_on_ch(int ch, int key, int vel);
void voice_note_off_ch(int ch, int key);
void voice_note_on_ch_at(int ch, int key, int vel, LONGLONG timestamp_qpc);
void voice_note_on_stacked_ch_at(int ch, int key, int vel, int stack_count, LONGLONG timestamp_qpc);
void voice_note_off_ch_at(int ch, int key, LONGLONG timestamp_qpc);

// Batch note-on: push up to `count` (ch,key,vel,timestamp) tuples in a single call.
// Much lower per-event overhead than calling voice_note_on_ch_at in a loop.
// Returns number of events successfully enqueued.
typedef struct {
    int ch;
    int key;
    int vel;
    LONGLONG timestamp_qpc;
} VoiceNoteOnBatch;
int voice_note_on_batch(const VoiceNoteOnBatch *events, int count);
void voice_control_change(int ch, int cc, int val);
void voice_program_change(int ch, int program);
void voice_pitch_bend(int ch, int lsb, int msb);
void voice_control_change_at(int ch, int cc, int val, LONGLONG timestamp_qpc);
void voice_program_change_at(int ch, int program, LONGLONG timestamp_qpc);
void voice_pitch_bend_at(int ch, int lsb, int msb, LONGLONG timestamp_qpc);
void voice_send_short_at(unsigned int msg, LONGLONG timestamp_qpc);
void voice_send_note_event_at(int qch, int key, int value, int is_note_on, int stack_count, LONGLONG timestamp_qpc);
void voice_set_master_volume_14bit(int value14);
void voice_set_master_volume_14bit_at(int value14, LONGLONG timestamp_qpc);
void voice_set_drum_part(int ch, int enabled);
void voice_set_drum_part_at(int ch, int enabled, LONGLONG timestamp_qpc);
void voice_set_scale_tune_at(int ch, int note, int cents, LONGLONG timestamp_qpc);
void voice_set_gs_part_mode(int enabled);
void voice_set_render_timing(LONGLONG block_start_qpc, LONGLONG qpc_freq);
void voice_set_vor_volume_mode(int mode);
int voice_shutdown(void);
void voice_render(short* buffer, int num_samples);
void voice_render_float(float* buffer, int num_samples);
void voice_set_output_soft_clip_enabled(int enabled);

#endif // VOICE_H
