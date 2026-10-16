#!/usr/bin/env python3
"""Headless Chromium checks against a served site/ (tools/serve_site.py running).

  python3 tools/browser_bench.py parser FILE.mid        parser-bench.html report
  python3 tools/browser_bench.py audio FILE.mid SF.sf2 [SECONDS] [--gpu]
        WASMIDI Audio: load both, play SECONDS (default 20) and print the synth
        telemetry every second (voices, render load, buffer fill, underruns).
  python3 tools/browser_bench.py shot PAGE OUT.png      screenshot (e.g. audio.html)

Options: --url http://127.0.0.1:8080  --headed
Chromium: Playwright's (PLAYWRIGHT_BROWSERS_PATH, e.g. /opt/pw-browsers). Headless
Chromium normally has no real GPU: WebGPU results there are not representative."""
import asyncio
import sys

from playwright.async_api import async_playwright

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = [a for a in sys.argv[1:] if a.startswith("--")]
url = next((f.split("=", 1)[1] for f in flags if f.startswith("--url=")), "http://127.0.0.1:8080")
headed = "--headed" in flags


async def main():
    if not args:
        sys.exit(__doc__)
    mode = args[0]
    async with async_playwright() as p:
        browser = await p.chromium.launch(
            headless=not headed,
            args=["--enable-unsafe-webgpu", "--enable-features=Vulkan", "--autoplay-policy=no-user-gesture-required"])
        page = await browser.new_page(viewport={"width": 1280, "height": 900})
        page.on("console", lambda m: print("[console]", m.type, m.text) if m.type in ("error", "warning") else None)
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
            await page.goto(url + "/audio.html")
            await page.wait_for_function("() => globalThis.crossOriginIsolated", timeout=60000)
            await page.set_input_files("#sf2-input", args[2])
            await page.wait_for_function("() => globalThis.WasmidiSnappyBridge && WasmidiSnappyBridge.state.soundfontLoaded",
                                         timeout=600000)
            if "--gpu" in flags:
                await page.click("#gpu")
            await page.set_input_files("#midi-input", args[1])
            await page.wait_for_function("() => !document.getElementById('play').disabled", timeout=3600000)
            await page.click("#play")
            for second in range(int(seconds)):
                await page.wait_for_timeout(1000)
                st = await page.evaluate("""() => { const s = WasmidiSnappyBridge.state; return {
                    t: WasmidiSnappyBridge.getAudioClock(), voices: s.activeVoices, load: s.renderLoadPercent,
                    buffer: s.ringFillPercent, underruns: s.underruns, dropped: s.droppedNotes, gpu: s.gpuStatus }; }""")
                print("%3d s  clock %7.2f  voices %6s  load %5s%%  buffer %5s%%  underruns %s  dropped %s  gpu %s" % (
                    second + 1, st["t"] or 0, st["voices"], round(st["load"] or 0), round(st["buffer"] or 0),
                    st["underruns"], st["dropped"], st["gpu"]))
            print(await page.text_content("#log"))
        elif mode == "shot":
            await page.goto(url + "/" + args[1])
            await page.wait_for_timeout(2000)
            await page.screenshot(path=args[2], full_page=True)
        await browser.close()

asyncio.run(main())
