"use strict";
// Opens a MIDI or SoundFont from inside a .zip / .7z / .xz / .gz (HANDOFF sec. 110).
// WasmidiArchive.open(file, { kind }) resolves to the extracted File, or null if the
// user cancelled or it failed (the window shows why).
(function () {
    const SPOOL_DIR = "wasmidi-archive";
    const LOCK_PREFIX = "wasmidi-archive:";
    const ARCHIVE_RE = /\.(zip|7z|xz|txz|gz|gzip)$/i;
    const KINDS = {
        midi: { re: /\.(mid|midi|kar|rmi|smf)$/i, label: "MIDI", type: "audio/midi" },
        soundfont: { re: /\.(sf2|sf3)$/i, label: "SoundFont", type: "application/octet-stream" },
        any: { re: /\.(mid|midi|kar|rmi|smf|sf2|sf3)$/i, label: "file", type: "" }
    };

    const scriptSrc = document.currentScript && document.currentScript.src;
    const version = (() => {
        try { return new URL(scriptSrc).searchParams.get("v") || "dev"; } catch (_) { return "dev"; }
    })();
    const held = {};

    function isArchive(file) {
        return !!file && ARCHIVE_RE.test(String(file.name || ""));
    }

    function formatBytes(n) {
        if (!(n >= 0)) return "?";
        if (n >= 1 << 30) return (n / (1 << 30)).toFixed(2) + " GB";
        if (n >= 1 << 20) return (n / (1 << 20)).toFixed(1) + " MB";
        if (n >= 1 << 10) return (n / (1 << 10)).toFixed(0) + " KB";
        return n + " B";
    }

    function holdLock(name) {
        if (!navigator.locks)
            return Promise.resolve(() => {});
        return new Promise(granted => {
            navigator.locks.request(name, () => new Promise(release => granted(release)));
        });
    }

    async function spoolDir(create) {
        if (!navigator.storage || !navigator.storage.getDirectory)
            return null;
        try {
            const root = await navigator.storage.getDirectory();
            return await root.getDirectoryHandle(SPOOL_DIR, { create });
        } catch (_) {
            return null;
        }
    }

    async function removeSpool(name) {
        const dir = await spoolDir(false);
        if (dir) {
            try { await dir.removeEntry(name); } catch (_) {}
        }
    }

    // Spool files of pages that are gone (their lock was released with them).
    async function removeStale() {
        const dir = await spoolDir(false);
        if (!dir || !navigator.locks || !dir.entries)
            return;
        const state = await navigator.locks.query();
        const names = new Set([...(state.held || []), ...(state.pending || [])].map(l => l.name));
        const stale = [];
        for await (const [name] of dir.entries())
            if (!names.has(LOCK_PREFIX + name))
                stale.push(name);
        for (const name of stale) {
            try { await dir.removeEntry(name); } catch (_) {}
        }
    }

    // ---------- window ----------

    const CSS = `
.wa-shade{position:fixed;inset:0;z-index:2147483000;display:grid;place-items:center;background:rgba(7,7,26,.72);font:12px Tahoma,Verdana,'Segoe UI',Geneva,sans-serif;color:#dcd8f0}
.wa-win{width:min(520px,calc(100% - 32px));max-height:calc(100% - 32px);display:flex;flex-direction:column;background:#16132f;padding:4px;box-shadow:inset -1px -1px #000,inset 1px 1px #5a4cb5,inset -2px -2px #0b0922,inset 2px 2px #2e2768}
.wa-title{height:20px;flex:none;display:flex;align-items:center;padding:0 4px;background:linear-gradient(90deg,#1f175a,#6b55d4);color:#fff;font-weight:bold;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.wa-body{padding:10px 8px;display:flex;flex-direction:column;gap:8px;min-height:0}
.wa-text{overflow-wrap:anywhere}
.wa-list{overflow:auto;min-height:0;max-height:50vh;background:#0b0922;box-shadow:inset 1px 1px #000,inset -1px -1px #2e2768;padding:2px}
.wa-item{display:flex;gap:8px;width:100%;text-align:left;background:none;border:0;color:inherit;font:inherit;padding:4px 6px;cursor:pointer}
.wa-item:hover,.wa-item:focus{background:#2e2768;outline:none}
.wa-item .wa-name{flex:1;overflow-wrap:anywhere}
.wa-item .wa-size{flex:none;color:#9a93c8}
.wa-bar{height:14px;background:#0b0922;box-shadow:inset 1px 1px #000,inset -1px -1px #2e2768}
.wa-fill{height:100%;width:0;background:linear-gradient(90deg,#6b55d4,#b09cff)}
.wa-row{display:flex;justify-content:flex-end;gap:6px}
.wa-btn{min-width:72px;padding:3px 10px;background:#2a2457;color:#fff;border:0;font:inherit;cursor:pointer;box-shadow:inset -1px -1px #000,inset 1px 1px #7a6ae0}
.wa-btn:active{box-shadow:inset 1px 1px #000,inset -1px -1px #7a6ae0}
.wa-err{color:#ff9fb3}`;

    function makeWindow(title) {
        if (!document.getElementById("wasmidi-archive-css")) {
            const style = document.createElement("style");
            style.id = "wasmidi-archive-css";
            style.textContent = CSS;
            document.head.appendChild(style);
        }
        const shade = document.createElement("div");
        shade.className = "wa-shade";
        shade.innerHTML = '<div class="wa-win" role="dialog" aria-modal="true"><div class="wa-title"></div><div class="wa-body"></div></div>';
        shade.querySelector(".wa-title").textContent = title;
        document.body.appendChild(shade);
        const body = shade.querySelector(".wa-body");
        return {
            body,
            clear() { body.textContent = ""; },
            text(t, cls) {
                const div = document.createElement("div");
                div.className = "wa-text" + (cls ? " " + cls : "");
                div.textContent = t;
                body.appendChild(div);
                return div;
            },
            buttons(list) {
                const row = document.createElement("div");
                row.className = "wa-row";
                for (const [label, fn] of list) {
                    const b = document.createElement("button");
                    b.className = "wa-btn";
                    b.type = "button";
                    b.textContent = label;
                    b.onclick = fn;
                    row.appendChild(b);
                }
                body.appendChild(row);
                return row;
            },
            close() { shade.remove(); }
        };
    }

    function pick(win, entries, kind) {
        return new Promise(resolve => {
            win.clear();
            win.text("This archive has " + entries.length + " files. Pick the " + KINDS[kind].label + " to open:");
            const list = document.createElement("div");
            list.className = "wa-list";
            for (const e of entries) {
                const b = document.createElement("button");
                b.type = "button";
                b.className = "wa-item";
                b.innerHTML = '<span class="wa-name"></span><span class="wa-size"></span>';
                b.querySelector(".wa-name").textContent = e.name;
                b.querySelector(".wa-size").textContent = e.sizeKnown ? formatBytes(e.size) : "";
                b.onclick = () => resolve(e);
                list.appendChild(b);
            }
            win.body.appendChild(list);
            win.buttons([["Cancel", () => resolve(null)]]);
            const first = list.querySelector("button");
            if (first) first.focus();
        });
    }

    function failure(win, message) {
        return new Promise(resolve => {
            win.clear();
            win.text(message, "wa-err");
            const row = win.buttons([["Close", () => resolve(null)]]);
            row.querySelector("button").focus();
        });
    }

    function call(worker, message) {
        return new Promise((resolve, reject) => {
            worker.onmessage = event => {
                const data = event.data || {};
                if (data.type === "progress") {
                    if (call.onProgress) call.onProgress(data);
                    return;
                }
                if (data.type === "error") reject(Object.assign(new Error(data.message), { name: data.name || "Error" }));
                else resolve(data);
            };
            worker.onerror = event => reject(new Error(event.message || "Archive Worker failed"));
            worker.postMessage(message);
        });
    }

    async function open(file, options) {
        let kind = KINDS[options && options.kind] ? options.kind : "midi";
        const spec = KINDS[kind];
        const win = makeWindow("Open " + spec.label + " from " + (file.name || "archive"));
        win.text("Reading archive…");
        let worker = null;
        let cancelled = false;
        let release = null;
        let spool = "";
        const stop = () => {
            cancelled = true;
            if (worker) worker.terminate();
        };
        try {
            worker = new Worker("archive-worker.js?v=" + encodeURIComponent(version));
            const listing = await call(worker, { type: "list", file });
            let entries = listing.entries.filter(e => spec.re.test(e.name));
            if (entries.length === 0)
                entries = listing.entries;
            if (entries.length === 0) {
                await failure(win, "The archive is empty.");
                return null;
            }
            const entry = entries.length === 1 ? entries[0] : await pick(win, entries, kind);
            if (!entry)
                return null;
            const index = listing.entries.indexOf(entry);
            spool = Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 10) + ".bin";
            release = await holdLock(LOCK_PREFIX + spool);

            win.clear();
            win.text("Extracting " + entry.name + (entry.sizeKnown ? " (" + formatBytes(entry.size) + ")" : ""));
            const bar = document.createElement("div");
            bar.className = "wa-bar";
            bar.innerHTML = '<div class="wa-fill"></div>';
            win.body.appendChild(bar);
            const fill = bar.firstChild;
            const status = win.text(entry.method ? entry.method : "");
            win.buttons([["Cancel", stop]]);
            const started = performance.now();
            call.onProgress = p => {
                const seconds = (performance.now() - started) / 1000;
                if (p.size > 0) fill.style.width = Math.min(100, 100 * p.written / p.size).toFixed(1) + "%";
                status.textContent = formatBytes(p.written) + (p.size > 0 ? " of " + formatBytes(p.size) : "") +
                    (seconds > 0.5 ? " · " + formatBytes(p.written / seconds) + "/s" : "");
            };
            const cancelPromise = new Promise(resolve => {
                const check = setInterval(() => { if (cancelled) { clearInterval(check); resolve(null); } }, 100);
            });
            const done = await Promise.race([call(worker, { type: "extract", index, spool }), cancelPromise]);
            if (!done)
                return null;
            console.info("[WASMIDI archive] " + entry.name + ": " + formatBytes(done.written) + " in " +
                ((performance.now() - started) / 1000).toFixed(1) + " s" + (done.spool ? " (browser storage)" : " (memory)"));
            const name = String(entry.name).replace(/^.*[\\/]/, "");
            const type = kind === "any" ? (KINDS.midi.re.test(name) ? KINDS.midi.type : "") : spec.type;
            const out = new File([done.file], name, { type });
            if (kind === "any")
                kind = KINDS.soundfont.re.test(name) ? "soundfont" : "midi";
            const previous = held[kind];
            held[kind] = done.spool ? { spool, release } : null;
            if (!done.spool) release();
            release = null;
            if (previous) {
                previous.release();
                void removeSpool(previous.spool);
            }
            return out;
        } catch (error) {
            if (!cancelled) {
                let message = error && error.message ? error.message : String(error);
                if (/quota/i.test(message) || (error && error.name === "QuotaExceededError"))
                    message = "Not enough browser storage to extract this file. Free disk space and try again.";
                await failure(win, message);
            }
            return null;
        } finally {
            call.onProgress = null;
            if (worker) worker.terminate();
            if (release) {
                release();
                if (spool) setTimeout(() => { void removeSpool(spool); }, 500);
            }
            win.close();
        }
    }

    globalThis.WasmidiArchive = {
        isArchive,
        open,
        accept: ".zip,.7z,.xz,.txz,.gz",
        // Opens `file` as is, or extracts it first when it is an archive.
        async resolve(file, kind) {
            return isArchive(file) ? open(file, { kind }) : file;
        }
    };

    setTimeout(() => { removeStale().catch(() => {}); }, 3000);
})();
