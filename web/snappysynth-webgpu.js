"use strict";

// WasmiSynth WebGPU voice renderer (HANDOFF sec. 63). The CPU engine decides
// which voices sound and advances their state; the voices it offloads arrive as
// 160-byte requests (wgpu_voice_request in Voice/voice.c) and are rendered here.
// One pass per render segment (filter state is GPU-resident and must advance in
// segment order), then a fixed-order reduction per output sample.

const WGPU_REQUEST_BYTES = 160;
const WGPU_REQUEST_WORDS = WGPU_REQUEST_BYTES / 4;

const WGPU_SHADER = /* wgsl */ `
struct Req {
  sample_offset: u32, sample_channels: u32, loop_active: u32, env_state: u32,
  out_offset: u32, frames: u32, startup: u32, voice_id: u32,
  filter_enabled: u32, filter_reset: u32, segment: u32, sustain_down: u32,
  pending_off: i32, row_start: u32, pad1: u32, pad2: u32,
  position: f32, pitch: f32, gain_l: f32, gain_r: f32,
  env: f32, decay_dec: f32, sustain: f32, attack_inc: f32,
  release_alpha: f32, loop_start: f32, loop_end: f32, playback_end: f32,
  b0: f32, b1: f32, b2: f32, a1: f32,
  a2: f32, z1l: f32, z2l: f32, z1r: f32,
  z2r: f32, pad3: f32, pad4: f32, pad5: f32,
}
struct Params { first: u32, count: u32, block_frames: u32, total: u32, gain: f32, p0: f32, p1: f32, p2: f32 }

@group(0) @binding(0) var<storage, read> reqs: array<Req>;
@group(0) @binding(1) var<storage, read> pool: array<u32>;
@group(0) @binding(2) var<storage, read_write> scratch: array<f32>;
@group(0) @binding(3) var<storage, read_write> filters: array<vec4<f32>>;
@group(0) @binding(4) var<storage, read_write> outBuf: array<f32>;
@group(0) @binding(5) var<uniform> params: Params;

fn sample16(index: u32) -> f32 {
  let word = pool[index >> 1u];
  let bits16 = select(word & 0xffffu, word >> 16u, (index & 1u) == 1u);
  return f32(bitcast<i32>(bits16 << 16u) >> 16u);
}

@compute @workgroup_size(64)
fn renderVoices(@builtin(global_invocation_id) gid: vec3<u32>) {
  if (gid.x >= params.count) { return; }
  let r = reqs[params.first + gid.x];
  var pos = r.position;
  var env = r.env;
  var st = r.env_state;
  var pending = r.pending_off;
  let ch = r.sample_channels;
  var z = filters[r.voice_id];
  if (r.filter_reset == 1u) { z = vec4<f32>(r.z1l, r.z2l, r.z1r, r.z2r); }
  var alive = true;
  for (var f = 0u; f < r.frames; f = f + 1u) {
    var vl = 0.0;
    var vr = 0.0;
    if (alive && f >= r.startup) {
      let i = u32(pos);
      let frac = pos - f32(i);
      var nx = i + 1u;
      if (r.loop_active == 1u && f32(nx) >= r.loop_end) { nx = u32(r.loop_start); }
      let base = r.sample_offset;
      let l0 = sample16(base + i * ch);
      let l1 = sample16(base + nx * ch);
      let r0 = sample16(base + i * ch + ch - 1u);
      let r1 = sample16(base + nx * ch + ch - 1u);
      vl = l0 + (l1 - l0) * frac;
      vr = r0 + (r1 - r0) * frac;
      if (r.filter_enabled == 1u) {
        let fl = z.x + vl * r.b0;
        z.x = z.y + vl * r.b1 - fl * r.a1;
        z.y = vl * r.b2 - fl * r.a2;
        if (ch == 2u) {
          let fr = z.z + vr * r.b0;
          z.z = z.w + vr * r.b1 - fr * r.a1;
          z.w = vr * r.b2 - fr * r.a2;
          vr = fr;
        } else {
          vr = fl;
        }
        vl = fl;
      }
      vl = vl * r.gain_l * env * params.gain;
      vr = vr * r.gain_r * env * params.gain;
      if (pending > 0) {
        pending = pending - 1;
        if (pending == 0) {
          pending = -1;
          if (st != 3u && st != 4u && r.sustain_down == 0u) { st = 3u; }
        }
      }
      if (st == 0u) {
        env = env + r.attack_inc;
        if (env >= 1.0) { env = 1.0; st = 1u; }
      } else if (st == 1u) {
        env = env - r.decay_dec;
        if (env <= r.sustain) { env = r.sustain; st = 2u; }
      } else if (st == 3u) {
        env = env * r.release_alpha;
        if (env <= 1.0 / 65536.0) { alive = false; }
      }
      if (alive) {
        pos = pos + r.pitch;
        if (r.loop_active == 1u && pos >= r.loop_end) { pos = pos - (r.loop_end - r.loop_start); }
      }
    }
    let o = (r.row_start + f) * 2u;
    scratch[o] = vl;
    scratch[o + 1u] = vr;
  }
  filters[r.voice_id] = z;
}

@compute @workgroup_size(64)
fn reduceMix(@builtin(global_invocation_id) gid: vec3<u32>) {
  let outIndex = gid.x;
  if (outIndex >= params.block_frames * 2u) { return; }
  let frame = outIndex >> 1u;
  let channel = outIndex & 1u;
  var sum = 0.0;
  for (var k = 0u; k < params.total; k = k + 1u) {
    let r = reqs[k];
    if (frame >= r.out_offset && frame < r.out_offset + r.frames) {
      sum = sum + scratch[(r.row_start + frame - r.out_offset) * 2u + channel];
    }
  }
  outBuf[outIndex] = sum;
}
`;

class WasmiSynthGpu {
    static async create(maxVoices, blockFrames) {
        if (!self.navigator || !self.navigator.gpu)
            return { gpu: null, reason: "WebGPU is not available in this browser" };
        const adapter = await self.navigator.gpu.requestAdapter({ powerPreference: "high-performance" });
        if (!adapter)
            return { gpu: null, reason: "no WebGPU adapter" };
        const limits = adapter.limits;
        const device = await adapter.requestDevice({
            requiredLimits: {
                maxStorageBufferBindingSize: limits.maxStorageBufferBindingSize,
                maxBufferSize: limits.maxBufferSize
            }
        });
        const gpu = new WasmiSynthGpu(device, maxVoices, blockFrames);
        return { gpu, reason: "" };
    }

    constructor(device, maxVoices, blockFrames) {
        this.device = device;
        this.lost = false;
        device.lost.then(info => { this.lost = true; this.lostReason = (info && info.message) || "device lost"; });
        device.onuncapturederror = event => {
            this.lost = true;
            this.lostReason = (event && event.error && event.error.message) || "GPU validation error";
        };
        this.maxVoices = Math.max(1, maxVoices | 0);
        this.blockFrames = Math.max(1, blockFrames | 0);
        this.maxRequests = this.maxVoices * 8;
        this.blockFramesHint = this.blockFrames;
        const module = device.createShaderModule({ code: WGPU_SHADER });
        this.layout = device.createBindGroupLayout({
            entries: [
                { binding: 0, visibility: GPUShaderStage.COMPUTE, buffer: { type: "read-only-storage" } },
                { binding: 1, visibility: GPUShaderStage.COMPUTE, buffer: { type: "read-only-storage" } },
                { binding: 2, visibility: GPUShaderStage.COMPUTE, buffer: { type: "storage" } },
                { binding: 3, visibility: GPUShaderStage.COMPUTE, buffer: { type: "storage" } },
                { binding: 4, visibility: GPUShaderStage.COMPUTE, buffer: { type: "storage" } },
                { binding: 5, visibility: GPUShaderStage.COMPUTE, buffer: { type: "uniform", hasDynamicOffset: true } }
            ]
        });
        const pipelineLayout = device.createPipelineLayout({ bindGroupLayouts: [this.layout] });
        this.renderPipeline = device.createComputePipeline({
            layout: pipelineLayout, compute: { module, entryPoint: "renderVoices" } });
        this.reducePipeline = device.createComputePipeline({
            layout: pipelineLayout, compute: { module, entryPoint: "reduceMix" } });
        const storage = GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST;
        this.requestBuffer = device.createBuffer({ size: this.maxRequests * WGPU_REQUEST_BYTES, usage: storage });
        // Packed scratch rows hold every offloaded voice-frame of a block.
        this.scratchFrames = Math.max(this.blockFrames, Math.min(this.maxVoices * this.blockFrames,
            Math.floor(Math.min(device.limits.maxStorageBufferBindingSize, device.limits.maxBufferSize) / 8)));
        this.scratchBuffer = device.createBuffer({ size: this.scratchFrames * 8, usage: GPUBufferUsage.STORAGE });
        this.filterBuffer = device.createBuffer({ size: this.maxVoices * 16, usage: GPUBufferUsage.STORAGE });
        this.outBuffer = device.createBuffer({
            size: this.blockFrames * 2 * 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC });
        // Up to 64 segment passes + 1 reduction, 256-byte aligned uniform slots.
        this.paramSlots = 65;
        this.paramBuffer = device.createBuffer({
            size: this.paramSlots * 256, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
        this.readbacks = [];
        this.poolBuffer = null;
        this.bindGroup = null;
        this.poolBytes = 0;
    }

    destroy() {
        for (const b of [this.requestBuffer, this.scratchBuffer, this.filterBuffer, this.outBuffer,
                         this.paramBuffer, this.poolBuffer])
            if (b) b.destroy();
        for (const r of this.readbacks) r.destroy();
        this.device.destroy();
    }

    // Uploads every sample buffer the regions use, in the layout chosen by
    // ssw_wgpu_collect_samples(). Returns "" or the reason it cannot.
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
            if (this.poolBuffer) this.poolBuffer.destroy();
            this.poolBuffer = this.device.createBuffer({
                size: bytes, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
            const table = Module.HEAPU32.subarray(tablePtr >>> 2, (tablePtr >>> 2) + entries * 4);
            for (let e = 0; e < entries; ++e) {
                const address = table[e * 4];
                const count = table[e * 4 + 2];
                const offset = table[e * 4 + 3];
                // writeBuffer needs 4-byte aligned offsets and sizes: copy through a
                // staging array that starts at the even sample below `offset`.
                // offsets are even (voice.c), so each copy starts on a 4-byte word
                const words = Math.ceil(count / 2);
                const staging = new Int16Array(words * 2);
                staging.set(Module.HEAP16.subarray(address >>> 1, (address >>> 1) + count));
                this.device.queue.writeBuffer(this.poolBuffer, offset * 2, staging.buffer, 0, words * 4);
            }
            this.poolBytes = bytes;
            this.bindGroup = this.device.createBindGroup({
                layout: this.layout,
                entries: [
                    { binding: 0, resource: { buffer: this.requestBuffer } },
                    { binding: 1, resource: { buffer: this.poolBuffer } },
                    { binding: 2, resource: { buffer: this.scratchBuffer } },
                    { binding: 3, resource: { buffer: this.filterBuffer } },
                    { binding: 4, resource: { buffer: this.outBuffer } },
                    { binding: 5, resource: { buffer: this.paramBuffer, size: 32 } }
                ]
            });
            return "";
        } finally {
            free(tablePtr);
        }
    }

    // Copies this block's requests out of wasm memory and renders them. Resolves
    // to an interleaved stereo Float32Array of `frames` frames (already scaled by
    // the engine's global mix gain), or null when there was nothing to render.
    submit(Module, frames) {
        const count = Math.min(Module._ssw_wgpu_request_count() | 0, this.maxRequests);
        if (this.lost)
            return Promise.reject(new Error(this.lostReason || "GPU device lost"));
        if (count <= 0 || !this.bindGroup)
            return Promise.resolve(null);
        const ptr = Number(Module._ssw_wgpu_requests_ptr());
        const words = new Uint32Array(Module.HEAPU8.buffer.slice(ptr, ptr + count * WGPU_REQUEST_BYTES));
        // Packed scratch rows: row_start = sum of the frames of earlier requests.
        let row = 0;
        const segments = [];
        let segStart = 0;
        for (let i = 0; i < count; ++i) {
            const base = i * WGPU_REQUEST_WORDS;
            words[base + 13] = row;
            row += words[base + 5];
            if (i > 0 && words[base + 10] !== words[base - WGPU_REQUEST_WORDS + 10]) {
                segments.push([segStart, i - segStart]);
                segStart = i;
            }
        }
        segments.push([segStart, count - segStart]);
        if (row > this.scratchFrames || segments.length + 1 > this.paramSlots)
            return Promise.reject(new Error("GPU block exceeded its scratch capacity"));
        const device = this.device;
        device.queue.writeBuffer(this.requestBuffer, 0, words.buffer, 0, count * WGPU_REQUEST_BYTES);
        const gain = Module._ssw_global_mix_gain();
        const params = new ArrayBuffer(this.paramSlots * 256);
        const pv = new DataView(params);
        const slot = (index, first, n) => {
            const o = index * 256;
            pv.setUint32(o, first, true);
            pv.setUint32(o + 4, n, true);
            pv.setUint32(o + 8, frames, true);
            pv.setUint32(o + 12, count, true);
            pv.setFloat32(o + 16, gain, true);
        };
        segments.forEach(([first, n], i) => slot(i, first, n));
        const reduceSlot = segments.length;
        slot(reduceSlot, 0, count);
        device.queue.writeBuffer(this.paramBuffer, 0, params, 0, (reduceSlot + 1) * 256);
        const encoder = device.createCommandEncoder();
        segments.forEach(([, n], i) => {
            const pass = encoder.beginComputePass();
            pass.setPipeline(this.renderPipeline);
            pass.setBindGroup(0, this.bindGroup, [i * 256]);
            pass.dispatchWorkgroups(Math.ceil(n / 64));
            pass.end();
        });
        {
            const pass = encoder.beginComputePass();
            pass.setPipeline(this.reducePipeline);
            pass.setBindGroup(0, this.bindGroup, [reduceSlot * 256]);
            pass.dispatchWorkgroups(Math.ceil(frames * 2 / 64));
            pass.end();
        }
        const readback = this.readbacks.pop() || device.createBuffer({
            size: this.blockFrames * 2 * 4, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
        encoder.copyBufferToBuffer(this.outBuffer, 0, readback, 0, frames * 2 * 4);
        device.queue.submit([encoder.finish()]);
        return readback.mapAsync(GPUMapMode.READ, 0, frames * 2 * 4).then(() => {
            const result = new Float32Array(readback.getMappedRange(0, frames * 2 * 4).slice(0));
            readback.unmap();
            this.readbacks.push(readback);
            return result;
        });
    }
}

self.WasmiSynthGpu = WasmiSynthGpu;
