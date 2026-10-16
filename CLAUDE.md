# WASMIDI — contexto para Claude Code

Leer esto primero. El historial completo, con cada medición y decisión, está en
`HANDOFF.md` (§1-§74). Este archivo resume el estado al cierre de la rev. 78 y dice
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
- **Benchmark del roll y el teclado en Chromium**: `web/visual-bench.html` (`browser_bench.py visual`).

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

**El host de desarrollo anterior tenía 1 núcleo, sin red ni emsdk**: no medía
paralelismo real ni podía compilar el wasm. En Claude Code, si hay red y varios núcleos,
compilar y medir localmente:

```sh
tools/dev_setup.sh                 # una vez: emsdk 3.1.56, Qt 6.8.3 (aqt), Playwright/Chromium
source tools/dev_env.sh            # en cada shell que compila
tools/local_build.sh               # pruebas de host del CI + compilación + site/ (mismos pasos del CI)
python3 tools/serve_site.py site 8080 &          # sirve con COOP/COEP (pthreads)
python3 tools/browser_bench.py parser tools/data/Hypernova.mid
python3 tools/browser_bench.py audio tools/data/Hypernova.mid tools/data/MatheryLSD.sf2 30
python3 tools/browser_bench.py shot audio.html /tmp/audio.png
```

`tools/run_ci_step.py "<nombre del paso>"` ejecuta cualquier paso `run:` del workflow tal
cual (sin copias que se desincronicen); sin argumentos lista los pasos. Chromium headless
no suele tener GPU real: los resultados de WebGPU ahí no son representativos.

### Archivos de prueba

No están en el repo (son grandes y del usuario). Ponerlos en `tools/data/` (ignorado por
git) o pedírselos: **Hypernova.mid** (≈500 MB, 26 pistas, 960 PPQ, 44,7 M notas; zona
densa 98-104 s), **MatheryLSD.sf2** (11 regiones), **The Nuker 3 Ultimate F4.mid**
(15,9 GB, 9679 pistas, 2.080 M notas) y **The Nuker 4 F2.mid** (20,9 GB). Para pruebas
con letras, `tools/feeder_check` genera MIDIs sintéticos.

## Estado actual (rev. 78)

Rendimiento medido por el usuario:

- Carga de The Nuker 3 (15,9 GB, 9679 pistas, 2.080 millones de notas), Chrome 155,
  16 núcleos: 692 s (rev. 64) → 277 s (una pasada, rev. 66) → **42 s** (12 Workers en
  paralelo, rev. 67).
- WasmiSynth en host, Hypernova zona densa, 1024 voces, 1 worker: 1,57× tiempo real
  (rev. 67) → **0,67×** con admisión + tope de robos (rev. 68).
- 12288 voces cuestan ~8× más CPU que 1024 (el pool se llena); los workers casi no
  agregan costo. Auto usa Single siempre (rev. 72).
- Modo GPU (rev. 73): costo del lado CPU 6,2 → 2,8 ms (1024 voces) y 57 → 9,3 ms
  (12288 voces) por bloque de 11,6 ms; avance de estado en forma cerrada y reserva de
  peticiones por tile. **Sin verificar en un navegador con GPU real.**
- Rev. 74 (HANDOFF §70, medido en el contenedor de 4 hilos): el parser alimenta al synth
  con admisión por bloques y copias de echo plegadas; decodificación repartida en
  helpers con ≥ 6 núcleos. En Single ahora renderizan todos los workers (antes uno
  solo). Recorte del pico de The Nuker 4 (51,6 M note-ons/s) en Chromium a 1024 voces:
  0 underruns.
- Rev. 75 (HANDOFF §71): roll, teclado y contadores leen un flujo visual plegado (copias
  de echo juntas por pista y tick) que decodifican los mismos helpers del feed. En el
  pico, Chromium de 4 hilos con `?helpers=3&rawlimit=0`: roll nunca atrasado, teclado a
  60/s con ~3 ms de latencia (antes: roll segundos atrás, teclado 0,5/s). Medir en las
  PCs con `visual-bench.html`. Con fuente cruda (MIDI de menos de ~3-6 GB según memoria)
  no hay helpers: el flujo se decodifica en el Worker del parser.
- Rev. 76 (HANDOFF §72): empezar a mitad de The Nuker 4 congelaba roll y teclado (reset del
  teclado de 45 s por snapshots agotados). Snapshots por pista + snapshots compactos por
  tecla: reset en 548 s de 0,13 s en host y 0,19 s en Chromium.
- Rev. 77 (HANDOFF §73): los tiles del roll dejaban huecos donde había notas muy cortas
  (culling por centros de píxel y tiles dibujados corridos). Ahora una columna se tapa solo
  si las notas de adelante cubren todos sus ticks: 0 píxeles distintos en `cull_tile_sim`.
- Rev. 78 (HANDOFF §74): WasmiSynth renderiza las voces estéreo del bucle genérico de a
  cuatro con vectores (SIMD128 en wasm): ~1,5× menos CPU por voz en host; recorte del pico a
  6144 voces en Chromium: carga 162% → 128%, underruns 368 → 128; ≤ 1 LSB de diferencia.
  El usuario: 6144 voces estables en el 5900X; la meta de 24576 necesita el synth por GPU.
  `visual-bench.html` se pausa con la pestaña oculta; WASMIDI Audio suena oculta con todo
  lo visual congelado (`browser_bench.py audio ... --hide=S`).

Mecanismos activos y cómo desactivarlos:

| Mecanismo | Desactivar |
|---|---|
| Parser BPFA | `-DWASMIDI_PARSER_BPFA=0` |
| Fuente zstd forzada | `store.setRawSourceLimit(0)` (host) |
| Carga paralela | se salta sola con < 128 MiB, < 4 núcleos o 1 pista |
| Feed del synth desde el parser (admisión + echo plegado) | `?synthsource=feeder` (feeder JS) o `?synthsource=relay` |
| Helpers de decodificación (feed y flujo visual) | se saltan solos con < 6 núcleos o fuente cruda; `?helpers=N` fuerza N (0 los apaga) |
| Fuente cruda / zstd en el navegador | `?rawlimit=MiB` (`0` fuerza zstd) |
| Flujo visual plegado (roll + teclado) | `?visualstream=0`; `?visualdebug=1` mide en consola |
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
| Render de 4 voces a la vez (vectores) | `-DSSW_VOICE_QUAD=0` |
| Note-off FIFO / lista abierta | `-DSSW_NOTEOFF_FIFO=0` / `-DSSW_NOTEOFF_OPEN_LIST=0` |

## Pendientes abiertos (ver HANDOFF §67-§74)

1. **Meta del usuario: The Nuker 4 (y 3) en tiempo real de principio a fin a 1024
   voces por worker** en el Ryzen 9 5900X + RX 9070XT y el Ryzen 7 7435HS + RTX 2050,
   en WASMIDI y WASMIDI Audio, con MatheryLSD, sin diferencia audible frente a un MIDI
   sin robos ni descartes. Hecho en la rev. 74: (a) admisión en el parser y (b)
   decodificación repartida para el synth. Hecho en la rev. 75: etapa 2, roll/teclado/
   gráficos desde el flujo visual con helpers. Rev. 78: render de 4 voces por vectores.
   Medido por el usuario con la canción entera: 6144 voces estables en el 5900X (meta 24576
   = 1024 × 24 workers) y el roll se atrasa en 830-858 s con 8 helpers (faltan las líneas
   `[WASMIDI visual]` de ese tramo). Falta: etapa 3, synth por GPU estilo Kestrel; helpers
   con fuente cruda; saltos justo después del pico (las densidades del reset decodifican
   ese segundo en el Worker del parser).
2. **Verificar en el navegador lo de la rev. 70-73**: que el WGSL compile y WebGPU
   ande, cuánto rinde el modo GPU con 1 y 12 workers, WASMIDI Audio con audio real
   (ventanas, letras, gráficos, visualizador) y el arreglo de la SF2 que no recargaba.
3. Anteriores: reconstrucción del teclado en seeks lejanos de The Nuker (~15 s);
   que el hilo coordinador también tome tiles de render; GUI cosmética; mapeo GM de
   instrumentos/drums; precisión de CC.

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
- No dejar la descarga a GPU del motor encendida mientras el JS no la atiende (rev. 73):
  cualquier cambio en el ciclo de vida del core debe apagarla primero.

## Licencias de las referencias

Solo se toman ideas de: SuperVirtualMIDISynth (GPL-3), Kasaria (GPL-2), Kimrashi (GPL-3).
Kestrel es MPL-2.0. BPFA y el SnappySynthV2 original son referencias de arquitectura.
No copiar código GPL a este repositorio.
