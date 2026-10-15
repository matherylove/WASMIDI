/* Standalone harness for the rewritten ssw_render_queued_into() scheduling.
 * Stubs stand in for the voice engine; we verify frame accounting, render-call
 * bounds, boundary placement and event ordering. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <assert.h>
#include <time.h>

typedef struct { int sample_rate, num_channels, bits_per_sample, buffer_size, num_buffers, realtime_priority; } AudioConfig;
static AudioConfig g_cfg = {44100, 2, 32, 512, 16, 1};
static int64_t g_render_cursor = 1;
static int g_render_budget = 1;
static int g_render_limit_percent = 0; /* render limit (HANDOFF §29) is off in this harness */
static double g_render_limit_ema_us = 0.0;
static int t_admit_budget;
static int voice_get_admit_budget_us(void) { return t_admit_budget; }
static void voice_set_admit_budget_us(int us) { t_admit_budget = us; }
static int voice_get_admit_key_cap(int w) { (void)w; return 0; }
static void voice_set_admit_key_cap(int w, int v) { (void)w; (void)v; }
static int voice_get_admit_key_max(int w) { (void)w; return 0; }
static int voice_get_admit_work_us(int w) { (void)w; return 0; }
static int voice_get_worker_count(void) { return 1; }
#define SSW_LIMIT_MAX_WORKERS 256
static void ssw_render_limit_clear(void) {}
static double g_render_load_ema = -1.0;
static int g_ready = 1;
static double g_song_time_seconds = 0.0;
static int64_t g_song_frame = 0;
/* Telemetry sinks written by ssw_render_queued_into() (revision 20 split of
 * block cost into render vs. event dispatch). The harness only needs them to
 * exist; their values are not asserted here. */
static double g_last_render_us = 0.0;
static double g_last_dispatch_us = 0.0;
static double g_dispatch_accum_us = 0.0;

typedef struct { int64_t sample_frame; uint32_t message; } ssw_scheduled_event;
typedef struct ssw_event_block {
    struct ssw_event_block* next; size_t index, count, capacity; ssw_scheduled_event events[];
} ssw_event_block;
static ssw_event_block* g_event_head = NULL;
static ssw_event_block* g_event_tail = NULL;

/* The production core recycles consumed schedule slabs. Scheduling semantics
 * do not depend on that allocator policy, so the host harness can simply free
 * each slab while exercising the extracted control flow. */
static void ssw_event_block_release(ssw_event_block* block) { free(block); }

/* ---- instrumentation ---- */
static int   t_render_calls;
static int   t_frames_rendered;
static int   t_boundaries[64];
static int   t_nboundaries;
static float *t_out_base;
static int   t_bad_ptr;
static int   t_misaligned_segments;
static uint32_t t_dispatch_order[8192];
static int64_t  t_dispatch_ts[8192];
static int   t_ndispatch;
static double t_fake_cost_ms;       /* simulated per-render-call cost */
static double t_fake_now;

static double ssw_now_ms(void) { return t_fake_now; }

static void voice_set_render_timing(int64_t cursor, int rate) { (void)cursor; (void)rate; }

/* interleaved trace: RENDER markers and dispatched messages, in order */
#define TR_RENDER 0xFFFFFFFFu
static uint32_t t_trace[16384];
static int t_ntrace;
static void trace(uint32_t v){ if(t_ntrace<16384) t_trace[t_ntrace]=v; ++t_ntrace; }

static void voice_render_float(float* out, int frames) {
    ++t_render_calls;
    t_frames_rendered += frames;
    t_fake_now += t_fake_cost_ms;
    if (out < t_out_base) t_bad_ptr = 1;
    {
        long off = (long)(out - t_out_base);
        if (off % g_cfg.num_channels) t_bad_ptr = 1;
        if (t_nboundaries < 64) t_boundaries[t_nboundaries++] = (int)(off / g_cfg.num_channels);
    }
    /* the SIMD sustain batch path needs frames % 8 == 0 */
    if (frames % 8) ++t_misaligned_segments;
    trace(TR_RENDER);
    memset(out, 0, sizeof(float) * (size_t)frames * (size_t)g_cfg.num_channels);
}

static void dispatch_short_at_qpc(uint32_t msg, int64_t ts) {
    if (t_ndispatch < 8192) { t_dispatch_order[t_ndispatch] = msg; t_dispatch_ts[t_ndispatch] = ts; }
    ++t_ndispatch;
    trace(msg);
}

#define SSW_HARNESS_NO_NOW 1
#include "ssw_schedule_logic.inc"

/* ---- helpers ---- */
static void queue(const ssw_scheduled_event* evs, int n) {
    ssw_event_block* b = malloc(sizeof(ssw_event_block) + (size_t)n * sizeof(ssw_scheduled_event));
    b->next = NULL; b->index = 0; b->count = (size_t)n; b->capacity = (size_t)n;
    memcpy(b->events, evs, (size_t)n * sizeof(ssw_scheduled_event));
    if (g_event_tail) g_event_tail->next = b; else g_event_head = b;
    g_event_tail = b;
}
static void reset_counters(void) {
    t_render_calls = 0; t_frames_rendered = 0; t_nboundaries = 0;
    t_bad_ptr = 0; t_misaligned_segments = 0; t_ndispatch = 0; t_ntrace = 0;
}
static void clear_events(void) {
    while (g_event_head) { ssw_event_block* n = g_event_head->next; free(g_event_head); g_event_head = n; }
    g_event_tail = NULL;
}
static uint32_t cc(int ch, int num, int val) { return (uint32_t)(0xB0|ch) | ((uint32_t)num<<8) | ((uint32_t)val<<16); }
static uint32_t noteon(int ch, int key, int vel) { return (uint32_t)(0x90|ch) | ((uint32_t)key<<8) | ((uint32_t)vel<<16); }
static uint32_t bend(int ch, int lsb, int msb) { return (uint32_t)(0xE0|ch) | ((uint32_t)lsb<<8) | ((uint32_t)msb<<16); }
static uint32_t pc(int ch, int prog) { return (uint32_t)(0xC0|ch) | ((uint32_t)prog<<8); }

static int fails = 0;
#define CHECK(cond, ...) do { if(!(cond)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); ++fails; } } while(0)

static float outbuf[4096*2];

static void run_block(int frames) {
    reset_counters();
    t_out_base = outbuf;
    ssw_render_queued_into((uintptr_t)outbuf, frames);
}

int main(void) {
    const int F = 512;

    /* ---------- 1. no events: exactly one render, native behaviour ---------- */
    printf("1. block with no events\n");
    g_render_budget = 8; g_song_frame = 0;
    run_block(F);
    CHECK(t_render_calls == 1, "expected 1 render call, got %d", t_render_calls);
    CHECK(t_frames_rendered == F, "expected %d frames, got %d", F, t_frames_rendered);

    /* ---------- 2. notes only: still one render, timestamps preserved ------- */
    printf("2. note-only block (Black MIDI fast path)\n");
    clear_events();
    { ssw_scheduled_event e[600];
      for (int i = 0; i < 600; ++i) { e[i].sample_frame = g_song_frame + (i % F); e[i].message = noteon(i%16, 40+(i%40), 100); }
      queue(e, 600); }
    g_render_budget = 8;
    run_block(F);
    CHECK(t_render_calls == 1, "note-only block must stay one render, got %d", t_render_calls);
    CHECK(t_ndispatch == 600, "expected 600 dispatches, got %d", t_ndispatch);

    /* ---------- 3. the old pathology: dense discrete CC + program change ---- */
    printf("3. dense bank/RPN/program spam (old code split sample-exact)\n");
    clear_events();
    { ssw_scheduled_event e[400]; int n=0;
      for (int i = 0; i < 100; ++i) {
        int64_t f = g_song_frame + (i * 5);
        e[n].sample_frame=f; e[n++].message = cc(0, 0, i & 0x7f);     /* bank MSB */
        e[n].sample_frame=f; e[n++].message = cc(0, 32, i & 0x7f);    /* bank LSB */
        e[n].sample_frame=f; e[n++].message = cc(0, 101, 0);          /* RPN MSB  */
        e[n].sample_frame=f; e[n++].message = pc(0, i & 0x7f);        /* program  */
      }
      queue(e, n); }
    g_render_budget = 8;
    run_block(F);
    CHECK(t_render_calls == 1,
          "selector-only events must not open any boundary, got %d renders", t_render_calls);

    /* ---------- 4. dense continuous CC: bounded by budget ------------------- */
    printf("4. dense continuous CC sweep (CC7/CC11/pitch bend)\n");
    for (int budget = 1; budget <= 8; budget *= 2) {
        clear_events();
        { ssw_scheduled_event e[512]; int n=0;
          for (int i = 0; i < 170; ++i) {
            int64_t f = g_song_frame + (i * 3);
            e[n].sample_frame=f; e[n++].message = cc(0, 7, i & 0x7f);
            e[n].sample_frame=f; e[n++].message = cc(0, 11, i & 0x7f);
            e[n].sample_frame=f; e[n++].message = bend(0, i & 0x7f, 64);
          }
          queue(e, n); }
        g_render_budget = budget;
        run_block(F);
        CHECK(t_render_calls <= budget,
              "budget %d exceeded: %d render calls", budget, t_render_calls);
        CHECK(t_frames_rendered == F,
              "budget %d rendered %d frames, expected %d", budget, t_frames_rendered, F);
        CHECK(t_misaligned_segments == 0,
              "budget %d produced %d segments not a multiple of 8", budget, t_misaligned_segments);
        printf("   budget %d -> %d render calls, boundaries:", budget, t_render_calls);
        for (int i = 0; i < t_nboundaries; ++i) printf(" %d", t_boundaries[i]);
        printf("\n");
    }

    /* ---------- 5. boundaries land only on the grid ------------------------- */
    printf("5. boundaries stay on the grid\n");
    clear_events();
    { ssw_scheduled_event e[200]; int n=0;
      for (int i = 0; i < 200; ++i) { e[n].sample_frame = g_song_frame + (i*2+1); e[n++].message = cc(0, 7, i & 0x7f); }
      queue(e, n); }
    g_render_budget = 8;
    run_block(F);
    { int q = 64;
      for (int i = 1; i < t_nboundaries; ++i)
        CHECK(t_boundaries[i] % q == 0, "boundary %d is not a multiple of %d", t_boundaries[i], q); }

    /* ---------- 6. panic CC gets an exact boundary from its reserve --------- */
    printf("6. all-notes-off reserve under a starved budget\n");
    clear_events();
    { ssw_scheduled_event e[4]; int n=0;
      e[n].sample_frame=g_song_frame+100; e[n++].message = cc(3, 123, 0);
      e[n].sample_frame=g_song_frame+300; e[n++].message = cc(3, 120, 0);
      e[n].sample_frame=g_song_frame+400; e[n++].message = cc(3, 123, 0);
      e[n].sample_frame=g_song_frame+450; e[n++].message = cc(3, 121, 0);
      queue(e, n); }
    g_render_budget = 1;                 /* fully loaded */
    run_block(F);
    CHECK(t_render_calls <= 1 + 2, "panic reserve exceeded: %d renders", t_render_calls);
    CHECK(t_render_calls == 3, "expected 1 + 2 reserved renders, got %d", t_render_calls);
    CHECK(t_frames_rendered == F, "panic path rendered %d frames", t_frames_rendered);

    /* ---------- 7. dispatch order and offsets stay monotone ----------------- */
    printf("7. event order and per-segment offsets\n");
    clear_events();
    { ssw_scheduled_event e[12]; int n=0;
      e[n].sample_frame=g_song_frame+  0; e[n++].message = noteon(0,60,100);
      e[n].sample_frame=g_song_frame+ 70; e[n++].message = cc(0,7,10);
      e[n].sample_frame=g_song_frame+ 80; e[n++].message = noteon(0,64,100);
      e[n].sample_frame=g_song_frame+200; e[n++].message = bend(0,0,70);
      e[n].sample_frame=g_song_frame+205; e[n++].message = noteon(0,67,100);
      e[n].sample_frame=g_song_frame+500; e[n++].message = cc(0,11,64);
      queue(e, n); }
    g_render_budget = 8;
    { int64_t before = g_render_cursor; (void)before; run_block(F); }
    CHECK(t_ndispatch == 6, "expected 6 dispatches, got %d", t_ndispatch);
    for (int i = 1; i < t_ndispatch; ++i)
        CHECK(t_dispatch_ts[i] >= t_dispatch_ts[i-1],
              "dispatch timestamps went backwards at %d (%lld < %lld)",
              i, (long long)t_dispatch_ts[i], (long long)t_dispatch_ts[i-1]);
    CHECK(t_frames_rendered == F, "rendered %d frames", t_frames_rendered);

    /* ---------- 8. governor: backs off under load, recovers when idle ------- */
    printf("8. adaptive budget governor\n");
    g_render_budget = 8; g_render_load_ema = -1.0;
    t_fake_cost_ms = 9.0;                /* 9 ms per render call vs 11.6 ms budget */
    for (int b = 0; b < 12; ++b) {
        clear_events();
        { ssw_scheduled_event e[64]; int n=0;
          for (int i = 0; i < 64; ++i) { e[n].sample_frame = g_song_frame + i*8; e[n++].message = cc(0,7,i); }
          queue(e, n); }
        run_block(F);
    }
    printf("   after overload: budget=%d load_ema=%.2f\n", g_render_budget, g_render_load_ema);
    CHECK(g_render_budget == 1, "governor did not back off to 1 (got %d)", g_render_budget);

    t_fake_cost_ms = 0.05;               /* idle */
    for (int b = 0; b < 24; ++b) {
        clear_events();
        { ssw_scheduled_event e[64]; int n=0;
          for (int i = 0; i < 64; ++i) { e[n].sample_frame = g_song_frame + i*8; e[n++].message = cc(0,7,i); }
          queue(e, n); }
        run_block(F);
    }
    printf("   after idle:     budget=%d load_ema=%.2f\n", g_render_budget, g_render_load_ema);
    CHECK(g_render_budget == 8, "governor did not recover to 8 (got %d)", g_render_budget);

    /* ---------- 9. frame accounting over a long run ------------------------- */
    printf("9. cursor/song-frame accounting over 500 blocks\n");
    t_fake_cost_ms = 0.05;
    g_song_frame = 0; g_render_cursor = 1; g_render_budget = 8; g_render_load_ema = -1.0;
    { int64_t cursor0 = g_render_cursor; int total = 0;
      for (int b = 0; b < 500; ++b) {
        clear_events();
        { ssw_scheduled_event e[40]; int n=0;
          for (int i = 0; i < 40; ++i) {
            int64_t f = g_song_frame + (i*13) % F;
            e[n].sample_frame=f; e[n++].message = (i%3==0) ? cc(0,7,i&0x7f) : noteon(i%16, 50+(i%20), 90);
          }
          queue(e, n); }
        run_block(F);
        total += t_frames_rendered;
        CHECK(t_bad_ptr == 0, "bad output pointer in block %d", b);
      }
      CHECK(total == 500*F, "total rendered %d, expected %d", total, 500*F);
      CHECK(g_song_frame == (int64_t)500*F, "song_frame %lld", (long long)g_song_frame);
      CHECK(g_render_cursor - cursor0 == (int64_t)500*F,
            "render cursor advanced %lld, expected %d", (long long)(g_render_cursor-cursor0), 500*F);
    }

    /* ---------- 10. non-512 block sizes ------------------------------------- */
    printf("10. alternate buffer sizes\n");
    for (int f = 64; f <= 2048; f *= 2) {
        clear_events();
        { ssw_scheduled_event e[128]; int n=0;
          for (int i = 0; i < 128; ++i) { e[n].sample_frame = g_song_frame + (i * (f/128 ? f/128 : 1)) % f; e[n++].message = cc(0,11,i&0x7f); }
          queue(e, n); }
        g_render_budget = 8;
        run_block(f);
        CHECK(t_frames_rendered == f, "frames=%d rendered %d", f, t_frames_rendered);
        CHECK(t_render_calls <= 8, "frames=%d used %d renders", f, t_render_calls);
        CHECK(t_misaligned_segments == 0, "frames=%d produced misaligned segments", f);
    }

    /* ---------- 11. selector ordering: THE note-eating regression ---------- */
    printf("11. selector events are ordered against note admission\n");
    /* A note-on followed by a bank/program change on the same channel must have
     * a render between them. voice.c shards notes by key and channel events by
     * channel, so without that render the two sit in different worker queues,
     * are consumed concurrently, and the note resolves against the wrong
     * program -> no region -> silently dropped note. */
    clear_events();
    { ssw_scheduled_event e[8]; int n=0;
      e[n].sample_frame=g_song_frame+ 10; e[n++].message = noteon(5,60,100);
      e[n].sample_frame=g_song_frame+ 20; e[n++].message = cc(5,0,1);    /* bank MSB */
      e[n].sample_frame=g_song_frame+ 21; e[n++].message = pc(5,42);     /* program  */
      e[n].sample_frame=g_song_frame+ 30; e[n++].message = noteon(5,64,100);
      queue(e, n); }
    g_render_budget = 8;
    run_block(F);
    { int seen_note = 0, ok = 1;
      for (int i = 0; i < t_ntrace; ++i) {
        if (t_trace[i] == TR_RENDER) { seen_note = 0; continue; }
        if ((t_trace[i] & 0xf0u) == 0x90u) { seen_note = 1; continue; }
        if (ssw_is_selector_event(t_trace[i]) && seen_note) ok = 0;
      }
      CHECK(ok, "a selector was admitted after a note-on with no render between");
      CHECK(t_render_calls >= 2, "expected an ordering boundary, got %d renders", t_render_calls); }

    /* selector with NO note admitted since the last boundary: no split needed */
    printf("12. selector with no pending notes costs nothing\n");
    clear_events();
    { ssw_scheduled_event e[400]; int n=0;
      for (int i = 0; i < 100; ++i) {
        int64_t f = g_song_frame + i*5;
        e[n].sample_frame=f; e[n++].message = cc(0,0,i&0x7f);
        e[n].sample_frame=f; e[n++].message = cc(0,32,i&0x7f);
        e[n].sample_frame=f; e[n++].message = cc(0,101,0);
        e[n].sample_frame=f; e[n++].message = pc(0,i&0x7f);
      }
      queue(e, n); }
    g_render_budget = 8;
    run_block(F);
    CHECK(t_render_calls == 1, "selector spam without notes used %d renders", t_render_calls);

    /* selector on a DIFFERENT channel than the pending note: no boundary */
    printf("13. cross-channel selector does not force a boundary\n");
    clear_events();
    { ssw_scheduled_event e[8]; int n=0;
      e[n].sample_frame=g_song_frame+ 10; e[n++].message = noteon(2,60,100);
      e[n].sample_frame=g_song_frame+ 20; e[n++].message = pc(9,42);
      e[n].sample_frame=g_song_frame+ 30; e[n++].message = pc(11,7);
      queue(e, n); }
    g_render_budget = 8;
    run_block(F);
    CHECK(t_render_calls == 1, "cross-channel selectors used %d renders", t_render_calls);

    /* ordering boundaries must survive a starved budget */
    printf("14. ordering boundaries ignore the load budget\n");
    clear_events();
    { ssw_scheduled_event e[16]; int n=0;
      for (int i = 0; i < 6; ++i) {
        e[n].sample_frame=g_song_frame+ i*80;    e[n++].message = noteon(1,60+i,100);
        e[n].sample_frame=g_song_frame+ i*80+10; e[n++].message = pc(1,i);
      }
      queue(e, n); }
    g_render_budget = 1;              /* fully loaded */
    run_block(F);
    { int seen_note = 0, ok = 1;
      for (int i = 0; i < t_ntrace; ++i) {
        if (t_trace[i] == TR_RENDER) { seen_note = 0; continue; }
        if ((t_trace[i] & 0xf0u) == 0x90u) { seen_note = 1; continue; }
        if (ssw_is_selector_event(t_trace[i]) && seen_note) ok = 0;
      }
      CHECK(ok, "budget=1 dropped a required ordering boundary"); }
    CHECK(t_frames_rendered == F, "budget=1 ordering path rendered %d frames", t_frames_rendered);

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL CHECKS PASSED", fails);
    return fails ? 1 : 0;
}
