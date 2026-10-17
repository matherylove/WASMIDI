#!/usr/bin/env python3
"""Headless Chromium checks against a served site/ (tools/serve_site.py running).

  python3 tools/browser_bench.py parser FILE.mid        parser-bench.html report
  python3 tools/browser_bench.py audio FILE.mid SF.sf2 [SECONDS] [--gpu]
        WASMIDI Audio: load both, play SECONDS (default 20) and print the synth
        telemetry every second (voices, render load, buffer fill, underruns).
  python3 tools/browser_bench.py shot PAGE OUT.png      screenshot (e.g. audio.html)
  python3 tools/browser_bench.py visual FILE.mid START DURATION   visual-bench.html report
  python3 tools/browser_bench.py app FILE.mid SF.sf2 [SECONDS]
        WASMIDI (roll): load both by clicking the 1280x900 layout, play, and print every second
        the keyboard/counter round trips (requests, replies, latency) and how far the roll's
        prepared notes lead the playhead (ticks), measured on the parser Worker's messages.

Options: --url http://127.0.0.1:8080  --headed  --log (every console message)
         --query=helpers=3 (appended to the page URL)
         --voices=N --workers=N (audio: applies the WasmiSynth settings before playing)
         --seek=S (app: at second S click the position slider near the start)
         --hide=S (audio: from second S on, the page reports itself hidden, like a tab in
                   the background; the clock on the page must stop while the audio goes on)
Chromium: $WASMIDI_CHROMIUM, else /opt/pw-browsers/chromium if present, else Playwright's. Headless
Chromium normally has no real GPU: WebGPU results there are not representative."""
import asyncio
import os
import sys

from playwright.async_api import async_playwright

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = [a for a in sys.argv[1:] if a.startswith("--")]
url = next((f.split("=", 1)[1] for f in flags if f.startswith("--url=")), "http://127.0.0.1:8080")
headed = "--headed" in flags
log_all = "--log" in flags
query = next((f.split("=", 1)[1] for f in flags if f.startswith("--query=")), "")
qs = ("?" + query) if query else ""
chromium = os.environ.get("WASMIDI_CHROMIUM") or (
    "/opt/pw-browsers/chromium" if os.path.exists("/opt/pw-browsers/chromium") else None)


async def main():
    if not args:
        sys.exit(__doc__)
    mode = args[0]
    async with async_playwright() as p:
        browser = await p.chromium.launch(
            headless=not headed, executable_path=chromium,
            # The Vulkan flag breaks the app's WebGL2 roll in headless Chromium; WebGPU needs it.
            args=(["--autoplay-policy=no-user-gesture-required"] if mode == "app" else
                  ["--enable-unsafe-webgpu", "--enable-features=Vulkan", "--autoplay-policy=no-user-gesture-required"]))
        page = await browser.new_page(viewport={"width": 1280, "height": 900})
        page.on("console", lambda m: print("[console]", m.type, m.text) if log_all or m.type in ("error", "warning") else None)
        page.on("pageerror", lambda e: print("[pageerror]", e))
        if mode == "parser":
            await page.goto(url + "/parser-bench.html")
            await page.wait_for_function("() => !document.getElementById('run').disabled", timeout=120000)
            await page.set_input_files("#file", args[1])
            await page.click("#run")
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
                    shown: document.getElementById("clock").textContent }; }""")
                print("%3d s  clock %7.2f  shown %s  voices %6s  load %5s%%  buffer %5s%%  underruns %s  caught up %5.0f ms"
                      "  late notes %s  dropped %s  gpu %s" % (
                    second + 1, st["t"] or 0, st["shown"], st["voices"], round(st["load"] or 0), round(st["buffer"] or 0),
                    st["underruns"], st["caught"] * 1000, st["late"], st["dropped"], st["gpu"]))
            print(await page.text_content("#log"))
        elif mode == "visual":
            await page.goto(url + "/visual-bench.html" + qs)
            await page.set_input_files("#file", args[1])
            await page.fill("#start", args[2] if len(args) > 2 else "0")
            await page.fill("#duration", args[3] if len(args) > 3 else "20")
            await page.click("#run")
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
            await choose(268, 551, args[2])
            await page.wait_for_function("() => globalThis.WasmidiSnappyBridge && WasmidiSnappyBridge.state.soundfontLoaded",
                                         timeout=600000)
            voices = next((f.split("=", 1)[1] for f in flags if f.startswith("--voices=")), None)
            if voices:
                await page.mouse.dblclick(82, 630)
                await page.keyboard.press("Control+A")
                await page.keyboard.type(voices)
                await page.keyboard.press("Enter")
                await page.wait_for_timeout(1500)
            await choose(800, 400, args[1])
            await page.wait_for_function("() => window.__wb.loaded", timeout=3600000)
            await page.wait_for_timeout(3000)
            await page.mouse.click(25, 175)  # play, after the file row appears
            seek_at = next((int(f.split("=", 1)[1]) for f in flags if f.startswith("--seek=")), None)
            for second in range(int(seconds)):
                if seek_at is not None and second == seek_at:
                    await page.mouse.click(110, 170)  # position slider, about 18% of the song
                await page.wait_for_timeout(1000)
                st = await page.evaluate("""() => { const s = window.__wb, lat = s.lat.splice(0);
                    const r = { req: s.req, rep: s.rep, n: lat.length, avg: lat.length ? lat.reduce((a, b) => a + b) / lat.length : 0,
                        max: lat.length ? Math.max(...lat) : 0, tick: s.tick, lead: s.safe - s.tick, deltas: s.deltas,
                        voices: globalThis.WasmidiSnappyBridge ? WasmidiSnappyBridge.state.activeVoices : -1,
                        underruns: globalThis.WasmidiSnappyBridge ? WasmidiSnappyBridge.state.underruns : -1 };
                    s.req = s.rep = s.deltas = 0; return r; }""")
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
