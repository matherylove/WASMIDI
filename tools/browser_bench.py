#!/usr/bin/env python3
"""Headless Chromium checks against a served site/ (tools/serve_site.py running).

  python3 tools/browser_bench.py parser FILE.mid        parser-bench.html report
  python3 tools/browser_bench.py audio FILE.mid SF.sf2 [SECONDS] [--gpu] [--rms]
        WASMIDI Audio: load both, play SECONDS (default 20) and print the synth
        telemetry every second (voices, render load, buffer fill, underruns).
  python3 tools/browser_bench.py shot PAGE OUT.png      screenshot (e.g. audio.html)
  python3 tools/browser_bench.py visual FILE.mid START DURATION   visual-bench.html report
  python3 tools/browser_bench.py app FILE.mid SF.sf2 [SECONDS]
        WASMIDI (roll): load both by clicking the 1280x900 layout, play, and print every second
        the keyboard/counter round trips (requests, replies, latency) and how far the roll's
        prepared notes lead the playhead (ticks), measured on the parser Worker's messages.

Options: --url=http://127.0.0.1:8080  --headed  --log (every console message)
         --query=helpers=3 (appended to the page URL)
         --voices=N --workers=N (audio: applies the WasmiSynth settings before playing)
         --seek=S (app: at second S click the position slider near the start; --seekx=X clicks at x=X)
         --setting=NAME=VALUE (app: a saved WASMIDI setting before the page loads, e.g. noteSpeed=0.1)
         --shot=S:OUT (app: screenshot at second S; repeatable)
         --resize=S:WxH (app: at second S the page becomes W x H, e.g. 420x860 for the phone layout)
         --pause=S:D (app: pause at second S and resume D seconds later)
         --profile=S:D:OUT (app: CPU profile of the page's main thread from second S for D seconds,
                   saved to OUT for DevTools; prints the functions with the most self time.
                   Function names in wasmidi.wasm need cmake -DWASMIDI_PROFILE_FUNCS=ON)
         --trace=S:D:OUT (app: Chromium trace from second S for D seconds, saved to OUT for
                   chrome://tracing; prints the main thread's top-level tasks by total time)
         --uncapped (Chromium draws frames as fast as it can, without vsync)
         --memdetail=S (app: at second S, performance.measureUserAgentSpecificMemory() by Worker)
         --mem (app, audio, visual, parser: print the PSS of all Chromium processes, by kind, every
                second and the peak; app: also the MIDI load time from the file pick to the first frame)
         --hide=S (audio: from second S on, the page reports itself hidden, like a tab in
                   the background; the clock on the page must stop while the audio goes on)
Chromium: $WASMIDI_CHROMIUM, else /opt/pw-browsers/chromium if present, else Playwright's. Headless
Chromium normally has no real GPU: WebGPU results there are not representative."""
import asyncio
import collections
import json
import os
import sys
import time

from playwright.async_api import async_playwright

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = [a for a in sys.argv[1:] if a.startswith("--")]
url = next((f.split("=", 1)[1] for f in flags if f.startswith("--url=")), "http://127.0.0.1:8080")
headed = "--headed" in flags
mem = "--mem" in flags
memdetail = next((int(f.split("=", 1)[1]) for f in flags if f.startswith("--memdetail=")), None)
log_all = "--log" in flags
query = next((f.split("=", 1)[1] for f in flags if f.startswith("--query=")), "")
qs = ("?" + query) if query else ""
uncapped = ["--disable-frame-rate-limit", "--disable-gpu-vsync"] if "--uncapped" in flags else []
chromium = os.environ.get("WASMIDI_CHROMIUM") or (
    "/opt/pw-browsers/chromium" if os.path.exists("/opt/pw-browsers/chromium") else None)


def chromium_pss():
    """PSS in MiB of every Chromium process of this machine, by --type= (browser, renderer, gpu...)."""
    kinds = collections.Counter()
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open("/proc/%s/cmdline" % pid, "rb") as f:
                cmd = f.read().replace(b"\0", b" ").split()
            if not cmd or b"chrom" not in os.path.basename(cmd[0]).lower():
                continue
            kind = next((a.split(b"=", 1)[1].decode() for a in cmd if a.startswith(b"--type=")), "browser")
            with open("/proc/%s/smaps_rollup" % pid) as f:
                pss = next(int(l.split()[1]) for l in f if l.startswith("Pss:"))
            kinds[kind] += pss / 1024.0
        except (OSError, StopIteration, ValueError):
            pass
    return kinds


mem_peak = {"total": 0.0}


def mem_line():
    kinds = chromium_pss()
    total = sum(kinds.values())
    mem_peak["total"] = max(mem_peak["total"], total)
    return "PSS %6.0f MiB (peak %6.0f)  " % (total, mem_peak["total"]) + "  ".join(
        "%s %.0f" % (k, v) for k, v in sorted(kinds.items()))


async def main():
    if not args:
        sys.exit(__doc__)
    mode = args[0]
    async with async_playwright() as p:
        browser = await p.chromium.launch(
            headless=not headed, executable_path=chromium,
            # The Vulkan flag breaks the app's WebGL2 roll in headless Chromium; WebGPU needs it.
            args=uncapped + ["--enable-blink-features=ForceEagerMeasureMemory"] + (["--autoplay-policy=no-user-gesture-required"] if mode == "app" else
                  ["--enable-unsafe-webgpu", "--enable-features=Vulkan", "--autoplay-policy=no-user-gesture-required"]))
        page = await browser.new_page(viewport={"width": 1280, "height": 900})
        page.on("console", lambda m: print("[console]", m.type, m.text) if log_all or m.type in ("error", "warning") else None)
        page.on("pageerror", lambda e: print("[pageerror]", e))
        if mode == "parser":
            await page.goto(url + "/parser-bench.html" + qs)
            await page.set_input_files("#file", args[1])
            await page.wait_for_function("() => !document.getElementById('run').disabled", timeout=120000)
            await page.click("#run")
            while mem and not await page.evaluate("() => /total/.test(document.getElementById('report').textContent)"):
                await page.wait_for_timeout(1000)
                print(mem_line())
            await page.wait_for_function("() => /total/.test(document.getElementById('report').textContent)",
                                         timeout=3600000)
            print(await page.text_content("#report"))
        elif mode == "audio":
            seconds = float(args[3]) if len(args) > 3 else 20.0
            await page.goto(url + "/audio.html" + qs)
            await page.wait_for_function("() => globalThis.crossOriginIsolated", timeout=60000)
            await page.set_input_files("#sf2-input", args[2])
            await page.wait_for_function("() => globalThis.WasmidiSnappyBridge && WasmidiSnappyBridge.state.soundfontLoaded",
                                         timeout=600000)
            settings = {k: f.split("=", 1)[1] for f in flags for k in ("voices", "workers") if f.startswith("--" + k + "=")}
            if settings:
                await page.evaluate("""s => { for (const k in s) document.getElementById("c-" + k).value = s[k];
                    document.getElementById("apply").click(); }""", settings)
                await page.wait_for_timeout(1500)
            if "--gpu" in flags:
                await page.click("#gpu")
            await page.set_input_files("#midi-input", args[1])
            await page.wait_for_function("() => !document.getElementById('play').disabled", timeout=3600000)
            await page.click("#play")
            if "--rms" in flags:
                await page.evaluate("""() => { const o = WasmidiSnappyBridge.getOutput(); const an = o.context.createAnalyser();
                    an.fftSize = 32768; o.node.connect(an); const buf = new Float32Array(an.fftSize); globalThis.__rms = [];
                    setInterval(() => { an.getFloatTimeDomainData(buf); let s = 0; for (const x of buf) s += x * x;
                        __rms.push(Math.sqrt(s / buf.length)); }, 250); }""")
            hide = next((float(f.split("=", 1)[1]) for f in flags if f.startswith("--hide=")), None)
            for second in range(int(seconds)):
                if hide is not None and second == int(hide):
                    await page.evaluate("""() => {
                        Object.defineProperty(document, "hidden", { configurable: true, get: () => true });
                        Object.defineProperty(document, "visibilityState", { configurable: true, get: () => "hidden" });
                        document.dispatchEvent(new Event("visibilitychange")); }""")
                await page.wait_for_timeout(1000)
                st = await page.evaluate("""() => { const s = WasmidiSnappyBridge.state; return {
                    t: WasmidiSnappyBridge.getAudioClock(), voices: s.activeVoices, load: s.renderLoadPercent,
                    buffer: s.ringFillPercent, underruns: s.underruns, dropped: s.droppedNotes, gpu: s.gpuStatus,
                    caught: s.catchUpSeconds || 0, late: s.lateDropped || 0,
                    shown: document.getElementById("clock").textContent,
                    rms: globalThis.__rms ? globalThis.__rms.splice(0).reduce((a, b) => Math.max(a, b), 0) : null }; }""")
                print("%3d s  clock %7.2f  shown %s  voices %6s  load %5s%%  buffer %5s%%  underruns %s  caught up %5.0f ms"
                      "  late notes %s  dropped %s  gpu %s" % (
                    second + 1, st["t"] or 0, st["shown"], st["voices"], round(st["load"] or 0), round(st["buffer"] or 0),
                    st["underruns"], st["caught"] * 1000, st["late"], st["dropped"], st["gpu"]))
                if st["rms"] is not None:
                    print("      rms max %.4f" % st["rms"])
            print(await page.text_content("#log"))
        elif mode == "visual":
            await page.goto(url + "/visual-bench.html" + qs)
            await page.set_input_files("#file", args[1])
            await page.fill("#start", args[2] if len(args) > 2 else "0")
            await page.fill("#duration", args[3] if len(args) > 3 else "20")
            await page.click("#run")
            while mem and not await page.evaluate("() => /total:/.test(document.getElementById('report').textContent)"):
                await page.wait_for_timeout(1000)
                print(mem_line())
            await page.wait_for_function("() => /total:/.test(document.getElementById('report').textContent)",
                                         timeout=3600000)
            print(await page.text_content("#report"))
        elif mode == "app":
            seconds = float(args[3]) if len(args) > 3 else 20.0
            await page.add_init_script("""(() => {
                const W = window.Worker, st = window.__wb = { req: 0, rep: 0, lat: [], pending: new Map(),
                    safe: 0, tick: 0, deltas: 0, loaded: false, first: 0, last: 0 };
                window.Worker = function (url, opts) {
                    const w = new W(url, opts), post = w.postMessage.bind(w);
                    w.postMessage = (m, t) => {
                        if (m && m.type === "key-state") { st.req++; st.tick = m.tick; st.pending.set(m.requestId >>> 0, performance.now()); }
                        return post(m, t);
                    };
                    w.addEventListener("message", e => {
                        const m = e.data || {};
                        if (m.type === "live-state") {
                            const t0 = st.pending.get(m.requestId >>> 0);
                            if (t0 !== undefined) { st.rep++; st.lat.push(performance.now() - t0); st.pending.delete(m.requestId >>> 0); }
                        } else if (m.type === "sharp-render-delta") { st.deltas++; st.safe = Math.max(st.safe, m.safeThrough || 0); }
                        else if (m.type === "sharp-render-reset") st.safe = 0;
                        else if (m.type === "result-end") st.loaded = true;
                        else if (m.type === "runtime-error" || m.type === "error") console.error("[bench] worker " + m.type + ": " + (m.requestType || "") + " " + m.message);
                    });
                    return w;
                };
                window.Worker.prototype = W.prototype;
            })()""")
            settings = dict(f.split("=", 1)[1].split("=", 1) for f in flags if f.startswith("--setting="))
            if settings:
                await page.add_init_script("(s => { for (const k in s) localStorage.setItem('wasmidi.settings.v1.' + k, s[k]); })(%s)"
                                           % json.dumps(settings))
            await page.goto(url + "/index.html" + qs)
            await page.wait_for_function("() => globalThis.crossOriginIsolated", timeout=60000)
            chosen = {"path": None, "done": 0}
            async def on_chooser(fc):
                if chosen["path"]:
                    await fc.set_files(chosen["path"])
                    chosen["path"] = None
                    chosen["done"] += 1
            page.on("filechooser", lambda fc: asyncio.ensure_future(on_chooser(fc)))
            async def choose(x, y, path):
                # Qt may still be starting: click until the browser file picker has been answered.
                done = chosen["done"]
                chosen["path"] = path
                for _ in range(20):
                    await page.mouse.click(x, y)
                    for _ in range(10):
                        await page.wait_for_timeout(500)
                        if chosen["done"] > done:
                            return
                await page.screenshot(path="/tmp/wasmidi_picker_fail.png")
                raise RuntimeError("no file picker at %d,%d (screenshot /tmp/wasmidi_picker_fail.png)" % (x, y))
            await page.wait_for_timeout(20000)
            await choose(1176, 710, args[2])  # SF2… in the WasmiSynth group
            await page.wait_for_function("() => globalThis.WasmidiSnappyBridge && WasmidiSnappyBridge.state.soundfontLoaded",
                                         timeout=600000)
            voices = next((f.split("=", 1)[1] for f in flags if f.startswith("--voices=")), None)
            if voices:
                await page.mouse.dblclick(1067, 739)  # Voices field
                await page.keyboard.press("Control+A")
                await page.keyboard.type(voices)
                await page.keyboard.press("Enter")
                await page.wait_for_timeout(1500)
            if mem:
                print("before the MIDI: " + mem_line())
            picked = time.monotonic()
            await choose(800, 400, args[1])
            if mem:
                while not await page.evaluate("() => window.__wb.loaded"):
                    await page.wait_for_timeout(1000)
                    print("loading %5.0f s  %s" % (time.monotonic() - picked, mem_line()))
            await page.wait_for_function("() => window.__wb.loaded", timeout=3600000)
            loaded_at = time.monotonic()
            if mem:
                print("MIDI loaded in %.1f s (file picker to result-end)  %s" % (loaded_at - picked, mem_line()))
            await page.wait_for_timeout(3000)
            await page.mouse.click(906, 88)  # play
            seek_at = next((int(f.split("=", 1)[1]) for f in flags if f.startswith("--seek=")), None)
            profile = next((f.split("=", 1)[1].split(":", 2) for f in flags if f.startswith("--profile=")), None)
            trace = next((f.split("=", 1)[1].split(":", 2) for f in flags if f.startswith("--trace=")), None)
            cdp = await page.context.new_cdp_session(page) if profile or trace else None
            pause = next((f.split("=", 1)[1].split(":", 1) for f in flags if f.startswith("--pause=")), None)
            seek_x = next((int(f.split("=", 1)[1]) for f in flags if f.startswith("--seekx=")), 963)
            shots = [f.split("=", 1)[1].split(":", 1) for f in flags if f.startswith("--shot=")]
            resize = next((f.split("=", 1)[1].split(":", 1) for f in flags if f.startswith("--resize=")), None)
            for second in range(int(seconds)):
                if pause and second in (int(pause[0]), int(pause[0]) + int(pause[1])):
                    await page.mouse.click(943 if second == int(pause[0]) else 906, 88)  # pause, then play
                if seek_at is not None and second == seek_at:
                    await page.mouse.click(seek_x, 60)  # position slider, about 18% of the song by default
                if resize and second == int(resize[0]):
                    w, h = (int(v) for v in resize[1].split("x"))
                    await page.set_viewport_size({"width": w, "height": h})
                for shot in shots:
                    if second == int(shot[0]):
                        await page.screenshot(path=shot[1])
                if trace and second == int(trace[0]):
                    await browser.start_tracing(page=page, path=trace[2], categories=[
                        "toplevel", "blink", "v8", "gpu", "devtools.timeline", "disabled-by-default-devtools.timeline",
                        "cc", "viz", "ipc", "mojom", "scheduler"] + [c for c in os.environ.get("WASMIDI_TRACE", "").split(",") if c])
                if trace and second == int(trace[0]) + int(trace[1]):
                    await browser.stop_tracing()
                    with open(trace[2]) as f:
                        events = json.load(f)
                    events = events["traceEvents"] if isinstance(events, dict) else events
                    names = {(e["pid"], e["tid"]): e["args"]["name"] for e in events
                             if e.get("ph") == "M" and e.get("name") == "thread_name"}
                    main = {k for k, v in names.items() if v == "CrRendererMain"}
                    by_name = collections.Counter()
                    count = collections.Counter()
                    longest = collections.Counter()
                    for e in events:
                        if (e.get("pid"), e.get("tid")) in main and e.get("ph") == "X" and "dur" in e:
                            key = e["name"]
                            by_name[key] += e["dur"]
                            count[key] += 1
                            longest[key] = max(longest[key], e["dur"])
                    print("trace: main-thread events by total time (nested events counted again):")
                    for name, us in by_name.most_common(40):
                        print("  %9.0f ms  %6d x  max %8.0f ms  %s" % (us / 1000.0, count[name], longest[name] / 1000.0, name[:120]))
                if profile and second == int(profile[0]):
                    await cdp.send("Profiler.enable")
                    await cdp.send("Profiler.setSamplingInterval", {"interval": 1000})
                    await cdp.send("Profiler.start")
                if profile and second == int(profile[0]) + int(profile[1]):
                    result = (await cdp.send("Profiler.stop"))["profile"]
                    with open(profile[2], "w") as out:
                        json.dump(result, out)
                    nodes = {n["id"]: n for n in result["nodes"]}
                    self_us = collections.Counter()
                    deltas = result.get("timeDeltas", [])
                    for sample, dt in zip(result.get("samples", []), deltas[1:] + [0]):
                        frame = nodes[sample]["callFrame"]
                        self_us[frame["functionName"] or "(anonymous) " + frame["url"].rsplit("/", 1)[-1]] += dt
                    total = sum(self_us.values()) or 1
                    print("profile: %.1f s sampled, top self time:" % (total / 1e6))
                    for name, us in self_us.most_common(30):
                        print("  %6.1f%% %8.0f ms  %s" % (100.0 * us / total, us / 1000.0, name[:140]))
                await page.wait_for_timeout(1000)
                st = await page.evaluate("""() => { const s = window.__wb, lat = s.lat.splice(0);
                    const r = { req: s.req, rep: s.rep, n: lat.length, avg: lat.length ? lat.reduce((a, b) => a + b) / lat.length : 0,
                        max: lat.length ? Math.max(...lat) : 0, tick: s.tick, lead: s.safe - s.tick, deltas: s.deltas,
                        voices: globalThis.WasmidiSnappyBridge ? WasmidiSnappyBridge.state.activeVoices : -1,
                        underruns: globalThis.WasmidiSnappyBridge ? WasmidiSnappyBridge.state.underruns : -1 };
                    s.req = s.rep = s.deltas = 0; return r; }""")
                if mem:
                    print("      " + mem_line())
                if memdetail is not None and second == memdetail:
                    print(await page.evaluate("""async () => {
                        const r = await performance.measureUserAgentSpecificMemory(), by = {};
                        for (const b of r.breakdown) {
                            const a = b.attribution[0], key = a ? (a.scope + " " + String(a.url).split("/").pop().split("?")[0]) : "shared/other";
                            by[key] = (by[key] || 0) + b.bytes;
                        }
                        return "measureUserAgentSpecificMemory: " + (r.bytes / 1048576).toFixed(0) + " MiB; " + Object.entries(by)
                            .sort((x, y) => y[1] - x[1]).map(([k, v]) => k + " " + (v / 1048576).toFixed(0)).join(", ");
                    }"""))
                print("%3d s  tick %8d  key-state %3d req %3d rep  latency avg %6.1f max %6.1f ms  roll lead %8d ticks  deltas %4d  voices %6s  underruns %s" % (
                    second + 1, st["tick"], st["req"], st["rep"], st["avg"], st["max"], st["lead"], st["deltas"],
                    st["voices"], st["underruns"]))
            await page.screenshot(path=os.environ.get("WASMIDI_SHOT", "/tmp/wasmidi_app.png"))
        elif mode == "shot":
            await page.goto(url + "/" + args[1])
            await page.wait_for_timeout(2000)
            await page.screenshot(path=args[2], full_page=True)
        await browser.close()

asyncio.run(main())
