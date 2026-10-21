#!/usr/bin/env node
// Lists and extracts tools/archive_fixtures/* with web/archive-worker.js and the
// built wasmidi-archive-core.js, comparing every byte (HANDOFF sec. 110).
//   node tools/archive_smoke.cjs build/wasmidi-archive-core.js
//   node tools/archive_smoke.cjs build/wasmidi-archive-core.js big.7z   (speed only)
"use strict";
const fs = require("fs");
const path = require("path");
const crypto = require("crypto");

const corePath = path.resolve(process.argv[2] || "build/wasmidi-archive-core.js");
const archive = require(path.join(__dirname, "..", "web", "archive-worker.js"));
const factory = require(corePath);
const fixtures = path.join(__dirname, "archive_fixtures");

function content(blocks, seed) {
    let x = seed;
    const base = new Uint8Array(16384);
    for (let i = 0; i < 16384; i++) {
        x = (Math.imul(x, 1103515245) + 12345) & 0x7fffffff;
        base[i] = (x >>> 16) & 0xff;
    }
    const out = new Uint8Array(blocks * 16384);
    for (let i = 0; i < blocks; i++) {
        const blk = base.slice();
        blk[i % 16384] ^= 0x5a;
        blk[(i * 7919) % 16384] = i & 0xff;
        out.set(blk, i * 16384);
    }
    return out;
}

const README = new TextEncoder().encode("WASMIDI archive fixture\n");
const SONG = content(512, 12345);
const FULL = { "readme.txt": README, "midi/song.mid": SONG, "bank.sf2": content(40, 777) };
const EXPECT = {
    "lzma2.7z": FULL,
    "lzma.7z": FULL,
    "deflate.7z": FULL,
    "copy.7z": { "readme.txt": README, "midi/tiny.mid": content(4, 1) },
    "mixed.zip": FULL,
    "zip64.zip": { "midi/song.mid": SONG },
    "song.mid.xz": { "song.mid": SONG },
    "multi.mid.xz": { "multi.mid": SONG },
    "song.mid.gz": { "song.mid": SONG }
};

async function source(file) {
    const fd = fs.openSync(file, "r");
    const size = fs.fstatSync(fd).size;
    return {
        blob: await fs.openAsBlob(file),
        size,
        read(offset, length) {
            const out = Buffer.allocUnsafe(length);
            let at = 0;
            while (at < length) {
                const n = fs.readSync(fd, out, at, length - at, offset + at);
                if (n <= 0) throw new Error("short read");
                at += n;
            }
            return new Uint8Array(out.buffer, out.byteOffset, length);
        }
    };
}

function sameBytes(a, b) {
    return a.length === b.length && Buffer.compare(Buffer.from(a), Buffer.from(b)) === 0;
}

async function main() {
    const loadCore = () => factory();
    if (process.argv[3]) {
        const file = process.argv[3];
        const ctx = archive.archiveContext(await source(file), loadCore);
        const listing = await archive.archiveList(ctx, path.basename(file));
        const index = listing.entries.findIndex(e => /\.(mid|midi)$/i.test(e.name));
        const started = process.hrtime.bigint();
        let written = 0;
        const hash = crypto.createHash("md5");
        await archive.archiveExtract(ctx, Math.max(0, index), chunk => {
            written += chunk.length;
            if (process.env.HASH) hash.update(chunk);
        });
        const seconds = Number(process.hrtime.bigint() - started) / 1e9;
        console.log(listing.entries[Math.max(0, index)].name + ": " + written + " bytes in " +
            seconds.toFixed(1) + " s = " + (written / seconds / 1e6).toFixed(0) + " MB/s" +
            (process.env.HASH ? " md5 " + hash.digest("hex") : ""));
        return;
    }
    let failures = 0;
    for (const [name, files] of Object.entries(EXPECT)) {
        const ctx = archive.archiveContext(await source(path.join(fixtures, name)), loadCore);
        const listing = await archive.archiveList(ctx, name);
        const names = listing.entries.map(e => e.name).sort();
        const wanted = Object.keys(files).sort();
        if (JSON.stringify(names) !== JSON.stringify(wanted)) {
            console.error("FAIL " + name + ": entries " + JSON.stringify(names) + " != " + JSON.stringify(wanted));
            failures++;
            continue;
        }
        // Last entry first: in a solid 7z that skips the earlier ones inside the folder.
        for (let i = listing.entries.length - 1; i >= 0; i--) {
            const entry = listing.entries[i];
            const parts = [];
            await archive.archiveExtract(ctx, i, chunk => { parts.push(Buffer.from(chunk)); });
            const got = Buffer.concat(parts);
            const ok = sameBytes(got, files[entry.name]) &&
                (!entry.sizeKnown || entry.size === got.length);
            console.log((ok ? "OK   " : "FAIL ") + name + " " + entry.name + " (" + entry.method + ", " + got.length + " bytes)");
            if (!ok) failures++;
        }
    }
    if (failures) {
        console.error(failures + " archive check(s) failed");
        process.exit(1);
    }
    console.log("archive smoke: all entries identical");
}

main().catch(error => { console.error(error); process.exit(1); });
