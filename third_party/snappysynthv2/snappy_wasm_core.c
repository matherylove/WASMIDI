#include "buildconfig.h"
#include "Voice/voice.h"
#include "Parser/sf2_parser.h"
#include "Parser/sfz_parser.h"

#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

sfz_instrument* instrument = NULL;

static AudioConfig g_cfg = {44100, 2, 32, 512, 16, 1};
static float* g_out = NULL;
static int g_out_capacity_frames = 0;
static int64_t g_render_cursor = 1;
/* Adaptive per-block voice_render_float() budget; see ssw_render_queued_into().
 * 1 == exactly the native engine's behaviour (state applied at block start). */
static int    g_render_budget = 1;
/* rev. 45 (HANDOFF §29): keep block render time under this % of real time; 0 = off. */
static int    g_render_limit_percent = 100;
static double g_render_limit_ema_us = 0.0;
#define SSW_LIMIT_MAX_WORKERS 256
static void ssw_render_limit_clear(void) {
    for (int w = 0; w < SSW_LIMIT_MAX_WORKERS; ++w)
        if (voice_get_admit_key_cap(w) > 0) voice_set_admit_key_cap(w, 0);
    if (voice_get_admit_budget_us() > 0) voice_set_admit_budget_us(0);
}
static double g_render_load_ema = -1.0;
/* Cost of the most recent block, in microseconds of wall clock. Together with
 * the load EMA this separates "out of CPU" from "out of data": both produce
 * underruns and they need opposite fixes. */
static double g_last_render_us = 0.0;
/*
 * Split of that cost. ssw_render_queued_into() does two very different jobs:
 * it dispatches every event in the block, and it renders voices. Only the
 * second is spread across the worker pool; dispatch runs alone on this thread.
 * Timing them together cannot explain why adding workers stops helping, so
 * they are measured apart.
 */
static double g_last_dispatch_us = 0.0;
/* Controller events dropped because a later value in the same dispatch run
 * supersedes them. Cumulative. */
static int g_controllers_collapsed = 0;
static double g_dispatch_accum_us = 0.0;
static int g_ready = 0;
static int g_max_voices = 16384;
static int g_min_voices = 0;
static int g_soundfont_layers = 0;

/*
 * Browser event admission queue. The native SnappySynth hot path receives
 * already-scheduled events before voice_render_float(); doing the equivalent
 * classification in JavaScript once per event was one of the largest WASM
 * overheads on Black MIDI. Batches stay as independent C blocks so a several-
 * million-event look-ahead never reallocates/copies one giant queue.
 */
typedef struct {
    int64_t sample_frame;
    uint32_t message;
    /* Index of the next non-note event for this MIDI channel inside the same
     * producer slab, or -1.  Built once when the slab is admitted so the
     * controller-adjacency test in the realtime render loop is O(1) instead of
     * repeatedly scanning through thousands of intervening note events. */
    int32_t next_channel_index;
} ssw_scheduled_event;

typedef struct ssw_event_block {
    struct ssw_event_block* next;
    size_t index;
    size_t count;
    size_t capacity;
    ssw_scheduled_event events[];
} ssw_event_block;

static ssw_event_block* g_event_head = NULL;
static ssw_event_block* g_event_tail = NULL;
static ssw_event_block* g_event_free = NULL;
static int g_event_free_count = 0;
static double g_song_time_seconds = 0.0;
static int64_t g_song_frame = 0;

/* Keep a small set of the largest schedule slabs alive. Browser playback feeds
 * the synth in large fixed-size batches, so malloc/free on every producer
 * message needlessly re-enters the shared WASM allocator and can trigger a
 * memory.grow synchronization across all pthreads. Reusing the exact same slab
 * changes no event ordering or timing; it only moves allocation out of the hot
 * producer/render loop. */
#ifndef SSW_EVENT_BLOCK_POOL_MAX
#define SSW_EVENT_BLOCK_POOL_MAX 8
#endif

/* Roll back both event-frontier optimizations with one compile define:
 *   -DSSW_REV38_EVENT_HOTPATH=0
 * This intentionally leaves soundfont correctness/materialization fixes and
 * Debug-metric gating enabled; those are independent bug fixes. */
#ifndef SSW_REV38_EVENT_HOTPATH
#define SSW_REV38_EVENT_HOTPATH 1
#endif

static ssw_event_block* ssw_event_block_acquire(size_t capacity) {
#if SSW_REV38_EVENT_HOTPATH
    ssw_event_block** link = &g_event_free;
    ssw_event_block* best = NULL;
    ssw_event_block** best_link = NULL;

    while (*link) {
        ssw_event_block* block = *link;
        if (block->capacity >= capacity &&
            (!best || block->capacity < best->capacity)) {
            best = block;
            best_link = link;
            if (block->capacity == capacity) break;
        }
        link = &block->next;
    }

    if (best) {
        *best_link = best->next;
        --g_event_free_count;
        best->next = NULL;
        best->index = 0;
        best->count = 0;
        return best;
    }
#endif

    if (capacity > (SIZE_MAX - sizeof(ssw_event_block)) /
                       sizeof(ssw_scheduled_event)) {
        return NULL;
    }
    const size_t bytes = sizeof(ssw_event_block) +
        capacity * sizeof(ssw_scheduled_event);
    ssw_event_block* block = (ssw_event_block*)malloc(bytes);
    if (!block) return NULL;
    block->next = NULL;
    block->index = 0;
    block->count = 0;
    block->capacity = capacity;
    return block;
}

static void ssw_event_block_release(ssw_event_block* block) {
    if (!block) return;
#if !SSW_REV38_EVENT_HOTPATH
    free(block);
    return;
#else
    block->index = 0;
    block->count = 0;

    if (g_event_free_count >= SSW_EVENT_BLOCK_POOL_MAX) {
        /* The producer normally uses a stable large batch size, but seeks and
         * tail batches can be much smaller. Keep the largest slabs so a run of
         * tiny batches cannot evict the allocation that matters on the next
         * dense passage. */
        ssw_event_block** smallest_link = &g_event_free;
        ssw_event_block** link = &g_event_free;
        size_t smallest_capacity = SIZE_MAX;
        while (*link) {
            if ((*link)->capacity < smallest_capacity) {
                smallest_capacity = (*link)->capacity;
                smallest_link = link;
            }
            link = &(*link)->next;
        }
        if (block->capacity <= smallest_capacity) {
            free(block);
            return;
        }
        ssw_event_block* evicted = *smallest_link;
        *smallest_link = evicted->next;
        free(evicted);
        /* Count remains at the pool maximum; insert the larger replacement. */
        block->next = g_event_free;
        g_event_free = block;
        return;
    }

    block->next = g_event_free;
    g_event_free = block;
    ++g_event_free_count;
#endif
}

static void ssw_free_event_block_pool(void) {
    ssw_event_block* block = g_event_free;
    while (block) {
        ssw_event_block* next = block->next;
        free(block);
        block = next;
    }
    g_event_free = NULL;
    g_event_free_count = 0;
}

/* llround() is surprisingly expensive in WASM. All scheduler times are
 * non-negative after clamping, so +0.5 followed by an integer conversion is
 * exactly the same rounding rule as llround() for every valid input we admit. */
static inline int64_t ssw_seconds_to_frame(double seconds) {
    const int rate = g_cfg.sample_rate > 0 ? g_cfg.sample_rate : 44100;
    double scaled;
    if (!isfinite(seconds) || seconds <= 0.0) return 0;
    scaled = seconds * (double)rate;
    if (!isfinite(scaled) || scaled >= (double)INT64_MAX - 0.5)
        return INT64_MAX;
#if SSW_REV38_EVENT_HOTPATH
    return (int64_t)(scaled + 0.5);
#else
    return (int64_t)llround(scaled);
#endif
}

void ssw_clear_events(void) {
    ssw_event_block* block = g_event_head;
    while (block) {
        ssw_event_block* next = block->next;
        ssw_event_block_release(block);
        block = next;
    }
    g_event_head = NULL;
    g_event_tail = NULL;
}

void ssw_set_song_time(double seconds) {
    if (!isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    g_song_time_seconds = seconds;
    g_song_frame = ssw_seconds_to_frame(seconds);
}

int ssw_queue_events(const uint32_t* messages, const double* times, int count) {
    if (!messages || !times || count <= 0) return count == 0 ? 1 : 0;

    const size_t incoming = (size_t)count;
    ssw_event_block* block = ssw_event_block_acquire(incoming);
    if (!block) return 0;
    block->next = NULL;
    block->index = 0;
    block->count = incoming;

    for (size_t i = 0; i < incoming; ++i) {
        double t = times[i];
        if (!isfinite(t) || t < 0.0) t = 0.0;
        block->events[i].sample_frame = ssw_seconds_to_frame(t);
        block->events[i].message = messages[i];
        block->events[i].next_channel_index = -1;
    }

    /*
     * Build the channel-event adjacency links once per producer batch.
     *
     * enqueue_event() routes notes and channel state into different worker
     * queues, so notes do not separate two controllers for the native queue's
     * coalescing rule.  The old browser-side reproduction rediscovered the
     * next channel event by scanning forward for every CC.  On Black MIDI that
     * can revisit the same note run millions of times.  A reverse pass gives
     * the exact same next-event relation with linear work and no allocation.
     *
     * We intentionally do not link across ssw_event_block boundaries: the old
     * implementation did not scan across them either, so this is a pure
     * implementation optimization with identical collapse decisions.
     */
    {
        int32_t next_non_note[16];
        for (int ch = 0; ch < 16; ++ch) next_non_note[ch] = -1;
        for (size_t rev = incoming; rev-- > 0;) {
            ssw_scheduled_event* event = &block->events[rev];
            const uint32_t message = event->message;
            const uint32_t command = message & 0xf0u;
            const uint32_t channel = message & 0x0fu;
            event->next_channel_index = next_non_note[channel];
            if (command != 0x90u && command != 0x80u)
                next_non_note[channel] = (int32_t)rev;
        }
    }

    if (g_event_tail) g_event_tail->next = block;
    else g_event_head = block;
    g_event_tail = block;
    return 1;
}

/* Source-exposed runtime/performance settings. */
static int g_requested_workers = 0;       /* 0 = original auto policy */
static int g_note_sharding = 0;           /* 0 auto, 1 channel, 2 hash */
static int g_steal_score_cache = 1;       /* SS_STEAL_SCORE_CACHE default */
static int g_fast_note_off = 1;           /* SS_FAST_NOTE_OFF default */
static int g_validate_state = 0;           /* SS_VALIDATE_STATE default off */
static int g_soft_clip = 1;                /* realtime source default on */

static void ssw_set_env_int(const char* name, int enabled, int value) {
    char buffer[32];
    if (!name || !*name) return;
    if (!enabled) {
        unsetenv(name);
        return;
    }
    snprintf(buffer, sizeof(buffer), "%d", value);
    setenv(name, buffer, 1);
}

static void ssw_apply_source_runtime_options(void) {
    ssw_set_env_int("SS_WORKERS", g_requested_workers > 0, g_requested_workers);

    if (g_note_sharding == 1) setenv("SS_NOTE_SHARDING", "channel", 1);
    else if (g_note_sharding == 2) setenv("SS_NOTE_SHARDING", "hash", 1);
    else unsetenv("SS_NOTE_SHARDING");

    setenv("SS_STEAL_SCORE_CACHE", g_steal_score_cache ? "1" : "0", 1);
    setenv("SS_FAST_NOTE_OFF", g_fast_note_off ? "1" : "0", 1);

    if (g_validate_state) setenv("SS_VALIDATE_STATE", "1", 1);
    else unsetenv("SS_VALIDATE_STATE");
}

/* ---- Exact SnappySynthV2 soundfont stacking policy ---- */
static int region_selector_is_wildcard(const sfz_region* region) {
    return region && region->midi_bank < 0 && region->midi_program < 0;
}

static int region_selector_matches(const sfz_region* a, const sfz_region* b) {
    if (!a || !b) return 0;
    if (region_selector_is_wildcard(a) || region_selector_is_wildcard(b))
        return region_selector_is_wildcard(a) && region_selector_is_wildcard(b);
    return a->midi_bank == b->midi_bank && a->midi_program == b->midi_program;
}

static int region_is_melodic_program(const sfz_region* region) {
    if (!region || region->midi_is_drum > 0) return 0;
    return region->midi_program >= 0;
}

static int region_is_default_melodic_program(const sfz_region* region) {
    if (!region_is_melodic_program(region)) return 0;
    if (region->midi_program != 0) return 0;
    return region->midi_bank < 0 || region->midi_bank == 0;
}

static int region_keyvel_overlaps(const sfz_region* a, const sfz_region* b) {
    if (!a || !b) return 0;
    if (a->hikey < b->lokey || b->hikey < a->lokey) return 0;
    if (a->hivel < b->lovel || b->hivel < a->lovel) return 0;
    return 1;
}

static int region_is_overridden_by_incoming(const sfz_region* existing,
                                             const sfz_instrument* incoming) {
    if (!existing || !incoming) return 0;
    for (int i = 0; i < incoming->num_regions; ++i) {
        const sfz_region* r = &incoming->regions[i];
        if (region_selector_matches(existing, r)) return 1;
        if (incoming->format == SOUND_FONT_FORMAT_SFZ &&
            region_selector_is_wildcard(r) &&
            region_is_default_melodic_program(existing) &&
            region_keyvel_overlaps(existing, r)) return 1;
    }
    return 0;
}

static void remove_overridden_regions(sfz_instrument* existing,
                                      const sfz_instrument* incoming) {
    int write_index = 0;
    if (!existing || !incoming || existing->num_regions <= 0 || incoming->num_regions <= 0)
        return;
    for (int i = 0; i < existing->num_regions; ++i) {
        sfz_region* region = &existing->regions[i];
        if (region_is_overridden_by_incoming(region, incoming)) {
            free(region->opcodes);
            region->opcodes = NULL;
            region->opcode_count = 0;
            region->opcode_cap = 0;
            continue;
        }
        if (write_index != i) existing->regions[write_index] = existing->regions[i];
        ++write_index;
    }
    existing->num_regions = write_index;
    sfz_invalidate_region_cache(existing);
}

static int append_instrument_regions(sfz_instrument* existing, sfz_instrument* incoming) {
    if (!existing || !incoming || incoming->num_regions <= 0) return 1;
    int old_count = existing->num_regions;
    int total = old_count + incoming->num_regions;
    sfz_region* regions = (sfz_region*)realloc(existing->regions,
                                                (size_t)total * sizeof(sfz_region));
    if (!regions) return 0;
    existing->regions = regions;
    memcpy(existing->regions + old_count, incoming->regions,
           (size_t)incoming->num_regions * sizeof(sfz_region));
    existing->num_regions = total;
    sfz_invalidate_region_cache(existing);
    return 1;
}

/* A soundfont is not "ready" merely because the parser produced regions.
 * Every region must already own decoded PCM and, when its source rate differs
 * from the synth rate, a target-rate buffer. Otherwise the first notes silently
 * fall back to the expensive interpolating path and loading work leaks into
 * realtime playback. */
static int ssw_instrument_is_materialized(const sfz_instrument* inst,
                                          int target_sample_rate) {
    if (!inst || inst->num_regions <= 0 || target_sample_rate <= 0)
        return 0;

    for (int i = 0; i < inst->num_regions; ++i) {
        const sfz_region* region = &inst->regions[i];
        const wav_data* source = region->sample_data;
        if (!region->cache_entry || !source || !source->data ||
            source->num_samples <= 0 || source->num_channels <= 0 ||
            source->sample_rate <= 0) {
            return 0;
        }

        if (source->sample_rate != target_sample_rate) {
            const wav_data* prepared = region->resampled_data;
            if (!region->is_resampled || !prepared || prepared == source ||
                !prepared->data || prepared->num_samples <= 0 ||
                prepared->sample_rate != target_sample_rate) {
                return 0;
            }
        }
    }
    return 1;
}

/* ---- Exact SnappySynthV2 GS/XG/GM short-message routing and SysEx state ---- */
static volatile LONG g_gs_part_map_active = 0;
static int g_gs_part_to_channel[16] = { 9,0,1,2,3,4,5,6,7,8,10,11,12,13,14,15 };
static int g_gs_channel_part_count[16] = {0};
static int g_gs_channel_parts[16][16] = {{0}};

static void gs_rebuild_channel_parts(void) {
    for (int ch = 0; ch < 16; ++ch) g_gs_channel_part_count[ch] = 0;
    for (int part = 0; part < 16; ++part) {
        int ch = g_gs_part_to_channel[part];
        if (ch < 0 || ch >= 16) continue;
        int n = g_gs_channel_part_count[ch];
        if (n < 16) g_gs_channel_parts[ch][n] = part;
        if (n < 16) g_gs_channel_part_count[ch] = n + 1;
    }
}

static void gs_reset_part_map(int active) {
    static const int defaults[16] = { 9,0,1,2,3,4,5,6,7,8,10,11,12,13,14,15 };
    for (int i = 0; i < 16; ++i) g_gs_part_to_channel[i] = defaults[i];
    gs_rebuild_channel_parts();
    voice_set_gs_part_mode(active);
    InterlockedExchange(&g_gs_part_map_active, active ? 1 : 0);
}

static int gs_part_addr_to_part_index(int part_addr) { return part_addr & 0x0F; }

static void dispatch_midi_part_event_at_qpc(int part, int cmd, int b1, int b2,
                                             int stack_count, int64_t timestamp_qpc) {
    if ((unsigned int)part >= 16) return;
    switch (cmd) {
        case 0x90:
            if (b2 > 0) voice_send_note_event_at(part, b1, b2, 1, stack_count, timestamp_qpc);
            else voice_send_note_event_at(part, b1, 0, 0, 1, timestamp_qpc);
            break;
        case 0x80: voice_send_note_event_at(part, b1, 0, 0, 1, timestamp_qpc); break;
        case 0xB0: voice_control_change_at(part, b1, b2, timestamp_qpc); break;
        case 0xC0: voice_program_change_at(part, b1, timestamp_qpc); break;
        case 0xE0: voice_pitch_bend_at(part, b1, b2, timestamp_qpc); break;
        default: break;
    }
}

static void sysex_normalize_payload(const unsigned char** data, int* length) {
    if (!data || !length || !*data || *length <= 0) return;
    if ((*data)[0] == 0xF0) { ++(*data); --(*length); }
    if (*length > 0 && (*data)[*length - 1] == 0xF7) --(*length);
}

static int sysex_matches_gm_reset(const unsigned char* data, int length) {
    if (!data || length < 4) return 0;
    sysex_normalize_payload(&data, &length);
    return length >= 4 && data[0] == 0x7E && data[2] == 0x09 &&
           (data[3] == 0x01 || data[3] == 0x03);
}

static int sysex_matches_gs_reset(const unsigned char* data, int length) {
    if (!data || length < 9) return 0;
    sysex_normalize_payload(&data, &length);
    return length >= 9 && data[0] == 0x41 && data[2] == 0x42 && data[3] == 0x12 &&
           data[4] == 0x40 && data[5] == 0x00 && data[6] == 0x7F && data[7] == 0x00;
}

static int sysex_matches_xg_reset(const unsigned char* data, int length) {
    if (!data || length < 7) return 0;
    sysex_normalize_payload(&data, &length);
    return length >= 7 && data[0] == 0x43 && data[2] == 0x4C && data[3] == 0x00 &&
           data[4] == 0x00 && data[5] == 0x7E && data[6] == 0x00;
}

static int sysex_get_universal_master_volume_14bit(const unsigned char* data, int length,
                                                    int* out_value14) {
    if (!data || !out_value14 || length < 6) return 0;
    sysex_normalize_payload(&data, &length);
    if (length < 6 || data[0] != 0x7F || data[2] != 0x04 || data[3] != 0x01) return 0;
    *out_value14 = (((int)data[5] & 0x7F) << 7) | ((int)data[4] & 0x7F);
    return 1;
}

static int sysex_get_gs_drum_part(const unsigned char* data, int length,
                                  int* out_part, int* out_mode) {
    int addr;
    if (!data || !out_part || !out_mode || length < 9) return 0;
    sysex_normalize_payload(&data, &length);
    if (length < 9 || data[0] != 0x41 || data[2] != 0x42 || data[3] != 0x12 ||
        data[4] != 0x40 || data[6] != 0x15) return 0;
    addr = (int)data[5] & 0x7F;
    if ((addr & 0x70) != 0x10) return 0;
    *out_part = gs_part_addr_to_part_index(addr);
    if (*out_part < 0 || *out_part >= 16) return 0;
    *out_mode = (int)data[7] & 0x7F;
    return 1;
}

static int sysex_get_gs_receive_channel(const unsigned char* data, int length,
                                        int* out_part, int* out_channel) {
    int addr;
    if (!data || !out_part || !out_channel || length < 9) return 0;
    sysex_normalize_payload(&data, &length);
    if (length < 9 || data[0] != 0x41 || data[2] != 0x42 || data[3] != 0x12 ||
        data[4] != 0x40 || data[6] != 0x02) return 0;
    addr = (int)data[5] & 0x7F;
    if ((addr & 0x70) != 0x10) return 0;
    *out_part = gs_part_addr_to_part_index(addr);
    if (data[7] <= 0x0F) *out_channel = (int)data[7];
    else if (data[7] == 0x10) *out_channel = -1;
    else return 0;
    return *out_part >= 0 && *out_part < 16;
}

static int sysex_apply_gs_scale_tuning(const unsigned char* data, int length,
                                       int64_t timestamp_qpc) {
    int addr, part, count;
    if (!data || length < 10) return 0;
    sysex_normalize_payload(&data, &length);
    if (length < 10 || data[0] != 0x41 || data[2] != 0x42 || data[3] != 0x12 ||
        data[4] != 0x40 || data[6] != 0x40) return 0;
    addr = (int)data[5] & 0x7F;
    if ((addr & 0x70) != 0x10) return 0;
    part = gs_part_addr_to_part_index(addr);
    if (part < 0 || part >= 16) return 0;
    count = length - 8;
    if (count > 12) count = 12;
    for (int i = 0; i < count; ++i)
        voice_set_scale_tune_at(part, i, ((int)data[7 + i] & 0x7F) - 64, timestamp_qpc);
    return 1;
}

static void apply_gm_reset_at_qpc(int64_t timestamp_qpc) {
    for (int ch = 0; ch < 16; ++ch) {
        voice_control_change_at(ch, 120, 0, timestamp_qpc);
        voice_control_change_at(ch, 121, 0, timestamp_qpc);
        voice_control_change_at(ch, 123, 0, timestamp_qpc);
        voice_control_change_at(ch, 0, 0, timestamp_qpc);
        voice_control_change_at(ch, 32, 0, timestamp_qpc);
        voice_program_change_at(ch, 0, timestamp_qpc);
        voice_pitch_bend_at(ch, 0, 64, timestamp_qpc);
        voice_set_drum_part_at(ch, 0, timestamp_qpc);
    }
    voice_set_master_volume_14bit_at(0x3FFF, timestamp_qpc);
}

static void dispatch_sysex_data_at_qpc(const unsigned char* data, int length,
                                       int64_t timestamp_qpc) {
    int master, part, mode, recv_part, recv_channel;
    if (!data || length <= 0) return;
    if (sysex_matches_gm_reset(data, length)) {
        gs_reset_part_map(0); apply_gm_reset_at_qpc(timestamp_qpc); return;
    }
    if (sysex_matches_gs_reset(data, length)) {
        gs_reset_part_map(1); apply_gm_reset_at_qpc(timestamp_qpc);
        voice_set_drum_part_at(0, 1, timestamp_qpc); return;
    }
    if (sysex_matches_xg_reset(data, length)) {
        gs_reset_part_map(0); apply_gm_reset_at_qpc(timestamp_qpc); return;
    }
    if (sysex_get_universal_master_volume_14bit(data, length, &master)) {
        voice_set_master_volume_14bit_at(master, timestamp_qpc); return;
    }
    if (sysex_apply_gs_scale_tuning(data, length, timestamp_qpc)) return;
    if (sysex_get_gs_receive_channel(data, length, &recv_part, &recv_channel)) {
        g_gs_part_to_channel[recv_part] = recv_channel;
        gs_rebuild_channel_parts();
        InterlockedExchange(&g_gs_part_map_active, 1);
        return;
    }
    if (sysex_get_gs_drum_part(data, length, &part, &mode)) {
        voice_set_drum_part_at(part, mode, timestamp_qpc);
        return;
    }
}

/*
 * Controller admission, matching what the native engine actually receives.
 *
 * voice.c is byte-for-byte identical to the original here: same CC coalescing
 * in enqueue_event(), same vor_break_channel_sequence() on every non-note
 * event. Identical code on an identical stream cannot behave differently, so
 * the stall on controller-dense material has to come from the stream itself.
 *
 * The native engine is fed one event at a time as it arrives, and its render
 * thread drains the queue continuously in between, so a burst of controller
 * events on one channel collapses against the previous queued event long before
 * a block's worth has accumulated. This port schedules ahead and hands the
 * engine every event of a block in one go, so that collapse never gets the
 * chance to happen and the engine sees the full, uncollapsed burst.
 *
 * This pass restores the aggregate the native engine would have seen: within
 * one dispatch run, a controller value that is immediately superseded by
 * another value of the same channel and controller, with no note in between on
 * that channel, is dropped. The last value still lands, at its own timestamp.
 * The engine is untouched.
 *
 * The exclusions match enqueue_event()'s own list exactly: controllers where a
 * repeat is a second real action rather than a newer value.
 */
/*
 * Reproduces the coalescing the native engine performs in its own event queue.
 *
 * voice.c is byte-for-byte identical to the original, so identical code on an
 * identical stream cannot behave differently. The difference is the stream: the
 * native engine is fed one event at a time and its queue absorbs a controller
 * into the next one of the same channel and number, while this port schedules
 * ahead and hands over a whole block at once, so that absorption never happens
 * and the engine sees every step of every burst.
 *
 * History worth keeping: a first version collapsed by SUPERSESSION (drop any
 * value a later one overrides, stopping at notes). It cut ALLOC from ~32x BUSY
 * to 8x, but automation came out stepped and the user reported controller
 * response as clearly worse than the original -- it was throwing away the
 * intermediate steps of ramps. The rule is adjacency and notes do not break it;
 * see ssw_controller_absorbed_by_next().
 */
#ifndef SSW_COLLAPSE_CONTROLLER_BURSTS
#define SSW_COLLAPSE_CONTROLLER_BURSTS 1
#endif

static int ssw_cc_is_collapsible(uint32_t cc) {
    switch (cc & 0x7fu) {
        case 6: case 38: case 98: case 99: case 100: case 101:
        case 120: case 121: case 123:
            return 0;
        default:
            return 1;
    }
}

/*
 * True when the native engine's own queue coalescing would have absorbed this
 * controller into the next one.
 *
 * The rule is adjacency, not supersession, and it ignores notes. enqueue_event()
 * routes note events with g_note_worker_map[ch][key] and channel events with
 * g_channel_worker_map[ch], so notes and controllers land in DIFFERENT worker
 * queues: a note never sits between two controllers in the channel queue and
 * never breaks their adjacency. What native collapses is two controllers of the
 * same channel and number that are consecutive in the channel event stream.
 *
 * An earlier attempt got this wrong in both directions at once -- it stopped at
 * notes, which native does not do, and it scanned forward without limit, which
 * native also does not do. The result dropped the intermediate steps of
 * automation ramps and the controller response was audibly worse than the
 * original. This version drops a controller only when the very next channel
 * event on that channel is the same controller, which is exactly what the
 * native queue does and leaves ramps intact.
 */
static int ssw_controller_absorbed_by_next(
    const ssw_scheduled_event* events,
    size_t index,
    size_t count,
    int64_t block_end_frame) {

    const uint32_t message = events[index].message;
    const uint32_t command = message & 0xf0u;
    const uint32_t channel = message & 0x0fu;

    if (command != 0xb0u)
        return 0;
    if (!ssw_cc_is_collapsible((message >> 8) & 0x7fu))
        return 0;

    const int32_t next_index = events[index].next_channel_index;
    if (next_index < 0 || (size_t)next_index >= count)
        return 0;

    const ssw_scheduled_event* next = &events[next_index];
    if (next->sample_frame >= block_end_frame)
        return 0;

    const uint32_t other = next->message;
    const uint32_t other_command = other & 0xf0u;
    /* next_channel_index already skipped notes and other channels.  Therefore
     * this is exactly the event the old forward scan would have returned. */
    return (other & 0x0fu) == channel &&
           other_command == 0xb0u &&
           ((other >> 8) & 0x7fu) == ((message >> 8) & 0x7fu);
}

static void dispatch_short_at_qpc(uint32_t msg, int64_t timestamp_qpc) {
    unsigned status = msg & 0xFFu;
    int cmd = (int)(status & 0xF0u);
    int ch = (int)(status & 0x0Fu);
    int b1 = (int)((msg >> 8) & 0x7Fu);
    int b2 = (int)((msg >> 16) & 0x7Fu);
    int stack_count = (int)((msg >> 24) & 0xFFu) + 1;

    if (g_gs_part_map_active) {
        int count = g_gs_channel_part_count[ch];
        for (int i = 0; i < count; ++i)
            dispatch_midi_part_event_at_qpc(g_gs_channel_parts[ch][i], cmd, b1, b2,
                                             stack_count, timestamp_qpc);
        return;
    }

    switch (cmd) {
        case 0x90:
            if (b2 > 0) voice_send_note_event_at(ch, b1, b2, 1, stack_count, timestamp_qpc);
            else voice_send_note_event_at(ch, b1, 0, 0, 1, timestamp_qpc);
            break;
        case 0x80: voice_send_note_event_at(ch, b1, 0, 0, 1, timestamp_qpc); break;
        case 0xB0: voice_control_change_at(ch, b1, b2, timestamp_qpc); break;
        case 0xC0: voice_program_change_at(ch, b1, timestamp_qpc); break;
        case 0xE0: voice_pitch_bend_at(ch, b1, b2, timestamp_qpc); break;
        default: break;
    }
}

int ssw_init_ex(int sample_rate,
                int channels,
                int bits_per_sample,
                int block_frames,
                int num_buffers,
                int realtime_priority,
                int max_voices,
                int min_voices,
                int workers,
                int note_sharding,
                int steal_score_cache,
                int fast_note_off,
                int validate_state,
                int soft_clip) {
    ssw_render_limit_clear();
    if (g_ready) voice_shutdown();
    g_ready = 0;

    g_cfg.sample_rate = sample_rate > 0 ? sample_rate : 44100;
    g_cfg.num_channels = channels > 0 ? channels : 2;
    if (g_cfg.num_channels > 2) g_cfg.num_channels = 2;

    g_cfg.bits_per_sample =
        bits_per_sample > 0 ? bits_per_sample : 32;

    g_cfg.buffer_size =
        block_frames > 0 ? block_frames : 480;

    g_cfg.num_buffers =
        num_buffers > 0 ? num_buffers : 16;

    g_cfg.realtime_priority =
        realtime_priority ? 1 : 0;

    g_max_voices =
        max_voices > 0 ? max_voices : 16384;

    g_min_voices =
        min_voices > 0 ? min_voices : 0;

    if (g_min_voices > g_max_voices)
        g_max_voices = g_min_voices;

    g_requested_workers =
        workers > 0 ? workers : 0;

    g_note_sharding =
        note_sharding >= 1 && note_sharding <= 2
            ? note_sharding
            : 0;

    g_steal_score_cache =
        steal_score_cache ? 1 : 0;

    g_fast_note_off =
        fast_note_off ? 1 : 0;

    g_validate_state =
        validate_state ? 1 : 0;

    g_soft_clip =
        soft_clip ? 1 : 0;

    ssw_apply_source_runtime_options();

    /*
     * This is the supplied SnappySynthV2 voice_init_with_count() unchanged.
     * It owns its freelists, sampled/O(1) steal paths, VOR state, pressure
     * thresholds, channel/hash sharding and internal pthread workers.
     */
    voice_init_with_count(
        g_max_voices,
        &g_cfg);

    voice_set_output_soft_clip_enabled(
        g_soft_clip);

    gs_reset_part_map(0);

    if (instrument) {
        sfz_apply_presampling(
            instrument,
            g_cfg.sample_rate);
        if (!ssw_instrument_is_materialized(instrument, g_cfg.sample_rate)) {
            voice_shutdown();
            return 0;
        }
        voice_refresh_all_region_caches();
        voice_prewarm_note_caches();
    }

    g_render_cursor = 1;
    g_song_time_seconds = 0.0;
    g_song_frame = 0;
    g_render_budget = 1;
    g_render_load_ema = -1.0;
    ssw_clear_events();
    g_ready = 1;
    return 1;
}

int ssw_init(int sample_rate, int channels, int block_frames, int max_voices) {
    return ssw_init_ex(
        sample_rate,
        channels,
        32,
        block_frames,
        16,
        1,
        max_voices,
        0,
        0,
        0,
        1,
        1,
        0,
        1);
}

int ssw_load_sf2(const char* path) {
    if (!g_ready || !path || !*path) return 0;
    sfz_instrument* next = sf2_load_as_instrument(path);
    if (!next) return 0;

    /* Finish every load-time sample transformation before exposing this layer
     * to the live instrument. This also makes failure atomic: an allocation
     * failure during pre-resampling leaves the currently playing soundfont
     * untouched instead of installing a half-prepared layer. */
    sfz_apply_presampling(next, g_cfg.sample_rate);
    if (!ssw_instrument_is_materialized(next, g_cfg.sample_rate)) {
        sfz_free(next);
        return 0;
    }

    if (!instrument) {
        instrument = next;
    } else {
        remove_overridden_regions(instrument, next);
        if (!append_instrument_regions(instrument, next)) {
            sfz_free(next);
            return 0;
        }
        sfz_transfer_sample_cache(instrument, next);
        if (next->format == SOUND_FONT_FORMAT_SF2 ||
            instrument->format == SOUND_FONT_FORMAT_UNKNOWN ||
            instrument->format == SOUND_FONT_FORMAT_SFZ)
            instrument->format = next->format;
        strncpy(instrument->source_path, next->source_path,
                sizeof(instrument->source_path) - 1);
        instrument->source_path[sizeof(instrument->source_path) - 1] = '\0';
        free(next->regions);
        next->regions = NULL;
        next->num_regions = 0;
        free(next);
    }

    ++g_soundfont_layers;
    /* Re-run the cheap cache-binding pass on the merged instrument so the
     * diagnostic counters describe the complete live font. All expensive
     * resampling for the incoming layer was already completed above. */
    sfz_apply_presampling(instrument, g_cfg.sample_rate);
    if (!ssw_instrument_is_materialized(instrument, g_cfg.sample_rate))
        return 0;
    /*
     * Re-apply the per-region runtime cache now that the regions exist.
     *
     * refresh_region_runtime_cache() folds sample_rate / g_audio.sample_rate
     * into cached_pitch_base_multiplier, and it used to run only from
     * voice_init_with_count(). On the FIRST soundfont the regions are created
     * after that call, so they never received the correction: every voice
     * played at a ratio that is not 1.0, no_interp was false for all of them,
     * and every voice fell out of the SIMD paths into the scalar loop.
     * Loading a second soundfont re-entered init with regions present and
     * silently fixed it, which is why the first load always performed badly
     * and any reload appeared to cure it -- reported as 400% load in sessions
     * with no real work in them.
     *
     * The pitch multiplier also sets playback rate, so this is a correctness
     * fix as much as a performance one.
     */
    voice_refresh_all_region_caches();
    /*
     * Fill the worker-local note/region caches now rather than letting the
     * music fill them one key at a time. The engine's own notes list these as
     * "single-region note caches" and "worker-local note/region caches"; they
     * validate against the instrument pointer and the channel selector version,
     * so a soundfont load invalidates every entry. Leaving them cold is why the
     * first pass over a file is slow, why repeating the same file keeps getting
     * faster, and why switching soundfont makes it slow again.
     *
     * This only moves when the lookups happen, not what they return, so the
     * sound and the timing are unaffected.
     */
    voice_prewarm_note_caches();
    return instrument->num_regions;
}

int ssw_warmup(void) {
    if (!g_ready || !instrument || !instrument->num_regions) return 0;
    const int frames = g_cfg.buffer_size > 0 ? g_cfg.buffer_size : 512;
    const int channels = g_cfg.num_channels > 0 ? g_cfg.num_channels : 2;
    float *scratch = (float*)calloc((size_t)frames * (size_t)channels, sizeof(float));
    if (!scratch) return 0;
    uint32_t note_on = 0x90u | (0u & 0x0fu);
    note_on |= (60u << 8);
    note_on |= (80u << 16);
    dispatch_short_at_qpc(note_on, 0);
    voice_render_float(scratch, frames);
    /* Silence the warmup note through all-sound-off on every channel. */
    for (int ch = 0; ch < 16; ++ch) {
        uint32_t msg = 0xb0u | (uint32_t)ch;
        msg |= (120u << 8);   /* CC 120 = all sound off */
        msg |= (0u << 16);
        dispatch_short_at_qpc(msg, 0);
    }

    /* Those CC120 messages live in the synth's internal worker queues, not in
     * g_event_head, so ssw_clear_events() cannot remove them. The old warmup
     * returned here and made the first real playback block drain the cleanup
     * from loading. Finish the cleanup while we are still in the load phase. */
    voice_render_float(scratch, frames);

    free(scratch);
    return 1;
}

void ssw_clear_soundfonts(void) {
    if (g_ready) {
        voice_shutdown();
        g_ready = 0;
    }
    if (instrument) { sfz_free(instrument); instrument = NULL; }
    g_soundfont_layers = 0;
    ssw_init_ex(
        g_cfg.sample_rate,
        g_cfg.num_channels,
        g_cfg.bits_per_sample,
        g_cfg.buffer_size,
        g_cfg.num_buffers,
        g_cfg.realtime_priority,
        g_max_voices,
        g_min_voices,
        g_requested_workers,
        g_note_sharding,
        g_steal_score_cache,
        g_fast_note_off,
        g_validate_state,
        g_soft_clip);
}

void ssw_set_volume(float linear) {
    if (linear < 0.0f) linear = 0.0f;
    if (linear > 1.0f) linear = 1.0f;
    voice_set_master_volume_14bit((int)(linear * 16383.0f + 0.5f));
}

void ssw_set_vor_mode(int mode) { voice_set_vor_volume_mode(mode ? 1 : 0); }

void ssw_set_soft_clip(int enabled) {
    g_soft_clip = enabled ? 1 : 0;
    if (g_ready)
        voice_set_output_soft_clip_enabled(g_soft_clip);
}

void ssw_reset(void) {
    g_render_limit_ema_us = 0.0;
    ssw_render_limit_clear();
    if (!g_ready) return;
    gs_reset_part_map(0);
    apply_gm_reset_at_qpc(g_render_cursor);
    g_render_cursor = 1;
    g_song_time_seconds = 0.0;
    g_song_frame = 0;
    g_render_budget = 1;
    g_render_load_ema = -1.0;
    ssw_clear_events();
}

void ssw_send_sysex(const uint8_t* data, int length, uint32_t offset) {
    if (!g_ready || !data || length <= 0) return;
    dispatch_sysex_data_at_qpc(data, length, g_render_cursor + (int64_t)offset);
}

static int ssw_render_to_buffer(float* out_buffer,
                                const uint32_t* messages,
                                const uint32_t* offsets,
                                int count,
                                int frames) {
    if (!g_ready || !out_buffer || frames <= 0)
        return 0;

    voice_set_render_timing(g_render_cursor, g_cfg.sample_rate);
    for (int i = 0; i < count; ++i) {
        uint32_t offset = offsets ? offsets[i] : 0u;
        if (offset >= (uint32_t)frames)
            offset = (uint32_t)(frames - 1);
        dispatch_short_at_qpc(
            messages[i],
            g_render_cursor + (int64_t)offset);
    }

    /* voice_render_float() owns output initialization/first-worker copy just
     * like the native engine. Clearing here as well only doubles memory
     * bandwidth on every device block. */
    voice_render_float(out_buffer, frames);
    g_render_cursor += frames;
    return 1;
}

uintptr_t ssw_render(const uint32_t* messages, const uint32_t* offsets,
                     int count, int frames) {
    if (!g_ready || frames <= 0)
        return 0;

    if (frames > g_out_capacity_frames) {
        float* next = (float*)realloc(
            g_out,
            sizeof(float) * (size_t)frames * (size_t)g_cfg.num_channels);
        if (!next)
            return 0;
        g_out = next;
        g_out_capacity_frames = frames;
    }

    if (!ssw_render_to_buffer(g_out, messages, offsets, count, frames))
        return 0;

    return (uintptr_t)g_out;
}

/*
 * Browser hot path: render directly into a caller-owned region inside the
 * module's Shared WebAssembly.Memory.  The AudioWorklet can read that same
 * SharedArrayBuffer, eliminating the old Worker -> transferable ArrayBuffer
 * PCM copy without changing one DSP operation in voice_render_float().
 */
int ssw_render_into(uintptr_t out_ptr,
                    const uint32_t* messages,
                    const uint32_t* offsets,
                    int count,
                    int frames) {
    return ssw_render_to_buffer(
        (float*)out_ptr,
        messages,
        offsets,
        count,
        frames);
}

/*
 * A native KDMAPI stream does not pay a browser pthread/futex round trip for
 * every controller byte.  The native realtime backend never splits a render
 * on an event at all: src/Audio/winmm_output.c chooses its chunk size purely
 * from elapsed device time, and voice.c applies CC/program/pitch-bend state
 * when the worker drains its channel queue at the START of that chunk.  Only
 * note-on/note-off carry a sample offset (qpc_to_sample_offset_clamped).
 *
 * The previous browser build split the block at every state event, and at the
 * exact sample for discrete CCs and program changes.  That is far finer than
 * the engine it is porting, and it is very expensive here, because the cost of
 * voice_render_float() is dominated by per-call fixed work rather than by the
 * frame count:
 *
 *   - two full worker barriers, each an emscripten futex park/unpark;
 *   - an InterlockedAdd + memcpy rebuild of the global render queue;
 *   - the per-voice setup chain (voice struct, region, gains, branch cascade),
 *     paid in full for every active voice on every call;
 *   - loss of the SIMD sustain paths, which require frames to be a multiple
 *     of 8 (fast stereo batch) or 4 (no-interp paths).
 *
 * So a CC-dense passage multiplied the render cost by the number of state
 * events, not by anything musical.  This is what produced stalls at only a few
 * hundred voices in passages that were not dense.
 *
 * The replacement keeps controller timing finer than the native engine when
 * there is CPU headroom, and collapses to exactly the native behaviour (one
 * voice_render_float() per block, state applied at block start) when there is
 * not.  That is also what the native build does implicitly: its WinMM render
 * thread emits tiny chunks while it is idle and whole buffers while it is
 * busy.  Three rules:
 *
 *   1. Boundaries may only land on a fixed grid, never on an arbitrary event
 *      sample.  The grid is a multiple of 8 frames so the SIMD sustain paths
 *      stay eligible.
 *   2. Selector events -- bank select, RPN/NRPN select, data entry and program
 *      change -- get an EXACT boundary whenever a note has been admitted on
 *      that channel since the last boundary.  This is not optional.  voice.c
 *      routes note events by key hash (g_note_worker_map) and channel events by
 *      channel (g_channel_worker_map), so once there is more than one worker a
 *      note-on and a program change sit in DIFFERENT worker queues and are
 *      consumed concurrently within one render cycle.  Only a render boundary
 *      orders them.  Without it, note-ons resolve against the wrong
 *      bank/program, the region selector matches nothing, and the note is
 *      silently dropped.
 *      The note gate is what makes this affordable: when no note has been
 *      admitted since the last boundary, no queue holds anything for that
 *      channel to be reordered against, so the selector can be applied
 *      immediately.  Material that spams bank/RPN/program without notes in
 *      between therefore costs nothing, while musical material keeps exact
 *      ordering.
 *   3. Events that only change ALREADY SOUNDING voices (continuous CC, pitch
 *      bend) are quantized onto the grid.  These need to be heard at the right
 *      time, not ordered against note admission.
 *   4. The number of voice_render_float() calls per block is hard-capped and
 *      driven by measured load, with fast backoff and gradual recovery.  The
 *      cap governs rule 3 only; correctness boundaries are never skipped.
 *   5. All-sound-off style controllers (CC 120/121/123-127) get exact
 *      boundaries from a small separate reserve.
 *
 * Notes keep their original sample timestamps in every case; the engine's own
 * event ordering, coalescing and VOR identity are untouched.
 */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
static inline double ssw_now_ms(void) { return emscripten_get_now(); }
#else
#include <time.h>
static inline double ssw_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}
#endif

/* Finest scheduling cell, about 1.45 ms at 44.1 kHz. Multiple of 8. */
#define SSW_STATE_QUANTUM_MIN 64
/* Absolute ceiling on voice_render_float() calls per block. */
#define SSW_MAX_BLOCK_RENDERS 8
/* Extra boundaries reserved for all-notes/all-sound-off style controllers,
 * which do change currently sounding voices and are rare in normal material. */
#define SSW_PANIC_RENDER_RESERVE 2
/* Load thresholds, as a fraction of the block's own realtime budget. */
#define SSW_LOAD_BACKOFF 0.45
#define SSW_LOAD_RECOVER 0.20

/*
 * All-notes-off / all-sound-off / reset-controllers / mono-poly. These do
 * affect sounding voices and are normally sparse, so they are allowed an exact
 * boundary from a small separate reserve even when the load budget is 1.
 */
static int ssw_is_panic_cc(uint32_t cc) {
    switch (cc & 0x7fu) {
        case 120: case 121: case 123:
        case 124: case 125: case 126: case 127:
            return 1;
        default:
            return 0;
    }
}

/*
 * Set to 1 to make every selector event open a boundary unconditionally, i.e.
 * exactly the pre-governor behaviour. Rollback switch if the note gate below is
 * ever suspected of dropping notes again.
 */
#ifndef SSW_FORCE_SELECTOR_BOUNDARIES
#define SSW_FORCE_SELECTOR_BOUNDARIES 0
#endif

/*
 * Selector events choose the state a FUTURE note-on resolves against: bank
 * select, RPN/NRPN select, data entry and program change. They must be ordered
 * exactly against note events on the same channel, because voice.c shards note
 * events by key and channel events by channel, so with more than one worker the
 * two live in different queues and are consumed concurrently. Data entry is
 * included because RPN 0 is pitch-bend range, which changes sounding voices as
 * well; exact is the safe side for it.
 */
static int ssw_is_selector_event(uint32_t message) {
    const uint32_t command = message & 0xf0u;

    if (command == 0xc0u)
        return 1;                       /* program change */
    if (command != 0xb0u)
        return 0;

    switch ((message >> 8) & 0x7fu) {
        case 0:                         /* bank select MSB */
        case 6:                         /* data entry MSB  */
        case 32:                        /* bank select LSB */
        case 38:                        /* data entry LSB  */
        case 98:                        /* NRPN LSB */
        case 99:                        /* NRPN MSB */
        case 100:                       /* RPN LSB  */
        case 101:                       /* RPN MSB  */
            return 1;
        default:
            return 0;
    }
}

/*
 * Does this event change what already-sounding voices produce? These only need
 * to be heard at the right time, not ordered against note admission, so they
 * are the ones the load governor may quantize onto the grid.
 */
static int ssw_affects_sounding_voices(uint32_t message) {
    const uint32_t command = message & 0xf0u;

    if (command == 0xe0u)
        return 1;                       /* pitch bend */
    if (command != 0xb0u)
        return 0;

    switch ((message >> 8) & 0x7fu) {
        case 64:                        /* sustain */
        case 65:                        /* portamento on/off */
        // All other CCs (volume, pan, expression, modulation, cutoff, etc.) are
        // removed from the boundary-forcing list. They update g_channel_render
        // silently and the next voice_render_float picks them up without splitting.
        // With 100k+ CC/s this prevents thousands of segment splits per second.
            return 1;
        default:
            return 0;
    }
}

/* Grid size for this block, given how many renders we may spend on it. */
static int ssw_state_quantum(int frames, int budget) {
    int quantum;
    if (budget <= 1 || frames <= SSW_STATE_QUANTUM_MIN)
        return frames;
    quantum = (frames + budget - 1) / budget;
    if (quantum < SSW_STATE_QUANTUM_MIN)
        quantum = SSW_STATE_QUANTUM_MIN;
    quantum = (quantum + 7) & ~7;      /* keep the SIMD sustain paths eligible */
    if (quantum > frames)
        quantum = frames;
    return quantum;
}

/*
 * Fast backoff, gradual recovery. Halving on overload reaches the native
 * single-render behaviour within three blocks; doubling on idle takes three
 * blocks to return to the finest grid, which prevents the budget from
 * oscillating around a threshold.
 */
static void ssw_update_render_budget(double used_ms, int frames) {
    const int rate = g_cfg.sample_rate > 0 ? g_cfg.sample_rate : 44100;
    const double realtime_ms = (double)frames * 1000.0 / (double)rate;
    double load;

    if (realtime_ms <= 0.0)
        return;

    load = used_ms / realtime_ms;
    if (!isfinite(load) || load < 0.0)
        load = 0.0;

    if (g_render_load_ema < 0.0) g_render_load_ema = load;
    else g_render_load_ema += 0.25 * (load - g_render_load_ema);

    if (g_render_load_ema > SSW_LOAD_BACKOFF) {
        g_render_budget /= 2;
        if (g_render_budget < 1) g_render_budget = 1;
    } else if (g_render_load_ema < SSW_LOAD_RECOVER) {
        g_render_budget *= 2;
        if (g_render_budget > SSW_MAX_BLOCK_RENDERS)
            g_render_budget = SSW_MAX_BLOCK_RENDERS;
    }
}

int ssw_render_queued_into(uintptr_t out_ptr, int frames) {
    if (!g_ready || !out_ptr || frames <= 0) return 0;

    const int64_t block_start_frame = g_song_frame;
    const int64_t block_end_frame = block_start_frame + (int64_t)frames;
    const int channels = g_cfg.num_channels > 0 ? g_cfg.num_channels : 2;
    const int budget = g_render_budget;
    const int quantum = ssw_state_quantum(frames, budget);
    const double started_ms = ssw_now_ms();
    int segment_start = 0;
    int splits_used = 0;
    int panic_splits_used = 0;
    /* Per-channel: has a note been admitted since the last boundary? Selector
     * events only have to force a boundary when the answer is yes. */
    unsigned char note_pending[16];

    memset(note_pending, 0, sizeof(note_pending));
    g_dispatch_accum_us = 0.0;
    voice_set_render_timing(g_render_cursor, g_cfg.sample_rate);

    while (g_event_head) {
        ssw_event_block* block = g_event_head;
        while (block->index < block->count) {
            const ssw_scheduled_event* event = &block->events[block->index];
            if (event->sample_frame >= block_end_frame) goto events_done;

            int64_t frame = event->sample_frame - block_start_frame;
            if (frame < 0) frame = 0;
            if (frame >= frames) frame = frames - 1;

            {
                const uint32_t command = event->message & 0xf0u;
                const int ch = (int)(event->message & 0x0fu);
                const int panic =
                    command == 0xb0u &&
                    ssw_is_panic_cc((event->message >> 8) & 0x7fu);
                /* Correctness boundary: never subject to the load budget. */
                const int ordering =
                    !panic &&
                    ssw_is_selector_event(event->message) &&
#if SSW_FORCE_SELECTOR_BOUNDARIES
                    1;
#else
                    note_pending[ch];
#endif
                int boundary = segment_start;

                if (panic) {
                    if (panic_splits_used < SSW_PANIC_RENDER_RESERVE)
                        boundary = (int)frame;
                } else if (ordering) {
                    boundary = (int)frame;
                } else if (ssw_affects_sounding_voices(event->message) &&
                           splits_used < budget - 1) {
                    boundary = ((int)frame / quantum) * quantum;
                }

                if (boundary > segment_start) {
                    const int segment_frames = boundary - segment_start;
                    voice_render_float(
                        ((float*)out_ptr) +
                            (size_t)segment_start * (size_t)channels,
                        segment_frames);
                    g_render_cursor += segment_frames;
                    segment_start = boundary;
                    if (panic) ++panic_splits_used;
                    else if (!ordering) ++splits_used;
                    /* Everything queued before this boundary has been consumed,
                     * so nothing is left for a selector to be reordered
                     * against. */
                    memset(note_pending, 0, sizeof(note_pending));
                    voice_set_render_timing(g_render_cursor, g_cfg.sample_rate);
                }

                {
#if SSW_COLLAPSE_CONTROLLER_BURSTS
                    /* Skip a controller value that a later one in this same run
                     * supersedes, which is the collapse the native engine gets
                     * for free by being fed in real time. */
                    if (ssw_controller_absorbed_by_next(
                            block->events, block->index, block->count,
                            block_end_frame)) {
                        ++g_controllers_collapsed;
                    } else
#endif
                    dispatch_short_at_qpc(
                        event->message,
                        g_render_cursor + (frame - segment_start));
                }

                if (command == 0x90u || command == 0x80u)
                    note_pending[ch] = 1u;
            }
            ++block->index;
        }

        g_event_head = block->next;
        if (!g_event_head) g_event_tail = NULL;
        ssw_event_block_release(block);
    }

events_done:
    if (segment_start < frames) {
        const int remaining = frames - segment_start;
        voice_render_float(
            ((float*)out_ptr) + (size_t)segment_start * (size_t)channels,
            remaining);
        g_render_cursor += remaining;
    }
    g_song_frame = block_end_frame;
    g_song_time_seconds =
        (double)g_song_frame /
        (double)(g_cfg.sample_rate > 0 ? g_cfg.sample_rate : 44100);

    {
        const double elapsed_ms = ssw_now_ms() - started_ms;
        g_last_render_us = elapsed_ms * 1000.0;
        g_last_dispatch_us = g_dispatch_accum_us;
        ssw_update_render_budget(elapsed_ms, frames);
        if (g_render_limit_percent > 0 && frames > 0) {
            /* Over the limit, the workers that set the block time cap how many
               note-ons each (channel, key) may start per cycle: spam keys shed
               notes, sparse keys (melody) never do (HANDOFF sec. 41). */
            const double target_us = (double)g_render_limit_percent * 10000.0 * (double)frames /
                (double)(g_cfg.sample_rate > 0 ? g_cfg.sample_rate : 44100);
            g_render_limit_ema_us = g_render_limit_ema_us * 0.75 + elapsed_ms * 1000.0 * 0.25;
            const double spent_us = g_render_limit_ema_us;
            int workers = voice_get_worker_count();
            if (workers > SSW_LIMIT_MAX_WORKERS) workers = SSW_LIMIT_MAX_WORKERS;
            int budget = voice_get_admit_budget_us();
            double slowest = 0.0;
            for (int w = 0; w < workers; ++w) {
                const double us = (double)voice_get_admit_work_us(w);
                if (us > slowest) slowest = us;
            }
            int all_at_min = 1, any_limited = 0;
            for (int w = 0; w < workers; ++w) {
                const double work_us = (double)voice_get_admit_work_us(w);
                const int bottleneck = work_us >= slowest * 0.6 && work_us > target_us * 0.25;
                const int key_max = voice_get_admit_key_max(w);
                int cap = voice_get_admit_key_cap(w);
                if (spent_us > target_us && bottleneck) {
                    if (cap == 0) cap = key_max > 2 ? key_max / 2 : 1;
                    else cap = cap > 1 ? (cap * 4) / 5 : 1;
                    if (cap < 1) cap = 1;
                } else if (cap > 0 && (spent_us < target_us * 0.85 || !bottleneck)) {
                    cap += cap / 4 + 1;
                    if (cap > key_max * 2 + 4) cap = 0;
                }
                if (cap > 0) { any_limited = 1; if (cap > 1) all_at_min = 0; }
                if (cap != voice_get_admit_key_cap(w)) voice_set_admit_key_cap(w, cap);
            }
            if (spent_us > target_us && any_limited && all_at_min) {
                budget = budget > 0 ? (budget * 3) / 4 : (int)(target_us * 0.5);
                if (budget < 200) budget = 200;
                voice_set_admit_budget_us(budget);
            } else if (budget > 0 && spent_us < target_us * 0.7) {
                budget += budget / 8 + 50;
                voice_set_admit_budget_us(budget > (int)(target_us * 4.0) ? 0 : budget);
            }
        } else {
            g_render_limit_ema_us = 0.0;
            ssw_render_limit_clear();
        }
    }
    return 1;
}

int ssw_active_voices(void) { return GetVoiceStats().active_voices; }
int ssw_free_voices(void) { return GetVoiceStats().free_voices; }
int ssw_steals(void) {
    long value = GetVoiceStats().steals;
    if (value < 0) return 0;
    return value > 2147483647L ? 2147483647 : (int)value;
}
/*
 * Free voices that workers handed back to the global pool (cumulative). A
 * rising value while STEALS/s is high means the pool is genuinely full; a flat
 * zero while notes vanish means workers are hoarding and rebalance is not
 * running. See rebalance_worker_freelist() in voice.c.
 */
int ssw_rebalanced(void) {
    long value = GetVoiceStats().rebalanced;
    if (value < 0) return 0;
    return value > 2147483647L ? 2147483647 : (int)value;
}
/*
 * Note-ons that were dropped with no sound (cumulative): no free voice in the
 * worker, none in the global pool, none borrowable, and steal_voice_fast()
 * found nothing to take. This is the "notes that do not sound" symptom made
 * countable; before this it was invisible in every stat.
 */
int ssw_dropped_notes(void) {
    long value = GetVoiceStats().drops;
    if (value < 0) return 0;
    return value > 2147483647L ? 2147483647 : (int)value;
}
int ssw_layer_count(void) { return g_soundfont_layers; }
int ssw_region_count(void) { return instrument ? instrument->num_regions : 0; }
int ssw_channels(void) { return g_cfg.num_channels; }
int ssw_sample_rate(void) { return g_cfg.sample_rate; }
int ssw_bits_per_sample(void) { return g_cfg.bits_per_sample; }
int ssw_num_buffers(void) { return g_cfg.num_buffers; }
int ssw_worker_count(void) { return voice_get_worker_count(); }
int ssw_worker_thread_failures(void) { return voice_get_worker_thread_failures(); }

/*
 * Render load as a fraction of the block's own realtime budget, times 1000.
 * 1000 means a block took exactly as long to render as it lasts, so the synth
 * is at the edge of what the CPU can do. Well under 1000 while underruns are
 * climbing means the renderer is not being asked to run often enough, which is
 * a scheduling problem rather than a throughput one.
 */
int ssw_render_load_x1000(void) {
    const double load = g_render_load_ema < 0.0 ? 0.0 : g_render_load_ema;
    return (int)(load * 1000.0 + 0.5);
}
int ssw_last_render_us(void) { return (int)(g_last_render_us + 0.5); }
/* Of the block's cost, the part spent admitting events rather than rendering
 * voices. This part is single threaded, so a large share here is why adding
 * workers stops helping. */
int ssw_last_dispatch_us(void) { return (int)(g_last_dispatch_us + 0.5); }
int ssw_render_budget(void) { return g_render_budget; }
void ssw_set_render_limit_percent(int percent) { g_render_limit_percent = percent < 0 ? 0 : percent; }
int ssw_render_limit_percent(void) { return g_render_limit_percent; }
int ssw_admit_budget_us(void) { return voice_get_admit_budget_us(); }
int ssw_limited_notes(void) { return voice_get_admit_limited_notes(); }
int ssw_admit_key_cap(int worker) { return voice_get_admit_key_cap(worker); }
int ssw_detected_cores(void) { return voice_get_detected_cores(); }

/*
 * Render path split for the last cycle. The SIMD batch has entry conditions
 * (stereo, sustain, no interpolation, suitable frame count); a voice missing
 * any of them falls to the scalar loop. A block time alone cannot distinguish
 * an expensive DSP from one where almost nothing is vectorized.
 */
int ssw_path_fast_voices(void) { return voice_get_path_fast(); }
int ssw_path_scalar_voices(void) { return voice_get_path_scalar(); }
/*
 * Breakdown of why voices miss the vectorized path, for the last cycle. Each
 * blocker needs a different fix -- interpolation needs a resampling kernel,
 * looping needs the wrap inside the vector loop, the filter needs a vectorized
 * biquad -- so the split says which to write first.
 */
/* Distinct workers that consumed at least one render-queue chunk last cycle.
 * Read together with BUSY: 24 here with BUSY at 0 means the timer is wrong;
 * 1 or 2 here means the pool is not sharing the queue. */
/* Summed time across workers spent draining events, allocating voices and
 * stealing, before any rendering. BUSY excludes this; BLOCK includes it. */
int ssw_alloc_us(void) { return voice_get_alloc_us(); }
/* Note-offs consumed and voices actually freed in the last cycle. Frees
 * trailing note-offs means voices are not coming back, which is what keeps the
 * pool pinned above the pressure line where the steal guards start failing. */
/*
 * Cumulative voice lifecycle. Note-ons that actually got a voice, and voices
 * returned to a free stack by either recycling path. Cumulative rather than a
 * per-cycle ratio because a voice is freed when its release envelope finishes,
 * not when its note-off arrives, so comparing the two inside one block is
 * meaningless. If recycled tracks started over time the pool is simply too
 * small for the material; if it falls behind and stays behind, voices are not
 * coming back.
 */
/*
 * Result of the last presampling pass. If the first soundfont load reports
 * regions skipped, or zero resampled where a reload reports many, that is the
 * first-load slowdown: unpresampled regions resample at run time inside the
 * render loop.
 */
/*
 * Turns the diagnostic counters on and off at runtime. Off by default: the
 * per-controller profile costs two clock reads per controller event and the
 * path counters an atomic add per voice per block, which is worth paying while
 * chasing something and pure waste otherwise. The numbers a normal user sees
 * (rate, active, free, workers, underruns) are engine state and never gated.
 */
int ssw_set_debug_metrics(int enabled) {
    voice_set_debug_metrics(enabled);
    return 1;
}

int ssw_controllers_collapsed(void) { return g_controllers_collapsed; }

/*
 * Dominant controller of the last cycle, packed: number << 16 | percent of the
 * controller dispatch time it accounts for. Read statically no CC case is
 * expensive, so if one number dominates the cost is indirect and this says
 * which one to chase.
 */
int ssw_cc_hotspot(void) {
    int best = -1;
    int best_us = 0;
    int total_us = 0;
    for (int cc = 0; cc < 128; ++cc) {
        const int us = voice_get_cc_us(cc);
        total_us += us;
        if (us > best_us) { best_us = us; best = cc; }
    }
    if (best < 0 || total_us <= 0) return 0;
    return ((best & 0x7f) << 16) | ((best_us * 100) / total_us);
}
int ssw_cc_hotspot_count(void) {
    int best = -1, best_us = 0;
    for (int cc = 0; cc < 128; ++cc) {
        const int us = voice_get_cc_us(cc);
        if (us > best_us) { best_us = us; best = cc; }
    }
    return best < 0 ? 0 : voice_get_cc_count(best);
}
int ssw_presample_seen(void) { return g_presample_regions_seen; }
int ssw_presample_skipped(void) { return g_presample_regions_skipped; }
int ssw_presample_resampled(void) { return g_presample_regions_resampled; }

int ssw_notes_started(void) { return voice_get_notes_started(); }
int ssw_voices_recycled(void) { return voice_get_voices_recycled(); }
int ssw_workers_participating(void) { return voice_get_workers_participating(); }
int ssw_miss_interp(void) { return voice_get_miss_interp(); }
int ssw_miss_loop(void) { return voice_get_miss_loop(); }
int ssw_miss_filter(void) { return voice_get_miss_filter(); }
int ssw_miss_other(void) { return voice_get_miss_other(); }
int ssw_path_simd_voice_voices(void) { return voice_get_path_simd_voice(); }
/*
 * Summed busy time across workers for the last cycle. Compared against the
 * block's own wall clock this says whether the pool actually overlaps: near the
 * worker count times the block time means real parallelism, near the block time
 * alone means the workers are serialized.
 */
int ssw_worker_busy_us(void) { return voice_get_worker_busy_us(); }

void ssw_shutdown(void) {
    if (g_ready) voice_shutdown();
    g_ready = 0;
    if (instrument) { sfz_free(instrument); instrument = NULL; }
    g_soundfont_layers = 0;
    free(g_out); g_out = NULL; g_out_capacity_frames = 0;
    ssw_clear_events();
    ssw_free_event_block_pool();
    g_song_time_seconds = 0.0;
    g_song_frame = 0;
}
