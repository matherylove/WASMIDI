#include "sf2_parser.h"
#include "../Debug/logger.h"
#include "../Debug/errorhandler.h"
#include "../Debug/errors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#pragma pack(push, 1)
typedef struct {
    char id[4];
    uint32_t size;
} riff_chunk_header;

typedef struct {
    char name[20];
    uint16_t preset;
    uint16_t bank;
    uint16_t bag_index;
    uint32_t library;
    uint32_t genre;
    uint32_t morphology;
} sf2_phdr_record;

typedef struct {
    uint16_t gen_index;
    uint16_t mod_index;
} sf2_bag_record;

typedef struct {
    uint16_t oper;
    int16_t amount;
} sf2_gen_record;

typedef struct {
    char name[20];
    uint16_t bag_index;
} sf2_inst_record;

typedef struct {
    char name[20];
    uint32_t start;
    uint32_t end;
    uint32_t start_loop;
    uint32_t end_loop;
    uint32_t sample_rate;
    uint8_t original_pitch;
    int8_t pitch_correction;
    uint16_t sample_link;
    uint16_t sample_type;
} sf2_shdr_record;
#pragma pack(pop)

enum {
    SF2_GEN_START_ADDRS_OFFSET = 0,
    SF2_GEN_END_ADDRS_OFFSET = 1,
    SF2_GEN_START_LOOP_ADDRS_OFFSET = 2,
    SF2_GEN_END_LOOP_ADDRS_OFFSET = 3,
    SF2_GEN_START_ADDRS_COARSE_OFFSET = 4,
    SF2_GEN_MOD_LFO_TO_PITCH = 5,
    SF2_GEN_VIB_LFO_TO_PITCH = 6,
    SF2_GEN_MOD_ENV_TO_PITCH = 7,
    SF2_GEN_INITIAL_FILTER_FC = 8,
    SF2_GEN_INITIAL_FILTER_Q = 9,
    SF2_GEN_END_ADDRS_COARSE_OFFSET = 12,
    SF2_GEN_PAN = 17,
    SF2_GEN_DELAY_VOL_ENV = 33,
    SF2_GEN_ATTACK_VOL_ENV = 34,
    SF2_GEN_HOLD_VOL_ENV = 35,
    SF2_GEN_DECAY_VOL_ENV = 36,
    SF2_GEN_SUSTAIN_VOL_ENV = 37,
    SF2_GEN_RELEASE_VOL_ENV = 38,
    SF2_GEN_INSTRUMENT = 41,
    SF2_GEN_KEY_RANGE = 43,
    SF2_GEN_VEL_RANGE = 44,
    SF2_GEN_START_LOOP_ADDRS_COARSE_OFFSET = 45,
    SF2_GEN_KEYNUM = 46,
    SF2_GEN_VELOCITY = 47,
    SF2_GEN_INITIAL_ATTENUATION = 48,
    SF2_GEN_END_LOOP_ADDRS_COARSE_OFFSET = 50,
    SF2_GEN_COARSE_TUNE = 51,
    SF2_GEN_FINE_TUNE = 52,
    SF2_GEN_SAMPLE_ID = 53,
    SF2_GEN_SAMPLE_MODES = 54,
    SF2_GEN_SCALE_TUNING = 56,
    SF2_GEN_EXCLUSIVE_CLASS = 57,
    SF2_GEN_OVERRIDING_ROOT_KEY = 58
};

enum {
    SF2_SAMPLE_MONO = 1,
    SF2_SAMPLE_RIGHT = 2,
    SF2_SAMPLE_LEFT = 4,
    SF2_SAMPLE_LINKED = 8,
    SF2_SAMPLE_ROM_MONO = 0x8001,
    SF2_SAMPLE_ROM_RIGHT = 0x8002,
    SF2_SAMPLE_ROM_LEFT = 0x8004,
    SF2_SAMPLE_ROM_LINKED = 0x8008
};

static int sf2_sample_root_key_from_header(const sf2_shdr_record* sh) {
    if (!sh) return -1;
    return sh->original_pitch <= 127u ? (int)sh->original_pitch : -1;
}

typedef struct {
    int set_key_range;
    int set_vel_range;
    int set_root_key;
    int set_pan;
    int set_cutoff;
    int set_resonance;
    int set_instrument;
    int set_sample_id;
    int set_exclusive_class;
    int set_sample_modes;
    int set_scale_tuning;
    int set_attenuation;
    int set_attack;
    int set_decay;
    int set_sustain;
    int set_release;
    int lokey;
    int hikey;
    int lovel;
    int hivel;
    int root_key;
    int key_override;
    int vel_override;
    int pan;
    int cutoff_cents;
    int resonance_cents;
    int instrument;
    int sample_id;
    int exclusive_class;
    int sample_modes;
    int scale_tuning;
    int attenuation_cb;
    int coarse_tune;
    int fine_tune;
    int start_offset;
    int end_offset;
    int loop_start_offset;
    int loop_end_offset;
    float attack_seconds;
    float decay_seconds;
    float sustain_level;
    float release_seconds;
} sf2_zone;

typedef struct {
    char name[256];
    int preset_count;
    int instrument_count;
    int sample_count;
} sf2_collection_info;

typedef struct {
    sf2_phdr_record* phdr;
    size_t phdr_count;
    sf2_bag_record* pbag;
    size_t pbag_count;
    sf2_gen_record* pgen;
    size_t pgen_count;
    sf2_inst_record* inst;
    size_t inst_count;
    sf2_bag_record* ibag;
    size_t ibag_count;
    sf2_gen_record* igen;
    size_t igen_count;
    sf2_shdr_record* shdr;
    size_t shdr_count;
    short* sample_pool;
    size_t sample_pool_count;
    sf2_collection_info info;
} sf2_file;

static void sf2_zero_meta(sf2_metadata* meta) {
    if (!meta) return;
    memset(meta, 0, sizeof(*meta));
}

static int sf2_read_bytes(FILE* f, void* dst, size_t size) {
    return fread(dst, 1, size, f) == size;
}

static void sf2_skip_chunk(FILE* f, uint32_t size) {
    long skip = (long)size;
    if (size & 1u) skip += 1;
    fseek(f, skip, SEEK_CUR);
}

static void sf2_read_info_string(FILE* f, uint32_t size, char* dst, size_t dst_size) {
    if (!dst || dst_size == 0) {
        sf2_skip_chunk(f, size);
        return;
    }
    size_t to_read = size;
    if (to_read >= dst_size) to_read = dst_size - 1;
    if (to_read > 0) {
        fread(dst, 1, to_read, f);
    }
    dst[to_read] = '\0';
    if (size > to_read) {
        fseek(f, (long)(size - (uint32_t)to_read), SEEK_CUR);
    }
    if (size & 1u) {
        fseek(f, 1, SEEK_CUR);
    }
}

static float sf2_timecents_to_seconds(int amount) {
    if (amount <= -32768) return 0.0f;
    return powf(2.0f, (float)amount * (1.0f / 1200.0f));
}

static float sf2_abs_cents_to_hz(int cents) {
    float hz = 8.176f * powf(2.0f, (float)cents * (1.0f / 1200.0f));
    if (hz < 20.0f) hz = 20.0f;
    if (hz > 20000.0f) hz = 20000.0f;
    return hz;
}

static void sf2_zone_init(sf2_zone* zone) {
    memset(zone, 0, sizeof(*zone));
    zone->lokey = 0;
    zone->hikey = 127;
    zone->lovel = 0;
    zone->hivel = 127;
    zone->root_key = -1;
    zone->key_override = -1;
    zone->vel_override = -1;
    zone->pan = 0;
    zone->cutoff_cents = 13500;
    zone->resonance_cents = 0;
    zone->instrument = -1;
    zone->sample_id = -1;
    zone->exclusive_class = 0;
    zone->sample_modes = 0;
    zone->scale_tuning = 100;
    zone->attenuation_cb = 0;
    zone->coarse_tune = 0;
    zone->fine_tune = 0;
    zone->attack_seconds = 0.0f;
    zone->decay_seconds = 0.0f;
    zone->sustain_level = 1.0f;
    zone->release_seconds = 0.0f;
}

static int sf2_max_int(int a, int b) {
    return a > b ? a : b;
}

static int sf2_min_int(int a, int b) {
    return a < b ? a : b;
}

static float sf2_merge_time_seconds(float dst_seconds, int dst_set, float src_seconds) {
    return dst_set ? (dst_seconds * src_seconds) : src_seconds;
}

static float sf2_merge_sustain_level(float dst_level, int dst_set, float src_level) {
    return dst_set ? (dst_level * src_level) : src_level;
}

static void sf2_zone_merge(sf2_zone* dst, const sf2_zone* src) {
    if (!dst || !src) return;
    if (src->set_key_range) {
        if (dst->set_key_range) {
            dst->lokey = sf2_max_int(dst->lokey, src->lokey);
            dst->hikey = sf2_min_int(dst->hikey, src->hikey);
        } else {
            dst->lokey = src->lokey;
            dst->hikey = src->hikey;
        }
        dst->set_key_range = 1;
    }
    if (src->set_vel_range) {
        if (dst->set_vel_range) {
            dst->lovel = sf2_max_int(dst->lovel, src->lovel);
            dst->hivel = sf2_min_int(dst->hivel, src->hivel);
        } else {
            dst->lovel = src->lovel;
            dst->hivel = src->hivel;
        }
        dst->set_vel_range = 1;
    }
    if (src->set_root_key) {
        dst->root_key = src->root_key;
        dst->set_root_key = 1;
    }
    if (src->key_override >= 0) dst->key_override = src->key_override;
    if (src->vel_override >= 0) dst->vel_override = src->vel_override;
    if (src->set_pan) {
        dst->pan = src->pan;
        dst->set_pan = 1;
    }
    if (src->set_cutoff) {
        dst->cutoff_cents = src->cutoff_cents;
        dst->set_cutoff = 1;
    }
    if (src->set_resonance) {
        dst->resonance_cents = src->resonance_cents;
        dst->set_resonance = 1;
    }
    if (src->set_instrument) {
        dst->instrument = src->instrument;
        dst->set_instrument = 1;
    }
    if (src->set_sample_id) {
        dst->sample_id = src->sample_id;
        dst->set_sample_id = 1;
    }
    if (src->set_exclusive_class) {
        dst->exclusive_class = src->exclusive_class;
        dst->set_exclusive_class = 1;
    }
    if (src->set_sample_modes) {
        dst->sample_modes = src->sample_modes;
        dst->set_sample_modes = 1;
    }
    if (src->set_scale_tuning) {
        dst->scale_tuning = src->scale_tuning;
        dst->set_scale_tuning = 1;
    }
    if (src->set_attenuation) {
        dst->attenuation_cb += src->attenuation_cb;
        dst->set_attenuation = 1;
    }
    dst->coarse_tune += src->coarse_tune;
    dst->fine_tune += src->fine_tune;
    dst->start_offset += src->start_offset;
    dst->end_offset += src->end_offset;
    dst->loop_start_offset += src->loop_start_offset;
    dst->loop_end_offset += src->loop_end_offset;
    if (src->set_attack) {
        dst->attack_seconds = sf2_merge_time_seconds(dst->attack_seconds, dst->set_attack, src->attack_seconds);
        dst->set_attack = 1;
    }
    if (src->set_decay) {
        dst->decay_seconds = sf2_merge_time_seconds(dst->decay_seconds, dst->set_decay, src->decay_seconds);
        dst->set_decay = 1;
    }
    if (src->set_sustain) {
        dst->sustain_level = sf2_merge_sustain_level(dst->sustain_level, dst->set_sustain, src->sustain_level);
        if (dst->sustain_level < 0.0f) dst->sustain_level = 0.0f;
        if (dst->sustain_level > 1.0f) dst->sustain_level = 1.0f;
        dst->set_sustain = 1;
    }
    if (src->set_release) {
        dst->release_seconds = sf2_merge_time_seconds(dst->release_seconds, dst->set_release, src->release_seconds);
        dst->set_release = 1;
    }
}

static void sf2_apply_generator(sf2_zone* zone, const sf2_gen_record* gen) {
    uint16_t raw = (uint16_t)gen->amount;
    uint8_t lo = (uint8_t)(raw & 0xFFu);
    uint8_t hi = (uint8_t)((raw >> 8) & 0xFFu);
    switch (gen->oper) {
        case SF2_GEN_KEY_RANGE:
            zone->lokey = lo;
            zone->hikey = hi;
            zone->set_key_range = 1;
            break;
        case SF2_GEN_VEL_RANGE:
            zone->lovel = lo;
            zone->hivel = hi;
            zone->set_vel_range = 1;
            break;
        case SF2_GEN_KEYNUM:
            zone->key_override = (int)gen->amount;
            break;
        case SF2_GEN_VELOCITY:
            zone->vel_override = (int)gen->amount;
            break;
        case SF2_GEN_OVERRIDING_ROOT_KEY:
            zone->root_key = (int)gen->amount;
            zone->set_root_key = 1;
            break;
        case SF2_GEN_PAN:
            zone->pan = (int)gen->amount / 5;
            if (zone->pan < -100) zone->pan = -100;
            if (zone->pan > 100) zone->pan = 100;
            zone->set_pan = 1;
            break;
        case SF2_GEN_INITIAL_FILTER_FC:
            if ((int)gen->amount > 0) {
                zone->cutoff_cents = (int)gen->amount;
                if (zone->cutoff_cents < 1500) zone->cutoff_cents = 1500;
                if (zone->cutoff_cents > 13500) zone->cutoff_cents = 13500;
                zone->set_cutoff = 1;
            }
            break;
        case SF2_GEN_INITIAL_FILTER_Q:
            zone->resonance_cents = (int)gen->amount;
            zone->set_resonance = 1;
            break;
        case SF2_GEN_INSTRUMENT:
            zone->instrument = (int)(uint16_t)gen->amount;
            zone->set_instrument = 1;
            break;
        case SF2_GEN_SAMPLE_ID:
            zone->sample_id = (int)(uint16_t)gen->amount;
            zone->set_sample_id = 1;
            break;
        case SF2_GEN_INITIAL_ATTENUATION:
            zone->attenuation_cb += (int)gen->amount;
            zone->set_attenuation = 1;
            break;
        case SF2_GEN_COARSE_TUNE:
            zone->coarse_tune += (int)gen->amount;
            break;
        case SF2_GEN_FINE_TUNE:
            zone->fine_tune += (int)gen->amount;
            break;
        case SF2_GEN_SAMPLE_MODES:
            zone->sample_modes = (int)gen->amount;
            zone->set_sample_modes = 1;
            break;
        case SF2_GEN_SCALE_TUNING:
            zone->scale_tuning = (int)gen->amount;
            zone->set_scale_tuning = 1;
            break;
        case SF2_GEN_EXCLUSIVE_CLASS:
            zone->exclusive_class = (int)(uint16_t)gen->amount;
            zone->set_exclusive_class = 1;
            break;
        case SF2_GEN_ATTACK_VOL_ENV:
            zone->attack_seconds = sf2_timecents_to_seconds((int)gen->amount);
            zone->set_attack = 1;
            break;
        case SF2_GEN_DECAY_VOL_ENV:
            zone->decay_seconds = sf2_timecents_to_seconds((int)gen->amount);
            zone->set_decay = 1;
            break;
        case SF2_GEN_SUSTAIN_VOL_ENV:
            zone->sustain_level = powf(10.0f, -(float)gen->amount * (1.0f / 200.0f));
            if (zone->sustain_level < 0.0f) zone->sustain_level = 0.0f;
            if (zone->sustain_level > 1.0f) zone->sustain_level = 1.0f;
            zone->set_sustain = 1;
            break;
        case SF2_GEN_RELEASE_VOL_ENV:
            zone->release_seconds = sf2_timecents_to_seconds((int)gen->amount);
            zone->set_release = 1;
            break;
        case SF2_GEN_START_ADDRS_OFFSET:
            zone->start_offset += (int)gen->amount;
            break;
        case SF2_GEN_END_ADDRS_OFFSET:
            zone->end_offset += (int)gen->amount;
            break;
        case SF2_GEN_START_LOOP_ADDRS_OFFSET:
            zone->loop_start_offset += (int)gen->amount;
            break;
        case SF2_GEN_END_LOOP_ADDRS_OFFSET:
            zone->loop_end_offset += (int)gen->amount;
            break;
        case SF2_GEN_START_ADDRS_COARSE_OFFSET:
            zone->start_offset += ((int)gen->amount) * 32768;
            break;
        case SF2_GEN_END_ADDRS_COARSE_OFFSET:
            zone->end_offset += ((int)gen->amount) * 32768;
            break;
        case SF2_GEN_START_LOOP_ADDRS_COARSE_OFFSET:
            zone->loop_start_offset += ((int)gen->amount) * 32768;
            break;
        case SF2_GEN_END_LOOP_ADDRS_COARSE_OFFSET:
            zone->loop_end_offset += ((int)gen->amount) * 32768;
            break;
        default:
            break;
    }
}

static int sf2_zone_has_instrument(const sf2_zone* zone) {
    return zone && zone->set_instrument && zone->instrument >= 0;
}

static int sf2_zone_has_sample(const sf2_zone* zone) {
    return zone && zone->set_sample_id && zone->sample_id >= 0;
}

static void sf2_read_zone_range(const sf2_gen_record* gens, size_t start, size_t end, sf2_zone* out_zone) {
    sf2_zone_init(out_zone);
    for (size_t i = start; i < end; ++i) {
        sf2_apply_generator(out_zone, &gens[i]);
    }
}

static void sf2_parse_info_list(FILE* f, uint32_t list_size, sf2_metadata* meta) {
    long start = ftell(f);
    while ((uint32_t)(ftell(f) - start) + 8 <= list_size) {
        riff_chunk_header chunk;
        if (!sf2_read_bytes(f, &chunk, sizeof(chunk))) break;
        if (memcmp(chunk.id, "INAM", 4) == 0) {
            sf2_read_info_string(f, chunk.size, meta->name, sizeof(meta->name));
        } else if (memcmp(chunk.id, "isng", 4) == 0) {
            sf2_read_info_string(f, chunk.size, meta->engine, sizeof(meta->engine));
        } else if (memcmp(chunk.id, "IPRD", 4) == 0) {
            sf2_read_info_string(f, chunk.size, meta->product, sizeof(meta->product));
        } else if (memcmp(chunk.id, "ICMT", 4) == 0) {
            sf2_read_info_string(f, chunk.size, meta->comments, sizeof(meta->comments));
        } else {
            sf2_skip_chunk(f, chunk.size);
        }
    }
    fseek(f, start + list_size, SEEK_SET);
}

static void sf2_parse_pdta_meta(FILE* f, uint32_t list_size, sf2_metadata* meta) {
    long start = ftell(f);
    while ((uint32_t)(ftell(f) - start) + 8 <= list_size) {
        riff_chunk_header chunk;
        if (!sf2_read_bytes(f, &chunk, sizeof(chunk))) break;
        if (memcmp(chunk.id, "phdr", 4) == 0) {
            meta->preset_count = (int)(chunk.size / sizeof(sf2_phdr_record));
            if (meta->preset_count > 0) meta->preset_count -= 1;
            sf2_skip_chunk(f, chunk.size);
        } else if (memcmp(chunk.id, "inst", 4) == 0) {
            meta->instrument_count = (int)(chunk.size / sizeof(sf2_inst_record));
            if (meta->instrument_count > 0) meta->instrument_count -= 1;
            sf2_skip_chunk(f, chunk.size);
        } else if (memcmp(chunk.id, "shdr", 4) == 0) {
            meta->sample_count = (int)(chunk.size / sizeof(sf2_shdr_record));
            if (meta->sample_count > 0) meta->sample_count -= 1;
            sf2_skip_chunk(f, chunk.size);
        } else {
            sf2_skip_chunk(f, chunk.size);
        }
    }
    fseek(f, start + list_size, SEEK_SET);
}

int sf2_read_metadata(const char* path, sf2_metadata* out_meta) {
    FILE* f;
    riff_chunk_header riff;
    char form[4];
    sf2_zero_meta(out_meta);
    if (!path || !out_meta) return 0;

    f = fopen(path, "rb");
    if (!f) return 0;

    if (!sf2_read_bytes(f, &riff, sizeof(riff)) || memcmp(riff.id, "RIFF", 4) != 0) {
        fclose(f);
        return 0;
    }
    if (!sf2_read_bytes(f, form, sizeof(form)) || memcmp(form, "sfbk", 4) != 0) {
        fclose(f);
        return 0;
    }

    while (fread(&riff, sizeof(riff), 1, f) == 1) {
        if (memcmp(riff.id, "LIST", 4) == 0 && riff.size >= 4) {
            char list_type[4];
            uint32_t payload_size = riff.size - 4;
            if (!sf2_read_bytes(f, list_type, sizeof(list_type))) break;
            if (memcmp(list_type, "INFO", 4) == 0) {
                sf2_parse_info_list(f, payload_size, out_meta);
            } else if (memcmp(list_type, "pdta", 4) == 0) {
                sf2_parse_pdta_meta(f, payload_size, out_meta);
            } else {
                sf2_skip_chunk(f, payload_size);
            }
        } else {
            sf2_skip_chunk(f, riff.size);
        }
    }

    fclose(f);
    return 1;
}

static void sf2_file_free(sf2_file* sf2) {
    if (!sf2) return;
    free(sf2->phdr);
    free(sf2->pbag);
    free(sf2->pgen);
    free(sf2->inst);
    free(sf2->ibag);
    free(sf2->igen);
    free(sf2->shdr);
    free(sf2->sample_pool);
    memset(sf2, 0, sizeof(*sf2));
}

static int sf2_read_chunk_array(FILE* f, void** out_data, size_t elem_size, uint32_t chunk_size) {
    void* data;
    if (!out_data || elem_size == 0) return 0;
    data = malloc(chunk_size);
    if (!data) return 0;
    if (chunk_size > 0 && fread(data, 1, chunk_size, f) != chunk_size) {
        free(data);
        return 0;
    }
    *out_data = data;
    if (chunk_size & 1u) fseek(f, 1, SEEK_CUR);
    return 1;
}

static int sf2_parse_sdta_list(FILE* f, uint32_t list_size, sf2_file* sf2) {
    long start = ftell(f);
    while ((uint32_t)(ftell(f) - start) + 8 <= list_size) {
        riff_chunk_header chunk;
        if (!sf2_read_bytes(f, &chunk, sizeof(chunk))) return 0;
        if (memcmp(chunk.id, "smpl", 4) == 0) {
            free(sf2->sample_pool);
            sf2->sample_pool = (short*)malloc(chunk.size);
            if (!sf2->sample_pool) return 0;
            if (fread(sf2->sample_pool, 1, chunk.size, f) != chunk.size) return 0;
            sf2->sample_pool_count = chunk.size / sizeof(short);
            if (chunk.size & 1u) fseek(f, 1, SEEK_CUR);
        } else {
            sf2_skip_chunk(f, chunk.size);
        }
    }
    fseek(f, start + list_size, SEEK_SET);
    return 1;
}

static int sf2_parse_pdta_list(FILE* f, uint32_t list_size, sf2_file* sf2) {
    long start = ftell(f);
    while ((uint32_t)(ftell(f) - start) + 8 <= list_size) {
        riff_chunk_header chunk;
        if (!sf2_read_bytes(f, &chunk, sizeof(chunk))) return 0;
        if (memcmp(chunk.id, "phdr", 4) == 0) {
            free(sf2->phdr);
            sf2->phdr = NULL;
            sf2->phdr_count = chunk.size / sizeof(sf2_phdr_record);
            if (!sf2_read_chunk_array(f, (void**)&sf2->phdr, sizeof(sf2_phdr_record), chunk.size)) return 0;
        } else if (memcmp(chunk.id, "pbag", 4) == 0) {
            free(sf2->pbag);
            sf2->pbag = NULL;
            sf2->pbag_count = chunk.size / sizeof(sf2_bag_record);
            if (!sf2_read_chunk_array(f, (void**)&sf2->pbag, sizeof(sf2_bag_record), chunk.size)) return 0;
        } else if (memcmp(chunk.id, "pgen", 4) == 0) {
            free(sf2->pgen);
            sf2->pgen = NULL;
            sf2->pgen_count = chunk.size / sizeof(sf2_gen_record);
            if (!sf2_read_chunk_array(f, (void**)&sf2->pgen, sizeof(sf2_gen_record), chunk.size)) return 0;
        } else if (memcmp(chunk.id, "inst", 4) == 0) {
            free(sf2->inst);
            sf2->inst = NULL;
            sf2->inst_count = chunk.size / sizeof(sf2_inst_record);
            if (!sf2_read_chunk_array(f, (void**)&sf2->inst, sizeof(sf2_inst_record), chunk.size)) return 0;
        } else if (memcmp(chunk.id, "ibag", 4) == 0) {
            free(sf2->ibag);
            sf2->ibag = NULL;
            sf2->ibag_count = chunk.size / sizeof(sf2_bag_record);
            if (!sf2_read_chunk_array(f, (void**)&sf2->ibag, sizeof(sf2_bag_record), chunk.size)) return 0;
        } else if (memcmp(chunk.id, "igen", 4) == 0) {
            free(sf2->igen);
            sf2->igen = NULL;
            sf2->igen_count = chunk.size / sizeof(sf2_gen_record);
            if (!sf2_read_chunk_array(f, (void**)&sf2->igen, sizeof(sf2_gen_record), chunk.size)) return 0;
        } else if (memcmp(chunk.id, "shdr", 4) == 0) {
            free(sf2->shdr);
            sf2->shdr = NULL;
            sf2->shdr_count = chunk.size / sizeof(sf2_shdr_record);
            if (!sf2_read_chunk_array(f, (void**)&sf2->shdr, sizeof(sf2_shdr_record), chunk.size)) return 0;
        } else {
            sf2_skip_chunk(f, chunk.size);
        }
    }
    fseek(f, start + list_size, SEEK_SET);
    return 1;
}

static int sf2_file_load(const char* path, sf2_file* sf2) {
    FILE* f;
    riff_chunk_header riff;
    char form[4];
    memset(sf2, 0, sizeof(*sf2));
    f = fopen(path, "rb");
    if (!f) return 0;
    if (!sf2_read_bytes(f, &riff, sizeof(riff)) || memcmp(riff.id, "RIFF", 4) != 0) {
        fclose(f);
        return 0;
    }
    if (!sf2_read_bytes(f, form, sizeof(form)) || memcmp(form, "sfbk", 4) != 0) {
        fclose(f);
        return 0;
    }

    while (fread(&riff, sizeof(riff), 1, f) == 1) {
        if (memcmp(riff.id, "LIST", 4) == 0 && riff.size >= 4) {
            char list_type[4];
            uint32_t payload_size = riff.size - 4;
            if (!sf2_read_bytes(f, list_type, sizeof(list_type))) break;
            if (memcmp(list_type, "INFO", 4) == 0) {
                sf2_metadata meta;
                sf2_zero_meta(&meta);
                sf2_parse_info_list(f, payload_size, &meta);
                strncpy(sf2->info.name, meta.name, sizeof(sf2->info.name) - 1);
                sf2->info.preset_count = meta.preset_count;
                sf2->info.instrument_count = meta.instrument_count;
                sf2->info.sample_count = meta.sample_count;
            } else if (memcmp(list_type, "sdta", 4) == 0) {
                if (!sf2_parse_sdta_list(f, payload_size, sf2)) {
                    fclose(f);
                    sf2_file_free(sf2);
                    return 0;
                }
            } else if (memcmp(list_type, "pdta", 4) == 0) {
                if (!sf2_parse_pdta_list(f, payload_size, sf2)) {
                    fclose(f);
                    sf2_file_free(sf2);
                    return 0;
                }
            } else {
                sf2_skip_chunk(f, payload_size);
            }
        } else {
            sf2_skip_chunk(f, riff.size);
        }
    }
    fclose(f);

    if (!sf2->phdr || !sf2->pbag || !sf2->pgen || !sf2->inst || !sf2->ibag || !sf2->igen || !sf2->shdr || !sf2->sample_pool) {
        sf2_file_free(sf2);
        return 0;
    }
    if (sf2->phdr_count < 2 || sf2->inst_count < 2 || sf2->shdr_count < 2) {
        sf2_file_free(sf2);
        return 0;
    }
    return 1;
}

/* Partner of a stereo half only when both headers point at each other with
 * opposite sides (HANDOFF sec. 101); any other link is played as mono. */
static int sf2_stereo_partner(const sf2_file* sf2, int sample_index) {
    const sf2_shdr_record* sh;
    const sf2_shdr_record* other;
    uint16_t type, other_type;
    if (!sf2 || sample_index < 0 || (size_t)sample_index >= sf2->shdr_count - 1) return -1;
    sh = &sf2->shdr[sample_index];
    type = sh->sample_type & 0x7FFFu;
    if (type != SF2_SAMPLE_LEFT && type != SF2_SAMPLE_RIGHT) return -1;
    if (sh->sample_link >= (uint16_t)(sf2->shdr_count - 1) || sh->sample_link == (uint16_t)sample_index) return -1;
    other = &sf2->shdr[sh->sample_link];
    other_type = other->sample_type & 0x7FFFu;
    if (other->sample_link != (uint16_t)sample_index) return -1;
    if (other_type != (type == SF2_SAMPLE_LEFT ? SF2_SAMPLE_RIGHT : SF2_SAMPLE_LEFT)) return -1;
    return (int)sh->sample_link;
}

static wav_data* sf2_build_wav_for_sample(const sf2_file* sf2, int sample_index, wav_data** sample_cache) {
    const sf2_shdr_record* sh;
    uint16_t type;
    int left_index = sample_index;
    int right_index = -1;
    int frames;
    wav_data* wav;
    if (!sf2 || !sample_cache || sample_index < 0 || (size_t)sample_index >= sf2->shdr_count - 1) return NULL;
    if (sample_cache[sample_index]) return sample_cache[sample_index];

    sh = &sf2->shdr[sample_index];
    type = sh->sample_type & 0x7FFFu;
    if (sf2_stereo_partner(sf2, sample_index) >= 0) {
        if (type == SF2_SAMPLE_LEFT) {
            right_index = (int)sh->sample_link;
        } else {
            right_index = sample_index;
            left_index = (int)sh->sample_link;
        }
    }

    if (right_index >= 0 && sample_cache[left_index]) {
        sample_cache[sample_index] = sample_cache[left_index];
        return sample_cache[sample_index];
    }

    if (left_index < 0 || (size_t)left_index >= sf2->shdr_count - 1) return NULL;
    {
        const sf2_shdr_record* left = &sf2->shdr[left_index];
        const sf2_shdr_record* right = (right_index >= 0 && (size_t)right_index < sf2->shdr_count - 1) ? &sf2->shdr[right_index] : NULL;
        uint32_t left_start = left->start;
        uint32_t left_end = left->end;
        uint32_t right_start = right ? right->start : 0;
        uint32_t right_end = right ? right->end : 0;
        if (left_end <= left_start || left_end > sf2->sample_pool_count) return NULL;
        if (right && (right_end <= right_start || right_end > sf2->sample_pool_count)) right = NULL;
        frames = (int)(left_end - left_start);
        if (right) {
            int right_frames = (int)(right_end - right_start);
            if (right_frames < frames) frames = right_frames;
        }
        if (frames <= 0) return NULL;
        wav = (wav_data*)calloc(1, sizeof(wav_data));
        if (!wav) return NULL;
        wav->num_channels = right ? 2 : 1;
        wav->sample_rate = (int)left->sample_rate;
        wav->num_samples = frames;
        wav->root_key = sf2_sample_root_key_from_header(left);
        wav->root_key_fraction = (float)left->pitch_correction * 0.01f;
        wav->has_loop = (left->end_loop > left->start_loop);
        wav->loop_start = wav->has_loop ? (int)(left->start_loop - left->start) : 0;
        wav->loop_end = wav->has_loop ? (int)(left->end_loop - left->start) : 0;
        wav->data = (short*)malloc(sizeof(short) * (size_t)frames * (size_t)wav->num_channels);
        if (!wav->data) {
            free(wav);
            return NULL;
        }
        for (int i = 0; i < frames; ++i) {
            wav->data[i * wav->num_channels] = sf2->sample_pool[left_start + (uint32_t)i];
            if (wav->num_channels == 2) {
                wav->data[i * 2 + 1] = sf2->sample_pool[right_start + (uint32_t)i];
            }
        }
        if (wav->loop_end > wav->num_samples) wav->loop_end = wav->num_samples;
        if (wav->loop_start >= wav->loop_end) wav->has_loop = 0;
        sample_cache[left_index] = wav;
        if (right_index >= 0) sample_cache[right_index] = wav;
        sample_cache[sample_index] = wav;
        return wav;
    }
}

static void sf2_trim_name(const char* src, char* dst, size_t dst_size) {
    size_t len;
    if (!dst || dst_size == 0) return;
    memset(dst, 0, dst_size);
    if (!src) return;
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
    len = strlen(dst);
    while (len > 0 && (dst[len - 1] == ' ' || dst[len - 1] == '\0')) {
        dst[--len] = '\0';
    }
}

static int sf2_zone_range_start(const sf2_bag_record* bags, size_t bag_count, size_t idx) {
    if (!bags || idx >= bag_count) return -1;
    return (int)bags[idx].gen_index;
}

static int sf2_zone_range_end(const sf2_bag_record* bags, size_t bag_count, size_t idx, size_t gen_count) {
    if (!bags || idx >= bag_count) return -1;
    if (idx + 1 < bag_count) return (int)bags[idx + 1].gen_index;
    return (int)gen_count;
}

static int sf2_region_from_zone(sfz_instrument* inst,
                                const sf2_file* sf2,
                                const sf2_zone* preset_zone,
                                const sf2_zone* inst_zone,
                                wav_data** sample_cache,
                                int preset_bank,
                                int preset_program) {
    sf2_zone merged;
    sfz_region* region;
    const sf2_shdr_record* sh;
    char cache_key[512];
    wav_data* sample;
    uint16_t sample_type;
    int linked_stereo = 0;
    int stereo_partner;
    if (!inst || !sf2 || !inst_zone || !sf2_zone_has_sample(inst_zone)) return 0;

    sf2_zone_init(&merged);
    if (preset_zone) sf2_zone_merge(&merged, preset_zone);
    sf2_zone_merge(&merged, inst_zone);
    if (merged.lokey < 0) merged.lokey = 0;
    if (merged.hikey > 127) merged.hikey = 127;
    if (merged.lovel < 0) merged.lovel = 0;
    if (merged.hivel > 127) merged.hivel = 127;
    if (merged.lokey > merged.hikey || merged.lovel > merged.hivel) return 0;
    if (!merged.set_sample_id || merged.sample_id < 0 || (size_t)merged.sample_id >= sf2->shdr_count - 1) return 0;

    sh = &sf2->shdr[merged.sample_id];
    sample_type = sh->sample_type & 0x7FFFu;
    stereo_partner = sf2_stereo_partner(sf2, merged.sample_id);
    if (sample_type == SF2_SAMPLE_RIGHT && stereo_partner >= 0) {
        return 0;
    }

    sample = sf2_build_wav_for_sample(sf2, merged.sample_id, sample_cache);
    if (!sample) return 0;
    linked_stereo = (sample->num_channels == 2 && stereo_partner >= 0);

    region = sfz_append_region(inst);
    if (!region) return 0;
    sf2_trim_name(sh->name, region->sample, sizeof(region->sample));
    region->midi_bank = preset_bank;
    region->midi_program = preset_program;
    if (merged.key_override >= 0) {
        region->key_override = merged.key_override;
        region->lokey = merged.lokey;
        region->hikey = merged.hikey;
    } else {
        region->lokey = merged.lokey;
        region->hikey = merged.hikey;
    }
    if (merged.vel_override >= 0) {
        region->vel_override = merged.vel_override;
        region->lovel = merged.lovel;
        region->hivel = merged.hivel;
    } else {
        region->lovel = merged.lovel;
        region->hivel = merged.hivel;
    }
    region->pitch_keycenter = merged.set_root_key
        ? ((merged.root_key < 0) ? 0 : ((merged.root_key > 127) ? 127 : merged.root_key))
        : sf2_sample_root_key_from_header(sh);
    region->pitch_keytrack = (float)merged.scale_tuning * 0.01f;
    region->transpose = merged.coarse_tune;
    region->tune_cents = merged.fine_tune;
    region->pan = linked_stereo ? 0 : merged.pan;
    region->attenuation = merged.attenuation_cb;
    region->group = merged.exclusive_class;
    region->off_by = merged.exclusive_class;
    region->loop_mode = merged.set_sample_modes && (merged.sample_modes != 0) ? 2 : 0;
    region->offset = (int)sh->start + merged.start_offset;
    region->end = (int)sh->end + merged.end_offset;
    region->loop_start = (int)sh->start_loop + merged.loop_start_offset;
    region->loop_end = (int)sh->end_loop + merged.loop_end_offset;
    region->ampeg_attack = merged.set_attack ? merged.attack_seconds : sf2_timecents_to_seconds(-12000);
    region->ampeg_decay = merged.set_decay ? merged.decay_seconds : sf2_timecents_to_seconds(-12000);
    region->ampeg_sustain = merged.set_sustain ? merged.sustain_level : 1.0f;
    region->ampeg_release = merged.set_release ? merged.release_seconds : sf2_timecents_to_seconds(-12000);
    if (merged.set_cutoff) {
        region->filter_type = 1;
        region->cutoff_hz = sf2_abs_cents_to_hz(merged.cutoff_cents);
    }
    if (merged.set_resonance) {
        region->resonance_q = 0.707f + ((float)merged.resonance_cents * 0.01f);
    }
    snprintf(cache_key, sizeof(cache_key), "%s#sf2sample:%d", inst->source_path, merged.sample_id);
    if (!sfz_region_attach_sample_owned(inst, region, cache_key, sample)) {
        return 0;
    }
    region->offset -= (int)sh->start;
    region->end -= (int)sh->start;
    region->loop_start -= (int)sh->start;
    region->loop_end -= (int)sh->start;
    sfz_region_finalize(region);
    return 1;
}

sfz_instrument* sf2_load_as_instrument(const char* path) {
    sf2_file sf2;
    sfz_instrument* inst = NULL;
    wav_data** sample_cache = NULL;
    int loaded_regions = 0;
    if (!path) {
        ShowErrorWithLocation(BADINPUT, __LINE__, __FILE__);
        return NULL;
    }
    if (!sf2_file_load(path, &sf2)) {
        ShowErrorWithMessage(SF2LOADFAIL, path);
        return NULL;
    }

    inst = sfz_create_instrument(path, SOUND_FONT_FORMAT_SF2);
    if (!inst) {
        sf2_file_free(&sf2);
        ShowError(OUTOFMEMORY);
        return NULL;
    }

    sample_cache = (wav_data**)calloc(sf2.shdr_count, sizeof(wav_data*));
    if (!sample_cache) {
        sf2_file_free(&sf2);
        sfz_free(inst);
        ShowError(OUTOFMEMORY);
        return NULL;
    }

    if (sf2.phdr_count < 2 || sf2.inst_count < 2 || sf2.shdr_count < 2) {
        free(sample_cache);
        sf2_file_free(&sf2);
        sfz_free(inst);
        ShowErrorWithMessage(SF2LOADFAIL, path);
        return NULL;
    }

    if (sf2.info.name[0]) {
        strncpy(inst->default_path, sf2.info.name, sizeof(inst->default_path) - 1);
    }

    for (size_t preset_idx = 0; preset_idx + 1 < sf2.phdr_count; ++preset_idx) {
        int preset_program = (int)(sf2.phdr[preset_idx].preset & 0x7Fu);
        int preset_bank = (int)sf2.phdr[preset_idx].bank;
        size_t pbag_start = sf2.phdr[preset_idx].bag_index;
        size_t pbag_end = sf2.phdr[preset_idx + 1].bag_index;
        sf2_zone preset_global;
        int have_preset_global = 0;
        sf2_zone_init(&preset_global);

        for (size_t pz = pbag_start; pz < pbag_end; ++pz) {
            int gen_start = sf2_zone_range_start(sf2.pbag, sf2.pbag_count, pz);
            int gen_end = sf2_zone_range_end(sf2.pbag, sf2.pbag_count, pz, sf2.pgen_count);
            sf2_zone preset_zone;
            if (gen_start < 0 || gen_end < gen_start || (size_t)gen_end > sf2.pgen_count) {
                ShowErrorWithLocation(HOWTHEFUCK, __LINE__, __FILE__);
                continue;
            }
            sf2_read_zone_range(sf2.pgen, (size_t)gen_start, (size_t)gen_end, &preset_zone);
            if (!sf2_zone_has_instrument(&preset_zone)) {
                preset_global = preset_zone;
                have_preset_global = 1;
                continue;
            }
            if (preset_zone.instrument < 0 || (size_t)preset_zone.instrument >= sf2.inst_count - 1) {
                continue;
            }

            {
                size_t ibag_start = sf2.inst[preset_zone.instrument].bag_index;
                size_t ibag_end = sf2.inst[preset_zone.instrument + 1].bag_index;
                sf2_zone inst_global;
                int have_inst_global = 0;
                sf2_zone_init(&inst_global);
                for (size_t iz = ibag_start; iz < ibag_end; ++iz) {
                    int igen_start = sf2_zone_range_start(sf2.ibag, sf2.ibag_count, iz);
                    int igen_end = sf2_zone_range_end(sf2.ibag, sf2.ibag_count, iz, sf2.igen_count);
                    sf2_zone inst_zone;
                    sf2_zone merged_preset;
                    if (igen_start < 0 || igen_end < igen_start || (size_t)igen_end > sf2.igen_count) {
                        ShowErrorWithLocation(HOWTHEFUCK, __LINE__, __FILE__);
                        continue;
                    }
                    sf2_read_zone_range(sf2.igen, (size_t)igen_start, (size_t)igen_end, &inst_zone);
                    if (!sf2_zone_has_sample(&inst_zone)) {
                        inst_global = inst_zone;
                        have_inst_global = 1;
                        continue;
                    }
                    sf2_zone_init(&merged_preset);
                    if (have_preset_global) sf2_zone_merge(&merged_preset, &preset_global);
                    sf2_zone_merge(&merged_preset, &preset_zone);
                    if (have_inst_global) {
                        sf2_zone tmp = merged_preset;
                        sf2_zone_merge(&tmp, &inst_global);
                        merged_preset = tmp;
                    }
                    if (sf2_region_from_zone(inst, &sf2, &merged_preset, &inst_zone, sample_cache,
                                             preset_bank, preset_program)) {
                        ++loaded_regions;
                    }
                }
            }
        }
    }

    free(sample_cache);
    sf2_file_free(&sf2);

    if (loaded_regions <= 0 || inst->num_regions <= 0) {
        sfz_free(inst);
        ShowErrorWithMessage(SF2LOADFAIL, path);
        return NULL;
    }

    logger_log("SF2 loaded: %d regions from %s\n", inst->num_regions, path);
    return inst;
}
