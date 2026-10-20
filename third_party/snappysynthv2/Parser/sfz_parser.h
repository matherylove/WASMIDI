#ifndef SFZ_PARSER_H
#define SFZ_PARSER_H

#include "wav_loader.h"

typedef struct sfz_sample_cache_entry sfz_sample_cache_entry;
typedef struct {
    int* indices;
    int count;
} sfz_region_bucket;

typedef enum {
    SOUND_FONT_FORMAT_UNKNOWN = 0,
    SOUND_FONT_FORMAT_SFZ     = 1,
    SOUND_FONT_FORMAT_SF2     = 2,
    SOUND_FONT_FORMAT_SF3     = 3,
    SOUND_FONT_FORMAT_DLS     = 4
} SnappySoundfontFormat;

typedef struct {
    char key[64];
    char value[256];
} sfz_opcode;

typedef struct {
    // Global opcodes
    int ampeg_attack;
    int ampeg_decay;
    int ampeg_sustain;
    int ampeg_release;
} sfz_global;

typedef struct {
    // Region opcodes
    char sample[260];
    int midi_bank;
    int midi_program;
    int midi_is_drum;
    int lokey;
    int hikey;
    int lovel;
    int hivel;
    int xfin_lovel;
    int xfin_hivel;
    int xfout_lovel;
    int xfout_hivel;
    int pitch_keycenter;
    float pitch_keytrack;
    int key_override;
    int vel_override;
    int transpose;
    int tune_cents;
    float tune_multiplier;
    int pan;
    float pan_l_gain;
    float pan_r_gain;
    float volume_db;
    float volume_gain;
    int offset;
    int end;
    int group;
    int off_by;
    int trigger;
    int loop_mode;
    int loop_start;
    int loop_end;
    float ampeg_attack;
    float ampeg_hold;
    float ampeg_decay;
    float ampeg_sustain;
    float ampeg_release;
    float ampeg_veltrack;
    float ampeg_attack_veltrack;
    float ampeg_decay_veltrack;
    float ampeg_decay_keytrack;
    float ampeg_sustain_veltrack;
    float ampeg_release_veltrack;
    int filter_type;
    float cutoff_hz;
    float fil_veltrack;
    float resonance_q;
    int attenuation; // Attenuation in centibels (cB)
    float attenuation_gain;
    float gain_veltrack;
    wav_data* sample_data;
    sfz_sample_cache_entry* cache_entry;

    // PRE-RESAMPLING OPTIMIZATION
    wav_data* resampled_data;    // Pre-resampled to target sample rate
    float original_sample_rate;  // Original sample rate for pitch calculations
    int is_resampled;           // Flag indicating if resampling was done
    unsigned int runtime_flags;
    float cached_attack_inc;
    float cached_decay_mul;
    float cached_release_alpha;
    float cached_base_gain;
    float cached_pitch_base_multiplier;
    sfz_opcode* opcodes;
    int opcode_count;
    int opcode_cap;
} sfz_region;

typedef struct {
    sfz_region* regions;
    int num_regions;
    char default_path[260];
    char source_path[260];
    sfz_sample_cache_entry* sample_cache;
    SnappySoundfontFormat format;
    unsigned int region_revision;
    unsigned int selector_cache_revision;
    sfz_region_bucket selector_buckets[3][129];
    sfz_region_bucket selector_key_buckets[3][129][128];
} sfz_instrument;

sfz_instrument* sfz_parse(const char* path);
void sfz_free(sfz_instrument* inst);
sfz_instrument* sfz_create_instrument(const char* source_path, SnappySoundfontFormat format);
sfz_region* sfz_append_region(sfz_instrument* inst);
void sfz_region_init_defaults(sfz_region* region);
void sfz_region_finalize(sfz_region* region);
int sfz_region_attach_sample_owned(sfz_instrument* inst, sfz_region* region, const char* cache_key, wav_data* sample);
void sfz_transfer_sample_cache(sfz_instrument* dst, sfz_instrument* src);
void sfz_invalidate_region_cache(sfz_instrument* inst);

// PRE-RESAMPLING OPTIMIZATION
void sfz_apply_presampling(sfz_instrument* inst, int target_sample_rate);
/* Outcome of the last presampling pass; see the definition for why. */
extern int g_presample_regions_seen;
extern int g_presample_regions_skipped;
extern int g_presample_regions_resampled;
sfz_region* sfz_find_region(sfz_instrument* inst, int key, int vel);
sfz_region* sfz_find_region_for_program(sfz_instrument* inst, int key, int vel,
                                        int midi_bank_combined, int midi_bank_msb,
                                        int midi_bank_lsb, int midi_program,
                                        int is_drum_part);
int sfz_collect_exact_regions_for_program(sfz_instrument* inst, int key, int vel,
                                          int midi_bank_combined, int midi_bank_msb,
                                          int midi_bank_lsb, int midi_program,
                                          int is_drum_part,
                                          sfz_region** out_regions,
                                          int max_regions);
int sfz_region_get_opcode_count(const sfz_region* region);
const sfz_opcode* sfz_region_get_opcode(const sfz_region* region, int opcode_index);

#endif // SFZ_PARSER_H
