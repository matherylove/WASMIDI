# WASMIDI — contexto para Claude Code

Leer esto primero. El historial completo, con cada medición y decisión, está en
`HANDOFF.md` (§1-§67). Este archivo resume el estado al cierre de la rev. 72 y dice
cómo trabajar. Todo en español: el usuario escribe en español.

## El proyecto

WASMIDI es un reproductor de **Black MIDI** (decenas de millones a miles de millones de
notas, cientos de miles a millones de notas por segundo) que corre en el navegador.
Se despliega en **https://midi.dekx.cc** (GitHub Pages, `CNAME`) desde la rama `main`
con `.github/workflows/build-wasm.yml`.

Partes:

- **App Qt6 compilada a WebAssembly** (`src/`): ventana, controles QML
  (`src/qml/Controls.qml`), transporte, scheduler del synth (`src/mainwindow.cpp`,
  mucho JS embebido con `EM_JS`), piano roll **horizontal** en WebGL2
  (`src/renderer/gl_renderer.cpp`), caché de tiles culleados estilo BPFA
  (`src/renderer/note_raster_compositor.*`).
- **Parser de MIDI** en su propio módulo wasm **Memory64** dentro de un Worker
  (`src/midi/`, `web/midi-parser-worker.js`). Store activo: `BpfaMidiStore`
  (`src/midi/bpfa_midi_store.{hpp,cpp}`), port del diseño de BPFA detrás del contrato
  de `MidiMappedStore`. Fuente cruda o comprimida con zstd según memoria; carga en una
  sola pasada; carga en paralelo con Workers auxiliares (`web/midi-parser-helper.js`).
- **WasmiSynth**: fork de **SnappySynthV2** (synth de SoundFonts en C) compilado con
  pthreads (`third_party/snappysynthv2/`: `Voice/voice.c` es el motor,
  `snappy_wasm_core.c` la API `ssw_*`). Corre en `web/snappysynth-worker.js`, sale por
  un AudioWorklet con un ring en memoria compartida; `web/snappysynth_bridge.js` es la
  API JS (`globalThis.WasmidiSnappyBridge`). Render opcional de voces por **WebGPU**
  (`web/snappysynth-webgpu.js`).
- **WASMIDI Audio** (`web/audio.html`, `audio.css`, `wasmidi-audio.js`): reproductor
  estilo Winamp con la estética de dekx.cc que usa el mismo parser y WasmiSynth **sin**
  piano roll, teclado ni gráficos, para aislar problemas de audio.
- **Benchmark del parser en Chromium**: `web/parser-bench.html`.

## El usuario y la forma de trabajo

- Juan, Colombia. Programa en C++, pero revisa más código del que escribe.
- **No guarda nada en su PC**: todo vive en GitHub y se compila en GitHub Actions.
  En el chat anterior pedía "solo el zip del proyecto al terminar". En Claude Code
  **preguntarle si prefiere commits/push** (el push a `main` despliega) o seguir con zip.
- Prueba en Chrome/Brave en Windows: un Ryzen 9 5900X (24 hilos), otra PC de 16
  núcleos y otra de 24 núcleos, `deviceMemory` 32 GB. Manda capturas y logs de consola.
  Brave puede reportar `hardwareConcurrency` bajo.
- **Las explicaciones van en `HANDOFF.md`, no en comentarios de código** (los comentarios
  no deben repetir el handoff). Cada revisión agrega una sección `## N. Revisión X — …`
  al final con: pedido, causa, cambios, medición, verificación y lo no verificado.
- Quiere **benchmarks en Chromium real**, no solo en host.
- Prefiere arreglos a mediciones, pero acepta medir si se explica qué decide la medición.
- **Regla visual fija: la regla de capas del piano roll no puede cambiar** (la nota que
  empieza después queda arriba; empate: orden de fusión de pistas).
- Desde la rev. 68 el usuario aceptó que WasmiSynth descarte notas en zonas
  imposibles (admisión, tope de robos); antes la regla era "sonar igual que SSv2".

## Build y despliegue

- CI: Ubuntu 24.04, **Emscripten 3.1.56**, **Qt 6.8.3** (aqt), Ninja,
  `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$QT_ROOT_DIR/lib/cmake/Qt6/qt.toolchain.cmake ...`.
  zstd 1.5.6 entra por `FetchContent` para el parser. El CI arma `site/`, copia los JS de
  `web/`, pone huellas de versión (`?v=`) en `index.html` y `audio.html`, valida y
  publica.
- Exports del wasm: listas en `CMakeLists.txt` (`EXPORTED_FUNCTIONS` del parser y del
  synth). **Toda función nueva `wmp_*` / `ssw_*` llamada desde JS debe agregarse ahí.**
- `MANIFEST.sha256`: regenerar después de cada cambio con
  `python3 tools/regen_manifest.py` (la lista de archivos está en ese script; agregar
  ahí los archivos nuevos). `README.md` no va en el manifest.

## Verificación sin el toolchain (host)

```sh
# Motor del synth (C, sin Emscripten)
cd third_party/snappysynthv2
gcc -fsyntax-only -Wall -I. -DSNAPPYSYNTH_WASM=1 Voice/voice.c
gcc -fsyntax-only -Wall -I. -DSNAPPYSYNTH_WASM=1 snappy_wasm_core.c
cd ../..
for f in web/*.js; do node --check "$f"; done

# Arneses del CI que extraen código real de voice.c / del core
python3 tools/extract_ssw_schedule_logic.py && cc -O1 -Wall -o /tmp/h tools/ssw_schedule_harness.c -lm && /tmp/h
python3 tools/extract_voice_rebalance_logic.py && cc -O1 -Wall -pthread -o /tmp/b tools/worker_freelist_borrow_check.c && /tmp/b
S=third_party/snappysynthv2
cc -O2 -I$S -DSNAPPYSYNTH_WASM=1 -DSSW_WORKER_CYCLE_TRAMPOLINE=1 -pthread -o /tmp/wc tools/worker_cycle_smoke.c \
   $S/snappy_wasm_core.c $S/Voice/voice.c $S/Parser/sf2_parser.c $S/Parser/sfz_parser.c $S/Parser/wav_loader.c $S/wasm_stubs.c -lm && /tmp/wc

# Parser BPFA contra el parser original (synth, renderer, teclado, páginas; también
# carga paralela simulada). Necesita libzstd.so.1; el script genera un zstd.h mínimo.
cd tools/feeder_check && ./check.sh   # espera solo IDENTICAL, ningún DIFFER
```

Bancos de prueba (necesitan los archivos de prueba del usuario, ver abajo):

- `tools/host_bench/midibench.c` — WasmiSynth real en host con un MIDI y un SF2.
  `./midibench SF2 MID WORKERS START DUR [out.wav]`. Variables: `VOICES`, `SHARD`
  (0 auto, 1 canal, 2 hash, 3 single), `DET=1` (determinístico), `LIMIT`, `ADMIT`,
  `STEALCAP`, `DBG=1` (desglose ALLOC/BUSY por segundo), `GPUEMU=1` (emula en C el
  shader WebGPU). Compilar con `tools/host_bench/build.sh` o como en HANDOFF §61-§63.
- `tools/host_bench/large_midi_bench.cpp` — carga e índice de MIDIs gigantes.
- `tools/feeder_check/*_compare.cpp` aceptan `BPFA_ZSTD=1` y `BPFA_PARALLEL=N`.

**El host de desarrollo anterior tenía 1 núcleo**: no medía paralelismo real. Si este
entorno tiene varios núcleos, red o Chromium, aprovecharlo: con emsdk 3.1.56 + Qt 6.8.3
se puede compilar localmente y medir en Chromium headless (Playwright) sirviendo
`site/` con `Cross-Origin-Opener-Policy: same-origin` y
`Cross-Origin-Embedder-Policy: require-corp`.

## Estado actual (rev. 72)

Rendimiento medido por el usuario:

- Carga de The Nuker 3 (15,9 GB, 9679 pistas, 2.080 millones de notas), Chrome 155,
  16 núcleos: 692 s (rev. 64) → 277 s (una pasada, rev. 66) → **42 s** (12 Workers en
  paralelo, rev. 67).
- WasmiSynth en host, Hypernova zona densa, 1024 voces, 1 worker: 1,57× tiempo real
  (rev. 67) → **0,67×** con admisión + tope de robos (rev. 68).

Mecanismos activos y cómo desactivarlos:

| Mecanismo | Desactivar |
|---|---|
| Parser BPFA | `-DWASMIDI_PARSER_BPFA=0` |
| Fuente zstd forzada | `store.setRawSourceLimit(0)` (host) |
| Carga paralela | se salta sola con < 128 MiB, < 4 núcleos o 1 pista |
| Tiles culleados del roll | `?tiles=0` en la URL |
| Límite de render del synth | `?synthlimit=0` / `ssw_set_render_limit_percent(0)` |
| Admisión por bloque (25% de voces) | `ssw_set_admission_percent(0)` |
| Tope de robos por ciclo (25%) | `ssw_set_steal_cap_percent(0)` |
| Limitador + bloqueador de DC | botón "Soft clip" apagado |
| Reparto Single (Auto) | botón Sharding → Channel/Hash |
| Tiles de render de 256 voces y suma en orden fijo | sin rollback (rev. 69) |
| Modo determinístico | `ssw_set_deterministic(1)` (para pruebas) |
| WebGPU | botón CPU/GPU |
| Trampolín de `worker_cycle` | `-DSSW_WORKER_CYCLE_TRAMPOLINE=0` |
| Note-off FIFO / lista abierta | `-DSSW_NOTEOFF_FIFO=0` / `-DSSW_NOTEOFF_OPEN_LIST=0` |

## Pendientes abiertos (en orden de llegada; ver HANDOFF §67)

1. **WebGPU con 12 workers mucho más lento que con 1 worker** (con 1 worker rinde igual
   que CPU). Sin investigar; hipótesis en §67.
2. **"¿Dónde están los controles?"** — pregunta del usuario sin contexto suficiente;
   aclarar a qué controles se refiere antes de tocar nada.
3. **Ventanas/paneles minimizados no deben usar CPU ni GPU** y deben poder reanudarse
   rápido y barato. Aclarar si habla de la pestaña/ventana del navegador minimizada, de
   paneles de la app o de WASMIDI Audio.
4. **Optimizar el uso de recursos de lo visual sin que se vea choppy**, con pocos frames
   o de mala calidad.
5. **Bug: a veces, tras cambiar de synth, el programa descarga la SoundFont y no puede
   cargar otra.**
6. Los anteriores sin cerrar: confirmar en el navegador que el shader WGSL compila y
   que WebGPU anda (rev. 70); objetivo pedido de 60 fps mínimo y 1024 voces constantes
   en The Nuker 3 (plan de §60 del chat: decodificación repartida durante la
   reproducción, teclado/gráficos sin decodificar, tiles culleados repartidos);
   reconstrucción del teclado en seeks lejanos de The Nuker (~15 s); coordinador que
   también tome tiles de render; GUI cosmética; mapeo GM de instrumentos/drums;
   precisión de CC.

El usuario dijo explícitamente **"no hagas nada todavía"** sobre los puntos 2-5 al
pedir este handoff: empezar aclarando y proponiendo.

## Errores a no repetir (detalle en HANDOFF §7)

- No mandar `configure` parcial al worker del synth (reinicializa con mínimos).
- Ninguna estructura nueva compartida entre workers sin un único escritor; 1 núcleo en
  host no demuestra ausencia de carreras.
- Anclar ediciones en texto único y verificar dónde aterrizó cada inserción
  (`voice.c` tiene ~7000 líneas con indentación muy profunda).
- Antes de reemplazar un componente, buscar todos sus usos, también en `tools/` y en el
  CI (`tools/midi_parser_bootstrap_smoke.cjs` ejercita todo el contrato `wmp_*`).
- No decidir con benchmarks de host lo que está dominado por el navegador.
- No comparar mediciones que cambian dos variables a la vez.
- No editar `HANDOFF.md` reemplazando bloques: solo agregar o corregir en el lugar.
- No afirmar que una herramienta no existe sin buscarla en todo el sistema.

## Licencias de las referencias

Solo se toman ideas de: SuperVirtualMIDISynth (GPL-3), Kasaria (GPL-2), Kimrashi (GPL-3).
Kestrel es MPL-2.0. BPFA y el SnappySynthV2 original son referencias de arquitectura.
No copiar código GPL a este repositorio.
