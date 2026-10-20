//
// sfz_parser.c - SnappySynth V2
//

#include "sfz_parser.h"
#include "wav_loader.h"
#include "../Debug/logger.h"
#include "../buildconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <limits.h>
#include <float.h>
#ifndef _WIN32
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#endif

#define SFZ_OUTPUT_NORMALIZATION_GAIN 0.50f

#if defined(LEGACY_XP) || defined(LEGACY_NT4)
typedef CRITICAL_SECTION sfz_selector_lock;
#define SFZ_SELECTOR_LOCK_INIT { 0 }
static LONG g_selector_cache_lock_ready = 0;
static void sfz_selector_lock_init_once(sfz_selector_lock* lock) {
    if (InterlockedCompareExchange(&g_selector_cache_lock_ready, 1, 0) == 0) {
        InitializeCriticalSection(lock);
    } else {
        while (InterlockedCompareExchange(&g_selector_cache_lock_ready, 0, 0) == 0) {
            Sleep(0);
        }
    }
}
static void sfz_selector_lock_acquire(sfz_selector_lock* lock) {
    sfz_selector_lock_init_once(lock);
    EnterCriticalSection(lock);
}
static void sfz_selector_lock_release(sfz_selector_lock* lock) {
    LeaveCriticalSection(lock);
}
#else
typedef SRWLOCK sfz_selector_lock;
#define SFZ_SELECTOR_LOCK_INIT SRWLOCK_INIT
static void sfz_selector_lock_acquire(sfz_selector_lock* lock) {
    AcquireSRWLockExclusive(lock);
}
static void sfz_selector_lock_release(sfz_selector_lock* lock) {
    ReleaseSRWLockExclusive(lock);
}
#endif

typedef struct {
    int lokey;
    int hikey;
    int lovel;
    int hivel;
    int xfin_lovel;
    int xfin_hivel;
    int xfout_lovel;
    int xfout_hivel;
    int pitch_keycenter;
    int transpose;
    int tune_cents;
    int pan;
    int volume_millibels;
    int offset;
    int end;
    int group;
    int off_by;
    int trigger;
    int loop_mode;
    int loop_start;
    int loop_end;
    int ampeg_attack_millis;
    int ampeg_hold_millis;
    int ampeg_decay_millis;
    int ampeg_sustain_permille;
    int ampeg_release_millis;
    float ampeg_veltrack;
    float ampeg_attack_veltrack;
    float ampeg_decay_veltrack;
    float ampeg_sustain_veltrack;
    float ampeg_release_veltrack;
    int filter_type;
    int cutoff_hz;
    float fil_veltrack;
    int resonance_cents;
    int attenuation;
    float gain_veltrack;
    char sample[260];
} sfz_region_defaults;

typedef enum {
    SFZ_PARSE_CTX_NONE,
    SFZ_PARSE_CTX_GLOBAL,
    SFZ_PARSE_CTX_MASTER,
    SFZ_PARSE_CTX_GROUP,
    SFZ_PARSE_CTX_REGION
} sfz_parse_context;

static sfz_selector_lock g_selector_cache_lock = SFZ_SELECTOR_LOCK_INIT;

// Forward declaration
static void sfz_parse_reset_state(void);
static void sfz_parse_internal(sfz_instrument* inst,
                               const char* path,
                               sfz_region_defaults* global_defs,
                               sfz_region_defaults* master_defs,
                               sfz_region_defaults* group_defs);

struct sfz_sample_cache_entry {
    char full_path[512];
    wav_data* original;
    wav_data* resampled;
    int resampled_rate;
    struct sfz_sample_cache_entry* next;
};

enum {
    SFZ_SELECTOR_DRUM_WILDCARD = 0,
    SFZ_SELECTOR_DRUM_MELODIC = 1,
    SFZ_SELECTOR_DRUM_RHYTHM = 2,
    SFZ_SELECTOR_PROGRAM_BUCKETS = 129
};

enum {
    SFZ_FILTER_NONE = 0,
    SFZ_FILTER_LPF_2P = 1
};

enum {
    SFZ_REGION_RUNTIME_UNIT_KEYTRACK      = 1u << 0,
    SFZ_REGION_RUNTIME_ZERO_KEYTRACK      = 1u << 1,
    SFZ_REGION_RUNTIME_VEL_ATTACK         = 1u << 2,
    SFZ_REGION_RUNTIME_VEL_DECAY          = 1u << 3,
    SFZ_REGION_RUNTIME_VEL_SUSTAIN        = 1u << 4,
    SFZ_REGION_RUNTIME_VEL_RELEASE        = 1u << 5,
    SFZ_REGION_RUNTIME_VEL_CUTOFF         = 1u << 6,
    SFZ_REGION_RUNTIME_VEL_XFADE          = 1u << 7,
    SFZ_REGION_RUNTIME_KEY_DECAY          = 1u << 8
};

enum {
    SFZ_TRIGGER_ATTACK = 0,
    SFZ_TRIGGER_RELEASE = 1
};

enum {
    SFZ_LOOP_NONE = 0,
    SFZ_LOOP_CONTINUOUS = 1,
    SFZ_LOOP_SUSTAIN = 2,
    SFZ_LOOP_ONE_SHOT = 3
};

static void init_region_defaults(sfz_region_defaults* defs) {
    defs->sample[0] = '\0';
    defs->lokey = INT_MIN;
    defs->hikey = INT_MIN;
    defs->lovel = INT_MIN;
    defs->hivel = INT_MIN;
    defs->xfin_lovel = INT_MIN;
    defs->xfin_hivel = INT_MIN;
    defs->xfout_lovel = INT_MIN;
    defs->xfout_hivel = INT_MIN;
    defs->pitch_keycenter = INT_MIN;
    defs->transpose = INT_MIN;
    defs->tune_cents = INT_MIN;
    defs->pan = INT_MIN;
    defs->volume_millibels = INT_MIN;
    defs->offset = INT_MIN;
    defs->end = INT_MIN;
    defs->group = INT_MIN;
    defs->off_by = INT_MIN;
    defs->trigger = INT_MIN;
    defs->loop_mode = INT_MIN;
    defs->loop_start = INT_MIN;
    defs->loop_end = INT_MIN;
    defs->ampeg_attack_millis = INT_MIN;
    defs->ampeg_hold_millis = INT_MIN;
    defs->ampeg_decay_millis = INT_MIN;
    defs->ampeg_sustain_permille = INT_MIN;
    defs->ampeg_release_millis = INT_MIN;
    defs->ampeg_veltrack = FLT_MAX;
    defs->ampeg_attack_veltrack = FLT_MAX;
    defs->ampeg_decay_veltrack = FLT_MAX;
    defs->ampeg_sustain_veltrack = FLT_MAX;
    defs->ampeg_release_veltrack = FLT_MAX;
    defs->filter_type = INT_MIN;
    defs->cutoff_hz = INT_MIN;
    defs->fil_veltrack = FLT_MAX;
    defs->resonance_cents = INT_MIN;
    defs->attenuation = INT_MIN;
    defs->gain_veltrack = FLT_MAX;
}

static inline float db_to_gain(float db) {
    return powf(10.0f, db * (1.0f / 20.0f));
}

static inline float centibels_to_gain(int cb) {
    if (cb <= 0) return 1.0f;
    return powf(10.0f, (float)(-cb) * (1.0f / 200.0f));
}

static inline void pan_to_gains(int pan, float* out_l, float* out_r) {
    float pan_norm = (float)pan / 100.0f;
    if (pan_norm < -1.0f) pan_norm = -1.0f;
    if (pan_norm > 1.0f) pan_norm = 1.0f;
    *out_l = (pan_norm <= 0.0f) ? 1.0f : (1.0f - pan_norm);
    *out_r = (pan_norm >= 0.0f) ? 1.0f : (1.0f + pan_norm);
}

static inline void apply_region_defaults_from(sfz_region* region, const sfz_region_defaults* defs) {
    if (defs->lokey != INT_MIN) region->lokey = defs->lokey;
    if (defs->hikey != INT_MIN) region->hikey = defs->hikey;
    if (defs->lovel != INT_MIN) region->lovel = defs->lovel;
    if (defs->hivel != INT_MIN) region->hivel = defs->hivel;
    if (defs->xfin_lovel != INT_MIN) region->xfin_lovel = defs->xfin_lovel;
    if (defs->xfin_hivel != INT_MIN) region->xfin_hivel = defs->xfin_hivel;
    if (defs->xfout_lovel != INT_MIN) region->xfout_lovel = defs->xfout_lovel;
    if (defs->xfout_hivel != INT_MIN) region->xfout_hivel = defs->xfout_hivel;
    if (defs->pitch_keycenter != INT_MIN) region->pitch_keycenter = defs->pitch_keycenter;
    if (defs->transpose != INT_MIN) region->transpose = defs->transpose;
    if (defs->tune_cents != INT_MIN) region->tune_cents = defs->tune_cents;
    if (defs->pan != INT_MIN) region->pan = defs->pan;
    if (defs->volume_millibels != INT_MIN) region->volume_db = (float)defs->volume_millibels * 0.001f;
    if (defs->offset != INT_MIN) region->offset = defs->offset;
    if (defs->end != INT_MIN) region->end = defs->end;
    if (defs->group != INT_MIN) region->group = defs->group;
    if (defs->off_by != INT_MIN) region->off_by = defs->off_by;
    if (defs->trigger != INT_MIN) region->trigger = defs->trigger;
    if (defs->loop_mode != INT_MIN) region->loop_mode = defs->loop_mode;
    if (defs->loop_start != INT_MIN) region->loop_start = defs->loop_start;
    if (defs->loop_end != INT_MIN) region->loop_end = defs->loop_end;
    if (defs->ampeg_attack_millis != INT_MIN) region->ampeg_attack = (float)defs->ampeg_attack_millis * 0.001f;
    if (defs->ampeg_hold_millis != INT_MIN) region->ampeg_hold = (float)defs->ampeg_hold_millis * 0.001f;
    if (defs->ampeg_decay_millis != INT_MIN) region->ampeg_decay = (float)defs->ampeg_decay_millis * 0.001f;
    if (defs->ampeg_sustain_permille != INT_MIN) region->ampeg_sustain = (float)defs->ampeg_sustain_permille * 0.001f;
    if (defs->ampeg_release_millis != INT_MIN) region->ampeg_release = (float)defs->ampeg_release_millis * 0.001f;
    if (defs->ampeg_veltrack != FLT_MAX) region->ampeg_veltrack = defs->ampeg_veltrack;
    if (defs->ampeg_attack_veltrack != FLT_MAX) region->ampeg_attack_veltrack = defs->ampeg_attack_veltrack;
    if (defs->ampeg_decay_veltrack != FLT_MAX) region->ampeg_decay_veltrack = defs->ampeg_decay_veltrack;
    if (defs->ampeg_sustain_veltrack != FLT_MAX) region->ampeg_sustain_veltrack = defs->ampeg_sustain_veltrack;
    if (defs->ampeg_release_veltrack != FLT_MAX) region->ampeg_release_veltrack = defs->ampeg_release_veltrack;
    if (defs->filter_type != INT_MIN) region->filter_type = defs->filter_type;
    if (defs->cutoff_hz != INT_MIN) region->cutoff_hz = (float)defs->cutoff_hz;
    if (defs->fil_veltrack != FLT_MAX) region->fil_veltrack = defs->fil_veltrack;
    if (defs->resonance_cents != INT_MIN) region->resonance_q = 0.707f + ((float)defs->resonance_cents * 0.01f);
    if (defs->attenuation != INT_MIN) {
        region->attenuation = defs->attenuation;
        region->attenuation_gain = centibels_to_gain(region->attenuation);
    }
    if (defs->gain_veltrack != FLT_MAX) region->gain_veltrack = defs->gain_veltrack;
}

static inline void region_finalize_static_params(sfz_region* region) {
    region->attenuation_gain = centibels_to_gain(region->attenuation);
    region->volume_gain = db_to_gain(region->volume_db);
    pan_to_gains(region->pan, &region->pan_l_gain, &region->pan_r_gain);
    region->cached_base_gain = region->attenuation_gain * region->volume_gain *
        (SFZ_OUTPUT_NORMALIZATION_GAIN * (1.0f / 32768.0f));
    region->cached_pitch_base_multiplier = region->tune_multiplier;
    if (region->resonance_q < 0.1f) region->resonance_q = 0.707f;
    if (region->ampeg_sustain >= 0.0f) {
        if (region->ampeg_sustain > 1.0f) region->ampeg_sustain = 1.0f;
        if (region->ampeg_sustain < 0.0f) region->ampeg_sustain = 0.0f;
    }
    region->runtime_flags = 0;
    if (fabsf(region->pitch_keytrack - 1.0f) < 0.000001f) {
        region->runtime_flags |= SFZ_REGION_RUNTIME_UNIT_KEYTRACK;
    } else if (fabsf(region->pitch_keytrack) < 0.000001f) {
        region->runtime_flags |= SFZ_REGION_RUNTIME_ZERO_KEYTRACK;
    }
    if (fabsf(region->ampeg_attack_veltrack) > 0.0001f) region->runtime_flags |= SFZ_REGION_RUNTIME_VEL_ATTACK;
    if (fabsf(region->ampeg_decay_veltrack) > 0.0001f) region->runtime_flags |= SFZ_REGION_RUNTIME_VEL_DECAY;
    if (fabsf(region->ampeg_decay_keytrack) > 0.0001f) region->runtime_flags |= SFZ_REGION_RUNTIME_KEY_DECAY;
    if (fabsf(region->ampeg_sustain_veltrack) > 0.0001f) region->runtime_flags |= SFZ_REGION_RUNTIME_VEL_SUSTAIN;
    if (fabsf(region->ampeg_release_veltrack) > 0.0001f) region->runtime_flags |= SFZ_REGION_RUNTIME_VEL_RELEASE;
    if (fabsf(region->fil_veltrack) > 0.0001f) region->runtime_flags |= SFZ_REGION_RUNTIME_VEL_CUTOFF;
    if ((region->xfin_lovel >= 0 && region->xfin_hivel >= 0) ||
        (region->xfout_lovel >= 0 && region->xfout_hivel >= 0)) {
        region->runtime_flags |= SFZ_REGION_RUNTIME_VEL_XFADE;
    }
}

static inline void region_apply_defaults(sfz_region* region,
                                         const sfz_region_defaults* global_defs,
                                         const sfz_region_defaults* master_defs,
                                         const sfz_region_defaults* group_defs) {
    region->midi_bank = -1;
    region->midi_program = -1;
    region->midi_is_drum = -1;
    region->lokey = -1;
    region->hikey = -1;
    region->lovel = 0;
    region->hivel = 127;
    region->xfin_lovel = -1;
    region->xfin_hivel = -1;
    region->xfout_lovel = -1;
    region->xfout_hivel = -1;
    region->pitch_keycenter = -1;
    region->pitch_keytrack = 1.0f;
    region->key_override = -1;
    region->vel_override = -1;
    region->transpose = 0;
    region->tune_cents = 0;
    region->tune_multiplier = 1.0f;
    region->pan = 0;
    region->pan_l_gain = 1.0f;
    region->pan_r_gain = 1.0f;
    region->volume_db = 0.0f;
    region->volume_gain = 1.0f;
    region->offset = 0;
    region->end = -1;
    region->group = 0;
    region->off_by = 0;
    region->trigger = SFZ_TRIGGER_ATTACK;
    region->loop_mode = SFZ_LOOP_NONE;
    region->loop_start = -1;
    region->loop_end = -1;
    region->ampeg_attack = -1.0f;
    region->ampeg_hold = 0.0f;
    region->ampeg_decay = -1.0f;
    region->ampeg_sustain = -1.0f;
    region->ampeg_release = -1.0f;
    region->ampeg_veltrack = 0.0f;
    region->ampeg_attack_veltrack = 0.0f;
    region->ampeg_decay_veltrack = 0.0f;
    region->ampeg_decay_keytrack = 0.0f;
    region->ampeg_sustain_veltrack = 0.0f;
    region->ampeg_release_veltrack = 0.0f;
    region->filter_type = SFZ_FILTER_NONE;
    region->cutoff_hz = 0.0f;
    region->fil_veltrack = 0.0f;
    region->resonance_q = 0.707f;
    region->attenuation = 0;
    region->attenuation_gain = 1.0f;
    region->gain_veltrack = 0.0f;
    region->runtime_flags = 0;
    region->cached_base_gain = 0.0f;
    region->cached_pitch_base_multiplier = 1.0f;
    region->cached_attack_inc = -1.0f;
    region->cached_decay_mul = -1.0f;
    region->cached_release_alpha = -1.0f;
    region->cache_entry = NULL;
    region->opcodes = NULL;
    region->opcode_count = 0;
    region->opcode_cap = 0;

    apply_region_defaults_from(region, global_defs);
    apply_region_defaults_from(region, master_defs);
    apply_region_defaults_from(region, group_defs);
    region_finalize_static_params(region);
}

static int region_add_opcode(sfz_region* region, const char* key, const char* value) {
    if (!region || !key || !value) return 0;
    if (region->opcode_count >= region->opcode_cap) {
        int new_cap = region->opcode_cap > 0 ? region->opcode_cap * 2 : 32;
        sfz_opcode* new_ops = (sfz_opcode*)realloc(region->opcodes, sizeof(sfz_opcode) * (size_t)new_cap);
        if (!new_ops) return 0;
        region->opcodes = new_ops;
        region->opcode_cap = new_cap;
    }
    sfz_opcode* op = &region->opcodes[region->opcode_count++];
    strncpy(op->key, key, sizeof(op->key) - 1);
    op->key[sizeof(op->key) - 1] = '\0';
    strncpy(op->value, value, sizeof(op->value) - 1);
    op->value[sizeof(op->value) - 1] = '\0';
    return 1;
}

static inline void trim_inplace(char* s) {
    char* start = s;
    while (*start && isspace((unsigned char)*start)) {
        ++start;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static void strip_sfz_comments(char* line) {
    int in_quote = 0;
    char quote_char = '\0';
    for (char* p = line; *p; ++p) {
        if (in_quote) {
            if (*p == quote_char) {
                in_quote = 0;
            }
            continue;
        }
        if (*p == '"' || *p == '\'') {
            in_quote = 1;
            quote_char = *p;
            continue;
        }
        if (*p == '/' && p[1] == '/') {
            *p = '\0';
            break;
        }
    }
}

static int parse_midi_key_value(const char* text, int* out_key) {
    char buf[64];
    size_t len = strlen(text);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, text, len);
    buf[len] = '\0';
    trim_inplace(buf);
    if (!buf[0]) return 0;

    char* endptr = NULL;
    long numeric = strtol(buf, &endptr, 10);
    if (endptr && *endptr == '\0') {
        if (numeric < 0) numeric = 0;
        if (numeric > 127) numeric = 127;
        *out_key = (int)numeric;
        return 1;
    }

    char note = (char)tolower((unsigned char)buf[0]);
    int semitone = -1;
    switch (note) {
        case 'c': semitone = 0; break;
        case 'd': semitone = 2; break;
        case 'e': semitone = 4; break;
        case 'f': semitone = 5; break;
        case 'g': semitone = 7; break;
        case 'a': semitone = 9; break;
        case 'b': semitone = 11; break;
        default: return 0;
    }

    int idx = 1;
    if (buf[idx] == '#' || buf[idx] == 's' || buf[idx] == 'S') {
        ++semitone;
        ++idx;
    } else if (buf[idx] == 'b' || buf[idx] == 'B') {
        --semitone;
        ++idx;
    }

    char* octave_ptr = &buf[idx];
    if (!*octave_ptr) return 0;
    long octave = strtol(octave_ptr, &endptr, 10);
    if (!endptr || *endptr != '\0') return 0;

    int midi = (int)((octave + 1) * 12 + semitone);
    if (midi < 0) midi = 0;
    if (midi > 127) midi = 127;
    *out_key = midi;
    return 1;
}

static int parse_midi_key_range(const char* text, int* out_low, int* out_high) {
    char buf[64];
    char* dash;
    int low;
    int high;
    if (!text || !out_low || !out_high) return 0;
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    trim_inplace(buf);
    dash = strchr(buf, '-');
    if (!dash) return 0;
    *dash = '\0';
    if (!parse_midi_key_value(buf, &low)) return 0;
    if (!parse_midi_key_value(dash + 1, &high)) return 0;
    if (low > high) {
        int tmp = low;
        low = high;
        high = tmp;
    }
    *out_low = low;
    *out_high = high;
    return 1;
}

static int sfz_next_opcode(const char* line, int* cursor, char* key, size_t key_cap, char* value, size_t value_cap) {
    int i = *cursor;
    while (line[i] && isspace((unsigned char)line[i])) {
        ++i;
    }
    if (!line[i]) {
        *cursor = i;
        return 0;
    }

    int key_start = i;
    while (line[i] && line[i] != '=' && !isspace((unsigned char)line[i])) {
        ++i;
    }
    if (line[i] != '=') {
        while (line[i] && !isspace((unsigned char)line[i])) {
            ++i;
        }
        *cursor = i;
        return sfz_next_opcode(line, cursor, key, key_cap, value, value_cap);
    }

    int key_len = i - key_start;
    if (key_len <= 0) {
        *cursor = i + 1;
        return sfz_next_opcode(line, cursor, key, key_cap, value, value_cap);
    }
    if ((size_t)key_len >= key_cap) key_len = (int)key_cap - 1;
    memcpy(key, line + key_start, (size_t)key_len);
    key[key_len] = '\0';
    ++i; // skip '='

    while (line[i] && isspace((unsigned char)line[i])) {
        ++i;
    }

    int value_len = 0;
    if (line[i] == '"' || line[i] == '\'') {
        char quote = line[i++];
        int value_start = i;
        while (line[i] && line[i] != quote) {
            ++i;
        }
        value_len = i - value_start;
        if ((size_t)value_len >= value_cap) value_len = (int)value_cap - 1;
        memcpy(value, line + value_start, (size_t)value_len);
        value[value_len] = '\0';
        if (line[i] == quote) ++i;
    } else {
        int value_start = i;
        int value_end = i;
        while (line[i]) {
            if (!isspace((unsigned char)line[i])) {
                value_end = ++i;
                continue;
            }
            int j = i;
            while (line[j] && isspace((unsigned char)line[j])) ++j;
            int k = j;
            while (line[k] && (isalnum((unsigned char)line[k]) || line[k] == '_')) ++k;
            if (!line[j] || (k > j && line[k] == '=')) break;
            i = j;
        }
        i = value_end;
        value_len = i - value_start;
        if ((size_t)value_len >= value_cap) value_len = (int)value_cap - 1;
        memcpy(value, line + value_start, (size_t)value_len);
        value[value_len] = '\0';
    }

    *cursor = i;
    return 1;
}

static sfz_sample_cache_entry* sfz_find_sample_cache(sfz_instrument* inst, const char* full_path) {
    for (sfz_sample_cache_entry* entry = inst->sample_cache; entry; entry = entry->next) {
        if (_stricmp(entry->full_path, full_path) == 0) {
            return entry;
        }
    }
    return NULL;
}

static sfz_sample_cache_entry* sfz_get_or_load_sample(sfz_instrument* inst, const char* full_path) {
    sfz_sample_cache_entry* entry = sfz_find_sample_cache(inst, full_path);
    if (entry) return entry;

    wav_data* wav = full_path[0] == '*' ? wav_make_waveform(full_path, 44100) : wav_load(full_path);
    if (!wav) return NULL;

    entry = (sfz_sample_cache_entry*)calloc(1, sizeof(sfz_sample_cache_entry));
    if (!entry) {
        wav_free(wav);
        return NULL;
    }
    strncpy(entry->full_path, full_path, sizeof(entry->full_path) - 1);
    entry->original = wav;
    entry->resampled = NULL;
    entry->resampled_rate = 0;
    entry->next = inst->sample_cache;
    inst->sample_cache = entry;
    return entry;
}

sfz_instrument* sfz_create_instrument(const char* source_path, SnappySoundfontFormat format) {
    sfz_instrument* inst = (sfz_instrument*)calloc(1, sizeof(sfz_instrument));
    if (!inst) return NULL;
    if (source_path) {
        strncpy(inst->source_path, source_path, sizeof(inst->source_path) - 1);
        inst->source_path[sizeof(inst->source_path) - 1] = '\0';
    }
    inst->format = format;
    inst->region_revision = 1;
    return inst;
}

void sfz_region_init_defaults(sfz_region* region) {
    sfz_region_defaults empty_defs;
    if (!region) return;
    init_region_defaults(&empty_defs);
    memset(region, 0, sizeof(*region));
    region_apply_defaults(region, &empty_defs, &empty_defs, &empty_defs);
}

void sfz_region_finalize(sfz_region* region) {
    if (!region) return;
    if (region->sample_data && region->pitch_keycenter == -1 && region->sample_data->root_key >= 0) {
        region->pitch_keycenter = region->sample_data->root_key;
    }
    if (region->hikey == -1) {
        if (region->pitch_keycenter != -1) {
            region->lokey = region->pitch_keycenter;
            region->hikey = region->pitch_keycenter;
        } else {
            region->lokey = 60;
            region->hikey = 60;
            region->pitch_keycenter = 60;
        }
    }
    if (region->lokey == -1) {
        region->lokey = region->hikey;
    }
    if (region->pitch_keycenter == -1) {
        region->pitch_keycenter = (region->lokey + region->hikey) / 2;
    }
    if (region->sample_data) {
        float sample_tune_semitones = region->sample_data->root_key_fraction;
        float total_tune_semitones = (float)region->transpose + ((float)region->tune_cents * 0.01f) - sample_tune_semitones;
        region->tune_multiplier = powf(2.0f, total_tune_semitones * (1.0f / 12.0f));
        if (region->offset < 0) region->offset = 0;
        if (region->offset > region->sample_data->num_samples) region->offset = region->sample_data->num_samples;
        if (region->end < 0 || region->end > region->sample_data->num_samples) region->end = region->sample_data->num_samples;
        if (region->end < region->offset) region->end = region->offset;
        if (region->loop_start < 0 && region->sample_data->has_loop) region->loop_start = region->sample_data->loop_start;
        if (region->loop_end < 0 && region->sample_data->has_loop) region->loop_end = region->sample_data->loop_end;
        if (region->loop_start < region->offset) region->loop_start = region->offset;
        if (region->loop_end < 0 || region->loop_end > region->end) region->loop_end = region->end;
        if (region->loop_start >= region->loop_end) region->loop_mode = SFZ_LOOP_NONE;
    } else {
        region->tune_multiplier = powf(2.0f, ((float)region->transpose + ((float)region->tune_cents * 0.01f)) * (1.0f / 12.0f));
    }
    region_finalize_static_params(region);
}

sfz_region* sfz_append_region(sfz_instrument* inst) {
    sfz_region* new_regions;
    sfz_region* region;
    if (!inst) return NULL;
    new_regions = (sfz_region*)realloc(inst->regions, sizeof(sfz_region) * (size_t)(inst->num_regions + 1));
    if (!new_regions) return NULL;
    inst->regions = new_regions;
    region = &inst->regions[inst->num_regions++];
    sfz_region_init_defaults(region);
    sfz_invalidate_region_cache(inst);
    return region;
}

int sfz_region_attach_sample_owned(sfz_instrument* inst, sfz_region* region, const char* cache_key, wav_data* sample) {
    sfz_sample_cache_entry* entry;
    const char* key;
    if (!inst || !region || !sample) return 0;
    key = (cache_key && cache_key[0]) ? cache_key : region->sample;
    if (!key || !key[0]) key = "<memory>";
    entry = sfz_find_sample_cache(inst, key);
    if (!entry) {
        entry = (sfz_sample_cache_entry*)calloc(1, sizeof(sfz_sample_cache_entry));
        if (!entry) return 0;
        strncpy(entry->full_path, key, sizeof(entry->full_path) - 1);
        entry->full_path[sizeof(entry->full_path) - 1] = '\0';
        entry->original = sample;
        entry->resampled = NULL;
        entry->resampled_rate = 0;
        entry->next = inst->sample_cache;
        inst->sample_cache = entry;
    } else if (entry->original != sample) {
        wav_free(sample);
    }
    region->cache_entry = entry;
    region->sample_data = entry->original;
    region->resampled_data = NULL;
    region->is_resampled = 0;
    return 1;
}

void sfz_transfer_sample_cache(sfz_instrument* dst, sfz_instrument* src) {
    sfz_sample_cache_entry* tail;
    if (!dst || !src || !src->sample_cache) return;
    if (!dst->sample_cache) {
        dst->sample_cache = src->sample_cache;
        src->sample_cache = NULL;
        return;
    }
    tail = src->sample_cache;
    while (tail->next) {
        tail = tail->next;
    }
    tail->next = dst->sample_cache;
    dst->sample_cache = src->sample_cache;
    src->sample_cache = NULL;
}

static int parse_filter_type_value(const char* value) {
    if (_stricmp(value, "lpf_2p") == 0 || _stricmp(value, "lpf_1p") == 0 || _stricmp(value, "lpf") == 0) {
        return SFZ_FILTER_LPF_2P;
    }
    return SFZ_FILTER_NONE;
}

static int parse_trigger_value(const char* value) {
    if (_stricmp(value, "release") == 0) return SFZ_TRIGGER_RELEASE;
    return SFZ_TRIGGER_ATTACK;
}

static int sfz_program_bucket_index(int midi_program) {
    if (midi_program < 0 || midi_program > 127) return 0;
    return midi_program + 1;
}

static int sfz_drum_bucket_index(int midi_is_drum) {
    if (midi_is_drum > 0) return SFZ_SELECTOR_DRUM_RHYTHM;
    if (midi_is_drum == 0) return SFZ_SELECTOR_DRUM_MELODIC;
    return SFZ_SELECTOR_DRUM_WILDCARD;
}

static void sfz_free_selector_cache_unlocked(sfz_instrument* inst) {
    if (!inst) return;
    for (int d = 0; d < 3; ++d) {
        for (int p = 0; p < SFZ_SELECTOR_PROGRAM_BUCKETS; ++p) {
            free(inst->selector_buckets[d][p].indices);
            inst->selector_buckets[d][p].indices = NULL;
            inst->selector_buckets[d][p].count = 0;
            for (int k = 0; k < 128; ++k) {
                free(inst->selector_key_buckets[d][p][k].indices);
                inst->selector_key_buckets[d][p][k].indices = NULL;
                inst->selector_key_buckets[d][p][k].count = 0;
            }
        }
    }
    inst->selector_cache_revision = 0;
}

static void sfz_free_selector_cache(sfz_instrument* inst) {
    sfz_selector_lock_acquire(&g_selector_cache_lock);
    sfz_free_selector_cache_unlocked(inst);
    sfz_selector_lock_release(&g_selector_cache_lock);
}

void sfz_invalidate_region_cache(sfz_instrument* inst) {
    if (!inst) return;
    ++inst->region_revision;
    sfz_free_selector_cache(inst);
}

static int sfz_rebuild_selector_cache(sfz_instrument* inst);

/* The count tables are only needed for a rebuild; zeroing them on every lookup cost more
 * than the lookup (HANDOFF sec. 76). */
static int sfz_ensure_selector_cache(sfz_instrument* inst) {
    if (!inst) return 0;
    if (inst->selector_cache_revision == inst->region_revision) return 1;
    return sfz_rebuild_selector_cache(inst);
}

static int sfz_rebuild_selector_cache(sfz_instrument* inst) {
    int counts[3][SFZ_SELECTOR_PROGRAM_BUCKETS] = {{0}};
    int key_counts[3][SFZ_SELECTOR_PROGRAM_BUCKETS][128] = {{{0}}};

    sfz_selector_lock_acquire(&g_selector_cache_lock);
    if (inst->selector_cache_revision == inst->region_revision) {
        sfz_selector_lock_release(&g_selector_cache_lock);
        return 1;
    }

    sfz_free_selector_cache_unlocked(inst);

    for (int i = 0; i < inst->num_regions; ++i) {
        const sfz_region* region = &inst->regions[i];
        int d = sfz_drum_bucket_index(region->midi_is_drum);
        int p = sfz_program_bucket_index(region->midi_program);
        counts[d][p] += 1;
        if (region->lokey >= 0 && region->hikey >= region->lokey) {
            int lo = region->lokey < 0 ? 0 : region->lokey;
            int hi = region->hikey > 127 ? 127 : region->hikey;
            for (int k = lo; k <= hi; ++k) {
                key_counts[d][p][k] += 1;
            }
        }
    }

    for (int d = 0; d < 3; ++d) {
        for (int p = 0; p < SFZ_SELECTOR_PROGRAM_BUCKETS; ++p) {
            if (counts[d][p] <= 0) continue;
            inst->selector_buckets[d][p].indices = (int*)malloc(sizeof(int) * (size_t)counts[d][p]);
            if (!inst->selector_buckets[d][p].indices) {
                sfz_free_selector_cache_unlocked(inst);
                sfz_selector_lock_release(&g_selector_cache_lock);
                return 0;
            }
            inst->selector_buckets[d][p].count = 0;
            for (int k = 0; k < 128; ++k) {
                if (key_counts[d][p][k] <= 0) continue;
                inst->selector_key_buckets[d][p][k].indices =
                    (int*)malloc(sizeof(int) * (size_t)key_counts[d][p][k]);
                if (!inst->selector_key_buckets[d][p][k].indices) {
                    sfz_free_selector_cache_unlocked(inst);
                    sfz_selector_lock_release(&g_selector_cache_lock);
                    return 0;
                }
                inst->selector_key_buckets[d][p][k].count = 0;
            }
        }
    }

    for (int i = 0; i < inst->num_regions; ++i) {
        const sfz_region* region = &inst->regions[i];
        int d = sfz_drum_bucket_index(region->midi_is_drum);
        int p = sfz_program_bucket_index(region->midi_program);
        sfz_region_bucket* bucket = &inst->selector_buckets[d][p];
        bucket->indices[bucket->count++] = i;
        if (region->lokey >= 0 && region->hikey >= region->lokey) {
            int lo = region->lokey < 0 ? 0 : region->lokey;
            int hi = region->hikey > 127 ? 127 : region->hikey;
            for (int k = lo; k <= hi; ++k) {
                sfz_region_bucket* key_bucket = &inst->selector_key_buckets[d][p][k];
                if (key_bucket->indices) {
                    key_bucket->indices[key_bucket->count++] = i;
                }
            }
        }
    }

    inst->selector_cache_revision = inst->region_revision;
    sfz_selector_lock_release(&g_selector_cache_lock);
    return 1;
}

static int sfz_add_candidate_bucket(const sfz_region_bucket** buckets,
                                    int count,
                                    const sfz_region_bucket* bucket) {
    if (!bucket || bucket->count <= 0) return count;
    for (int i = 0; i < count; ++i) {
        if (buckets[i] == bucket) return count;
    }
    buckets[count++] = bucket;
    return count;
}

static int sfz_get_candidate_buckets(sfz_instrument* inst,
                                     int midi_program,
                                     int is_drum_part,
                                     const sfz_region_bucket** out_buckets,
                                     int max_buckets) {
    int count = 0;
    int exact_drum = is_drum_part ? SFZ_SELECTOR_DRUM_RHYTHM : SFZ_SELECTOR_DRUM_MELODIC;
    int exact_prog = sfz_program_bucket_index(midi_program);
    int fallback_prog = sfz_program_bucket_index(0);
    if (!inst || !out_buckets || max_buckets < 6) return 0;
    if (!sfz_ensure_selector_cache(inst)) return 0;

    count = sfz_add_candidate_bucket(out_buckets, count, &inst->selector_buckets[exact_drum][exact_prog]);
    count = sfz_add_candidate_bucket(out_buckets, count, &inst->selector_buckets[exact_drum][0]);
    if (is_drum_part) {
        count = sfz_add_candidate_bucket(out_buckets, count, &inst->selector_buckets[exact_drum][fallback_prog]);
    }
    count = sfz_add_candidate_bucket(out_buckets, count, &inst->selector_buckets[SFZ_SELECTOR_DRUM_WILDCARD][exact_prog]);
    count = sfz_add_candidate_bucket(out_buckets, count, &inst->selector_buckets[SFZ_SELECTOR_DRUM_WILDCARD][0]);
    if (is_drum_part) {
        count = sfz_add_candidate_bucket(out_buckets, count, &inst->selector_buckets[SFZ_SELECTOR_DRUM_WILDCARD][fallback_prog]);
    }
    return count;
}

static int sfz_get_candidate_key_buckets(sfz_instrument* inst,
                                         int midi_program,
                                         int is_drum_part,
                                         int key,
                                         const sfz_region_bucket** out_buckets,
                                         int max_buckets) {
    int count = 0;
    int exact_drum = is_drum_part ? SFZ_SELECTOR_DRUM_RHYTHM : SFZ_SELECTOR_DRUM_MELODIC;
    int exact_prog = sfz_program_bucket_index(midi_program);
    int fallback_prog = sfz_program_bucket_index(0);
    if (!inst || !out_buckets || max_buckets < 6 || key < 0 || key > 127) return 0;
    if (!sfz_ensure_selector_cache(inst)) return 0;

    count = sfz_add_candidate_bucket(out_buckets, count, &inst->selector_key_buckets[exact_drum][exact_prog][key]);
    count = sfz_add_candidate_bucket(out_buckets, count, &inst->selector_key_buckets[exact_drum][0][key]);
    if (is_drum_part) {
        count = sfz_add_candidate_bucket(out_buckets, count, &inst->selector_key_buckets[exact_drum][fallback_prog][key]);
    }
    count = sfz_add_candidate_bucket(out_buckets, count, &inst->selector_key_buckets[SFZ_SELECTOR_DRUM_WILDCARD][exact_prog][key]);
    count = sfz_add_candidate_bucket(out_buckets, count, &inst->selector_key_buckets[SFZ_SELECTOR_DRUM_WILDCARD][0][key]);
    if (is_drum_part) {
        count = sfz_add_candidate_bucket(out_buckets, count, &inst->selector_key_buckets[SFZ_SELECTOR_DRUM_WILDCARD][fallback_prog][key]);
    }
    return count;
}

static int parse_loop_mode_value(const char* value) {
    if (_stricmp(value, "loop_continuous") == 0 || _stricmp(value, "continuous") == 0) return SFZ_LOOP_CONTINUOUS;
    if (_stricmp(value, "loop_sustain") == 0 || _stricmp(value, "sustain") == 0) return SFZ_LOOP_SUSTAIN;
    if (_stricmp(value, "one_shot") == 0) return SFZ_LOOP_ONE_SHOT;
    return SFZ_LOOP_NONE;
}

/* dir(base) + middle + leaf, with '\' read as '/' outside Windows. */
static void sfz_join_path(char* out, size_t cap, const char* base, const char* middle, const char* leaf) {
    char dir[512];
    strncpy(dir, base, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char* cut = NULL;
    for (char* p = dir; *p; ++p) {
        if (*p == '/' || *p == '\\') cut = p;
    }
    if (cut) cut[1] = '\0';
    else dir[0] = '\0';
    snprintf(out, cap, "%s%s%s", dir, middle, leaf);
#ifndef _WIN32
    for (char* p = out; *p; ++p) {
        if (*p == '\\') *p = '/';
    }
    /* Collapse "a/./b" and "a//b"; ".." is left for the file system. */
    char* w = out;
    for (char* r = out; *r;) {
        if (r[0] == '/' && r[1] == '/') { ++r; continue; }
        if (r[0] == '/' && r[1] == '.' && r[2] == '/') { r += 2; continue; }
        *w++ = *r++;
    }
    *w = '\0';
#endif
}

/* Sample libraries made on Windows often spell names with a different case
 * than the files have; find each path part ignoring case. */
static void sfz_resolve_path_case(char* path) {
#ifndef _WIN32
    struct stat st;
    if (!path[0] || stat(path, &st) == 0) return;
    char built[512];
    size_t used = 0;
    const char* p = path;
    if (*p == '/') {
        built[used++] = '/';
        ++p;
    }
    built[used] = '\0';
    while (*p) {
        const char* end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char part[256];
        if (len >= sizeof(part)) return;
        memcpy(part, p, len);
        part[len] = '\0';
        char probe[512];
        snprintf(probe, sizeof(probe), "%s%s", built, part);
        if (stat(probe, &st) != 0) {
            DIR* d = opendir(used ? built : ".");
            if (!d) return;
            struct dirent* e;
            int found = 0;
            while ((e = readdir(d)) != NULL) {
                if (strcasecmp(e->d_name, part) == 0) {
                    strncpy(part, e->d_name, sizeof(part) - 1);
                    part[sizeof(part) - 1] = '\0';
                    found = 1;
                    break;
                }
            }
            closedir(d);
            if (!found) return;
        }
        int n = snprintf(built + used, sizeof(built) - used, "%s%s", part, end ? "/" : "");
        if (n < 0 || (size_t)n >= sizeof(built) - used) return;
        used += (size_t)n;
        p = end ? end + 1 : p + len;
    }
    strncpy(path, built, 511);
    path[511] = '\0';
#else
    (void)path;
#endif
}

static void sfz_load_region_sample(sfz_instrument* inst, sfz_region* region, const char* sample_path) {
    if (!region || !sample_path || !sample_path[0]) return;
    strncpy(region->sample, sample_path, sizeof(region->sample) - 1);
    region->sample[sizeof(region->sample) - 1] = '\0';

    char full_path[512];
    if (sample_path[0] == '*') {
        strncpy(full_path, sample_path, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
    } else {
        sfz_join_path(full_path, sizeof(full_path), inst->source_path, inst->default_path, sample_path);
        sfz_resolve_path_case(full_path);
    }

    sfz_sample_cache_entry* entry = sfz_get_or_load_sample(inst, full_path);
    region->cache_entry = entry;
    region->sample_data = entry ? entry->original : NULL;
    region->resampled_data = NULL;
    region->is_resampled = 0;
#ifdef DEBUG
    if (region->sample_data) {
        logger_log("Loaded sample: %s\n", full_path);
    } else {
        logger_log("Failed to load sample: %s\n", full_path);
    }
#endif
}

static inline float catmull_rom_sample(float y0, float y1, float y2, float y3, float t) {
    float a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
    float a1 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    float a2 = -0.5f * y0 + 0.5f * y2;
    return ((a0 * t + a1) * t + a2) * t + y1;
}

static inline int clamp_index(int idx, int count) {
    if (idx < 0) return 0;
    if (idx >= count) return count - 1;
    return idx;
}

sfz_instrument* sfz_parse(const char* path) {
    sfz_instrument* inst = (sfz_instrument*)calloc(1, sizeof(sfz_instrument));
    if (!inst) {
#ifdef DEBUG
        logger_log("Failed to allocate memory for sfz_instrument\n");
#endif
        return NULL;
    }
    strncpy(inst->source_path, path, sizeof(inst->source_path) - 1);
    inst->source_path[sizeof(inst->source_path) - 1] = '\0';
    inst->format = SOUND_FONT_FORMAT_SFZ;

    sfz_region_defaults global_defs;
    sfz_region_defaults master_defs;
    sfz_region_defaults group_defs;
    init_region_defaults(&global_defs);
    init_region_defaults(&master_defs);
    init_region_defaults(&group_defs);

    sfz_parse_reset_state();
    sfz_parse_internal(inst, path, &global_defs, &master_defs, &group_defs);

    /* A region whose sample is missing or unreadable cannot sound; drop it so
     * the rest of the instrument still loads. */
    int kept = 0;
    for (int i = 0; i < inst->num_regions; i++) {
        if (inst->regions[i].sample_data) {
            if (kept != i) inst->regions[kept] = inst->regions[i];
            ++kept;
        } else {
            free(inst->regions[i].opcodes);
        }
    }
    inst->num_regions = kept;

    if (inst->num_regions == 0) {
        sfz_free(inst);
        return NULL;
    }

    // Post-process regions to set defaults only where needed
    for (int i = 0; i < inst->num_regions; i++) {
        sfz_region* region = &inst->regions[i];
        if (region->sample_data && region->pitch_keycenter == -1 && region->sample_data->root_key >= 0) {
            region->pitch_keycenter = region->sample_data->root_key;
        }
        
        // If no explicit key range was set, try to derive from pitch_keycenter
        if (region->hikey == -1) {
            if (region->pitch_keycenter != -1) {
                // Single-key region centered on pitch_keycenter
                region->lokey = region->pitch_keycenter;
                region->hikey = region->pitch_keycenter;
            } else {
                // Last resort: use a single key based on region index
                int default_key = 21 + i; // Start from A0 (21)
                if (default_key > 127) default_key = 127;
                region->lokey = default_key;
                region->hikey = default_key;
                region->pitch_keycenter = default_key;
            }
        }
        
        if (region->lokey == -1) {
            region->lokey = region->hikey;
        }
        
        if (region->pitch_keycenter == -1) {
            region->pitch_keycenter = (region->lokey + region->hikey) / 2;
        }

        if (region->sample_data) {
            float sample_tune_semitones = region->sample_data->root_key_fraction;
            float total_tune_semitones = (float)region->transpose + ((float)region->tune_cents * 0.01f) - sample_tune_semitones;
            region->tune_multiplier = powf(2.0f, total_tune_semitones * (1.0f / 12.0f));
            if (region->offset < 0) region->offset = 0;
            if (region->offset > region->sample_data->num_samples) region->offset = region->sample_data->num_samples;
            if (region->end < 0 || region->end > region->sample_data->num_samples) region->end = region->sample_data->num_samples;
            if (region->end < region->offset) region->end = region->offset;
            if (region->loop_start < 0 && region->sample_data->has_loop) region->loop_start = region->sample_data->loop_start;
            if (region->loop_end < 0 && region->sample_data->has_loop) region->loop_end = region->sample_data->loop_end;
            if (region->loop_start < region->offset) region->loop_start = region->offset;
            if (region->loop_end < 0 || region->loop_end > region->end) region->loop_end = region->end;
            if (region->loop_start >= region->loop_end) region->loop_mode = SFZ_LOOP_NONE;
        } else {
            region->tune_multiplier = powf(2.0f, ((float)region->transpose + ((float)region->tune_cents * 0.01f)) * (1.0f / 12.0f));
        }
        region_finalize_static_params(region);
#ifdef DEBUG
        logger_log("Region %d: sample=%s, lokey=%d, hikey=%d, lovel=%d, hivel=%d, pitch_keycenter=%d, transpose=%d, tune=%d, cutoff=%.2f, filter_type=%d, sample_rate=%d, root=%d+%.4f\n",
                   i, region->sample, region->lokey, region->hikey, region->lovel, region->hivel, region->pitch_keycenter,
                   region->transpose, region->tune_cents,
                   region->cutoff_hz, region->filter_type,
                   region->sample_data ? region->sample_data->sample_rate : 0,
                   region->sample_data ? region->sample_data->root_key : -1,
                   region->sample_data ? region->sample_data->root_key_fraction : 0.0f);
#endif
    }

    return inst;
}

static void sfz_apply_opcode(sfz_region* region, sfz_region_defaults* defs, const char* key, const char* value) {
    int midi_key = -1;
    int parsed_key = parse_midi_key_value(value, &midi_key);
    int midi_key_low = -1;
    int midi_key_high = -1;
    int parsed_range = parse_midi_key_range(value, &midi_key_low, &midi_key_high);

    if (defs) {
        if (strcmp(key, "lokey") == 0 && parsed_key) {
            defs->lokey = midi_key;
        } else if (strcmp(key, "hikey") == 0 && parsed_key) {
            defs->hikey = midi_key;
        } else if (strcmp(key, "key") == 0 && parsed_range) {
            defs->lokey = midi_key_low;
            defs->hikey = midi_key_high;
            defs->pitch_keycenter = (midi_key_low + midi_key_high) / 2;
        } else if (strcmp(key, "lovel") == 0) {
            defs->lovel = atoi(value);
        } else if (strcmp(key, "hivel") == 0) {
            defs->hivel = atoi(value);
        } else if (strcmp(key, "xfin_lovel") == 0) {
            defs->xfin_lovel = atoi(value);
        } else if (strcmp(key, "xfin_hivel") == 0) {
            defs->xfin_hivel = atoi(value);
        } else if (strcmp(key, "xfout_lovel") == 0) {
            defs->xfout_lovel = atoi(value);
        } else if (strcmp(key, "xfout_hivel") == 0) {
            defs->xfout_hivel = atoi(value);
        } else if ((strcmp(key, "pitch_keycenter") == 0 || strcmp(key, "key") == 0) && parsed_key) {
            defs->pitch_keycenter = midi_key;
            if (strcmp(key, "key") == 0) {
                defs->lokey = midi_key;
                defs->hikey = midi_key;
            }
        } else if (strcmp(key, "pan") == 0) {
            defs->pan = atoi(value);
        } else if (strcmp(key, "volume") == 0) {
            defs->volume_millibels = (int)lrintf((float)atof(value) * 1000.0f);
        } else if (strcmp(key, "offset") == 0) {
            defs->offset = atoi(value);
        } else if (strcmp(key, "end") == 0) {
            defs->end = atoi(value);
        } else if (strcmp(key, "group") == 0) {
            defs->group = atoi(value);
        } else if (strcmp(key, "off_by") == 0) {
            defs->off_by = atoi(value);
        } else if (strcmp(key, "trigger") == 0) {
            defs->trigger = parse_trigger_value(value);
        } else if (strcmp(key, "loop_mode") == 0) {
            defs->loop_mode = parse_loop_mode_value(value);
        } else if (strcmp(key, "loop_start") == 0) {
            defs->loop_start = atoi(value);
        } else if (strcmp(key, "loop_end") == 0) {
            defs->loop_end = atoi(value);
        } else if (strcmp(key, "ampeg_attack") == 0) {
            defs->ampeg_attack_millis = (int)lrintf((float)atof(value) * 1000.0f);
        } else if (strcmp(key, "ampeg_hold") == 0) {
            defs->ampeg_hold_millis = (int)lrintf((float)atof(value) * 1000.0f);
        } else if (strcmp(key, "ampeg_decay") == 0) {
            defs->ampeg_decay_millis = (int)lrintf((float)atof(value) * 1000.0f);
        } else if (strcmp(key, "ampeg_sustain") == 0) {
            defs->ampeg_sustain_permille = (int)lrintf((float)atof(value) * 10.0f);
        } else if (strcmp(key, "ampeg_release") == 0) {
            defs->ampeg_release_millis = (int)lrintf((float)atof(value) * 1000.0f);
        } else if (strcmp(key, "ampeg_veltrack") == 0) {
            defs->ampeg_veltrack = (float)atof(value);
        } else if (strcmp(key, "ampeg_attack_veltrack") == 0) {
            defs->ampeg_attack_veltrack = (float)atof(value);
        } else if (strcmp(key, "ampeg_decay_veltrack") == 0) {
            defs->ampeg_decay_veltrack = (float)atof(value);
        } else if (strcmp(key, "ampeg_sustain_veltrack") == 0) {
            defs->ampeg_sustain_veltrack = (float)atof(value);
        } else if (strcmp(key, "ampeg_release_veltrack") == 0) {
            defs->ampeg_release_veltrack = (float)atof(value);
        } else if (strcmp(key, "fil_type") == 0) {
            defs->filter_type = parse_filter_type_value(value);
        } else if (strcmp(key, "cutoff") == 0) {
            defs->cutoff_hz = atoi(value);
        } else if (strcmp(key, "fil_veltrack") == 0) {
            defs->fil_veltrack = (float)atof(value);
        } else if (strcmp(key, "resonance") == 0) {
            defs->resonance_cents = (int)lrintf((float)atof(value) * 100.0f);
        } else if (strcmp(key, "transpose") == 0) {
            defs->transpose = atoi(value);
        } else if (strcmp(key, "tune") == 0) {
            defs->tune_cents = atoi(value);
        } else if (strcmp(key, "attenuation") == 0) {
            defs->attenuation = atoi(value);
        } else if (strcmp(key, "gain_veltrack") == 0) {
            defs->gain_veltrack = (float)atof(value);
        }
        return;
    }

    if (!region) return;
    if (strcmp(key, "lokey") == 0 && parsed_key) {
        region->lokey = midi_key;
    } else if (strcmp(key, "hikey") == 0 && parsed_key) {
        region->hikey = midi_key;
    } else if (strcmp(key, "key") == 0 && parsed_range) {
        region->lokey = midi_key_low;
        region->hikey = midi_key_high;
        region->pitch_keycenter = (midi_key_low + midi_key_high) / 2;
    } else if (strcmp(key, "lovel") == 0) {
        region->lovel = atoi(value);
    } else if (strcmp(key, "hivel") == 0) {
        region->hivel = atoi(value);
    } else if (strcmp(key, "xfin_lovel") == 0) {
        region->xfin_lovel = atoi(value);
    } else if (strcmp(key, "xfin_hivel") == 0) {
        region->xfin_hivel = atoi(value);
    } else if (strcmp(key, "xfout_lovel") == 0) {
        region->xfout_lovel = atoi(value);
    } else if (strcmp(key, "xfout_hivel") == 0) {
        region->xfout_hivel = atoi(value);
    } else if (strcmp(key, "pitch_keycenter") == 0 && parsed_key) {
        region->pitch_keycenter = midi_key;
    } else if (strcmp(key, "key") == 0 && parsed_key) {
        region->lokey = midi_key;
        region->hikey = midi_key;
        region->pitch_keycenter = midi_key;
    } else if (strcmp(key, "pan") == 0) {
        region->pan = atoi(value);
        pan_to_gains(region->pan, &region->pan_l_gain, &region->pan_r_gain);
    } else if (strcmp(key, "volume") == 0) {
        region->volume_db = (float)atof(value);
        region->volume_gain = db_to_gain(region->volume_db);
    } else if (strcmp(key, "offset") == 0) {
        region->offset = atoi(value);
    } else if (strcmp(key, "end") == 0) {
        region->end = atoi(value);
    } else if (strcmp(key, "group") == 0) {
        region->group = atoi(value);
    } else if (strcmp(key, "off_by") == 0) {
        region->off_by = atoi(value);
    } else if (strcmp(key, "trigger") == 0) {
        region->trigger = parse_trigger_value(value);
    } else if (strcmp(key, "loop_mode") == 0) {
        region->loop_mode = parse_loop_mode_value(value);
    } else if (strcmp(key, "loop_start") == 0) {
        region->loop_start = atoi(value);
    } else if (strcmp(key, "loop_end") == 0) {
        region->loop_end = atoi(value);
    } else if (strcmp(key, "ampeg_attack") == 0) {
        region->ampeg_attack = (float)atof(value);
    } else if (strcmp(key, "ampeg_hold") == 0) {
        region->ampeg_hold = (float)atof(value);
    } else if (strcmp(key, "ampeg_decay") == 0) {
        region->ampeg_decay = (float)atof(value);
    } else if (strcmp(key, "ampeg_sustain") == 0) {
        region->ampeg_sustain = (float)atof(value) * 0.01f;
    } else if (strcmp(key, "ampeg_release") == 0) {
        region->ampeg_release = (float)atof(value);
    } else if (strcmp(key, "ampeg_veltrack") == 0) {
        region->ampeg_veltrack = (float)atof(value);
    } else if (strcmp(key, "ampeg_attack_veltrack") == 0) {
        region->ampeg_attack_veltrack = (float)atof(value);
    } else if (strcmp(key, "ampeg_decay_veltrack") == 0) {
        region->ampeg_decay_veltrack = (float)atof(value);
    } else if (strcmp(key, "ampeg_sustain_veltrack") == 0) {
        region->ampeg_sustain_veltrack = (float)atof(value);
    } else if (strcmp(key, "ampeg_release_veltrack") == 0) {
        region->ampeg_release_veltrack = (float)atof(value);
    } else if (strcmp(key, "fil_type") == 0) {
        region->filter_type = parse_filter_type_value(value);
    } else if (strcmp(key, "cutoff") == 0) {
        region->cutoff_hz = (float)atof(value);
    } else if (strcmp(key, "fil_veltrack") == 0) {
        region->fil_veltrack = (float)atof(value);
    } else if (strcmp(key, "resonance") == 0) {
        region->resonance_q = 0.707f + ((float)atof(value) * 0.01f);
    } else if (strcmp(key, "transpose") == 0) {
        region->transpose = atoi(value);
    } else if (strcmp(key, "tune") == 0) {
        region->tune_cents = atoi(value);
        region->tune_multiplier = powf(2.0f, ((float)region->transpose + ((float)region->tune_cents * 0.01f)) * (1.0f / 12.0f));
    } else if (strcmp(key, "attenuation") == 0) {
        region->attenuation = atoi(value);
        region->attenuation_gain = centibels_to_gain(region->attenuation);
    } else if (strcmp(key, "gain_veltrack") == 0) {
        region->gain_veltrack = (float)atof(value);
    }
}

#define SFZ_MAX_DEFINES 256

typedef struct {
    char name[64];
    char value[256];
} sfz_define;

static sfz_define g_sfz_defines[SFZ_MAX_DEFINES];
static int g_sfz_define_count = 0;
static int g_sfz_in_block_comment = 0;
static int g_sfz_include_depth = 0;
static sfz_parse_context g_sfz_context = SFZ_PARSE_CTX_NONE;

static void sfz_parse_reset_state(void) {
    g_sfz_define_count = 0;
    g_sfz_in_block_comment = 0;
    g_sfz_include_depth = 0;
    g_sfz_context = SFZ_PARSE_CTX_NONE;
}

static const char* sfz_canonical_opcode(const char* key) {
    if (strcmp(key, "loopmode") == 0) return "loop_mode";
    if (strcmp(key, "loopstart") == 0) return "loop_start";
    if (strcmp(key, "loopend") == 0) return "loop_end";
    if (strcmp(key, "offby") == 0) return "off_by";
    if (strcmp(key, "polyphony_group") == 0) return "group";
    if (strcmp(key, "filtype") == 0) return "fil_type";
    return key;
}

static void sfz_strip_block_comments(char* line) {
    char* out = line;
    for (char* p = line; *p;) {
        if (g_sfz_in_block_comment) {
            if (p[0] == '*' && p[1] == '/') {
                g_sfz_in_block_comment = 0;
                p += 2;
            } else {
                ++p;
            }
        } else if (p[0] == '/' && p[1] == '*') {
            g_sfz_in_block_comment = 1;
            p += 2;
        } else {
            *out++ = *p++;
        }
    }
    *out = '\0';
}

static void sfz_expand_defines(const char* in, char* out, size_t cap) {
    size_t o = 0;
    while (*in && o + 1 < cap) {
        if (*in == '$') {
            int best = -1;
            size_t best_len = 0;
            for (int i = 0; i < g_sfz_define_count; ++i) {
                size_t n = strlen(g_sfz_defines[i].name);
                if (n > best_len && strncmp(in, g_sfz_defines[i].name, n) == 0) {
                    best = i;
                    best_len = n;
                }
            }
            if (best >= 0) {
                for (const char* v = g_sfz_defines[best].value; *v && o + 1 < cap; ++v) out[o++] = *v;
                in += best_len;
                continue;
            }
        }
        out[o++] = *in++;
    }
    out[o] = '\0';
}

static void sfz_add_define(const char* text) {
    while (*text == ' ' || *text == '\t') ++text;
    if (*text != '$') return;
    char name[64];
    size_t n = 0;
    while (text[n] && !isspace((unsigned char)text[n]) && n + 1 < sizeof(name)) {
        name[n] = text[n];
        ++n;
    }
    name[n] = '\0';
    const char* value = text + n;
    while (*value == ' ' || *value == '\t') ++value;
    int slot = -1;
    for (int i = 0; i < g_sfz_define_count; ++i) {
        if (strcmp(g_sfz_defines[i].name, name) == 0) slot = i;
    }
    if (slot < 0) {
        if (g_sfz_define_count >= SFZ_MAX_DEFINES) return;
        slot = g_sfz_define_count++;
    }
    strncpy(g_sfz_defines[slot].name, name, sizeof(g_sfz_defines[slot].name) - 1);
    g_sfz_defines[slot].name[sizeof(g_sfz_defines[slot].name) - 1] = '\0';
    strncpy(g_sfz_defines[slot].value, value, sizeof(g_sfz_defines[slot].value) - 1);
    g_sfz_defines[slot].value[sizeof(g_sfz_defines[slot].value) - 1] = '\0';
    trim_inplace(g_sfz_defines[slot].value);
}

static void sfz_begin_region(sfz_instrument* inst,
                             sfz_region_defaults* global_defs,
                             sfz_region_defaults* master_defs,
                             sfz_region_defaults* group_defs) {
    sfz_region* region = sfz_append_region(inst);
    if (!region) return;
    region_apply_defaults(region, global_defs, master_defs, group_defs);
    const char* inherited = group_defs->sample[0] ? group_defs->sample
                          : master_defs->sample[0] ? master_defs->sample
                          : global_defs->sample;
    if (inherited[0]) sfz_load_region_sample(inst, region, inherited);
}

static void sfz_parse_segment(sfz_instrument* inst,
                              char* seg,
                              sfz_region_defaults* global_defs,
                              sfz_region_defaults* master_defs,
                              sfz_region_defaults* group_defs) {
    if (*seg == '<') {
        char* close = strchr(seg, '>');
        if (!close) return;
        *close = '\0';
        const char* header = seg + 1;
        if (strcmp(header, "global") == 0) {
            g_sfz_context = SFZ_PARSE_CTX_GLOBAL;
            init_region_defaults(global_defs);
            init_region_defaults(master_defs);
            init_region_defaults(group_defs);
        } else if (strcmp(header, "master") == 0) {
            g_sfz_context = SFZ_PARSE_CTX_MASTER;
            init_region_defaults(master_defs);
            init_region_defaults(group_defs);
        } else if (strcmp(header, "group") == 0) {
            g_sfz_context = SFZ_PARSE_CTX_GROUP;
            init_region_defaults(group_defs);
        } else if (strcmp(header, "region") == 0) {
            g_sfz_context = SFZ_PARSE_CTX_REGION;
            sfz_begin_region(inst, global_defs, master_defs, group_defs);
        } else {
            g_sfz_context = SFZ_PARSE_CTX_NONE;
        }
        seg = close + 1;
    }

    sfz_region* region = (g_sfz_context == SFZ_PARSE_CTX_REGION && inst->num_regions > 0)
        ? &inst->regions[inst->num_regions - 1]
        : NULL;
    sfz_region_defaults* defs = NULL;
    if (g_sfz_context == SFZ_PARSE_CTX_GLOBAL) defs = global_defs;
    else if (g_sfz_context == SFZ_PARSE_CTX_MASTER) defs = master_defs;
    else if (g_sfz_context == SFZ_PARSE_CTX_GROUP) defs = group_defs;

    int cursor = 0;
    char key_buf[64];
    char value[512];
    while (sfz_next_opcode(seg, &cursor, key_buf, sizeof(key_buf), value, sizeof(value))) {
        trim_inplace(key_buf);
        trim_inplace(value);
        if (!key_buf[0] || !value[0]) continue;
        const char* key = sfz_canonical_opcode(key_buf);
        if (strcmp(key, "default_path") == 0) {
            strncpy(inst->default_path, value, sizeof(inst->default_path) - 1);
            inst->default_path[sizeof(inst->default_path) - 1] = '\0';
            continue;
        }
        if (region) {
            region_add_opcode(region, key, value);
        }
        if (strcmp(key, "sample") == 0) {
            if (region) {
                sfz_load_region_sample(inst, region, value);
            } else if (defs) {
                strncpy(defs->sample, value, sizeof(defs->sample) - 1);
                defs->sample[sizeof(defs->sample) - 1] = '\0';
            }
            continue;
        }
        sfz_apply_opcode(region, defs, key, value);
    }
}

static void sfz_parse_internal(sfz_instrument* inst,
                               const char* path,
                               sfz_region_defaults* global_defs,
                               sfz_region_defaults* master_defs,
                               sfz_region_defaults* group_defs) {
    char resolved[512];
    strncpy(resolved, path, sizeof(resolved) - 1);
    resolved[sizeof(resolved) - 1] = '\0';
    sfz_resolve_path_case(resolved);
    FILE* f = fopen(resolved, "r");
    if (!f) {
#ifdef DEBUG
        logger_log("Could not open SFZ file: %s, Exiting...\n", path);
#endif
        return;
    }

    enum { SFZ_LINE_CAP = 16384 };
    char* line = (char*)malloc(SFZ_LINE_CAP);
    char* expanded = (char*)malloc(SFZ_LINE_CAP);
    if (!line || !expanded) {
        free(line);
        free(expanded);
        fclose(f);
        return;
    }

    while (fgets(line, SFZ_LINE_CAP, f)) {
        sfz_strip_block_comments(line);
        strip_sfz_comments(line);
        trim_inplace(line);
        if (!line[0]) continue;

        if (strncmp(line, "#define", 7) == 0) {
            sfz_add_define(line + 7);
            continue;
        }

        sfz_expand_defines(line, expanded, SFZ_LINE_CAP);

        if (strncmp(expanded, "#include", 8) == 0) {
            char* include_path_str = expanded + 8;
            while (*include_path_str == ' ' || *include_path_str == '\t') include_path_str++;
            if (*include_path_str == '"') {
                include_path_str++;
                char* end_quote = strchr(include_path_str, '"');
                if (end_quote) *end_quote = '\0';
            }
            char full_include_path[512];
            sfz_join_path(full_include_path, sizeof(full_include_path), inst->source_path, "", include_path_str);
            if (g_sfz_include_depth < 16) {
                ++g_sfz_include_depth;
                sfz_parse_internal(inst, full_include_path, global_defs, master_defs, group_defs);
                --g_sfz_include_depth;
            }
            continue;
        }

        char* seg = expanded;
        while (*seg) {
            char* next = strchr(seg + 1, '<');
            if (next) *next = '\0';
            sfz_parse_segment(inst, seg, global_defs, master_defs, group_defs);
            if (!next) break;
            *next = '<';
            seg = next;
        }
    }
    free(line);
    free(expanded);
    fclose(f);

    // Log attenuation values for debugging
    for (int i = 0; i < inst->num_regions; i++) {
        sfz_region* region = &inst->regions[i];
#ifdef DEBUG
        logger_log("Region %d: attenuation=%d\n", i, region->attenuation);
#endif
    }
}

void sfz_free(sfz_instrument* inst) {
    if (inst) {
        sfz_free_selector_cache(inst);
        sfz_sample_cache_entry* entry = inst->sample_cache;
        while (entry) {
            sfz_sample_cache_entry* next = entry->next;
            wav_free(entry->original);
            if (entry->resampled && entry->resampled != entry->original) {
                wav_free(entry->resampled);
            }
            free(entry);
            entry = next;
        }
        for (int i = 0; i < inst->num_regions; ++i) {
            free(inst->regions[i].opcodes);
        }
        free(inst->regions);
        free(inst);
    }
}

static int sfz_region_selector_score(const sfz_region* region,
                                     int midi_bank_combined,
                                     int midi_bank_msb,
                                     int midi_bank_lsb,
                                     int midi_program,
                                     int is_drum_part) {
    int score = 0;
    if (!region) return -1;

    if (region->midi_is_drum >= 0) {
        if (is_drum_part) {
            if (!region->midi_is_drum) return -1;
            score += 2;
        } else {
            if (region->midi_is_drum) return -1;
            score += 2;
        }
    }

    if (region->midi_program >= 0) {
        if (midi_program < 0) {
            score += 50;
        } else if (region->midi_program == midi_program) {
            score += 100;
        } else if (region->midi_program == 0 && is_drum_part) {
            score += 10;
        } else {
            return -1;
        }
    } else {
        score += 1;
    }

    if (region->midi_bank >= 0) {
        if (midi_bank_combined < 0 && midi_bank_msb < 0 && midi_bank_lsb < 0) {
            score += 10;
        } else {
            if (is_drum_part) {
                if (region->midi_bank == midi_bank_combined) {
                    score += 40;
                } else if ((region->midi_bank & 0x7F80) == 128 &&
                           (region->midi_bank & 0x7F) == (midi_bank_combined & 0x7F)) {
                    score += 30;
                } else if (region->midi_bank == 128) {
                    score += 20;
                } else {
                    return -1;
                }
            } else if (region->midi_bank == midi_bank_combined) {
                score += 40;
            } else if (region->midi_bank == midi_bank_lsb) {
                score += 30;
            } else if (region->midi_bank == midi_bank_msb) {
                score += 20;
            } else if (region->midi_bank == 0) {
                score += 2;
            } else {
                return -1;
            }
        }
    } else {
        score += 1;
    }

    return score;
}

int sfz_collect_exact_regions_for_program(sfz_instrument* inst, int key, int vel,
                                          int midi_bank_combined, int midi_bank_msb,
                                          int midi_bank_lsb, int midi_program,
                                          int is_drum_part,
                                          sfz_region** out_regions,
                                          int max_regions) {
    const sfz_region_bucket* buckets[6];
    int bucket_count;
    int best_selector_score = -1;
    int count = 0;
    if (!inst || !out_regions || max_regions <= 0) return 0;
    bucket_count = sfz_get_candidate_key_buckets(inst, midi_program, is_drum_part, key, buckets, 6);
    if (bucket_count <= 0) return 0;

    for (int b = 0; b < bucket_count; ++b) {
        const sfz_region_bucket* bucket = buckets[b];
        for (int i = 0; i < bucket->count; ++i) {
            sfz_region* r = &inst->regions[bucket->indices[i]];
            int selector_score = sfz_region_selector_score(r, midi_bank_combined, midi_bank_msb,
                                                           midi_bank_lsb, midi_program, is_drum_part);
            if (selector_score < 0) continue;
            if (key < r->lokey || key > r->hikey || vel < r->lovel || vel > r->hivel) continue;
            if (selector_score > best_selector_score) {
                best_selector_score = selector_score;
            }
        }
    }

    if (best_selector_score < 0) return 0;

    for (int b = 0; b < bucket_count && count < max_regions; ++b) {
        const sfz_region_bucket* bucket = buckets[b];
        for (int i = 0; i < bucket->count && count < max_regions; ++i) {
            sfz_region* r = &inst->regions[bucket->indices[i]];
            int selector_score = sfz_region_selector_score(r, midi_bank_combined, midi_bank_msb,
                                                           midi_bank_lsb, midi_program, is_drum_part);
            if (selector_score != best_selector_score) continue;
            if (key < r->lokey || key > r->hikey || vel < r->lovel || vel > r->hivel) continue;
            out_regions[count++] = r;
        }
    }

    return count;
}

sfz_region* sfz_find_region_for_program(sfz_instrument* inst, int key, int vel,
                                        int midi_bank_combined, int midi_bank_msb,
                                        int midi_bank_lsb, int midi_program,
                                        int is_drum_part) {
    const sfz_region_bucket* buckets[6];
    int bucket_count = 0;
    sfz_region* best_fit = NULL;
    int best_selector_score = -1;
    int best_distance = 128;
    int best_velocity_distance = 128;
    if (!inst) return NULL;
    // First pass: exact key + velocity match
    {
        const sfz_region_bucket* key_buckets[6];
        int key_bucket_count = sfz_get_candidate_key_buckets(inst, midi_program, is_drum_part, key, key_buckets, 6);
        for (int b = 0; b < key_bucket_count; ++b) {
            const sfz_region_bucket* bucket = key_buckets[b];
            for (int i = 0; i < bucket->count; ++i) {
                sfz_region* r = &inst->regions[bucket->indices[i]];
                int selector_score = sfz_region_selector_score(r, midi_bank_combined, midi_bank_msb, midi_bank_lsb, midi_program, is_drum_part);
                if (selector_score < 0) continue;
                if (key >= r->lokey && key <= r->hikey &&
                    vel >= r->lovel && vel <= r->hivel) {
                    int distance = abs(key - r->pitch_keycenter);
                    int vel_center = (r->lovel + r->hivel) / 2;
                    int vel_distance = abs(vel - vel_center);
                    if (!best_fit || selector_score > best_selector_score ||
                        (selector_score == best_selector_score &&
                         (distance < best_distance ||
                          (distance == best_distance && vel_distance < best_velocity_distance)))) {
                        best_fit = r;
                        best_selector_score = selector_score;
                        best_distance = distance;
                        best_velocity_distance = vel_distance;
                    }
                }
            }
        }
    }
    if (best_fit) return best_fit;

    if (is_drum_part) {
        // Drum kits should not pitch the nearest mapped key when a specific
        // percussion note is missing; silence is safer than melodic fallback.
        best_fit = NULL;
        best_selector_score = -1;
        best_velocity_distance = 128;
        {
            const sfz_region_bucket* key_buckets[6];
            int key_bucket_count = sfz_get_candidate_key_buckets(inst, midi_program, is_drum_part, key, key_buckets, 6);
            for (int b = 0; b < key_bucket_count; ++b) {
                const sfz_region_bucket* bucket = key_buckets[b];
                for (int i = 0; i < bucket->count; ++i) {
                    sfz_region* r = &inst->regions[bucket->indices[i]];
                    int selector_score = sfz_region_selector_score(r, midi_bank_combined, midi_bank_msb, midi_bank_lsb, midi_program, is_drum_part);
                    if (selector_score < 0) continue;
                    if (key < r->lokey || key > r->hikey) continue;
                    {
                        int vel_center = (r->lovel + r->hivel) / 2;
                        int vel_distance = abs(vel - vel_center);
                        if (!best_fit || selector_score > best_selector_score ||
                            (selector_score == best_selector_score && vel_distance < best_velocity_distance)) {
                            best_fit = r;
                            best_selector_score = selector_score;
                            best_velocity_distance = vel_distance;
                        }
                    }
                }
            }
        }
        return best_fit;
    }

    bucket_count = sfz_get_candidate_buckets(inst, midi_program, is_drum_part, buckets, 6);
    if (bucket_count <= 0) return NULL;

    // Second pass: nearest region that still matches velocity
    best_fit = NULL;
    best_selector_score = -1;
    best_distance = 128;
    for (int b = 0; b < bucket_count; ++b) {
        const sfz_region_bucket* bucket = buckets[b];
        for (int i = 0; i < bucket->count; ++i) {
            sfz_region* r = &inst->regions[bucket->indices[i]];
            int selector_score = sfz_region_selector_score(r, midi_bank_combined, midi_bank_msb, midi_bank_lsb, midi_program, is_drum_part);
            if (selector_score < 0) continue;
            if (vel < r->lovel || vel > r->hivel) continue;
            int distance = abs(key - r->pitch_keycenter);
            if (!best_fit || selector_score > best_selector_score ||
                (selector_score == best_selector_score && distance < best_distance)) {
                best_distance = distance;
                best_selector_score = selector_score;
                best_fit = r;
            }
        }
    }
    if (best_fit) return best_fit;

    // Final fallback: nearest region ignoring velocity
    best_fit = NULL;
    best_selector_score = -1;
    best_distance = 128;
    for (int b = 0; b < bucket_count; ++b) {
        const sfz_region_bucket* bucket = buckets[b];
        for (int i = 0; i < bucket->count; ++i) {
            sfz_region* r = &inst->regions[bucket->indices[i]];
            int selector_score = sfz_region_selector_score(r, midi_bank_combined, midi_bank_msb, midi_bank_lsb, midi_program, is_drum_part);
            if (selector_score < 0) continue;
            int distance = abs(key - r->pitch_keycenter);
            if (!best_fit || selector_score > best_selector_score ||
                (selector_score == best_selector_score && distance < best_distance)) {
                best_distance = distance;
                best_selector_score = selector_score;
                best_fit = r;
            }
        }
    }
    return best_fit;
}

sfz_region* sfz_find_region(sfz_instrument* inst, int key, int vel) {
    return sfz_find_region_for_program(inst, key, vel, -1, -1, -1, -1, 0);
}

// PRE-RESAMPLING OPTIMIZATION
// Resample all samples to target sample rate at (usually at load time)
static wav_data* resample_wav_data(wav_data* original, int target_sample_rate) {
    if (!original || original->sample_rate == target_sample_rate) {
        return original; // No resampling needed
    }

    float ratio = (float)target_sample_rate / (float)original->sample_rate;
    int new_num_samples = (int)(original->num_samples * ratio);

    wav_data* resampled = (wav_data*)calloc(1, sizeof(wav_data));
    if (!resampled) return original;

    resampled->sample_rate = target_sample_rate;
    resampled->num_channels = original->num_channels;
    resampled->num_samples = new_num_samples;
    resampled->root_key = original->root_key;
    resampled->root_key_fraction = original->root_key_fraction;
    resampled->has_loop = original->has_loop;
    resampled->loop_start = original->has_loop ? (int)llround((double)original->loop_start * (double)new_num_samples / (double)original->num_samples) : 0;
    resampled->loop_end = original->has_loop ? (int)llround((double)original->loop_end * (double)new_num_samples / (double)original->num_samples) : 0;
    resampled->data = (short*)malloc(new_num_samples * original->num_channels * sizeof(short));

    if (!resampled->data) {
        free(resampled);
        return original;
    }

    // Load-time cubic interpolation. This is still cheap offline, but cleaner than linear.
    for (int i = 0; i < new_num_samples; i++) {
        float src_pos = (float)i / ratio;
        int src_idx = (int)src_pos;
        float frac = src_pos - src_idx;

        for (int ch = 0; ch < original->num_channels; ch++) {
            int i0 = clamp_index(src_idx - 1, original->num_samples);
            int i1 = clamp_index(src_idx, original->num_samples);
            int i2 = clamp_index(src_idx + 1, original->num_samples);
            int i3 = clamp_index(src_idx + 2, original->num_samples);

            float y0 = (float)original->data[i0 * original->num_channels + ch];
            float y1 = (float)original->data[i1 * original->num_channels + ch];
            float y2 = (float)original->data[i2 * original->num_channels + ch];
            float y3 = (float)original->data[i3 * original->num_channels + ch];

            float interpolated = catmull_rom_sample(y0, y1, y2, y3, frac);
            if (interpolated > 32767.0f) interpolated = 32767.0f;
            if (interpolated < -32768.0f) interpolated = -32768.0f;
            resampled->data[i * original->num_channels + ch] = (short)interpolated;
        }
    }

    logger_log("Pre-resampled %s: %d->%d Hz, %d->%d samples\n",
               "sample", original->sample_rate, target_sample_rate,
               original->num_samples, new_num_samples);

    if (resampled->has_loop) {
        if (resampled->loop_start < 0) resampled->loop_start = 0;
        if (resampled->loop_end > resampled->num_samples) resampled->loop_end = resampled->num_samples;
        if (resampled->loop_start >= resampled->loop_end) resampled->has_loop = 0;
    }

    return resampled;
}

// Apply pre-resampling to all regions in an instrument
/*
 * Outcome of the last presampling pass, for diagnosis.
 *
 * On the first soundfont load the synth runs roughly 120x slower per voice than
 * after any reload, with the same voice count: BUSY 609 ms against 5 ms. That is
 * the render loop itself, not allocation, so the voices must be taking a
 * different data path -- the obvious candidate being that they resample at run
 * time because the regions were never presampled. These counters say whether
 * that is what happens instead of leaving it to inference: regions seen,
 * regions skipped for want of sample data or a cache entry, and regions that
 * ended up actually resampled.
 */
int g_presample_regions_seen = 0;
int g_presample_regions_skipped = 0;
int g_presample_regions_resampled = 0;

void sfz_apply_presampling(sfz_instrument* inst, int target_sample_rate) {
    if (!inst) return;
    
    /* Reset here, inside this function, after its own null guard. An earlier
     * edit anchored on the first `if (!inst) return;` in the file and landed in
     * sfz_free_selector_cache_unlocked() instead, which runs after loading and
     * zeroed the counters every time. PRESMP then read 0/0 unconditionally and
     * measured nothing at all. */
    g_presample_regions_seen = 0;
    g_presample_regions_skipped = 0;
    g_presample_regions_resampled = 0;

    logger_log("Applying pre-resampling to %d regions (target: %d Hz)\n",
               inst->num_regions, target_sample_rate);

    for (int i = 0; i < inst->num_regions; i++) {
        sfz_region* region = &inst->regions[i];
        ++g_presample_regions_seen;
    if (!region->sample_data || !region->cache_entry) { ++g_presample_regions_skipped; continue; }

        region->original_sample_rate = (float)region->sample_data->sample_rate;
        sfz_sample_cache_entry* entry = region->cache_entry;
        if (entry->resampled && entry->resampled_rate == target_sample_rate) {
            region->resampled_data = entry->resampled;
        } else {
            wav_data* prepared;
            if (entry->resampled && entry->resampled != entry->original) {
                wav_free(entry->resampled);
            }
            entry->resampled = NULL;
            entry->resampled_rate = 0;
            prepared = resample_wav_data(entry->original, target_sample_rate);

            /* resample_wav_data() intentionally returns the original buffer
             * when no conversion is needed, but historically it also returned
             * the original buffer when allocation failed. Do not mark that
             * failure as a successfully cached target-rate sample: doing so
             * poisoned the cache and made every later presampling pass believe
             * the slow runtime-resampling path was already prepared. */
            if (entry->original->sample_rate != target_sample_rate &&
                prepared == entry->original) {
                region->resampled_data = NULL;
                region->is_resampled = 0;
                ++g_presample_regions_skipped;
                continue;
            }

            entry->resampled = prepared;
            entry->resampled_rate = target_sample_rate;
            region->resampled_data = prepared;
        }
        region->is_resampled = (region->resampled_data != region->sample_data);
    if (region->is_resampled) ++g_presample_regions_resampled;

        if (region->is_resampled) {
            logger_log("Region %d: Pre-resampled %s (%d->%d Hz)\n",
                       i, region->sample,
                       (int)region->original_sample_rate, target_sample_rate);
        }
    }
}

int sfz_region_get_opcode_count(const sfz_region* region) {
    return region ? region->opcode_count : 0;
}

const sfz_opcode* sfz_region_get_opcode(const sfz_region* region, int opcode_index) {
    if (!region || opcode_index < 0 || opcode_index >= region->opcode_count) return NULL;
    return &region->opcodes[opcode_index];
}
