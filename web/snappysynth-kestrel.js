/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * WasmiSynth GPU voice engine after Kestrel (https://github.com/thtstickyboi/kestrel-midi,
 * MPL-2.0). HANDOFF sec. 109. */

"use strict";

const KGPU_WG = 64;
const KGPU_TILE = 16;
const KGPU_FIELDS = 34;
const KGPU_SPAWN_WORDS = 32;
const KGPU_STATE_WORDS = 16;
const KGPU_NWG_MAX = 512;
const KGPU_SLOTS = 2048;
const KGPU_CHAN_WORDS = 4;
const KGPU_MAX_CUTS = 4096;
const KGPU_MAX_TILES = 64;
const KGPU_UNIFORM_BYTES = 64;

const KGPU_COMMON = /* wgsl */ `
const WG: u32 = 64u;
const TILE: u32 = 16u;
const TILES_PER_CHAN: u32 = 4u;
const KEY_SLOTS: u32 = 2048u;
const RUN_BASE: u32 = 4097u;
const NO_RELEASE: u32 = 0xFFFFFFFFu;
const SILENCE: f32 = 1.0 / 65536.0;
const BEND_SHIFT: u32 = 24u;

const F_PHASE_HI: u32 = 0u;
const F_PHASE_LO: u32 = 1u;
const F_STEP_HI: u32 = 2u;
const F_STEP_LO: u32 = 3u;
const F_SMP_BASE: u32 = 4u;
const F_SMP_INFO: u32 = 5u;
const F_LOOP_START: u32 = 6u;
const F_LOOP_END: u32 = 7u;
const F_SMP_END: u32 = 8u;
const F_SLOT: u32 = 9u;
const F_ORDINAL: u32 = 10u;
const F_START_REL: u32 = 11u;
const F_HOLD: u32 = 12u;
const F_BIRTH: u32 = 13u;
const F_STAGE: u32 = 14u;
const F_LEVEL: u32 = 15u;
const F_GAIN_L: u32 = 16u;
const F_GAIN_R: u32 = 17u;
const F_ATTACK: u32 = 18u;
const F_DECAY: u32 = 19u;
const F_SUSTAIN: u32 = 20u;
const F_RELEASE: u32 = 21u;
const F_B0: u32 = 22u;
const F_B1: u32 = 23u;
const F_B2: u32 = 24u;
const F_A1: u32 = 25u;
const F_A2: u32 = 26u;
const F_Z1L: u32 = 27u;
const F_Z2L: u32 = 28u;
const F_Z1R: u32 = 29u;
const F_Z2R: u32 = 30u;
const F_STOP_REL: u32 = 31u;
const F_CGAIN_L: u32 = 32u;
const F_CGAIN_R: u32 = 33u;
const FIELDS: u32 = 34u;

const ST_ATTACK: u32 = 0u;
const ST_DECAY: u32 = 1u;
const ST_SUSTAIN: u32 = 2u;
const ST_RELEASE: u32 = 3u;
const ST_DEAD: u32 = 4u;
const ST_OFFED: u32 = 0x100u;

const INFO_LOOP: u32 = 4u;
const INFO_FILTER: u32 = 8u;

const S_LIVE: u32 = 0u;
const S_LIVE_NEW: u32 = 1u;
const S_K: u32 = 2u;
const S_BYTE: u32 = 3u;
const S_PREFIX: u32 = 4u;
const S_THRESH: u32 = 5u;
const S_STEAL: u32 = 6u;
const S_SPAWN: u32 = 7u;
const S_STOLEN: u32 = 8u;
const S_DROPPED: u32 = 9u;

struct U {
    block_frames: u32, tiles: u32, slots: u32, cap: u32,
    spawn_n: u32, max_steal: u32, nwg: u32, block: u32,
    gain: f32, cut_count: u32, cut_mask: u32, fade: u32,
    pad0: u32, pad1: u32, pad2: u32, pad3: u32,
}
@group(0) @binding(0) var<uniform> u: U;
`;

// Steal: k = voices over the pool cap, at most max_steal; the k lowest keys (envelope
// level times gain, then oldest) are found by a radix select over the four key bytes.
const KGPU_SELECT = KGPU_COMMON + /* wgsl */ `
@group(0) @binding(1) var<storage, read_write> voices: array<u32>;
@group(0) @binding(2) var<storage, read_write> state: array<u32>;
@group(0) @binding(3) var<storage, read_write> hist: array<atomic<u32>>;

fn steal_key(i: u32) -> u32 {
    let c = u.slots;
    let level = bitcast<f32>(voices[F_LEVEL * c + i]);
    let gl = abs(bitcast<f32>(voices[F_GAIN_L * c + i]) * bitcast<f32>(voices[F_CGAIN_L * c + i]));
    let gr = abs(bitcast<f32>(voices[F_GAIN_R * c + i]) * bitcast<f32>(voices[F_CGAIN_R * c + i]));
    let a = clamp(level * max(gl, gr), 0.0, 1.0);
    let q = u32(sqrt(a) * 65535.0);
    let age = min(u.block - voices[F_BIRTH * c + i], 0xFFFFu);
    return (q << 16u) | (0xFFFFu - age);
}

@compute @workgroup_size(1)
fn plan() {
    let live = state[S_LIVE];
    let n = min(u.spawn_n, u.cap);
    var k = 0u;
    if (live + n > u.cap) { k = min(live + n - u.cap, min(live, u.max_steal)); }
    let room = u.cap - min(live - k, u.cap);
    let spawn = min(n, room);
    state[S_STEAL] = k;
    state[S_K] = k;
    state[S_SPAWN] = spawn;
    state[S_BYTE] = 3u;
    state[S_PREFIX] = 0u;
    state[S_THRESH] = 0u;
    state[S_STOLEN] = state[S_STOLEN] + k;
    state[S_DROPPED] = state[S_DROPPED] + (u.spawn_n - spawn);
}

@compute @workgroup_size(256)
fn clear(@builtin(global_invocation_id) gid: vec3<u32>) {
    atomicStore(&hist[gid.x], 0u);
    if (gid.x == 0u) { atomicStore(&hist[256u], 0u); }
}

@compute @workgroup_size(64)
fn histogram(@builtin(global_invocation_id) gid: vec3<u32>, @builtin(num_workgroups) nwg: vec3<u32>) {
    if (state[S_STEAL] == 0u) { return; }
    let live = state[S_LIVE];
    let b = state[S_BYTE];
    let prefix = state[S_PREFIX];
    for (var i = gid.x; i < live; i = i + nwg.x * WG) {
        let key = steal_key(i);
        if (b == 3u || (key >> ((b + 1u) * 8u)) == (prefix >> ((b + 1u) * 8u))) {
            atomicAdd(&hist[(key >> (b * 8u)) & 255u], 1u);
        }
    }
}

@compute @workgroup_size(1)
fn refine() {
    if (state[S_STEAL] == 0u) { return; }
    let b = state[S_BYTE];
    let k = state[S_K];
    var cum = 0u;
    var chosen = 255u;
    for (var i = 0u; i < 256u; i = i + 1u) {
        let h = atomicLoad(&hist[i]);
        if (cum + h >= k) { chosen = i; break; }
        cum = cum + h;
    }
    state[S_PREFIX] = state[S_PREFIX] | (chosen << (b * 8u));
    state[S_K] = k - cum;
    if (b == 0u) { state[S_THRESH] = state[S_PREFIX]; } else { state[S_BYTE] = b - 1u; }
}

// Marks the chosen voices to fade out inside this block, starting at staggered frames.
@compute @workgroup_size(64)
fn mark(@builtin(global_invocation_id) gid: vec3<u32>, @builtin(num_workgroups) nwg: vec3<u32>) {
    if (state[S_STEAL] == 0u) { return; }
    let live = state[S_LIVE];
    let t = state[S_THRESH];
    let ties = state[S_K];
    let span = max(u.block_frames - u.fade, 1u);
    for (var i = gid.x; i < live; i = i + nwg.x * WG) {
        let key = steal_key(i);
        var take = key < t;
        if (key == t) { take = atomicAdd(&hist[256u], 1u) < ties; }
        if (take) {
            voices[F_STOP_REL * u.slots + i] = ((i * 2654435761u) ^ u.block) % span + 1u;
        }
    }
}
`;

const KGPU_SPAWN = KGPU_COMMON + /* wgsl */ `
@group(0) @binding(1) var<storage, read> cmds: array<u32>;
@group(0) @binding(2) var<storage, read_write> voices: array<u32>;
@group(0) @binding(3) var<storage, read_write> state: array<u32>;

@compute @workgroup_size(64)
fn spawn(@builtin(global_invocation_id) gid: vec3<u32>, @builtin(num_workgroups) nwg: vec3<u32>) {
    let n = state[S_SPAWN];
    let base = state[S_LIVE];
    let c = u.slots;
    for (var i = gid.x; i < n; i = i + nwg.x * WG) {
        let dst = base + i;
        if (dst >= c) { break; }
        let o = i * 32u;
        for (var k = 0u; k < 14u; k = k + 1u) { voices[k * c + dst] = cmds[o + k]; }
        for (var k = 16u; k < 27u; k = k + 1u) { voices[k * c + dst] = cmds[o + k]; }
        voices[F_STAGE * c + dst] = ST_ATTACK;
        voices[F_LEVEL * c + dst] = bitcast<u32>(0.0);
        voices[F_Z1L * c + dst] = 0u;
        voices[F_Z2L * c + dst] = 0u;
        voices[F_Z1R * c + dst] = 0u;
        voices[F_Z2R * c + dst] = 0u;
        voices[F_STOP_REL * c + dst] = 0u;
        voices[F_CGAIN_L * c + dst] = bitcast<u32>(-1.0);
        voices[F_CGAIN_R * c + dst] = bitcast<u32>(-1.0);
    }
}

@compute @workgroup_size(1)
fn commit() {
    state[S_LIVE] = min(state[S_LIVE] + state[S_SPAWN], u.slots);
}
`;

// One thread per voice, TILE frames at a time; each workgroup sums its voices into its own
// column of partials, so the mix never depends on scheduling.
const KGPU_RENDER = KGPU_COMMON + /* wgsl */ `
@group(0) @binding(1) var<storage, read> pool: array<u32>;
@group(0) @binding(2) var<storage, read> gates: array<u32>;
@group(0) @binding(3) var<storage, read_write> voices: array<u32>;
@group(0) @binding(4) var<storage, read_write> partials: array<f32>;
@group(0) @binding(5) var<storage, read> state: array<u32>;
@group(0) @binding(6) var<storage, read> chan: array<u32>;
@group(0) @binding(7) var<storage, read> cuts: array<u32>;

const M: u32 = TILE * 2u;
const PER_LANE: u32 = WG / M;
var<workgroup> sh: array<f32, 2048>;
var<workgroup> sh2: array<f32, 64>;
var<workgroup> wg_live: u32;

fn sample16(index: u32) -> f32 {
    let w = pool[index >> 1u];
    let bits = select(w & 0xffffu, w >> 16u, (index & 1u) == 1u);
    return f32(bitcast<i32>(bits << 16u) >> 16u);
}

fn mul32(a: u32, b: u32) -> vec2<u32> {
    let a0 = a & 0xFFFFu;
    let a1 = a >> 16u;
    let b0 = b & 0xFFFFu;
    let b1 = b >> 16u;
    let p00 = a0 * b0;
    let p01 = a0 * b1;
    let p10 = a1 * b0;
    let p11 = a1 * b1;
    let mid1 = p10 + (p00 >> 16u);
    let mid2 = p01 + (mid1 & 0xFFFFu);
    let lo = (mid2 << 16u) | (p00 & 0xFFFFu);
    let hi = p11 + (mid1 >> 16u) + (mid2 >> 16u);
    return vec2<u32>(lo, hi);
}

// (hi:lo) * factor / 2^24, as (hi, lo).
fn scale64(hi: u32, lo: u32, factor: u32) -> vec2<u32> {
    let pl = mul32(lo, factor);
    let ph = mul32(hi, factor);
    let b = pl.y + ph.x;
    var c = ph.y;
    if (b < ph.x) { c = c + 1u; }
    if ((c >> BEND_SHIFT) != 0u) { return vec2<u32>(0xFFFFFFFFu, 0xFFFFFFFFu); }
    let r_lo = (pl.x >> BEND_SHIFT) | (b << (32u - BEND_SHIFT));
    let r_hi = (b >> BEND_SHIFT) | (c << (32u - BEND_SHIFT));
    return vec2<u32>(r_hi, r_lo);
}

fn add64(hi: u32, lo: u32, add_hi: u32, add_lo: u32) -> vec2<u32> {
    let nlo = lo + add_lo;
    var carry = 0u;
    if (nlo < lo) { carry = 1u; }
    return vec2<u32>(hi + add_hi + carry, nlo);
}

// Frame of this block where the voice's note-off arrives: the first run of the slot
// whose note-off count reaches the voice's ordinal (FIFO, as the CPU engine).
fn release_frame_of(slot: u32, ordinal: u32) -> u32 {
    if (ordinal <= gates[slot]) { return 0u; }
    let s0 = gates[KEY_SLOTS + slot];
    let s1 = gates[KEY_SLOTS + slot + 1u];
    if (s1 <= s0 || gates[RUN_BASE + (s1 - 1u) * 2u] < ordinal) { return NO_RELEASE; }
    var lo = s0;
    var hi = s1 - 1u;
    while (lo < hi) {
        let mid = lo + (hi - lo) / 2u;
        if (gates[RUN_BASE + mid * 2u] >= ordinal) { hi = mid; } else { lo = mid + 1u; }
    }
    return gates[RUN_BASE + lo * 2u + 1u];
}

fn reduce_into_partials(tid: u32, wg: u32, nwg: u32, first_sample: u32) {
    let lane = tid / PER_LANE;
    let chunk = tid % PER_LANE;
    var acc = 0.0;
    for (var k = 0u; k < M; k = k + 1u) { acc = acc + sh[lane * WG + chunk + k * PER_LANE]; }
    sh2[lane * PER_LANE + chunk] = acc;
    workgroupBarrier();
    if (tid < M) {
        var total = 0.0;
        for (var k = 0u; k < PER_LANE; k = k + 1u) { total = total + sh2[tid * PER_LANE + k]; }
        let j = first_sample + tid;
        partials[j * nwg + wg] = partials[j * nwg + wg] + total;
    }
}

@compute @workgroup_size(64)
fn main(@builtin(local_invocation_index) tid: u32, @builtin(workgroup_id) wid: vec3<u32>) {
    if (tid == 0u) { wg_live = state[S_LIVE]; }
    let live = workgroupUniformLoad(&wg_live);
    let wg = wid.x;
    let nwg = u.nwg;
    let c = u.slots;
    for (var j = tid; j < u.tiles * M; j = j + WG) { partials[j * nwg + wg] = 0.0; }
    workgroupBarrier();

    var batch = wg;
    loop {
        if (batch * WG >= live) { break; }
        let v = batch * WG + tid;
        let here = v < live;

        var phase_hi = 0u;
        var phase_lo = 0u;
        var base_hi = 0u;
        var base_lo = 0u;
        var step_hi = 0u;
        var step_lo = 0u;
        var smp_base = 0u;
        var info = 0u;
        var ls = 0u;
        var le = 1u;
        var send = 1u;
        var slot = 0u;
        var start_rel = 0u;
        var hold = 0u;
        var birth = 0u;
        var stage = ST_DEAD;
        var offed = false;
        var level = 0.0;
        var gl = 0.0;
        var gr = 0.0;
        var att = 0.0;
        var dec = 1.0;
        var sus = 0.0;
        var rel = 0.0;
        var b0 = 0.0;
        var b1 = 0.0;
        var b2 = 0.0;
        var a1 = 0.0;
        var a2 = 0.0;
        var z1l = 0.0;
        var z2l = 0.0;
        var z1r = 0.0;
        var z2r = 0.0;
        var stop_rel = 0u;
        var cgl = 0.0;
        var cgr = 0.0;
        var dgl = 0.0;
        var dgr = 0.0;
        var rf = NO_RELEASE;
        var sustain_on = false;

        if (here) {
            phase_hi = voices[F_PHASE_HI * c + v];
            phase_lo = voices[F_PHASE_LO * c + v];
            base_hi = voices[F_STEP_HI * c + v];
            base_lo = voices[F_STEP_LO * c + v];
            step_hi = base_hi;
            step_lo = base_lo;
            smp_base = voices[F_SMP_BASE * c + v];
            info = voices[F_SMP_INFO * c + v];
            ls = voices[F_LOOP_START * c + v];
            le = voices[F_LOOP_END * c + v];
            send = voices[F_SMP_END * c + v];
            slot = voices[F_SLOT * c + v];
            start_rel = voices[F_START_REL * c + v];
            hold = voices[F_HOLD * c + v];
            birth = voices[F_BIRTH * c + v];
            let sw = voices[F_STAGE * c + v];
            stage = sw & 0xFFu;
            offed = (sw & ST_OFFED) != 0u;
            level = bitcast<f32>(voices[F_LEVEL * c + v]);
            gl = bitcast<f32>(voices[F_GAIN_L * c + v]);
            gr = bitcast<f32>(voices[F_GAIN_R * c + v]);
            att = bitcast<f32>(voices[F_ATTACK * c + v]);
            dec = bitcast<f32>(voices[F_DECAY * c + v]);
            sus = bitcast<f32>(voices[F_SUSTAIN * c + v]);
            rel = bitcast<f32>(voices[F_RELEASE * c + v]);
            if ((info & INFO_FILTER) != 0u) {
                b0 = bitcast<f32>(voices[F_B0 * c + v]);
                b1 = bitcast<f32>(voices[F_B1 * c + v]);
                b2 = bitcast<f32>(voices[F_B2 * c + v]);
                a1 = bitcast<f32>(voices[F_A1 * c + v]);
                a2 = bitcast<f32>(voices[F_A2 * c + v]);
                z1l = bitcast<f32>(voices[F_Z1L * c + v]);
                z2l = bitcast<f32>(voices[F_Z2L * c + v]);
                z1r = bitcast<f32>(voices[F_Z1R * c + v]);
                z2r = bitcast<f32>(voices[F_Z2R * c + v]);
            }
            stop_rel = voices[F_STOP_REL * c + v];
            cgl = bitcast<f32>(voices[F_CGAIN_L * c + v]);
            cgr = bitcast<f32>(voices[F_CGAIN_R * c + v]);
            if (!offed && stage < ST_RELEASE) {
                rf = release_frame_of(slot & 0x7FFu, voices[F_ORDINAL * c + v]);
            }
            let ch = (slot >> 7u) & 15u;
            if (((u.cut_mask >> ch) & 1u) != 0u) {
                let grp = slot >> 16u;
                for (var k = 0u; k < u.cut_count; k = k + 1u) {
                    let cf = cuts[k * 3u + 2u];
                    let cg = cuts[k * 3u + 1u];
                    if (cuts[k * 3u] == ch && (cg == 0u || cg == grp) && (birth != u.block || start_rel < cf)) {
                        let s = min(cf, u.block_frames - u.fade) + 1u;
                        if (stop_rel == 0u || s < stop_rel) { stop_rel = s; }
                    }
                }
            }
        }

        let ch = (slot >> 7u) & 15u;
        let chans = info & 3u;
        let loop_on = (info & INFO_LOOP) != 0u;
        let filter_on = (info & INFO_FILTER) != 0u;
        let newborn = birth == u.block;

        for (var tile = 0u; tile < u.tiles; tile = tile + 1u) {
            if (here && (tile % TILES_PER_CHAN) == 0u) {
                let row = ((tile / TILES_PER_CHAN) * 16u + ch) * 4u;
                let tl = bitcast<f32>(chan[row]);
                let tr = bitcast<f32>(chan[row + 1u]);
                let st = scale64(base_hi, base_lo, chan[row + 2u]);
                step_hi = st.x;
                step_lo = st.y;
                sustain_on = (chan[row + 3u] & 1u) != 0u;
                if (cgl < 0.0 || (newborn && tile * TILE <= start_rel)) {
                    cgl = tl;
                    cgr = tr;
                    dgl = 0.0;
                    dgr = 0.0;
                } else {
                    dgl = (tl - cgl) * (1.0 / 64.0);
                    dgr = (tr - cgr) * (1.0 / 64.0);
                }
            }
            for (var i = 0u; i < TILE; i = i + 1u) {
                let f = tile * TILE + i;
                var yl = 0.0;
                var yr = 0.0;
                if (here && stage < ST_DEAD && f >= start_rel && f < u.block_frames) {
                    if (!offed && f >= rf) { offed = true; }
                    if (offed && !sustain_on && stage < ST_RELEASE) { stage = ST_RELEASE; }
                    if (!loop_on && phase_hi >= send) {
                        stage = ST_DEAD;
                    } else {
                        let idx = phase_hi;
                        var nx = idx + 1u;
                        if (loop_on && nx >= le) { nx = ls; }
                        if (!loop_on && nx >= send) { nx = idx; }
                        let fr = f32(phase_lo) * (1.0 / 4294967296.0);
                        let o0 = smp_base + idx * chans;
                        let o1 = smp_base + nx * chans;
                        let l0 = sample16(o0);
                        var sl = l0 + (sample16(o1) - l0) * fr;
                        var sr = sl;
                        if (chans == 2u) {
                            let r0 = sample16(o0 + 1u);
                            sr = r0 + (sample16(o1 + 1u) - r0) * fr;
                        }
                        if (filter_on) {
                            let fl = z1l + sl * b0;
                            z1l = z2l + sl * b1 - fl * a1;
                            z2l = sl * b2 - fl * a2;
                            if (chans == 2u) {
                                let fq = z1r + sr * b0;
                                z1r = z2r + sr * b1 - fq * a1;
                                z2r = sr * b2 - fq * a2;
                                sr = fq;
                            } else {
                                sr = fl;
                            }
                            sl = fl;
                        }
                        var amp = level * u.gain;
                        if (stop_rel != 0u && f + 1u >= stop_rel) {
                            let d = f + 1u - stop_rel;
                            if (d >= u.fade) {
                                amp = 0.0;
                                stage = ST_DEAD;
                            } else {
                                amp = amp * (1.0 - f32(d) / f32(u.fade));
                            }
                        }
                        yl = sl * amp * gl * cgl;
                        yr = sr * amp * gr * cgr;
                        if (stage == ST_ATTACK) {
                            level = level + att;
                            if (level >= 1.0) { level = 1.0; stage = ST_DECAY; }
                        } else if (stage == ST_DECAY) {
                            if (hold > 0u) {
                                hold = hold - 1u;
                            } else {
                                level = level * dec;
                                if (level <= sus) { level = sus; stage = ST_SUSTAIN; }
                            }
                        } else if (stage == ST_RELEASE) {
                            level = level * rel;
                            if (level <= SILENCE) { stage = ST_DEAD; }
                        }
                        let np = add64(phase_hi, phase_lo, step_hi, step_lo);
                        phase_hi = np.x;
                        phase_lo = np.y;
                        if (loop_on && phase_hi >= le) {
                            let span = max(le - ls, 1u);
                            phase_hi = phase_hi - span;
                            if (phase_hi >= le) { phase_hi = ls + (phase_hi - ls) % span; }
                        }
                    }
                }
                sh[(i * 2u) * WG + tid] = yl;
                sh[(i * 2u + 1u) * WG + tid] = yr;
                cgl = cgl + dgl;
                cgr = cgr + dgr;
            }
            workgroupBarrier();
            reduce_into_partials(tid, wg, nwg, tile * M);
        }

        if (here) {
            voices[F_PHASE_HI * c + v] = phase_hi;
            voices[F_PHASE_LO * c + v] = phase_lo;
            voices[F_STAGE * c + v] = stage | select(0u, ST_OFFED, offed);
            voices[F_LEVEL * c + v] = bitcast<u32>(level);
            voices[F_HOLD * c + v] = hold;
            voices[F_START_REL * c + v] = 0u;
            voices[F_STOP_REL * c + v] = 0u;
            voices[F_CGAIN_L * c + v] = bitcast<u32>(cgl);
            voices[F_CGAIN_R * c + v] = bitcast<u32>(cgr);
            if (filter_on) {
                voices[F_Z1L * c + v] = bitcast<u32>(z1l);
                voices[F_Z2L * c + v] = bitcast<u32>(z2l);
                voices[F_Z1R * c + v] = bitcast<u32>(z1r);
                voices[F_Z2R * c + v] = bitcast<u32>(z2r);
            }
        }
        batch = batch + nwg;
    }
}
`;

// One thread per output sample sums the workgroup columns in order.
const KGPU_REDUCE = KGPU_COMMON + /* wgsl */ `
@group(0) @binding(1) var<storage, read> partials: array<f32>;
@group(0) @binding(2) var<storage, read_write> out_block: array<f32>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let j = gid.x;
    if (j >= u.block_frames * 2u) { return; }
    var sum = 0.0;
    for (var w = 0u; w < u.nwg; w = w + 1u) { sum = sum + partials[j * u.nwg + w]; }
    out_block[j] = sum;
}
`;

// Drops dead voices: exclusive scan of the live flags, then a scatter into the other
// voice buffer.
const KGPU_COMPACT = KGPU_COMMON + /* wgsl */ `
@group(0) @binding(1) var<storage, read> voices: array<u32>;
@group(0) @binding(2) var<storage, read_write> voices_out: array<u32>;
@group(0) @binding(3) var<storage, read_write> scan: array<u32>;
@group(0) @binding(4) var<storage, read_write> block_sums: array<u32>;
@group(0) @binding(5) var<storage, read_write> state: array<u32>;

var<workgroup> sh_scan: array<u32, 64>;
var<workgroup> wg_live: u32;

fn is_alive(i: u32) -> bool {
    return (voices[F_STAGE * u.slots + i] & 0xFFu) != ST_DEAD;
}

@compute @workgroup_size(64)
fn scan_local(@builtin(local_invocation_index) tid: u32, @builtin(workgroup_id) wid: vec3<u32>,
              @builtin(num_workgroups) nwg: vec3<u32>) {
    if (tid == 0u) { wg_live = state[S_LIVE]; }
    let live = workgroupUniformLoad(&wg_live);
    let blocks = (live + WG - 1u) / WG;
    var block = wid.x;
    loop {
        if (block >= blocks) { break; }
        let i = block * WG + tid;
        var alive = 0u;
        if (i < live && is_alive(i)) { alive = 1u; }
        sh_scan[tid] = alive;
        workgroupBarrier();
        var offset = 1u;
        loop {
            if (offset >= WG) { break; }
            var t = 0u;
            if (tid >= offset) { t = sh_scan[tid - offset]; }
            workgroupBarrier();
            sh_scan[tid] = sh_scan[tid] + t;
            workgroupBarrier();
            offset = offset << 1u;
        }
        if (i < u.slots) { scan[i] = sh_scan[tid] - alive; }
        if (tid == WG - 1u) { block_sums[block] = sh_scan[tid]; }
        workgroupBarrier();
        block = block + nwg.x;
    }
}

@compute @workgroup_size(64)
fn scan_blocks(@builtin(local_invocation_index) tid: u32) {
    if (tid == 0u) { wg_live = state[S_LIVE]; }
    let blocks = (workgroupUniformLoad(&wg_live) + WG - 1u) / WG;
    var running = 0u;
    var chunk = 0u;
    loop {
        if (chunk * WG >= blocks) { break; }
        let i = chunk * WG + tid;
        var v = 0u;
        if (i < blocks) { v = block_sums[i]; }
        sh_scan[tid] = v;
        workgroupBarrier();
        var offset = 1u;
        loop {
            if (offset >= WG) { break; }
            var t = 0u;
            if (tid >= offset) { t = sh_scan[tid - offset]; }
            workgroupBarrier();
            sh_scan[tid] = sh_scan[tid] + t;
            workgroupBarrier();
            offset = offset << 1u;
        }
        let total = sh_scan[WG - 1u];
        if (i < blocks) { block_sums[i] = running + sh_scan[tid] - v; }
        workgroupBarrier();
        running = running + total;
        chunk = chunk + 1u;
    }
    if (tid == 0u) { state[S_LIVE_NEW] = running; }
}

@compute @workgroup_size(64)
fn scatter(@builtin(global_invocation_id) gid: vec3<u32>, @builtin(num_workgroups) nwg: vec3<u32>) {
    let c = u.slots;
    let live = state[S_LIVE];
    for (var i = gid.x; i < live; i = i + nwg.x * WG) {
        if (is_alive(i)) {
            let dst = block_sums[i / WG] + scan[i];
            if (dst < c) {
                for (var f = 0u; f < FIELDS; f = f + 1u) { voices_out[f * c + dst] = voices[f * c + i]; }
            }
        }
    }
}

@compute @workgroup_size(1)
fn commit() {
    state[S_LIVE] = state[S_LIVE_NEW];
}
`;

class WasmiKestrelGpu {
    static async create(capacity, maxSteal, blockFrames) {
        if (!self.navigator || !self.navigator.gpu)
            return { gpu: null, reason: "WebGPU is not available in this browser" };
        const adapter = await self.navigator.gpu.requestAdapter({ powerPreference: "high-performance" });
        if (!adapter)
            return { gpu: null, reason: "no WebGPU adapter" };
        const limits = adapter.limits;
        const device = await adapter.requestDevice({
            requiredLimits: {
                maxStorageBufferBindingSize: limits.maxStorageBufferBindingSize,
                maxBufferSize: limits.maxBufferSize,
                maxStorageBuffersPerShaderStage: Math.min(8, limits.maxStorageBuffersPerShaderStage)
            }
        });
        const binding = Math.min(device.limits.maxStorageBufferBindingSize, device.limits.maxBufferSize);
        let cap = Math.max(1, capacity | 0);
        let steal = Math.max(1, Math.min(cap, maxSteal | 0));
        while ((cap + steal) * KGPU_FIELDS * 4 > binding && cap > 1024) {
            cap = Math.floor(cap / 2);
            steal = Math.max(1, Math.min(cap, steal));
        }
        const gpu = new WasmiKestrelGpu(device, cap, steal, blockFrames);
        const error = await gpu.compileErrors();
        if (error) { gpu.destroy(); return { gpu: null, reason: error }; }
        return { gpu, reason: "" };
    }

    constructor(device, capacity, maxSteal, blockFrames) {
        this.device = device;
        this.lost = false;
        device.lost.then(info => { this.lost = true; this.lostReason = (info && info.message) || "device lost"; });
        device.onuncapturederror = event => {
            if (!this.lost) this.lostReason = (event && event.error && event.error.message) || "GPU validation error";
            this.lost = true;
        };
        this.capacity = capacity;
        this.maxSteal = maxSteal;
        this.slots = capacity + maxSteal;
        this.blockFrames = Math.max(KGPU_TILE, Math.min(KGPU_MAX_TILES * 64, blockFrames | 0));
        this.maxSpawn = Math.min(this.slots, 131072);
        this.parity = 0;
        this.block = 0;
        this.generation = 0;
        this.liveKnown = 0;
        this.spawnSubmitted = 0;
        this.spawnSeen = 0;
        this.stats = { live: 0, stolen: 0, dropped: 0 };

        const modules = {};
        this.modules = modules;
        for (const [name, code] of [["select", KGPU_SELECT], ["spawn", KGPU_SPAWN], ["render", KGPU_RENDER],
                                    ["reduce", KGPU_REDUCE], ["compact", KGPU_COMPACT]])
            modules[name] = device.createShaderModule({ code, label: "kestrel " + name });
        // Explicit layouts: an "auto" layout would drop the bindings an entry point does not use.
        const kinds = { select: "usss", spawn: "urss", render: "urrssrrr", reduce: "urs", compact: "urssss" };
        this.layouts = {};
        const pipelineLayouts = {};
        for (const [name, kind] of Object.entries(kinds)) {
            this.layouts[name] = device.createBindGroupLayout({
                entries: [...kind].map((k, binding) => ({
                    binding, visibility: GPUShaderStage.COMPUTE,
                    buffer: { type: k === "u" ? "uniform" : k === "r" ? "read-only-storage" : "storage" }
                }))
            });
            pipelineLayouts[name] = device.createPipelineLayout({ bindGroupLayouts: [this.layouts[name]] });
        }
        const pipe = (module, entryPoint) => device.createComputePipeline({
            layout: pipelineLayouts[module], compute: { module: modules[module], entryPoint }, label: module + "." + entryPoint });
        this.pipes = {
            plan: pipe("select", "plan"), clear: pipe("select", "clear"), histogram: pipe("select", "histogram"),
            refine: pipe("select", "refine"), mark: pipe("select", "mark"),
            spawn: pipe("spawn", "spawn"), spawnCommit: pipe("spawn", "commit"),
            render: pipe("render", "main"), reduce: pipe("reduce", "main"),
            scanLocal: pipe("compact", "scan_local"), scanBlocks: pipe("compact", "scan_blocks"),
            scatter: pipe("compact", "scatter"), compactCommit: pipe("compact", "commit")
        };

        const storage = GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST;
        const make = (size, usage) => device.createBuffer({ size: Math.max(16, Math.ceil(size / 4) * 4), usage });
        this.uniform = make(KGPU_UNIFORM_BYTES, GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST);
        this.voices = [make(this.slots * KGPU_FIELDS * 4, GPUBufferUsage.STORAGE),
                       make(this.slots * KGPU_FIELDS * 4, GPUBufferUsage.STORAGE)];
        this.state = make(KGPU_STATE_WORDS * 4, storage | GPUBufferUsage.COPY_SRC);
        this.hist = make(257 * 4, GPUBufferUsage.STORAGE);
        this.cmds = make(this.maxSpawn * KGPU_SPAWN_WORDS * 4, storage);
        this.gateWords = KGPU_SLOTS * 2 + 1 + 65536;
        this.gates = make(this.gateWords * 4, storage);
        this.cuts = make(KGPU_MAX_CUTS * 3 * 4, storage);
        this.chan = make(KGPU_MAX_TILES * 16 * KGPU_CHAN_WORDS * 4, storage);
        this.scan = make(this.slots * 4, GPUBufferUsage.STORAGE);
        this.blockSums = make(Math.ceil(this.slots / KGPU_WG) * 4, GPUBufferUsage.STORAGE);
        const tilesMax = Math.ceil(this.blockFrames / KGPU_TILE);
        this.partials = make(tilesMax * KGPU_TILE * 2 * KGPU_NWG_MAX * 4, GPUBufferUsage.STORAGE);
        this.out = make(this.blockFrames * 2 * 4, GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC);
        this.readbacks = [];
        this.pool = null;
        this.poolBytes = 0;
        this.groups = null;
    }

    async compileErrors() {
        for (const [name, module] of Object.entries(this.modules)) {
            const info = await module.getCompilationInfo();
            const errors = info.messages.filter(m => m.type === "error");
            if (errors.length)
                return "shader " + name + ": " + errors.map(m => m.lineNum + ":" + m.linePos + " " + m.message).join("; ");
        }
        return "";
    }

    destroy() {
        for (const b of [this.uniform, this.voices[0], this.voices[1], this.state, this.hist, this.cmds, this.gates,
                         this.cuts, this.chan, this.scan, this.blockSums, this.partials, this.out, this.pool])
            if (b) b.destroy();
        for (const r of this.readbacks) r.destroy();
        this.device.destroy();
    }

    // Same sample layout as the offload renderer (ssw_wgpu_collect_samples).
    uploadSamples(Module, malloc, free) {
        const maxEntries = 1 << 16;
        const tablePtr = malloc(maxEntries * 16 + 8);
        if (!tablePtr) return "out of memory";
        try {
            const totalPtr = tablePtr + maxEntries * 16;
            const entries = Module._ssw_wgpu_collect_samples(tablePtr, maxEntries, totalPtr);
            const totalInt16 = Module.HEAPU32[totalPtr >>> 2] >>> 0;
            const bytes = Math.max(4, Math.ceil(totalInt16 / 2) * 4);
            if (bytes > this.device.limits.maxStorageBufferBindingSize)
                return "soundfont samples (" + Math.round(bytes / 1048576) + " MB) exceed the GPU buffer limit";
            if (this.pool) this.pool.destroy();
            this.pool = this.device.createBuffer({ size: bytes, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
            const table = Module.HEAPU32.subarray(tablePtr >>> 2, (tablePtr >>> 2) + entries * 4);
            for (let e = 0; e < entries; ++e) {
                const address = table[e * 4];
                const count = table[e * 4 + 2];
                const offset = table[e * 4 + 3];
                const words = Math.ceil(count / 2);
                const staging = new Int16Array(words * 2);
                staging.set(Module.HEAP16.subarray(address >>> 1, (address >>> 1) + count));
                this.device.queue.writeBuffer(this.pool, offset * 2, staging.buffer, 0, words * 4);
            }
            this.poolBytes = bytes;
            this.reset();
            return "";
        } finally {
            free(tablePtr);
        }
    }

    // Empties the pool (new soundfont, seek or core reset).
    reset() {
        this.device.queue.writeBuffer(this.state, 0, new Uint32Array(KGPU_STATE_WORDS));
        this.liveKnown = 0;
        this.spawnSubmitted = 0;
        this.spawnSeen = 0;
        this.stats.live = 0;
        ++this.generation;
        this.buildGroups();
    }

    buildGroups() {
        const d = this.device;
        const bg = (layout, buffers) => d.createBindGroup({
            layout: this.layouts[layout],
            entries: buffers.map((buffer, binding) => ({ binding, resource: { buffer } }))
        });
        this.groups = [0, 1].map(p => ({
            select: bg("select", [this.uniform, this.voices[p], this.state, this.hist]),
            spawn: bg("spawn", [this.uniform, this.cmds, this.voices[p], this.state]),
            render: bg("render", [this.uniform, this.pool, this.gates, this.voices[p], this.partials,
                                           this.state, this.chan, this.cuts]),
            compact: bg("compact", [this.uniform, this.voices[p], this.voices[1 - p], this.scan,
                                               this.blockSums, this.state])
        }));
        this.reduceGroup = bg("reduce", [this.uniform, this.partials, this.out]);
    }

    // Hands the block the core gathered (ssw_kgpu_finish_block) to the GPU. Resolves to
    // interleaved stereo of `frames` frames, or null when nothing sounds.
    submit(Module, frames, gain) {
        if (this.lost) return Promise.reject(new Error(this.lostReason || "GPU device lost"));
        if (!this.groups) return Promise.resolve(null);
        const h = Module.HEAPU32;
        const hp = Number(Module._ssw_kgpu_finish_block()) >>> 2;
        const spawnN = Math.min(h[hp], this.maxSpawn);
        const gateWords = h[hp + 2];
        const cutCount = h[hp + 3];
        const cutMask = h[hp + 4];
        const chanTiles = h[hp + 5];
        const spawnPtr = h[hp + 7], gatePtr = h[hp + 8], cutPtr = h[hp + 9], chanPtr = h[hp + 10];
        if (frames > this.blockFrames) return Promise.reject(new Error("GPU block larger than the engine"));
        // Upper bound of the live voices: the last read count plus every spawn since.
        const est = Math.min(this.slots, this.liveKnown + (this.spawnSubmitted - this.spawnSeen) + spawnN);
        if (est === 0) return Promise.resolve(null);
        const d = this.device, q = d.queue;
        ++this.block;
        const block = this.block;
        if (spawnN > 0)
            q.writeBuffer(this.cmds, 0, Module.HEAPU8, spawnPtr, spawnN * KGPU_SPAWN_WORDS * 4);
        if (gateWords > this.gateWords) {
            this.gates.destroy();
            this.gateWords = Math.max(gateWords, this.gateWords * 2);
            this.gates = d.createBuffer({ size: this.gateWords * 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
            this.buildGroups();
        }
        if (gateWords > 0) q.writeBuffer(this.gates, 0, Module.HEAPU8, gatePtr, gateWords * 4);
        if (cutCount > 0) q.writeBuffer(this.cuts, 0, Module.HEAPU8, cutPtr, cutCount * 3 * 4);
        q.writeBuffer(this.chan, 0, Module.HEAPU8, chanPtr, Math.max(1, chanTiles) * 16 * KGPU_CHAN_WORDS * 4);
        const tiles = Math.ceil(frames / KGPU_TILE);
        const nwg = Math.max(1, Math.min(KGPU_NWG_MAX, Math.ceil(est / KGPU_WG)));
        const uw = new Uint32Array(KGPU_UNIFORM_BYTES / 4);
        const uf = new Float32Array(uw.buffer);
        uw[0] = frames; uw[1] = tiles; uw[2] = this.slots; uw[3] = this.capacity;
        uw[4] = spawnN; uw[5] = this.maxSteal; uw[6] = nwg; uw[7] = block >>> 0;
        uf[8] = gain; uw[9] = cutCount; uw[10] = cutMask; uw[11] = Math.max(1, Math.min(64, frames >> 1));
        q.writeBuffer(this.uniform, 0, uw);
        // Every pass reaches all `est` voices in one sweep.
        const sweep = Math.max(1, Math.min(65535, Math.ceil(est / KGPU_WG)));
        const g = this.groups[this.parity];
        const enc = d.createCommandEncoder();
        const pass = enc.beginComputePass();
        const run = (pipeline, group, n) => { pass.setPipeline(pipeline); pass.setBindGroup(0, group); pass.dispatchWorkgroups(n); };
        run(this.pipes.plan, g.select, 1);
        for (let b = 0; b < 4; ++b) {
            run(this.pipes.clear, g.select, 1);
            run(this.pipes.histogram, g.select, sweep);
            run(this.pipes.refine, g.select, 1);
        }
        run(this.pipes.mark, g.select, sweep);
        if (spawnN > 0) run(this.pipes.spawn, g.spawn, Math.ceil(spawnN / KGPU_WG));
        run(this.pipes.spawnCommit, g.spawn, 1);
        run(this.pipes.render, g.render, nwg);
        run(this.pipes.reduce, this.reduceGroup, Math.ceil(frames * 2 / KGPU_WG));
        run(this.pipes.scanLocal, g.compact, sweep);
        run(this.pipes.scanBlocks, g.compact, 1);
        run(this.pipes.scatter, g.compact, sweep);
        run(this.pipes.compactCommit, g.compact, 1);
        pass.end();
        this.parity ^= 1;
        const outBytes = this.blockFrames * 2 * 4;
        const readback = this.readbacks.pop() || d.createBuffer({
            size: outBytes + KGPU_STATE_WORDS * 4, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
        enc.copyBufferToBuffer(this.out, 0, readback, 0, frames * 2 * 4);
        enc.copyBufferToBuffer(this.state, 0, readback, outBytes, KGPU_STATE_WORDS * 4);
        q.submit([enc.finish()]);
        this.spawnSubmitted += spawnN;
        const seenAfter = this.spawnSubmitted;
        const generation = this.generation;
        return readback.mapAsync(GPUMapMode.READ).then(() => {
            const mapped = readback.getMappedRange();
            const result = new Float32Array(mapped.slice(0, frames * 2 * 4));
            const st = new Uint32Array(mapped.slice(outBytes, outBytes + KGPU_STATE_WORDS * 4));
            readback.unmap();
            this.readbacks.push(readback);
            if (generation !== this.generation) return result;
            if (seenAfter > this.spawnSeen) {
                this.spawnSeen = seenAfter;
                this.liveKnown = st[0];
            }
            this.stats.live = st[0];
            this.stats.stolen = st[8];
            this.stats.dropped = st[9];
            return result;
        });
    }
}

self.WasmiKestrelGpu = WasmiKestrelGpu;
