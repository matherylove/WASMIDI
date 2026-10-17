class SnappySynthOutputProcessor extends AudioWorkletProcessor {
    constructor() {
        super();

        this.workerPort = null;

        this.playing = false;
        this.baseSongTime = 0.0;
        this.playedFrames = 0;
        this.lastClockReportFrame = 0;
        this.underruns = 0;
        this.starved = false;
        this.transportEpoch = 1;
        this.clockState = null;
        this.CLOCK_EPOCH = 0;
        this.CLOCK_FRAMES = 1;
        this.CLOCK_STARVED = 2;
        // On a fresh play/seek, request realtime PCM immediately but do not
        // count silence or advance the device clock until the producer has
        // delivered the first complete source block. This is startup priming,
        // not audio pre-rendering: the ring still holds only NumBuffers.
        this.primed = false;
        this.ringGeneration = -1;

        this.blockFrames = 512;
        this.numBuffers = 16;
        // Preserve Pass 10's post-synth gain semantics exactly. The shared-ring
        // optimization removes PCM transport copies, not the audible gain stage.
        this.outputGain = 1.0;
        this.lowWaterFrames = this.blockFrames * Math.max(2, Math.floor(this.numBuffers / 2));
        this.highWaterFrames = this.blockFrames * this.numBuffers;
        this.needOutstanding = false;
        // MessagePort delivery is reliable, but never make realtime audio depend
        // on one acknowledgement bit forever. If a producer message is delayed
        // behind an unusually expensive render/event burst, resend the same
        // idempotent deficit periodically. The Worker uses max(), not addition,
        // so this cannot inflate requested PCM depth.
        this.needOutstandingQuanta = 0;

        // Shared WebAssembly.Memory ring descriptor. PCM stays interleaved in
        // exactly the buffer written by voice_render_float(); this processor
        // only deinterleaves into WebAudio's planar output buffers.
        this.RING_READ = 0;
        this.RING_WRITE = 1;
        this.RING_AVAILABLE = 2;
        this.RING_GENERATION = 3;
        this.RING_CAPACITY = 4;
        this.RING_CHANNELS = 5;
        this.RING_BLOCK_FRAMES = 6;
        this.RING_STARVED = 7;
        this.RING_WORDS = 8;

        this.sharedMemory = null;
        this.headerPtr = 0;
        this.pcmPtr = 0;
        this.ringHeader = null;
        this.ringPcm = null;
        this.capacityFrames = 0;
        this.channels = 2;

        this.port.onmessage = event => {
            const data = event.data || {};

            if (data.type === "workerPort" && data.port) {
                this.workerPort = data.port;
                this.workerPort.onmessage = e => this.onWorkerMessage(e.data || {});
                this.workerPort.start();
                this.requestAudioIfNeeded();
                return;
            }

            if (data.type === "clockState" && data.buffer instanceof SharedArrayBuffer) {
                this.clockState = new Int32Array(data.buffer);
                this.transportEpoch = (Number(data.epoch) >>> 0) || 1;
                this.publishSharedClock();
                return;
            }

            if (data.type === "play") {
                if (data.resetClock) {
                    this.transportEpoch = (Number(data.epoch) >>> 0) || this.transportEpoch || 1;
                    this.flushRing();
                    this.baseSongTime = Number(data.time) || 0.0;
                    this.playedFrames = 0;
                    this.lastClockReportFrame = 0;
                    this.primed = false;
                    this.publishSharedClock();
                }
                this.playing = true;
                this.requestAudioIfNeeded();
                return;
            }

            if (data.type === "pause") {
                this.playing = false;
                return;
            }

            if (data.type === "flush") {
                if (Number.isFinite(data.epoch))
                    this.transportEpoch = (Number(data.epoch) >>> 0) || this.transportEpoch || 1;
                this.flushRing();
                if (Number.isFinite(data.time)) {
                    this.baseSongTime = Number(data.time);
                    this.playedFrames = 0;
                    this.lastClockReportFrame = 0;
                }
                this.publishSharedClock();
                return;
            }

            if (data.type === "volume") {
                this.outputGain =
                    Math.max(
                        0.0,
                        Math.min(
                            1.0,
                            Number(data.value) || 0.0));
                return;
            }

            if (data.type === "config") {
                const block =
                    Math.max(
                        1,
                        Number(data.blockFrames) | 0);

                const buffers =
                    Math.max(
                        1,
                        Math.min(
                            128,
                            Number(data.numBuffers) | 0 || this.numBuffers));

                this.blockFrames = block;
                this.numBuffers = buffers;
                this.updateWatermarks();
                this.requestAudioIfNeeded();
            }
        };
    }

    updateWatermarks() {
        const configuredHigh =
            Math.max(
                this.blockFrames,
                this.blockFrames * this.numBuffers);

        // Realtime-only audio: NumBuffers is the target queue depth. A larger
        // shared allocation must never turn into seconds of background PCM.
        const hardCapacity =
            this.capacityFrames > 0
                ? this.capacityFrames
                : configuredHigh;

        this.highWaterFrames =
            Math.min(configuredHigh, hardCapacity);

        if (this.highWaterFrames < 128 && hardCapacity >= 128)
            this.highWaterFrames = Math.min(hardCapacity, 256);

        this.lowWaterFrames =
            Math.max(
                Math.min(this.blockFrames, this.highWaterFrames),
                Math.floor(this.highWaterFrames / 2));
    }

    bindSharedRing(data) {
        if (!(data.memory instanceof SharedArrayBuffer))
            return;

        const headerPtr = Number(data.headerPtr) | 0;
        const pcmPtr = Number(data.pcmPtr) | 0;
        const capacityFrames = Math.max(1, Number(data.capacityFrames) | 0);
        const channels = Number(data.channels) === 1 ? 1 : 2;

        this.sharedMemory = data.memory;
        this.headerPtr = headerPtr;
        this.pcmPtr = pcmPtr;
        this.capacityFrames = capacityFrames;
        this.channels = channels;

        this.ringHeader =
            new Int32Array(
                this.sharedMemory,
                this.headerPtr,
                this.RING_WORDS);

        this.ringPcm =
            new Float32Array(
                this.sharedMemory,
                this.pcmPtr,
                this.capacityFrames * this.channels);

        // A newly published ring starts empty (or belongs to a new memory
        // generation after SF2/config allocation). Require one source block
        // before resuming audible clock advancement.
        this.primed = false;

        if (Number.isFinite(data.blockFrames))
            this.blockFrames = Math.max(1, Number(data.blockFrames) | 0);

        this.needOutstanding = false;
        this.needOutstandingQuanta = 0;
        this.updateWatermarks();
        this.requestAudioIfNeeded();
    }

    flushRing() {
        if (this.ringHeader) {
            // Invalidate any block currently being rendered by the Worker.
            Atomics.store(this.ringHeader, this.RING_STARVED, 0);
            Atomics.add(this.ringHeader, this.RING_GENERATION, 1);
            Atomics.store(this.ringHeader, this.RING_READ, 0);
            Atomics.store(this.ringHeader, this.RING_WRITE, 0);
            Atomics.store(this.ringHeader, this.RING_AVAILABLE, 0);
        }

        this.needOutstanding = false;
        this.needOutstandingQuanta = 0;
        this.starved = false;
        this.primed = false;
    }

    publishSharedClock() {
        if (!this.clockState)
            return;
        const sharedEpoch = Atomics.load(this.clockState, this.CLOCK_EPOCH) >>> 0;
        if (sharedEpoch !== (this.transportEpoch >>> 0))
            return;
        Atomics.store(this.clockState, this.CLOCK_FRAMES, this.playedFrames | 0);
        Atomics.store(this.clockState, this.CLOCK_STARVED, this.starved ? 1 : 0);
    }

    onWorkerMessage(data) {
        if (data.type === "sharedRing") {
            this.bindSharedRing(data);
            return;
        }

        if (data.type === "filled") {
            this.needOutstanding = false;
            this.needOutstandingQuanta = 0;
            this.requestAudioIfNeeded();
        }
    }

    requestAudioIfNeeded(forceRetry = false) {
        if (!this.workerPort || !this.playing || !this.ringHeader)
            return;

        if (this.needOutstanding && !forceRetry)
            return;

        const available =
            Math.max(
                0,
                Atomics.load(
                    this.ringHeader,
                    this.RING_AVAILABLE));

        if (available >= this.lowWaterFrames)
            return;

        const missing =
            Math.max(
                this.blockFrames,
                this.highWaterFrames - available);

        // Worker renders only full source blocks. This preserves the exact
        // AudioConfig.buffer_size cadence used by the original engine.
        const frames =
            Math.ceil(
                missing /
                Math.max(1, this.blockFrames)) *
            Math.max(1, this.blockFrames);

        this.needOutstanding = true;
        this.needOutstandingQuanta = 0;
        this.workerPort.postMessage({
            type: "need",
            frames
        });
    }

    reportClockIfNeeded(force = false) {
        if (!this.workerPort)
            return;

        if (!force &&
            this.playedFrames - this.lastClockReportFrame < 2048)
            return;

        this.lastClockReportFrame = this.playedFrames;

        this.workerPort.postMessage({
            type: "clock",
            epoch: this.transportEpoch >>> 0,
            songTime:
                this.baseSongTime +
                this.playedFrames / sampleRate,
            underruns: this.underruns,
            starved: this.starved
        });
    }

    copyFromRing(left, right, written, frames) {
        if (!this.ringHeader ||
            !this.ringPcm ||
            frames <= 0)
            return 0;

        let readFrame =
            Atomics.load(
                this.ringHeader,
                this.RING_READ);

        if (readFrame < 0 ||
            readFrame >= this.capacityFrames)
            readFrame = 0;

        let remaining = frames;
        let out = written;
        const unity =
            this.outputGain === 1.0;

        while (remaining > 0) {
            const contiguous =
                Math.min(
                    remaining,
                    this.capacityFrames - readFrame);

            if (this.channels === 1) {
                let src = readFrame;
                const limit = out + contiguous;

                if (unity) {
                    for (let dst = out; dst < limit; ++dst) {
                        const sample = this.ringPcm[src++];
                        left[dst] = sample;
                        right[dst] = sample;
                    }
                } else {
                    const gain = this.outputGain;
                    for (let dst = out; dst < limit; ++dst) {
                        const sample =
                            this.ringPcm[src++] * gain;
                        left[dst] = sample;
                        right[dst] = sample;
                    }
                }
            } else {
                let src = readFrame * 2;
                const limit = out + contiguous;

                if (unity) {
                    for (let dst = out; dst < limit; ++dst) {
                        left[dst] = this.ringPcm[src++];
                        right[dst] = this.ringPcm[src++];
                    }
                } else {
                    const gain = this.outputGain;
                    for (let dst = out; dst < limit; ++dst) {
                        left[dst] =
                            this.ringPcm[src++] * gain;
                        right[dst] =
                            this.ringPcm[src++] * gain;
                    }
                }
            }

            out += contiguous;
            remaining -= contiguous;
            readFrame += contiguous;

            if (readFrame >= this.capacityFrames)
                readFrame = 0;
        }

        Atomics.store(
            this.ringHeader,
            this.RING_READ,
            readFrame);

        Atomics.sub(
            this.ringHeader,
            this.RING_AVAILABLE,
            frames);

        return frames;
    }

    process(inputs, outputs) {
        const output = outputs[0];

        if (!output || output.length === 0)
            return true;

        const left = output[0];
        const right = output.length > 1 ? output[1] : output[0];
        const frames = left.length;

        if (!this.playing ||
            !this.ringHeader ||
            !this.ringPcm) {
            left.fill(0.0);
            if (right !== left)
                right.fill(0.0);

            this.requestAudioIfNeeded();
            return true;
        }

        const available =
            Math.max(
                0,
                Atomics.load(
                    this.ringHeader,
                    this.RING_AVAILABLE));

        // A ring reset by the Worker (seek, reconfigure) starts a new timeline:
        // its silence is not an underrun to catch up on.
        const generation = Atomics.load(this.ringHeader, this.RING_GENERATION);
        if (generation !== this.ringGeneration) {
            this.ringGeneration = generation;
            this.primed = false;
        }

        if (!this.primed) {
            // Wait for one complete synth source block before starting the
            // audible clock. The producer is already demand-driven, so this is
            // only a readiness barrier and never creates a seconds-long cache.
            const primeFrames =
                Math.max(
                    1,
                    Math.min(
                        this.capacityFrames,
                        this.blockFrames));
            if (available < primeFrames) {
                left.fill(0.0);
                if (right !== left)
                    right.fill(0.0);
                if (this.needOutstanding)
                    ++this.needOutstandingQuanta;
                this.requestAudioIfNeeded(
                    this.needOutstanding &&
                    this.needOutstandingQuanta >= 8);
                return true;
            }
            this.primed = true;
            this.starved = false;
        }

        // Audio is an independent realtime consumer, just like MPWGL2's MIDI
        // output path. Renderer/cache stalls must never cap how much already
        // rendered PCM the device is allowed to consume.
        const count =
            Math.min(
                frames,
                available);

        const written =
            this.copyFromRing(
                left,
                right,
                0,
                count);

        const starvedNow =
            written < frames;

        if (written < frames) {
            left.fill(0.0, written);
            if (right !== left)
                right.fill(0.0, written);
        }

        if (starvedNow) {
            ++this.underruns;
            Atomics.add(this.ringHeader, this.RING_STARVED, frames - written);
        }

        // Silence counts as played time: the Worker skips the same frames
        // (RING_STARVED), so the audio stays on the roll's clock.
        this.playedFrames += frames;

        const starvationChanged =
            starvedNow !== this.starved;
        this.starved = starvedNow;
        this.publishSharedClock();

        this.reportClockIfNeeded(starvationChanged);

        if (this.needOutstanding)
            ++this.needOutstandingQuanta;

        // About 64 render quanta is ~186 ms at 44.1 kHz/128-frame WebAudio
        // quanta. Re-sending the same deficit after that interval is cheap and
        // prevents a stale outstanding flag from becoming permanent silence.
        // During actual starvation retry sooner so recovery is prompt.
        const retryAfter = starvedNow ? 8 : 64;
        const retry = this.needOutstanding &&
            this.needOutstandingQuanta >= retryAfter;
        this.requestAudioIfNeeded(retry);

        return true;
    }
}

registerProcessor("snappysynth-output", SnappySynthOutputProcessor);
