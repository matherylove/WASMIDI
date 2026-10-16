#!/usr/bin/env python3
"""Regenerate MANIFEST.sha256 in `sha256sum` format (LF, forward slashes).

    python3 tools/regen_manifest.py          # rewrite
    python3 tools/regen_manifest.py --check  # verify, exit 1 on mismatch

The file list is the one below; add a path here when a new tracked file is
introduced. Validate with `sha256sum -c MANIFEST.sha256` on Linux."""
import hashlib, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "MANIFEST.sha256")

FILES = [
    ".github/workflows/build-wasm.yml",
    "CMakeLists.txt",
    "HANDOFF.md",
    "CLAUDE.md",
    "PORTING_STATUS.md",
    "VALIDATION.md",
    "src/keyboard.cpp",
    "src/keyboard.hpp",
    "src/mainwindow.cpp",
    "src/mainwindow.hpp",
    "src/midi/midi_document_codec.cpp",
    "src/midi/midi_document_codec.hpp",
    "src/midi/midi_mapped_store.cpp",
    "src/midi/midi_mapped_store.hpp",
    "src/midi/midi_parser.cpp",
    "src/midi/midi_parser.hpp",
    "src/midi/midi_worker_core.cpp",
    "src/pianoroll.cpp",
    "src/pianoroll.hpp",
    "src/qml/Controls.qml",
    "src/qml/MainWindow.qml",
    "src/renderer/gl_renderer.cpp",
    "src/renderer/gl_renderer.hpp",
    "third_party/snappysynthv2/UPSTREAM_NOTES.md",
    "third_party/snappysynthv2/Voice/voice.c",
    "third_party/snappysynthv2/Voice/voice.h",
    "third_party/snappysynthv2/compat/win_compat.h",
    "third_party/snappysynthv2/snappy_wasm_core.c",
    "tools/extract_voice_rebalance_logic.py",
    "tools/host_posix_env_shim.h",
    "tools/midi_parser_bootstrap_smoke.cjs",
    "tools/patch_emscripten_memory64_growth.py",
    "tools/regen_manifest.py",
    "tools/ssw_schedule_harness.c",
    "tools/worker_freelist_borrow_check.c",
    "web/coi-serviceworker.js",
    "web/midi-parser-worker.js",
    "web/parser-bench.html",
    "web/midi-parser-helper.js",
    "web/snappysynth-webgpu.js",
    "web/audio.html",
    "web/audio.css",
    "web/wasmidi-audio.js",
    "web/snappysynth-audio-worklet.js",
    "web/snappysynth-worker.js",
    "web/snappysynth_bridge.js",
    "web/synth-feeder-worker.js",
    "web/visual-cache-worker.js",
]


def digest(rel):
    h = hashlib.sha256()
    with open(os.path.join(ROOT, rel), "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    lines = ["%s  %s" % (digest(p), p) for p in FILES]
    text = "\n".join(lines) + "\n"
    if "--check" in sys.argv:
        current = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        if current != text:
            print("MANIFEST.sha256 is stale; run tools/regen_manifest.py")

            # Make CI failures actionable.  The old check only said that the
            # manifest was stale, which made it impossible to tell whether the
            # committed source tree, the workflow itself, or line-ending
            # normalization differed from the packaged drop.
            recorded = {}
            malformed = []
            for raw in current.splitlines():
                if "  " not in raw:
                    if raw.strip():
                        malformed.append(raw)
                    continue
                sha, rel = raw.split("  ", 1)
                recorded[rel] = sha

            changed = 0
            for rel in FILES:
                actual = digest(rel)
                old_sha = recorded.get(rel)
                if old_sha != actual:
                    changed += 1
                    print("MISMATCH: %s" % rel)
                    print("  manifest: %s" % (old_sha if old_sha is not None else "<missing>"))
                    print("  current:  %s" % actual)

            extras = sorted(set(recorded) - set(FILES))
            for rel in extras:
                changed += 1
                print("EXTRA manifest entry: %s" % rel)
            for raw in malformed:
                changed += 1
                print("MALFORMED manifest line: %s" % raw)

            if changed == 0:
                print("Hashes match, but manifest formatting/order differs from canonical output.")
            sys.exit(1)
        print("MANIFEST.sha256 ok (%d files)" % len(FILES))
        return
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("wrote %s (%d files)" % (OUT, len(FILES)))


if __name__ == "__main__":
    main()
