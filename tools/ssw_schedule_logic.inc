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

