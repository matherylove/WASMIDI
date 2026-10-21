#!/usr/bin/env python3
"""Emscripten 3.1.56 creates the shared Memory64 heap of the pthread build of the MIDI parser
with the old JS API ({"index": "i64"} and Number sizes), which current V8 refuses ("maximum
above the upper bound 65536"). Rewrite that one constructor in the linked JS to the current
API: {"address": "i64"} with BigInt sizes (HANDOFF sec. 114).

  patch_parser_shared_memory.py build/wasmidi-midi-parser.js
"""
import re
import sys

path = sys.argv[1]
text = open(path, encoding="utf-8").read()
pattern = re.compile(r'new WebAssembly\.Memory\(\{"initial":([^,{}]+),"maximum":([^,{}]+),"shared":true,"index":"i64"\}\)')
fixed = 'new WebAssembly.Memory({"initial":BigInt(%s),"maximum":BigInt(%s),"shared":true,"address":"i64"})'
done = 'new WebAssembly.Memory({"initial":BigInt('
if done in text and not pattern.search(text):
    sys.exit(0)
text, count = pattern.subn(lambda m: fixed % (m.group(1), m.group(2)), text)
if count != 1:
    sys.exit("patch_parser_shared_memory: expected one shared Memory64 constructor in %s, found %d" % (path, count))
open(path, "w", encoding="utf-8").write(text)
