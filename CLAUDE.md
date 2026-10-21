# WASMIDI — contexto para Claude Code

Leer esto primero. El historial completo, con cada medición y decisión, está en
`HANDOFF.md` (§1-§104). Este archivo resume el estado al cierre de la rev. 107 y dice
cómo trabajar. Todo en español: el usuario escribe en español.

## El proyecto

WASMIDI es un reproductor de **Black MIDI** (decenas de millones a miles de millones de
notas, cientos de miles a millones de notas por segundo) que corre en el navegador.
Se despliega en **https://midi.dekx.cc** (GitHub Pages, `CNAME`) desde la rama `main`
con `.github/workflows/build-wasm.yml`.

Partes:

- **App Qt6 compilada a WebAssembly** (`src/`): ventana, controles QML
  (`src/qml/MainWindow.qml`, `Dock.qml`, kit `Dk*`), transporte, scheduler del synth (`src/mainwindow.cpp`,
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
- `tools/host_bench/pipeline_bench.cpp` — feed del parser (admisión, echo plegado) + WasmiSynth,
  como en el navegador. `./pipeline_bench SF2 MID WORKERS DUR START...`. Variables: `VOICES`,
  `DET=1`, `PATHS=1` (fase de eventos y busy por bloque), `WAV=prefijo`, `GPUSIDE=1` (costo
  del lado CPU del modo GPU). Desde `tools/host_bench`, después de `feeder_check/check.sh` (deja
  un `zstd.h` en `/tmp/wasmidi_zstd_inc`): `g++ -O2 -std=c++17 -I/tmp/wasmidi_zstd_inc -I../../src
  -I../../src/midi -I$E -DSNAPPYSYNTH_WASM=1 -pthread -o pipeline_bench pipeline_bench.cpp
  ../../src/midi/{bpfa_midi_store,midi_mapped_store,midi_parser,midi_document_codec}.cpp` + los
  `.o` del synth compilados con `gcc -O3` como en `build.sh`, `-l:libzstd.so.1 -lm`.
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

## Estado actual (rev. 107)

Rendimiento medido por el usuario:

- Carga de The Nuker 3 (15,9 GB, 9679 pistas, 2.080 millones de notas), Chrome 155,
  16 núcleos: 692 s (rev. 64) → 277 s (una pasada, rev. 66) → **42 s** (12 Workers en
  paralelo, rev. 67).
- WasmiSynth en host, Hypernova zona densa, 1024 voces, 1 worker: 1,57× tiempo real
  (rev. 67) → **0,67×** con admisión + tope de robos (rev. 68).
- 12288 voces cuestan ~8× más CPU que 1024 (el pool se llena); los workers casi no
  agregan costo. Auto usó Single siempre (rev. 72) hasta la rev. 80.
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
- Rev. 79 (HANDOFF §75): modo GPU hacia 24576 voces. Note-offs apilados en una pasada (WAV
  idéntico), todas las voces van a la GPU (también las VOR con note-off pendiente y las que
  llegan al final de una muestra sin loop) y la suma en la GPU es por cuadro. Host, pico, 24576
  voces, `DET=1`: lado CPU del modo GPU 3,0× → 0,90× tiempo real (`pipeline_bench GPUSIDE=1`);
  modo CPU 4,4× → 3,1×. Lo que queda es la fase de eventos (serial). Sin GPU real medida.
- Rev. 80 (HANDOFF §76): Auto reparte los eventos con Hash por encima de 4096 voces (cada
  worker sus teclas y su pool; Single dejaba la fase de eventos en un hilo). Búsqueda de
  regiones SFZ sin borrar ~200 KB por nota. Host, pico, 24576 voces, 4 workers: lado CPU del
  modo GPU 0,87× → 0,62×. Con Hash el WAV no es byte a byte repetible; para eso, Single + `DET`.
- Rev. 81 (HANDOFF §77): el usuario llega a 12288 voces estables en el 5900X; a 24576 cruje y el
  roll se queda sin cuadros. "Stack gain" encendido por defecto (las copias de eco plegadas suman
  su volumen en vez de sonar como una nota) y su bajada de ganancia va a la GPU y al render de 4
  voces. Workers en Auto reservan los helpers del parser (24 hilos → 14 workers).
- Rev. 82 (HANDOFF §78): el roll se atrasaba en 829-858 s de The Nuker 4 porque cada tramo de
  los helpers releía entera la pista de tempo (1,9 MB sin eventos de canal). Los cursores ahora
  terminan en el último evento de canal de cada pista (`channelEnd`). El flujo visual guarda
  hasta 48 M palabras y no planea más con 32 M detrás del teclado (con 8 M el teclado se
  congelaba ~1 s cada 3 s). Chromium, archivo entero, 826-840 s: roll atrasado 846 → 0
  cuadros, teclado 60/s.
- Rev. 83 (HANDOFF §79): el usuario aclara que visual-bench y WASMIDI Audio rinden mucho mejor
  que WASMIDI (synth + roll + gráficos + teclado a la vez) y que todo junto debe andar sin lag ni
  cuadros perdidos: **verificar siempre en WASMIDI**. `?visualdebug=1` en WASMIDI escribe una línea
  `[WASMIDI app]` por segundo (cuadros, tareas largas, teclado, adelanto del roll, synth). El
  Chromium del contenedor no tiene GPU: ahí el roll de WASMIDI va a ~13 cuadros/s aun con un MIDI
  liviano, no sirve para medir cuadros.
- Rev. 84 (HANDOFF §80): reglas del usuario de sincronía. Un underrun ya no atrasa el audio: el
  worklet cuenta el silencio como tiempo y lo publica en el ring (`RING_STARVED`), el Worker salta
  esos cuadros (`ssw_skip_to`) y el core descarta los note-on que quedaron más de 25 ms atrás
  (con su note-off; `ssw_late_dropped`). El roll mantiene su último cuadro completo mientras faltan
  notas y, si todo lo preparado quedó a la izquierda de la pantalla, reconstruye en la posición
  actual. Chromium con 24576 voces (carga 360%): el reloj de audio avanza 1 s por segundo.
- Rev. 85 (HANDOFF §81): en el 7435HS con The Nuker 3 el roll perdía cuadros y la app se cerró.
  Los tiles culleados solo se armaban con el tile entero preparado, así que con poco adelanto el
  roll dibujaba hasta 1,7 M notas crudas por cuadro (la página esperaba a la GPU segundos). Ahora
  hay tiles parciales (cobertura hasta `sharpRemoteSafeThrough_`, 0 píxeles distintos): ≤ ~750 k
  instancias. La reconstrucción por atraso salta 1-8 ventanas adelante. El teclado detrás del flujo
  visual (tras un salto del roll) decodificaba las 9679 pistas, hasta 34 s por petición, y dejaba al
  synth sin eventos: ahora mantiene su última imagen hasta que el flujo lo cubre. Sin helpers,
  ≤ 0,25 s de canción por petición. Diagnóstico: `?rolldraw=0`, "drawn max"/"app memory" en
  `[WASMIDI app]`, `browser_bench.py app --profile/--trace`, `-DWASMIDI_PROFILE_FUNCS=ON`.
  Sin verificar con GPU real ni el cierre.
- Rev. 86 (HANDOFF §82): log del usuario de visual-bench con The Nuker 3 (16 núcleos, 5 helpers):
  roll atrasado en 122-135, 420-561, 710-717 y 817-866 s. Cada tramo visual retoma las 9679 pistas
  (~0,1 s): los tramos crecen hasta pistas × 1024 palabras (máx. 8 M) y se ordenan por conteo de
  ticks (host, 440 s: 7,3 → 1,5 s de helper por segundo de canción). Teclado en casillas planas
  (35,9 → 7,5 ns por palabra) y snapshots por tecla cada 4096 eventos (reset a 430 s: Chromium
  17,8 → ~1,4 s). En Chromium de 4 hilos 429-455 s sigue atrasándose ~0,6 s/s: el teclado satura
  el Worker del parser. Sin medir en las PCs.
- Rev. 87 (HANDOFF §83): con helpers, el teclado corre en su propio Worker (`midi-parser-helper.js`,
  mensajes `live-*`) con una copia del flujo visual; el Worker del parser solo le reconstruye las
  teclas en un reset (`exportLiveRebuild`). Bucle del teclado sin llamadas (25 → 11 ns por palabra) y
  anexar con una copia menos. Chromium de 4 hilos, 415-455 s: el Worker del parser baja a ~45% de
  uso; el roll va adelantado hasta 433 s y desde ahí lo limitan los 3 helpers (-9,5 s a 454 s, antes
  -14,8 s). Sin medir en las PCs; la ráfaga de 830 s sigue sin alcanzar.
- Rev. 88 (HANDOFF §84): visual-bench del usuario con la rev. 87 (16 núcleos): 60 cuadros/s todo el
  tema, roll atrasado solo en 128-129, 435-454, 711 y 828-841 s. WASMIDI perdía cuadros porque cada
  tile culleado (0,3-1 M notas) se juntaba y culleaba de una vez en el hilo de la página. Ahora un
  tile por vez se arma de a pedazos (3 ms por cuadro, 6 ms si está en pantalla). Chromium del
  contenedor hasta 140 s: segundos con un pedazo > 16 ms 54 → 4, máximo 102 → 23 ms. El cierre de
  WasmiSynth en pausa no se reprodujo (pausa de 80 s en 125 s).
- Rev. 89 (HANDOFF §85): el usuario sigue viendo 0 cuadros/s antes de 2:10 en WASMIDI con 12288 voces
  (el contador solo cuenta cuadros completos del roll: 0 es el roll esperando notas). Los helpers
  decodificaban cada tramo dos veces (feed del synth y flujo visual). Ahora, donde se encuentran en
  un borde de 4 bloques del feed, un tramo conjunto da los dos resultados (mismos bytes); el feed
  corre hasta la cabeza del flujo visual para encontrarse. Host: 4,43 → 3,21 s de helper por s a
  432 s. Chromium, 1024 voces hasta 140 s: helpers ocupados 49% → 34%. `[WASMIDI visual]` muestra
  `helpers N% busy`.
- Rev. 90 (HANDOFF §86): la rev. 89 no cambió nada para el usuario. Cada pedido a un helper
  descomprimía de nuevo el bloque de 64 KiB de cada una de las 9679 pistas; ahora cada pista guarda
  su bloque entre pedidos. Host 118-134 s: -19% de helper. Chromium, visual-bench de The Nuker 3 en
  118-137 s con 3 helpers: roll atrasado 782 → 293 cuadros; lo que queda es 127-130 s (~70 M
  palabras/s). Revisión de sharkmidi (GPL-3, ideas): el roll ya es el mismo diseño; su arreglo de
  eventos ordenado por tick en tramos RSB ocuparía 5,9 GB para The Nuker 3 (propuesta en §86).
- Rev. 91 (HANDOFF §87): el flujo visual era un vector que se copiaba al crecer y se movía al
  recortar (anexar hasta 1,1 s por segundo en el Worker del parser). Ahora es una lista de pedazos,
  uno por tramo: nada se mueve. Chromium 118-137 s: roll atrasado 293 → 200 cuadros, teclado
  40 → 47/s. Más tramos en vuelo o tramos más chicos no ayudan (los helpers no son el límite).
- Rev. 92 (HANDOFF §88): el flujo visual se guarda codificado como el RSB de sharkmidi (grupos
  tick/pista, corridas de estado, 1 byte por palabra): The Nuker 3 entero 32,6 → 4,78 GB. Barrido y
  teclado leen corridas enteras (`visualScanRuns`, `renderRun`). Los helpers decodifican por
  adelantado hasta un plan (32 M palabras por GiB de `deviceMemory`, máx. 1024 M), sin esperar al
  objetivo del roll. Chromium: desde 118 s roll atrasado 200 → 143 cuadros; desde 105 s, 0 en
  127-130 s (sin adelanto 61); teclado en 128 s 17 → 47/s. Revisión del código de BPFA (modo normal
  con tabla de notas de 8 B, modo look-ahead con tope de RAM y prellenado) en §88. El usuario pidió
  después dos modos separados como BPFA.
- Rev. 93 (HANDOFF §89): dos modos del roll, fila "Roll cache" en WASMIDI (se aplica al próximo MIDI):
  **Look-ahead** (por defecto, 1024 MB) prellena el flujo visual desde 0 s antes de abrir el MIDI y
  después lo rellena; **Full song** decodifica la canción entera al cargar y no recorta nada (si no
  entra en el heap ni en la mitad de `deviceMemory`, queda como look-ahead). Un salto dentro de lo
  guardado conserva el flujo. El Worker del teclado recibe cada pedazo cuando está a menos de 32 M
  palabras de él (antes, una copia de todo el plan). Tope del rellenado (un tramo por vez pasada la
  mitad del plan) y el barrido cede con `MessageChannel` en vez de `setTimeout(0)` (Chromium lo
  frenaba a ~250 pasadas/s). Chromium, The Nuker 3 0-140 s, 3 helpers: roll atrasado 15 (rev. 92)
  → 0 cuadros, adelanto mínimo -0,17 → 1,78 s, teclado en 128 s 11 → 44/s; prellenado de 1024 MB
  (hasta 278 s) en 17-20 s. The Nuker 3 entero: 4173 MiB en 95 s con 3 helpers (no entró en el tope
  de 4 GiB del contenedor y quedó como look-ahead).

- Rev. 94 (HANDOFF §91): mockup de la GUI nueva aprobado (https://claude.ai/artifact/6Rzqy1QJQrBaJq39comYMd,
  kit de dekx.css, fondo neural de `dekx.js`; falta cablearlo). WasmiSynth carga una carpeta SFZ
  (botón "SFZ", soltar carpeta en Audio): rutas de Windows sin mayúsculas, `#define`/`#include`, sample
  en `<group>`, WAV 8/24/32/float, FLAC/Ogg/Opus decodificados en la página. `tools/sfz_folder_check.c`.
- Rev. 95 (HANDOFF §92): GUI nueva, parte 1 del mockup aprobado. Ventana con el kit de dekx.cc
  (componentes `Dk*` en `src/qml/`, singleton `Dk.qml`), LCD, panel `Dock.qml` sin scroll, diálogo
  `AdvancedDialog.qml` (motor, contadores, archivo), barra de estado; fondo neural de `dekx.js` detrás de
  toda la página (`NeuralBackground`, C++) y el roll con fondo semitransparente; gráfico NPS/polifonía
  por cuadro (`LiveGraph`, C++); VT323 en los recursos. Faltan las partes 2-5 (notas y teclado, colores
  y avisos, cuadros sin tope, celular).
- Rev. 96 (HANDOFF §93): GUI parte 2. Notas con degradado por fila, borde de 1 px (solo si la nota
  mide ≥ 3 px) y damero de 2 px en la misma pasada; colores por vértice. Fondo del roll plano con líneas
  de do; cursor de 2 px del acento con brillo. Teclado vertical de 62 px a la izquierda del roll, una fila
  por tecla. Por software (peor caso): igual con muchas notas, 1,05-1,13× con notas largas superpuestas
  (cuadro entero 0,83-1,07×).
- Rev. 97 (HANDOFF §94): GUI parte 3. Diálogo "Note colors" (paleta de 16 en bucle por pista o canal,
  R/G/B/hex, import/export de config.xml `Visual/Colors/Color`), acento de la interfaz y del cursor con el
  color que más suena (`adaptColors`), avisos arriba a la derecha de polifonía y NPS máximos (de lo ya
  tocado) y de atraso con ROLL/KEYBOARD/GRAPHS/WASMISYNTH. Falta la parte 5 (celular).
- Rev. 98 (HANDOFF §95): GUI parte 4. El reloj de reproducción se movía cada 16 ms: a 144 Hz el roll
  repetía posiciones. Ahora avanza en cada cuadro (`advanceFrame` desde un `FrameAnimation`; el temporizador
  de 16 ms solo sin cuadros) y el pedazo de tiles por cuadro es 36%/18% del período de la pantalla. Sin
  medir en una pantalla de más de 60 Hz.
- Rev. 99 (HANDOFF §96): GUI parte 5. Modo celular vertical (`phone` en `MainWindow.qml`): roll con el tiempo
  hacia arriba (`setPortrait`, mismo culling), teclado abajo, LCD, transporte y pestañas Song/Colors/Synth/View.
  Avisos en la esquina del roll con los picos de NPS y polifonía de todo el MIDI (`wmp_song_peaks_js`,
  `noteOffDensity`) y contador de CC/s en el LCD. Sin probar en un celular real.
- Rev. 100 (HANDOFF §97): celular acostado o ventana baja (`compact`: menos de 540 px de alto o 860 de ancho)
  usa el encabezado, el panel por pestañas y una fila de transporte del celular con el roll horizontal. En
  vertical las notas caen hacia el teclado como Synthesia (cursor sobre el teclado, eje del tiempo invertido).
- Rev. 101 (HANDOFF §98): con menos de 540 px de alto (celular acostado) las notas también caen sobre un
  teclado abajo a lo ancho (`falling`): en horizontal las 128 teclas quedaban de ~1 px Deshecho en la rev. 102.
- Rev. 102 (HANDOFF §99): el celular acostado vuelve al roll horizontal; el teclado horizontal no dibuja
  sus divisores y bordes de 1 px con filas de menos de 4 px (se veía como rayas apiladas).
- Rev. 103 (HANDOFF §100): el panel de pestañas del celular va debajo del teclado (vertical) o a la derecha
  del roll (acostado), no encima de las notas. Filas de teclas en píxeles enteros con octavas iguales
  (`keyEdge`, roll, fondo y teclado); FBO del teclado a resolución del dispositivo. Ícono de Colors y reloj.
- Rev. 104 (HANDOFF §101): instrumentos GM equivocados y batería mala en WasmiSynth. Pares estéreo de la SF2 solo
  si los dos enlaces se apuntan entre sí (Arachno juntaba el piano con un trombón y liberaba muestras en uso);
  con Hash cada worker resuelve el programa con su propio estado de selector (copias `EVT_SELECTOR_COPY`), antes
  la primera nota tras un program change sonaba con el programa anterior; banco melódico de SF2 = MSB (o LSB),
  MSB 1 ya no cae en el banco 128 de batería.
- Rev. 105 (HANDOFF §102): comparado con BASSMIDI (biblioteca de Linux, `NOFX`). SF2 toca todas las capas de la
  nota; generadores según la especificación (local > global > defecto, el preset se suma), atenuación ×0,4, hold,
  decay en dB hasta sustain, Q y filtro como SF2, loops 1/3, pan de potencia constante; las clases exclusivas no
  apagan capas de la misma nota. Arachno: niveles a ~1 dB de BASSMIDI (antes hasta -14 dB). Falta envolvente de
  modulación, LFOs, moduladores, reverb y chorus.
- Rev. 106 (HANDOFF §103): el usuario pidió que el roll no dibuje superposiciones ni notas que no se ven. Cada tile
  es ahora una trama con la nota de arriba por píxel (`RasterTile`, regla de capas igual) y se dibuja con un solo
  cuadrilátero que la pinta con el mismo estilo: cada píxel una vez por cuadro (antes 8-246× más columnas en tiles
  densos). Scroll en píxeles enteros (`sharpScrollPx_`, `uViewFrac` para lo crudo). Los tiles ya no recorren el ring
  desde la cola: lista de notas largas (`longIds_`). The Nuker 3 a 2:00 con note speed 0,1 en el contenedor: armado
  2-11 ms/s con 0,5-2,5 M notas residentes. Sin medir con GPU real.
- Rev. 107 (HANDOFF §104): "application exit" en una zona densa de The Nuker 3: el ring del roll crecía de 8 M a 268 M
  notas y la reserva fallaba (heap de 4 GiB). La cola del ring no avanzaba durante una reconstrucción, y una reconstrucción
  que empezó con un salto del roll no terminaba nunca (su objetivo se corre con el cursor). Ahora la cola avanza siempre,
  las notas que siguen en pantalla una ventana después de empezar salen del ring a una lista aparte (`held_`, debajo de
  todo), el adelanto tiene presupuesto fijo (0,55 × 2^23 notas) y el ring tope de 2^26. Chromium, The Nuker 3 desde 0,
  note speed 0,1, 6144 voces, 218 s: máximo 0,58 M notas en el ring (antes subía sin bajar hasta 5,1 M).

Mecanismos activos y cómo desactivarlos:

| Mecanismo | Desactivar |
|---|---|
| Parser BPFA | `-DWASMIDI_PARSER_BPFA=0` |
| Fuente zstd forzada | `store.setRawSourceLimit(0)` (host) |
| Carga paralela | se salta sola con < 128 MiB, < 4 núcleos o 1 pista |
| Feed del synth desde el parser (admisión + echo plegado) | `?synthsource=feeder` (feeder JS) o `?synthsource=relay` |
| Helpers de decodificación (feed y flujo visual) | se saltan solos con < 6 núcleos o fuente cruda; `?helpers=N` fuerza N (0 los apaga) |
| Fuente cruda / zstd en el navegador | `?rawlimit=MiB` (`0` fuerza zstd) |
| Flujo visual plegado (roll + teclado) | `?visualstream=0`; `?visualdebug=1` mide en consola (Worker del parser y, en WASMIDI, `[WASMIDI app]`) |
| Tiles culleados del roll | `?tiles=0` en la URL |
| Tiles de solo lo visible: una celda por píxel con la nota de arriba, un cuadrilátero por tile; scroll en píxeles enteros (rev. 106) | `?tiles=0` (todo crudo, con sobredibujo) |
| Lista de notas largas para armar tiles sin recorrer el ring desde la cola (rev. 106) | sin interruptor (`longIds_`) |
| Cola del ring del roll avanza también durante una reconstrucción (rev. 107) | sin interruptor (`advanceSharpTail`) |
| Notas sostenidas fuera del ring, debajo de todo (rev. 107) | sin interruptor (`held_`; `long N` en `[WASMIDI app]`) |
| Adelanto del roll con presupuesto fijo y ring con tope de 2^26 notas (rev. 107) | sin interruptor (`kSharpResidentBudget`, `kSharpRingMax`) |
| Límite de render del synth | `?synthlimit=0` / `ssw_set_render_limit_percent(0)` |
| Admisión por bloque (25% de voces) | `ssw_set_admission_percent(0)` |
| Tope de robos por ciclo (25%) | `ssw_set_steal_cap_percent(0)` |
| Limitador + bloqueador de DC | botón "Soft clip" apagado |
| Suma de volumen de las pilas VOR / eco (rev. 81) | botón "Stack gain" (WASMIDI) o casilla Echo (Audio) |
| WasmiSynth alcanza al reloj tras un underrun (rev. 84) | sin interruptor; `ssw_skip_to` no se llama si `RING_STARVED` queda en 0 |
| Roll mantiene el último cuadro / se reconstruye si quedó atrás (rev. 84) | sin interruptor (`renderRoll`) |
| Tiles culleados parciales del roll (rev. 85) | `?tiles=0` (apaga todos los tiles) |
| Teclado mantiene su imagen si el flujo visual empieza más adelante (rev. 85) | sin interruptor (`liveHeld_`) |
| Tramos visuales que crecen hasta pistas × 1024 palabras (rev. 86) | sin interruptor (`visualSpanMax`) |
| Teclado en casillas planas por (pista, canal), snapshots cada 4096 eventos (rev. 86) | sin interruptor (`LiveSnapshotEventInterval`) |
| Teclado en su propio Worker con helpers (rev. 87) | `?keyworker=0` |
| Tiles culleados armados de a pedazos por cuadro (rev. 88) | `?tileslice=0` |
| Feed del synth y flujo visual en tramos conjuntos (rev. 89; apagados con el adelanto de la rev. 92) | `?jointdecode=1` los fuerza, `?jointdecode=0` |
| Cada pista guarda su bloque descomprimido entre pedidos a helpers (rev. 90) | sin interruptor |
| Flujo visual en pedazos, uno por tramo anexado (rev. 91) | sin interruptor |
| Flujo visual codificado en corridas, leído por corridas (rev. 92) | sin interruptor |
| Helpers decodifican el flujo visual por adelantado hasta el plan (rev. 92) | `?visualahead=0`; `?visualplan=M` (millones de palabras) |
| Modo del roll: look-ahead con prellenado / canción entera (rev. 93) | botón "Roll cache" (Look-ahead / Full song) y MB; `?rollmode=1/2`, `?rollmode=0` = rev. 92 sin prellenado; `?lookahead=MB` |
| Reset del roll dentro del flujo guardado lo conserva (rev. 93) | sin interruptor (`visualKeptOnReset`) |
| Teclado recibe pedazos por demanda (rev. 93) | sin interruptor (`liveForward`) |
| Tope del rellenado: un tramo por vez pasada la mitad del plan (rev. 93) | `?refillcap=0` |
| Barrido del roll cede con `MessageChannel` (rev. 93) | `?sweepyield=0` |
| Dibujo de notas del roll | `?rolldraw=0` (solo diagnóstico) |
| Reloj en cada cuadro mostrado y pedazo de tiles según la pantalla (rev. 98) | sin interruptor (`advanceFrame`, `framePeriodMs_`) |
| Modo celular vertical (rev. 99), notas que caen (rev. 100) | sale solo con ventana más ancha que alta o de 760 px o más |
| Diseño compacto para ventanas bajas o angostas (rev. 100) | sale solo con 540 px de alto y 860 de ancho o más |
| Reparto Auto: Single hasta 4096 voces, Hash arriba (rev. 80) | Advanced… → Sharding: Channel/Hash/Single |
| Tiles de render de 256 voces y suma en orden fijo | sin rollback (rev. 69) |
| Modo determinístico | `ssw_set_deterministic(1)` (para pruebas) |
| WebGPU | botón CPU/GPU |
| Trampolín de `worker_cycle` | `-DSSW_WORKER_CYCLE_TRAMPOLINE=0` |
| Render de 4 voces a la vez (vectores) | `-DSSW_VOICE_QUAD=0` |
| Note-off FIFO / lista abierta | `-DSSW_NOTEOFF_FIFO=0` / `-DSSW_NOTEOFF_OPEN_LIST=0` |

## Pendientes abiertos (lista completa en HANDOFF §90)

1. **Meta del usuario: The Nuker 4 (y 3) en tiempo real de principio a fin a 1024
   voces por worker** en el Ryzen 9 5900X + RX 9070XT y el Ryzen 7 7435HS + RTX 2050,
   en WASMIDI y WASMIDI Audio, con MatheryLSD, sin diferencia audible frente a un MIDI
   sin robos ni descartes. Hecho en la rev. 74: (a) admisión en el parser y (b)
   decodificación repartida para el synth. Hecho en la rev. 75: etapa 2, roll/teclado/
   gráficos desde el flujo visual con helpers. Rev. 78: render de 4 voces por vectores.
   Medido por el usuario con la canción entera: 6144 voces estables en el 5900X (meta 24576
   = 1024 × 24 workers) y el roll se atrasaba en 830-858 s con 8 helpers (arreglado en la
   rev. 82). Rev. 79: modo GPU con todas las voces y suma por cuadro.
   Rev. 80: Auto = Hash arriba de 4096 voces (fase de eventos repartida).
   Rev. 93: modos look-ahead y canción entera (como BPFA).
   Falta: cuadros del roll de solo lo visible (rev. 106) con GPU real; The Nuker 3 entero sin cierre (rev. 107) en las
   PCs; los dos modos de la rev. 93 en las PCs
   (canción entera de The Nuker 3 con 32 GB);
   el tiempo de carga (el usuario: sharkmidi abre mucho más rápido); The Nuker 3 en WASMIDI con 12288 voces (`helpers N% busy` de
   `[WASMIDI visual]`; probar 6144 voces para separar CPU del synth de decodificación),
   log de consola del cierre de WasmiSynth en pausa;
   la ráfaga de 830 s de The Nuker 3 (~121 M palabras/s: anexar + barrer no alcanzan, rev. 87);
   medir el modo GPU en GPU real y Auto (Hash) contra Single en las PCs; helpers
   con fuente cruda; saltos justo después del pico (las densidades del reset decodifican
   ese segundo en el Worker del parser).
2. **Verificar en el navegador lo de la rev. 70-73**: que el WGSL compile y WebGPU
   ande, cuánto rinde el modo GPU con 1 y 12 workers, WASMIDI Audio con audio real
   (ventanas, letras, gráficos, visualizador) y el arreglo de la SF2 que no recargaba.
3. Anteriores: que el hilo coordinador también tome tiles de render; GUI cosmética; precisión de CC;
   envolvente de modulación, LFOs, moduladores, reverb y chorus de SF2 (HANDOFF §102).

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
