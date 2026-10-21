"use strict";
// Archive reader for WASMIDI (HANDOFF sec. 110): lists .zip / .7z / .xz / .gz and
// streams one entry out, to the origin private file system or to memory.

const ARCHIVE_STATUS_FINISHED_WITH_MARK = 1;
const ARCHIVE_MEMORY_LIMIT = 256 * 1024 * 1024;
const ARCHIVE_SPOOL_DIR = "wasmidi-archive";
const ARCHIVE_LOCK_PREFIX = "wasmidi-archive:";

function archiveError(text) { return new Error(text); }

function u16(b, p) { return b[p] | (b[p + 1] << 8); }
function u32(b, p) { return (b[p] | (b[p + 1] << 8) | (b[p + 2] << 16)) + b[p + 3] * 0x1000000; }
function u64(b, p) { return u32(b, p) + u32(b, p + 4) * 0x100000000; }

function startsWith(b, sig) {
    if (b.length < sig.length)
        return false;
    for (let i = 0; i < sig.length; i++)
        if (b[i] !== sig[i])
            return false;
    return true;
}

const SIG_ZIP = [0x50, 0x4b, 0x03, 0x04];
const SIG_ZIP_EMPTY = [0x50, 0x4b, 0x05, 0x06];
const SIG_7Z = [0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c];
const SIG_XZ = [0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00];
const SIG_GZ = [0x1f, 0x8b];

function archiveFormat(head) {
    if (startsWith(head, SIG_ZIP) || startsWith(head, SIG_ZIP_EMPTY)) return "zip";
    if (startsWith(head, SIG_7Z)) return "7z";
    if (startsWith(head, SIG_XZ)) return "xz";
    if (startsWith(head, SIG_GZ)) return "gz";
    return "";
}

function stripSuffix(name, re) {
    const base = String(name || "").replace(/^.*[\\/]/, "");
    const stripped = base.replace(re, "");
    return stripped && stripped !== base ? stripped : base + ".out";
}

// ---------- 7z ----------

class SzReader {
    constructor(bytes) { this.b = bytes; this.p = 0; }
    byte() {
        if (this.p >= this.b.length)
            throw archiveError("Damaged 7z header");
        return this.b[this.p++];
    }
    bytes(n) {
        if (this.p + n > this.b.length)
            throw archiveError("Damaged 7z header");
        const out = this.b.subarray(this.p, this.p + n);
        this.p += n;
        return out;
    }
    number() {
        const first = this.byte();
        let mask = 0x80;
        let value = 0;
        for (let i = 0; i < 8; i++) {
            if ((first & mask) === 0)
                return value + (first & (mask - 1)) * Math.pow(2, 8 * i);
            value += this.byte() * Math.pow(2, 8 * i);
            mask >>= 1;
        }
        return value;
    }
    uint32() { const v = u32(this.b, this.p); this.bytes(4); return v; }
    bits(n) {
        const out = new Array(n);
        let mask = 0;
        let value = 0;
        for (let i = 0; i < n; i++) {
            if (mask === 0) { value = this.byte(); mask = 0x80; }
            out[i] = (value & mask) !== 0;
            mask >>= 1;
        }
        return out;
    }
    definedBits(n) {
        return this.byte() !== 0 ? new Array(n).fill(true) : this.bits(n);
    }
    digests(n) {
        const defined = this.definedBits(n);
        for (let i = 0; i < n; i++)
            if (defined[i])
                this.uint32();
        return defined;
    }
}

function szMethodName(id) {
    switch (id) {
    case "00": return "Copy";
    case "030101": return "LZMA";
    case "21": return "LZMA2";
    case "040108": return "Deflate";
    case "040109": return "Deflate64";
    case "040202": return "BZip2";
    case "030401": return "PPMd";
    case "03030103": return "BCJ";
    case "0303011b": return "BCJ2";
    case "03": return "Delta";
    case "06f10701": return "AES (encrypted)";
    default: return "method " + id;
    }
}

function szReadFolder(r) {
    const coders = [];
    const numCoders = r.number();
    let outTotal = 0;
    let inTotal = 0;
    for (let i = 0; i < numCoders; i++) {
        const flag = r.byte();
        const id = Array.from(r.bytes(flag & 0x0f), x => x.toString(16).padStart(2, "0")).join("");
        let numIn = 1;
        let numOut = 1;
        if (flag & 0x10) { numIn = r.number(); numOut = r.number(); }
        const props = (flag & 0x20) ? r.bytes(r.number()).slice() : new Uint8Array(0);
        coders.push({ id, props, numIn, numOut });
        inTotal += numIn;
        outTotal += numOut;
    }
    const bindOut = new Set();
    const bindIn = new Set();
    for (let i = 0; i < outTotal - 1; i++) {
        bindIn.add(r.number());
        bindOut.add(r.number());
    }
    const numPacked = inTotal - (outTotal - 1);
    const packed = [];
    if (numPacked === 1) {
        for (let i = 0; i < inTotal; i++)
            if (!bindIn.has(i)) { packed.push(i); break; }
    } else {
        for (let i = 0; i < numPacked; i++)
            packed.push(r.number());
    }
    return { coders, outTotal, bindOut, numPacked, unpackSizes: [], crcDefined: false };
}

function szFolderSize(folder) {
    for (let i = folder.unpackSizes.length - 1; i >= 0; i--)
        if (!folder.bindOut.has(i))
            return folder.unpackSizes[i];
    return 0;
}

function szReadStreamsInfo(r) {
    const info = { packPos: 0, packSizes: [], folders: [], subSizes: null, subCounts: null };
    for (;;) {
        const type = r.byte();
        if (type === 0x00)
            break;
        if (type === 0x06) {
            info.packPos = r.number();
            const n = r.number();
            for (;;) {
                const t = r.byte();
                if (t === 0x00) break;
                if (t === 0x09) { for (let i = 0; i < n; i++) info.packSizes.push(r.number()); }
                else if (t === 0x0a) r.digests(n);
                else throw archiveError("Unsupported 7z pack info");
            }
        } else if (type === 0x07) {
            if (r.byte() !== 0x0b) throw archiveError("Damaged 7z folder info");
            const n = r.number();
            if (r.byte() !== 0) throw archiveError("Unsupported external 7z folders");
            for (let i = 0; i < n; i++) info.folders.push(szReadFolder(r));
            if (r.byte() !== 0x0c) throw archiveError("Damaged 7z unpack sizes");
            for (const folder of info.folders)
                for (let i = 0; i < folder.outTotal; i++)
                    folder.unpackSizes.push(r.number());
            for (;;) {
                const t = r.byte();
                if (t === 0x00) break;
                if (t === 0x0a) r.digests(n).forEach((d, i) => { info.folders[i].crcDefined = d; });
                else throw archiveError("Unsupported 7z unpack info");
            }
        } else if (type === 0x08) {
            let t = r.byte();
            const counts = info.folders.map(() => 1);
            if (t === 0x0d) {
                for (let i = 0; i < counts.length; i++) counts[i] = r.number();
                t = r.byte();
            }
            const sizes = [];
            info.folders.forEach((folder, i) => {
                const n = counts[i];
                if (n === 0)
                    return;
                let sum = 0;
                if (t === 0x09) {
                    for (let j = 0; j < n - 1; j++) { const s = r.number(); sizes.push(s); sum += s; }
                }
                sizes.push(szFolderSize(folder) - sum);
            });
            if (t === 0x09)
                t = r.byte();
            while (t !== 0x00) {
                if (t === 0x0a) {
                    let unknown = 0;
                    counts.forEach((n, i) => {
                        if (n !== 1 || !info.folders[i].crcDefined) unknown += n;
                    });
                    r.digests(unknown);
                } else {
                    r.bytes(r.number());
                }
                t = r.byte();
            }
            info.subCounts = counts;
            info.subSizes = sizes;
        } else {
            throw archiveError("Damaged 7z streams info");
        }
    }
    if (!info.subCounts) {
        info.subCounts = info.folders.map(() => 1);
        info.subSizes = info.folders.map(szFolderSize);
    }
    return info;
}

function szReadFiles(r) {
    const n = r.number();
    const files = [];
    for (let i = 0; i < n; i++)
        files.push({ name: "", emptyStream: false, emptyFile: false, dir: false });
    let emptyCount = 0;
    for (;;) {
        const type = r.byte();
        if (type === 0x00)
            break;
        const size = r.number();
        const body = new SzReader(r.bytes(size));
        if (type === 0x0e) {
            const bits = body.bits(n);
            emptyCount = 0;
            bits.forEach((bit, i) => { files[i].emptyStream = bit; if (bit) emptyCount++; });
        } else if (type === 0x0f) {
            const bits = body.bits(emptyCount);
            let k = 0;
            files.forEach(f => { if (f.emptyStream) f.emptyFile = bits[k++]; });
        } else if (type === 0x11) {
            if (body.byte() !== 0) throw archiveError("Unsupported external 7z names");
            const text = body.b.subarray(body.p);
            let start = 0;
            let index = 0;
            for (let i = 0; i + 1 < text.length && index < n; i += 2) {
                if (text[i] === 0 && text[i + 1] === 0) {
                    files[index++].name = new TextDecoder("utf-16le").decode(text.subarray(start, i));
                    start = i + 2;
                }
            }
        } else if (type === 0x15) {
            const defined = body.definedBits(n);
            if (body.byte() !== 0) continue;
            defined.forEach((d, i) => {
                if (d) { const attr = body.uint32(); if (attr & 0x10) files[i].dir = true; }
            });
        }
    }
    files.forEach(f => { if (f.emptyStream && !f.emptyFile) f.dir = true; });
    return files;
}

async function szDecodeFolderToMemory(ctx, info, folderIndex) {
    const size = szFolderSize(info.folders[folderIndex]);
    const out = new Uint8Array(size);
    let at = 0;
    await szDecodeFolder(ctx, info, folderIndex, chunk => {
        const take = Math.min(chunk.length, size - at);
        out.set(chunk.subarray(0, take), at);
        at += take;
        return at < size;
    });
    return out;
}

async function list7z(ctx) {
    const src = ctx.src;
    const sig = src.read(0, 32);
    const nextOffset = u64(sig, 12);
    const nextSize = u64(sig, 20);
    if (32 + nextOffset + nextSize > src.size)
        throw archiveError("The 7z archive is truncated");
    let header = src.read(32 + nextOffset, nextSize);
    for (let guard = 0; guard < 4; guard++) {
        if (header.length === 0)
            return { entries: [] };
        if (header[0] !== 0x17)
            break;
        const r = new SzReader(header);
        r.byte();
        const info = szReadStreamsInfo(r);
        header = await szDecodeFolderToMemory(ctx, info, 0);
    }
    const r = new SzReader(header);
    if (r.byte() !== 0x01)
        throw archiveError("Damaged 7z header");
    let info = null;
    let files = [];
    for (;;) {
        const type = r.byte();
        if (type === 0x00)
            break;
        if (type === 0x02) {
            for (;;) { const t = r.byte(); if (t === 0) break; r.bytes(r.number()); }
        } else if (type === 0x03) {
            szReadStreamsInfo(r);
        } else if (type === 0x04) {
            info = szReadStreamsInfo(r);
        } else if (type === 0x05) {
            files = szReadFiles(r);
        } else {
            throw archiveError("Damaged 7z header");
        }
    }
    ctx.sz = info;
    const entries = [];
    if (!info)
        return { entries };
    let folder = 0;
    let sub = 0;
    let offset = 0;
    let streamIndex = 0;
    for (const f of files) {
        if (f.emptyStream)
            continue;
        while (folder < info.folders.length && sub >= info.subCounts[folder]) {
            folder++;
            sub = 0;
            offset = 0;
        }
        const size = info.subSizes[streamIndex++];
        entries.push({
            name: f.name, size, sizeKnown: true,
            folder, offset, solid: info.subCounts[folder] > 1,
            method: info.folders[folder].coders.map(c => szMethodName(c.id)).join("+")
        });
        sub++;
        offset += size;
    }
    return { entries };
}

async function szDecodeFolder(ctx, info, folderIndex, onChunk) {
    const folder = info.folders[folderIndex];
    if (folder.coders.length !== 1 || folder.numPacked !== 1)
        throw archiveError("Unsupported 7z method " +
            folder.coders.map(c => szMethodName(c.id)).join(" + ") +
            ": recompress with LZMA2, LZMA, Deflate or Copy");
    let packIndex = 0;
    for (let i = 0; i < folderIndex; i++)
        packIndex += info.folders[i].numPacked;
    let start = 32 + info.packPos;
    for (let i = 0; i < packIndex; i++)
        start += info.packSizes[i];
    const packSize = info.packSizes[packIndex];
    const size = szFolderSize(folder);
    const coder = folder.coders[0];
    switch (coder.id) {
    case "00": return copyRange(ctx, start, Math.min(packSize, size), onChunk);
    case "21": return lzmaDecode(ctx, true, coder.props, start, packSize, size, onChunk);
    case "030101": return lzmaDecode(ctx, false, coder.props, start, packSize, size, onChunk);
    case "040108": return inflateRange(ctx, "deflate-raw", start, packSize, onChunk);
    default:
        throw archiveError("Unsupported 7z method " + szMethodName(coder.id) +
            ": recompress with LZMA2, LZMA, Deflate or Copy");
    }
}

// ---------- zip ----------

function zipDecodeName(bytes, utf8) {
    if (!utf8) {
        try { return new TextDecoder("utf-8", { fatal: true }).decode(bytes); }
        catch (_) { return new TextDecoder("windows-1252").decode(bytes); }
    }
    return new TextDecoder("utf-8").decode(bytes);
}

function zipMethodName(method) {
    switch (method) {
    case 0: return "Store";
    case 8: return "Deflate";
    case 9: return "Deflate64";
    case 12: return "BZip2";
    case 14: return "LZMA";
    case 93: return "Zstandard";
    case 95: return "XZ";
    case 99: return "AES (encrypted)";
    default: return "method " + method;
    }
}

function listZip(ctx) {
    const src = ctx.src;
    const tailLen = Math.min(src.size, 65557);
    const tail = src.read(src.size - tailLen, tailLen);
    let eocd = -1;
    for (let i = tailLen - 22; i >= 0; i--)
        if (tail[i] === 0x50 && u32(tail, i) === 0x06054b50) { eocd = i; break; }
    if (eocd < 0)
        throw archiveError("Damaged zip archive (no central directory)");
    let count = u16(tail, eocd + 10);
    let cdSize = u32(tail, eocd + 12);
    let cdOffset = u32(tail, eocd + 16);
    if (eocd >= 20 && u32(tail, eocd - 20) === 0x07064b50) {
        const z = src.read(u64(tail, eocd - 20 + 8), 56);
        if (u32(z, 0) === 0x06064b50) {
            count = u64(z, 32);
            cdSize = u64(z, 40);
            cdOffset = u64(z, 48);
        }
    }
    const cd = src.read(cdOffset, cdSize);
    const entries = [];
    let p = 0;
    for (let i = 0; i < count && p + 46 <= cd.length; i++) {
        if (u32(cd, p) !== 0x02014b50)
            throw archiveError("Damaged zip central directory");
        const flags = u16(cd, p + 8);
        const method = u16(cd, p + 10);
        let csize = u32(cd, p + 20);
        let usize = u32(cd, p + 24);
        const nameLen = u16(cd, p + 28);
        const extraLen = u16(cd, p + 30);
        const commentLen = u16(cd, p + 32);
        let local = u32(cd, p + 42);
        const name = zipDecodeName(cd.subarray(p + 46, p + 46 + nameLen), (flags & 0x800) !== 0);
        let e = p + 46 + nameLen;
        const extraEnd = e + extraLen;
        while (e + 4 <= extraEnd) {
            const id = u16(cd, e);
            const len = u16(cd, e + 2);
            if (id === 0x0001) {
                let q = e + 4;
                if (usize === 0xffffffff) { usize = u64(cd, q); q += 8; }
                if (csize === 0xffffffff) { csize = u64(cd, q); q += 8; }
                if (local === 0xffffffff) { local = u64(cd, q); q += 8; }
            }
            e += 4 + len;
        }
        p = extraEnd + commentLen;
        if (name.endsWith("/"))
            continue;
        entries.push({
            name, size: usize, sizeKnown: true, method: zipMethodName(method),
            zipMethod: method, flags, csize, local
        });
    }
    return { entries };
}

async function extractZip(ctx, entry, onChunk) {
    if (entry.flags & 1)
        throw archiveError("The zip entry is encrypted");
    const head = ctx.src.read(entry.local, 30);
    if (u32(head, 0) !== 0x04034b50)
        throw archiveError("Damaged zip local header");
    const start = entry.local + 30 + u16(head, 26) + u16(head, 28);
    switch (entry.zipMethod) {
    case 0: return copyRange(ctx, start, entry.size, onChunk);
    case 8: return inflateRange(ctx, "deflate-raw", start, entry.csize, onChunk);
    case 14: {
        const h = ctx.src.read(start, 4);
        const propsSize = u16(h, 2);
        const props = ctx.src.read(start + 4, propsSize);
        const data = start + 4 + propsSize;
        return lzmaDecode(ctx, false, props, data, entry.csize - 4 - propsSize, entry.size, onChunk);
    }
    case 95: return xzDecode(ctx, start, start + entry.csize, onChunk);
    default:
        throw archiveError("Unsupported zip method " + zipMethodName(entry.zipMethod) +
            ": recompress with Deflate, LZMA or XZ");
    }
}

// ---------- xz ----------

function xzVarint(b, p) {
    let value = 0;
    for (let i = 0; i < 9; i++) {
        const byte = b[p.at++];
        if (byte === undefined)
            throw archiveError("Damaged xz data");
        value += (byte & 0x7f) * Math.pow(2, 7 * i);
        if ((byte & 0x80) === 0)
            return value;
    }
    throw archiveError("Damaged xz data");
}

function xzCheckSize(type) {
    return type === 0 ? 0 : 4 << Math.floor((type - 1) / 3);
}

function align4(n) { return n + ((4 - (n % 4)) % 4); }

// Uncompressed size from the stream indexes, walking back from the end.
function xzTotalSize(src, begin, end) {
    let total = 0;
    while (end > begin) {
        while (end - 4 >= begin) {
            const pad = src.read(end - 4, 4);
            if (pad[0] || pad[1] || pad[2] || pad[3]) break;
            end -= 4;
        }
        if (end - 12 < begin)
            break;
        const footer = src.read(end - 12, 12);
        if (footer[10] !== 0x59 || footer[11] !== 0x5a)
            throw archiveError("Damaged xz footer");
        const backward = (u32(footer, 4) + 1) * 4;
        const indexStart = end - 12 - backward;
        const index = src.read(indexStart, backward);
        const p = { at: 1 };
        const records = xzVarint(index, p);
        let blocks = 0;
        for (let i = 0; i < records; i++) {
            blocks += align4(xzVarint(index, p));
            total += xzVarint(index, p);
        }
        end = indexStart - blocks - 12;
    }
    return total;
}

async function xzDecode(ctx, begin, end, onChunk) {
    const src = ctx.src;
    let pos = begin;
    let stopped = false;
    const relay = chunk => {
        const r = onChunk(chunk);
        if (r && typeof r.then === "function")
            return r.then(v => { if (v === false) stopped = true; return v; });
        if (r === false) stopped = true;
        return r;
    };
    while (pos + 12 <= end && !stopped) {
        const head = src.read(pos, 12);
        if (!startsWith(head, SIG_XZ)) {
            if (!head[0] && !head[1] && !head[2] && !head[3]) { pos += 4; continue; }
            throw archiveError("Damaged xz stream");
        }
        const checkType = head[7] & 0x0f;
        const streamStart = pos;
        pos += 12;
        for (;;) {
            const first = src.read(pos, 1)[0];
            if (first === 0x00)
                break;
            const hsize = (first + 1) * 4;
            const h = src.read(pos, hsize);
            const flags = h[1];
            const p = { at: 2 };
            const packed = (flags & 0x40) ? xzVarint(h, p) : -1;
            const unpacked = (flags & 0x80) ? xzVarint(h, p) : -1;
            const filters = (flags & 3) + 1;
            const id = xzVarint(h, p);
            const propsSize = xzVarint(h, p);
            const props = h.slice(p.at, p.at + propsSize);
            if (filters !== 1 || id !== 0x21)
                throw archiveError("Unsupported xz filter chain: recompress with plain LZMA2 (xz -9)");
            pos += hsize;
            const result = await lzmaDecode(ctx, true, props, pos,
                packed >= 0 ? packed : end - pos, unpacked, relay);
            if (stopped)
                return;
            pos = streamStart + align4(pos + result.used - streamStart) + xzCheckSize(checkType);
        }
        const window = src.read(pos, Math.min(end - pos, 1 << 20));
        const p = { at: 1 };
        const records = xzVarint(window, p);
        for (let i = 0; i < records * 2; i++) xzVarint(window, p);
        pos = streamStart + align4(pos + p.at - streamStart) + 4 + 12;
    }
}

// ---------- gzip ----------

function gzName(src, fallback) {
    const h = src.read(0, Math.min(src.size, 4096));
    if (h[3] & 0x08) {
        let p = 10;
        if (h[3] & 0x04) p += 2 + u16(h, 10);
        const end = h.indexOf(0, p);
        if (end > p)
            return new TextDecoder("windows-1252").decode(h.subarray(p, end)).replace(/^.*[\\/]/, "");
    }
    return fallback;
}

// ---------- codecs ----------

async function copyRange(ctx, start, size, onChunk) {
    const step = 8 << 20;
    for (let at = 0; at < size; at += step) {
        const r = onChunk(ctx.src.read(start + at, Math.min(step, size - at)));
        if ((r && typeof r.then === "function" ? await r : r) === false)
            return;
    }
}

async function inflateRange(ctx, format, start, size, onChunk) {
    if (typeof DecompressionStream !== "function")
        throw archiveError("This browser cannot decompress Deflate (no DecompressionStream)");
    const stream = ctx.src.blob.slice(start, start + size).stream()
        .pipeThrough(new DecompressionStream(format));
    const reader = stream.getReader();
    for (;;) {
        const { done, value } = await reader.read();
        if (done)
            return;
        const r = onChunk(value);
        if ((r && typeof r.then === "function" ? await r : r) === false) {
            try { await reader.cancel(); } catch (_) {}
            return;
        }
    }
}

// Returns { used } = compressed bytes consumed.
async function lzmaDecode(ctx, lzma2, props, start, packSize, unpackSize, onChunk) {
    const M = await ctx.core();
    const propsPtr = M._malloc(Math.max(1, props.length));
    M.HEAPU8.set(props, propsPtr);
    const d = M._wa_dec_new(lzma2 ? 1 : 0, propsPtr, props.length, unpackSize >= 0 ? unpackSize : -1);
    M._free(propsPtr);
    if (!d)
        throw archiveError("Not enough memory for the " + (lzma2 ? "LZMA2" : "LZMA") + " dictionary");
    try {
        const inPtr = M._wa_dec_input(d);
        const cap = M._wa_dec_input_capacity();
        const end = start + packSize;
        let readPos = start;
        let have = 0;
        let produced = 0;
        for (;;) {
            if (have < cap && readPos < end) {
                const n = Math.min(cap - have, end - readPos);
                M.HEAPU8.set(ctx.src.read(readPos, n), inPtr + have);
                have += n;
                readPos += n;
            }
            // LZMA2 reads its end mark after the last byte: ask for it with finishEnd.
            let limit = -1;
            if (unpackSize >= 0) {
                const left = unpackSize - produced;
                if (left <= 0 && !lzma2)
                    break;
                limit = Math.min(left, 0x40000000);
            }
            const status = M._wa_dec_run(d, have, limit, lzma2 && limit >= 0 && limit === unpackSize - produced ? 1 : 0);
            if (status < 0)
                throw archiveError("Damaged " + (lzma2 ? "LZMA2" : "LZMA") + " data");
            const consumed = M._wa_dec_consumed(d);
            const outSize = M._wa_dec_out_size(d);
            if (consumed > 0) {
                M.HEAPU8.copyWithin(inPtr, inPtr + consumed, inPtr + have);
                have -= consumed;
            }
            if (outSize > 0) {
                produced += outSize;
                const out = M._wa_dec_out_ptr(d);
                const r = onChunk(M.HEAPU8.subarray(out, out + outSize));
                if ((r && typeof r.then === "function" ? await r : r) === false)
                    break;
            }
            if (status === ARCHIVE_STATUS_FINISHED_WITH_MARK)
                break;
            if (outSize === 0 && consumed === 0 && readPos >= end) {
                if (unpackSize < 0 || produced >= unpackSize)
                    break;
                throw archiveError("Truncated " + (lzma2 ? "LZMA2" : "LZMA") + " data");
            }
        }
        return { used: readPos - start - have, produced };
    } finally {
        M._wa_dec_free(d);
    }
}

// ---------- archive API ----------

async function archiveList(ctx, fileName) {
    const head = ctx.src.read(0, Math.min(ctx.src.size, 16));
    const format = archiveFormat(head);
    ctx.format = format;
    let result;
    if (format === "zip") {
        result = listZip(ctx);
    } else if (format === "7z") {
        result = await list7z(ctx);
    } else if (format === "xz") {
        result = { entries: [{ name: stripSuffix(fileName, /\.(xz|txz)$/i),
            size: xzTotalSize(ctx.src, 0, ctx.src.size), sizeKnown: true, method: "XZ" }] };
    } else if (format === "gz") {
        result = { entries: [{ name: gzName(ctx.src, stripSuffix(fileName, /\.(gz|gzip)$/i)),
            size: u32(ctx.src.read(ctx.src.size - 4, 4), 0), sizeKnown: false, method: "GZip" }] };
    } else {
        throw archiveError("Not a .zip, .7z, .xz or .gz archive");
    }
    result.format = format;
    ctx.entries = result.entries;
    return result;
}

// Streams entry `index`, calling onChunk(Uint8Array) in order. The chunk may
// alias decoder memory: copy it before the next call if it must be kept.
async function archiveExtract(ctx, index, onChunk) {
    const entry = ctx.entries[index];
    if (!entry)
        throw archiveError("No such archive entry");
    if (ctx.format === "zip")
        return extractZip(ctx, entry, onChunk);
    if (ctx.format === "xz")
        return xzDecode(ctx, 0, ctx.src.size, onChunk);
    if (ctx.format === "gz")
        return inflateRange(ctx, "gzip", 0, ctx.src.size, onChunk);
    let skip = entry.offset;
    let left = entry.size;
    if (left === 0)
        return;
    return szDecodeFolder(ctx, ctx.sz, entry.folder, chunk => {
        if (skip >= chunk.length) { skip -= chunk.length; return true; }
        const part = chunk.subarray(skip, Math.min(chunk.length, skip + left));
        skip = 0;
        left -= part.length;
        const r = onChunk(part);
        if (left <= 0) return r && typeof r.then === "function" ? r.then(() => false) : false;
        return r;
    });
}

function archiveContext(src, coreLoader) {
    let corePromise = null;
    return {
        src,
        core() {
            if (!corePromise)
                corePromise = coreLoader();
            return corePromise;
        }
    };
}

if (typeof module !== "undefined" && module.exports) {
    module.exports = { archiveContext, archiveList, archiveExtract, archiveFormat };
}

// ---------- Worker ----------

if (typeof WorkerGlobalScope !== "undefined" && self instanceof WorkerGlobalScope) {
    const version = new URLSearchParams(self.location.search).get("v") || "dev";
    let ctx = null;

    const loadCore = () => {
        importScripts("wasmidi-archive-core.js?v=" + encodeURIComponent(version));
        return self.WasmidiArchiveCore({
            locateFile: path => path + "?v=" + encodeURIComponent(version)
        });
    };

    const fileSource = file => {
        const reader = new FileReaderSync();
        return {
            blob: file,
            size: file.size,
            read(offset, length) {
                return new Uint8Array(reader.readAsArrayBuffer(file.slice(offset, offset + length)));
            }
        };
    };

    async function spoolDirectory() {
        if (!self.navigator || !navigator.storage || !navigator.storage.getDirectory)
            return null;
        try {
            const root = await navigator.storage.getDirectory();
            return await root.getDirectoryHandle(ARCHIVE_SPOOL_DIR, { create: true });
        } catch (_) {
            return null;
        }
    }

    // Removes spool files no open page holds a lock for.
    async function removeStale(dir) {
        if (!navigator.locks || !dir.entries)
            return;
        const state = await navigator.locks.query();
        const held = new Set([...(state.held || []), ...(state.pending || [])].map(l => l.name));
        const stale = [];
        for await (const [name] of dir.entries())
            if (!held.has(ARCHIVE_LOCK_PREFIX + name))
                stale.push(name);
        for (const name of stale) {
            try { await dir.removeEntry(name); } catch (_) {}
        }
    }

    async function extract(message) {
        const entry = ctx.entries[message.index];
        const size = entry.sizeKnown ? entry.size : -1;
        let written = 0;
        let lastReport = 0;
        const report = () => {
            const now = Date.now();
            if (now - lastReport < 200) return;
            lastReport = now;
            postMessage({ type: "progress", written, size });
        };
        const dir = (size < 0 || size > ARCHIVE_MEMORY_LIMIT) ? await spoolDirectory() : null;
        if (dir && dir.getFileHandle && self.FileSystemFileHandle &&
                FileSystemFileHandle.prototype.createSyncAccessHandle) {
            await removeStale(dir);
            const spool = message.spool;
            const handle = await dir.getFileHandle(spool, { create: true });
            const access = await handle.createSyncAccessHandle();
            try {
                access.truncate(0);
                if (size > 0)
                    access.truncate(size);
                await archiveExtract(ctx, message.index, chunk => {
                    let at = 0;
                    while (at < chunk.length)
                        at += access.write(at ? chunk.subarray(at) : chunk, { at: written + at });
                    written += chunk.length;
                    report();
                });
                if (written !== size)
                    access.truncate(written);
                access.flush();
            } finally {
                access.close();
            }
            const file = await handle.getFile();
            postMessage({ type: "done", file, spool, written });
            return;
        }
        const parts = [];
        await archiveExtract(ctx, message.index, chunk => {
            parts.push(chunk.slice());
            written += chunk.length;
            report();
        });
        postMessage({ type: "done", file: new Blob(parts), spool: "", written });
    }

    self.onmessage = async event => {
        const message = event.data || {};
        try {
            if (message.type === "list") {
                ctx = archiveContext(fileSource(message.file), loadCore);
                const result = await archiveList(ctx, message.file.name);
                postMessage({
                    type: "list", format: result.format,
                    entries: result.entries.map(e => ({
                        name: e.name, size: e.size, sizeKnown: e.sizeKnown, method: e.method, solid: !!e.solid
                    }))
                });
            } else if (message.type === "extract") {
                await extract(message);
            }
        } catch (error) {
            postMessage({ type: "error", name: error && error.name ? String(error.name) : "",
                message: error && error.message ? error.message : String(error) });
        }
    };
}
