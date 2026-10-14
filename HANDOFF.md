# WASMIDI — Handoff completo

**Revisión 40.** Escrito para alguien que llega sin ningún contexto. Leé las
secciones 1 a 5 antes de tocar código. La sección 5 es la más importante: dice
dónde está el problema hoy. **La sección 19 (rev. 40) es el cambio más reciente y
corrige la lectura del Problema A y de todas las capturas previas.**

---

## 1. Qué es el proyecto

WASMIDI es un reproductor de **Black MIDI** que corre en el navegador,
desplegado en `https://midi.dekx.cc` (GitHub Pages). Black MIDI son archivos con
decenas de millones de notas y cientos de miles a millones de notas por segundo.

Dos mitades:

- **Síntesis de audio.** Un port a WebAssembly de **SnappySynthV2 (SSv2)**, un
  sintetizador de SoundFonts escrito en C para Windows. Vive en
  `third_party/snappysynthv2/`, se compila con Emscripten, y reparte el render
  de voces entre varios pthreads (Web Workers reales).
- **Visualización.** Un piano roll **horizontal** en WebGL2
  (`src/renderer/gl_renderer.cpp`) dentro de una app Qt6 también compilada a
  WASM. El tiempo corre en X y el pitch en Y.

### Las dos restricciones que mandan sobre todo lo demás

1. **No cambiar cómo suena SSv2.** El port tiene que sonar igual que el
   original. Una optimización que altere qué voz suena, cuándo, o cómo, no sirve.
2. **No cambiar cómo se ve el piano roll.**

Meta de rendimiento: **8192 voces estables con 8 workers.**

### Proyectos de referencia

El usuario provee dos árboles de código como referencia, no como dependencias:

- **SSv2 nativo original.** Referencia de sonido y de rendimiento. Tiene su
  propio handoff (`AGENT.MD`) con la lista de optimizaciones del motor.
  Hardware de referencia del autor: Ryzen 3700X (16 hilos).
- **BPFA** (Better Piano From Above). Referencia de arquitectura de rendimiento;
  ya usa SSv2 como motor.

El autor de SSv2 es accesible a través del usuario y ya aportó una corrección
decisiva (ver §6).

---

## 2. Restricciones del entorno (crítico)

**En el entorno donde se edita este código NO hay Qt6 ni emsdk.** Esto define
qué se puede afirmar y qué no:

- `src/mainwindow.cpp`, `src/mainwindow.hpp`, `src/qml/*` y
  `src/renderer/gl_renderer.cpp` **no se pueden compilar ni una vez**.
- El build de Emscripten completo tampoco.
- Nada se puede ejecutar ni perfilar desde acá.

Todo cambio en esos archivos llega al usuario sin verificar. El usuario compila,
prueba en el navegador y manda capturas. **Esas capturas son la única fuente de
verdad sobre el comportamiento real.**

### Lo que sí se puede verificar

```
cd third_party/snappysynthv2
gcc -fsyntax-only -I. -DSNAPPYSYNTH_WASM=1 Voice/voice.c
gcc -fsyntax-only -I. -DSNAPPYSYNTH_WASM=1 snappy_wasm_core.c
gcc -fsyntax-only -I. -DSNAPPYSYNTH_WASM=1 Parser/sfz_parser.c
cd ../..
node --check web/snappysynth-worker.js
node --check web/snappysynth_bridge.js

# Tests de host (extraen el código real de voice.c, no copias)
python3 tools/extract_ssw_schedule_logic.py
cc -O1 -Wall -o /tmp/h tools/ssw_schedule_harness.c -lm && /tmp/h
python3 tools/extract_voice_rebalance_logic.py
cc -O1 -Wall -pthread -o /tmp/b tools/worker_freelist_borrow_check.c && /tmp/b
python3 tools/extract_vor_redundancy_logic.py
cc -O1 -Wall -o /tmp/v tools/vor_redundancy_check.c && /tmp/v
cc -O1 -o /tmp/s tools/worker_freelist_share_check.c && /tmp/s
node tools/pump_slice_sim.mjs

# rev. 40: motor nativo real, 24 workers, crear/destruir el pool (ver §19)
S=third_party/snappysynthv2
cc -O2 -I$S -DSNAPPYSYNTH_WASM=1 -DSSW_WORKER_CYCLE_TRAMPOLINE=1 -pthread \
   -o /tmp/wc tools/worker_cycle_smoke.c $S/snappy_wasm_core.c $S/Voice/voice.c \
   $S/Parser/sf2_parser.c $S/Parser/sfz_parser.c $S/Parser/wav_loader.c \
   $S/wasm_stubs.c -lm && timeout 30 /tmp/wc

# QML: solo balance de llaves, NO es compilación
python3 -c "import re;s=re.sub(r'//[^\n]*','',open('src/qml/Controls.qml').read());s=re.sub(r'\"(\\\\.|[^\"\\\\])*\"','\"\"',s);print(s.count('{'),s.count('}'))"
```

**`MANIFEST.sha256` cubre 41 archivos. Regenerarlo después de cada cambio** con
`python3 tools/regen_manifest.py`, o el usuario no puede validar el zip.

Entregar siempre el **proyecto completo como zip**, no parches.

---

## 3. Arquitectura

### Cadena de audio

```
midi-parser-worker.js  -> parsea el MIDI, agenda eventos con lookahead
        | scheduleBatch (lote de eventos por mensaje)
snappysynth-worker.js  -> Worker; hospeda el módulo WASM
        |  pump() -> ssw_render_queued_into() -> voice_render_float()
        |                                        \-> N pthreads: worker_thread()
        | ring SharedArrayBuffer
snappysynth-audio-worklet.js -> AudioWorklet, consume en quantums de 128
        |
snappysynth_bridge.js  -> estado compartido, leído por Qt vía EM_JS
```

**Diferencia estructural clave con el nativo:** el nativo recibe eventos uno por
uno en tiempo real (`SendDirectData`) y su hilo de render drena la cola entre
llegadas. **El port agenda por adelantado y despacha el bloque entero de eventos
de golpe antes de cada render.** Muchos problemas se explican por esta diferencia.

### Dentro de `worker_thread` (el hot path)

Cada ciclo de render, cada worker:

1. **Fase previa** (medida por `ALLOC`): drena sus colas de eventos, asigna
   voces para los note-on (con selección de regiones), y roba voces si el pool
   está lleno.
2. **Fase de render** (medida por `BUSY`): consume la cola de render y sintetiza
   las voces.

### Ruteo de eventos (importante)

`enqueue_event()` en `voice.c` enruta así:

```c
if (e.type == EVT_NOTE_ON || e.type == EVT_NOTE_OFF) w = g_note_worker_map[qch][e.key];
else                                                  w = g_channel_worker_map[qch];
```

Notas y eventos de canal van a **colas de workers distintas**. Consecuencias:

- Una nota nunca se interpone entre dos CC en la cola de canal.
- Un program change y un note-on del mismo canal se consumen **concurrentemente**
  en el mismo ciclo. Solo una frontera de render los ordena (ver §7, error 1).

### VOR (Voice Overlap Reduction)

Apila note-ons idénticos repetidos en una sola voz. Es lo que permite que
millones de notas por segundo quepan en 8192 voces. Dos note-on se apilan solo si
comparten `vor_token`, y ese token mezcla la generación **del canal**. **Cada CC
incrementa esa generación** (`vor_break_channel_sequence`), así que un CC entre
dos notas idénticas impide que se apilen. Es comportamiento del original, no del
port.

---

## 4. Telemetría

Los números viajan en el payload de stats, **no por consola** (imprimir desde un
worker a esa frecuencia frena el navegador; lo reportó el usuario).

Ruta para agregar un contador, cinco archivos:
`snappy_wasm_core.c` (export `ssw_*`) → `snappysynth-worker.js` →
`snappysynth_bridge.js` (`state.*`) → `mainwindow.cpp` (`EM_JS`) +
`mainwindow.hpp` (`Q_PROPERTY`) → `Controls.qml`. Agregar también el export en
`CMakeLists.txt` (`EXPORTED_FUNCTIONS`).

### Modo Debug

Botón **Debug** en el panel de SnappySynth. Apagado por defecto. Cuando está
apagado, los contadores caros **ni siquiera se recogen** (una compuerta única
`g_debug_metrics` en `voice.c`). Se puede prender **durante la reproducción** y
**no recarga la soundfont** — va por un camino directo al worker sin pasar por
`applySynthConfig()`. Esto es crítico: antes sí recargaba, y medir destruía el
estado que se quería medir.

Siempre visibles (baratos): `RATE`, `ACTIVE`, `FREE`, `WORKERS`, `VOICES/WKR`,
`REGIONS`, `UNDERRUNS`.

Solo con Debug:

| Contador | Qué mide |
|---|---|
| `LOAD %` | costo del bloque como % de su presupuesto de tiempo real |
| `BLOCK ms` | costo del último bloque |
| `ALLOC ms` | tiempo sumado entre workers en la fase previa (eventos, asignación, robo). **Corrección rev. 40:** también incluye la espera en la barrera `g_render_producers_ready`, así que con desbalance entre workers se infla hasta ~N× el worker más lento (ver §19.4) |
| `BUSY ms` | tiempo sumado entre workers en el render de voces |
| `WACT` | cuántos workers tomaron al menos un chunk de la cola de render |
| `SIMD %` / `MISS <razón>` | porción de voces en el camino vectorizado y por qué fallan |
| `CC#n x% cnt` | controlador que se lleva más tiempo de despacho |
| `CCOL` | controladores colapsados por adyacencia (ver §8) |
| `PRESMP seen/resmp/skip` | regiones recorridas / realmente resampleadas / no materializadas en el último presampleo |
| `STEALS/s`, `DROPPED` | robos por segundo, note-ons descartados |
| `RING %`, `LATE ms` | ocupación del ring de audio, retraso del pump |

`RECYC` fue corregido en la rev. 38: se recoge **solo con Debug** y cuenta una
voz cuando el barrido post-render realmente retira un `ENV_OFF`. Los movimientos
internos de freelists (refill/rebalance) ya no cuentan como reciclaje.

---

## 5. ESTADO ACTUAL: dónde está el problema hoy

Hay **dos problemas distintos**. Se trataron como uno durante mucho tiempo y eso
causó la mayor parte de los errores de la sesión.

### Problema A — Carga inicial: materialización de la soundfont

**Síntoma que sigue siendo la referencia de navegador:** tras cargar el SF2 por
primera vez, el render puede ir muy lento con pocas voces; repetir el MIDI mejora
y cambiar de soundfont puede reintroducirlo.

**Corrección importante de la rev. 38:** la lectura anterior de `PRESMP 0/0
skip` era inválida. El contador C ya tenía `g_presample_regions_seen`, pero el
panel mostraba únicamente `resampled/skipped`. Una región cuyo sample ya está a
la frecuencia destino se recorre correctamente y puede dejar ambos en cero. Por
tanto, aquella captura **no demostraba `num_regions == 0`**. El panel ahora muestra
`PRESMP <seen> seen <resmp> resmp / <skip> skip`. La condición útil es
`seen == REGIONS` y `skip == 0`; `resmp` puede ser 0 legítimamente.

**Bug real encontrado al revisar el código:** `resample_wav_data()` devuelve el
buffer original tanto cuando no hace falta convertir como cuando falla una
reserva. `sfz_apply_presampling()` marcaba en ambos casos `resampled_rate =
target_sample_rate`. Si la reserva había fallado con sample rates distintos, el
cache quedaba envenenado: las pasadas posteriores creían que el sample ya estaba
preparado y no reintentaban. En rev. 38 ese caso deja el cache sin preparar,
incrementa `skip` y la carga falla en vez de entrar a reproducción a medias.

**Contrato de carga nuevo (rev. 38):**

- La capa recién parseada se presamplea **antes** de incorporarse al instrumento
  vivo.
- Cada región debe tener `cache_entry`, PCM decodificado válido y, cuando el
  sample rate de origen difiere del synth, un `resampled_data` real a la tasa
  destino.
- `ssw_init_ex()` vuelve a validar/materializar la fuente existente cuando cambia
  la configuración del synth.
- Después se ejecutan `voice_refresh_all_region_caches()` y
  `voice_prewarm_note_caches()`.
- Si la condición no se cumple, `ssw_load_sf2()`/`ssw_init_ex()` devuelven fallo y
  el worker **no puede anunciar “SF2 ready”**.

Esto implementa el principio del usuario: todo el trabajo de la soundfont debe
quedar en RAM antes de comenzar el MIDI. **Aún falta validarlo en navegador**; no
se puede declarar resuelto desde este entorno. La prueba decisiva de rev. 38 es:
primera carga con `PRESMP seen == REGIONS`, `skip == 0`, y comparar `LOAD/BLOCK/
BUSY` en la primera reproducción contra la segunda. Si esas invariantes se cumplen
y la primera sigue siendo lenta, el presampleo deja de ser la hipótesis principal.

**Rev. 40 — causa probable encontrada leyendo el código (ver §19).** Tras cargar
la SF2, el estado de datos de C es el mismo en primera carga y en recarga. Lo
único que la recarga cambia es que destruye y recrea los hilos de síntesis. Como
`worker_thread` nunca retornaba y V8 no hace OSR en WebAssembly, los hilos creados
al abrir la página corrían el hot path con el código base (Liftoff) toda su vida.
Rev. 40 parte el bucle en `worker_cycle()`, llamado una vez por bloque.
**Confirmado por el usuario en navegador: con rev. 40 la primera carga ya rinde
bien desde el inicio. Problema A resuelto.**

### Problema B — Zona densa con CC: el pool se satura

**Síntoma:** en zonas con muchísimas notas y CC, lag fuerte.

**Evidencia:** `ALLOC` es **20-40× `BUSY`** en cinco capturas distintas. Un
perfilado del navegador lo confirma aparte: `wasm-function[47]` se lleva el 70%
del tiempo, todo *self time*, bajo `invokeEntryPoint` → `handleMessage` de
`snappysynth-core.worker.js`. Eso es el punto de entrada de un **pthread**, o sea
`worker_thread` con todo inlineado por `-O3`. Pool saturado (`FREE` en decenas),
decenas de miles de robos por segundo, cientos de miles de notas descartadas.

**Principio del usuario:** *en un diseño monolítico, bindear un CC no debería
poder lagear el render por sí solo.* Aplicar un CC debería ser escribir un entero
en el estado del canal. Si lagea, hay algo en el camino que no es eso. Candidato:
cada CC rompe el apilado VOR (§3), y el costo aparece después, en las notas que
ya no se apilan y consumen voz propia.

**Plan acordado:** atacar la fase previa **y** la frontera de eventos a la vez,
detrás de un define único, sin tocar nada que decida qué voz suena. Si no
mejora, la fase queda descartada entera. Frentes, por margen real:

1. **Frontera de eventos.** `ssw_queue_events()` hace un `malloc` por lote y un
   `llround` sobre `double` por evento. Aritmética pura, precalculable, resultado
   idéntico.
2. **Drenado de eventos.** Trabajo por evento que sea invariante del lote.
3. **Asignación.** Ampliar cobertura de los cachés note/region existentes.
4. **Robo.** Margen mínimo; casi todo cambia qué voz muere.

**Rev. 38 — primer pase seguro aplicado, pendiente de A/B en navegador:**

- `ssw_queue_events()` ya no obliga a `malloc/free` por cada lote. Mantiene un
  pool acotado de slabs reutilizables y conserva los más grandes para los lotes
  de 262.144 eventos. Rollback conjunto de esta frontera:
  `-DSSW_REV38_EVENT_HOTPATH=0`.
- La conversión `seconds -> frame` para timestamps no negativos usa el mismo
  redondeo de `llround()` (`floor(x + 0.5)` para este dominio) sin pagar la llamada
  matemática por evento. El mismo define restaura `llround()`.
- La instrumentación de `RECYC`, notas iniciadas y cobertura SIMD dejó de hacer
  atómicos globales por nota/voz cuando Debug está apagado. `worker_thread` toma
  una sola instantánea de la compuerta por ciclo. Esto elimina trabajo añadido
  por el port que no existe en el motor musical y no cambia ninguna decisión de
  voz.
- No se tocó `steal_voice_fast`, `alloc_voice`, selección de regiones, VOR,
  fronteras de selector ni el orden/timestamp de eventos.

Si `ALLOC` sigue dominando después de este pase, continuar dentro de
`worker_thread`: drenado/rebalance/cache locality. No modificar la política del
stealer sin una medición que lo exija.

---

## 6. Descartado con medición. NO volver a perseguirlo.

- **El DSP como causa de la zona densa.** `BUSY` despreciable frente a `ALLOC`.
- **Los CC como costo directo.** El **autor de SSv2** lo zanjó: *"un CC es un
  evento, una nota son dos; los CC son la mitad de caros que las notas"*. Con
  3,99M de CC contra 44,7M de notas son una fracción mínima. El `CC#7 100%` del
  perfilado por controlador era trivial: CC 7 era el único controlador presente.
- **El handler de CC.** Cada caso es un switch, una escritura y un
  `InterlockedExchange` sobre un flag con consumidor protegido. Ya es óptimo.
- **El reparto de la cola de render.** `WACT 23` de 24.
- **Vectorizar como solución.** `MISS INTERP 100%` es el caso **normal**:
  `no_interp` exige velocidad exactamente 1.0 (solo la nota en el root key, sin
  bend). Casi ninguna nota real califica.
- **El bloque de mezcla desacoplado.** No es costo fijo por llamada.
- **Voces por worker.** El nativo en el 5900X del usuario usaría los mismos 24
  workers.
- **`PK POLY` como medida de demanda de voces.** En Black MIDI la polifonía no
  se traduce en voces ni de cerca: capas de regiones, colas de release, y un
  flujo de notas que llega más rápido de lo que las releases se apagan. **El
  usuario corrigió este error dos veces.**
- **Fuga de voces.** `ACTIVE` sube y baja siguiendo la música.

---

## 7. Errores cometidos. No repetirlos.

1. **Quitar la frontera de render para bank/RPN/program change.** Parece seguro
   porque "la cola se drena en orden", pero notas y eventos de canal están en
   colas distintas (§3). Sin la frontera, los note-on resuelven contra
   banco/programa equivocado y **la nota se cae sin sonar**. Rollback:
   `SSW_FORCE_SELECTOR_BOUNDARIES=1`.
2. **Fragmentar el render por cada evento de estado.** Un bloque se volvía ~100
   llamadas a `voice_render_float()`.
3. **Punto fijo en el mapeo de columnas del culler visual.** Redondea distinto y
   descarta notas visibles. Usar el mismo `floor`/`ceil` en `double`.
4. **Agregar elementos a un `RowLayout` lleno** en QML. No hace wrap y desborda.
   Usar `Flow`.
5. **Ocultar media fila con `visible:`** condicionado a algo que no aplica a
   todos sus elementos. El panel salta y se pierde información.
6. **Comparar capturas que difieren en dos variables a la vez** (dos MIDIs
   distintos; primera carga contra sesión densa). Conclusiones inválidas.
7. **Colapsar CC por supersesión** en vez de por adyacencia. Rompe las rampas de
   automatización; el usuario lo oyó como respuesta de CC peor que el original.
8. **Anclar una edición en un patrón que aparece varias veces en el archivo.**
   Un reset de contador anclado en `if (!inst) return;` aterrizó en otra función
   y el contador midió basura durante varias revisiones. **Verificar dónde
   aterrizó toda inserción antes de entregar.**
9. **Leer mal el árbol del perfilador.** Se interpretó `invokeEntryPoint` como
   una llamada a la API por mensaje; es el arranque de un pthread.
10. **Sobrescribir el handoff sin releerlo.** Pasó dos veces. En la rev. 22 se
    reemplazó la sección de la rev. 21 de GPT en vez de agregar encima, y se
    perdió su trabajo documentado durante varias revisiones. En la rev. 37 se
    borró el documento entero y se reescribió de memoria, perdiendo la política
    de workers, la sección de memoria, los defines de rollback y el análisis de
    varios pendientes. Ambos se restauraron (secciones 12-14). **Regla: este
    documento solo se edita agregando o corrigiendo en el lugar; nunca se
    reemplaza un bloque entero sin haberlo leído y sin conservar su contenido.**
11. **Mandar un mensaje `configure` parcial al worker.** El handler de `configure`
    trata cada mensaje como configuración completa: los campos ausentes caen a sus
    mínimos (`maxVoices` 1, `blockFrames` 1, `numBuffers` 1, workers Auto) y
    reinicializa el motor. Los toggles en caliente van con su propio tipo de
    mensaje (ver §20).
12. **Estructuras intrusivas compartidas entre workers.** En rev. 41 la lista abierta
    del note-off usaba enlaces por voz; al readmitir una voz tomada del pool global,
    otro worker la desenganchaba de una lista ajena mientras su dueño la recorría. En
    el contenedor (1 núcleo) no se manifiesta; en el navegador cuelga un worker. Toda
    estructura nueva debe tener un único hilo escritor, y las pruebas de host con 1
    núcleo no demuestran ausencia de carreras.
13. **Acortar el horizonte de precarga visual para contener el ring** (rev. 48). La
    densidad estimada desde lo residente (`sharpTail_` → cabeza preparada sobre
    `viewStart` → `sharpRemoteSafeThrough_`) sale inflada, porque la cola retiene notas
    largas que empezaron mucho antes de `viewStart`; el horizonte caía al mínimo de 4
    pantallas y los frames se perdían mucho antes. No tocar el horizonte sin medirlo.
14. **Dejar sin implementar una función "que la app no usa"** (rev. 54). El CI
    (`midi_parser_bootstrap_smoke.cjs`) ejercita todo el contrato `wmp_*`; antes de
    reemplazar un componente, buscar sus usos también en `tools/`.
15. **Decidir con benchmarks de host algo dominado por el navegador** (rev. 58). La
    lectura bajo demanda ganaba 35% con `pread` en host y en el navegador cargó más
    lento: el costo real eran los cruces EM_JS y `FileReaderSync` por página.

---

## 8. Estado del código: qué está activo

| Mecanismo | Estado | Nota |
|---|---|---|
| Gobernador de segmentación | activo | fronteras en grilla, tope por carga medida |
| Fronteras de selector exactas | activo | correctitud, no optimización |
| Filtro de redundancia VOR | activo | suprime el break solo si el CC repite su valor; no ayudó en el MIDI de prueba porque sus CC cambian de verdad |
| Colapso de CC por adyacencia | **activo** | `SSW_COLLAPSE_CONTROLLER_BURSTS=1`; regla nativa; colapsó 1,78M sin mejorar |
| Rebalanceo de voces libres | activo | hecho por GPT, verificado |
| Pool de slabs de eventos | **activo rev. 38** | evita malloc/free por lote; rollback `SSW_REV38_EVENT_HOTPATH=0` |
| Redondeo rápido de timestamps | **activo rev. 38** | equivalente a `llround` para tiempos admitidos no negativos; mismo rollback |
| Validación de materialización SF | **activo rev. 38** | no declara lista una capa con PCM/cache/resample incompleto |
| Telemetría hot solo con Debug | **corregido rev. 38** | sin atómicos por note-on/voz en reproducción normal |
| Prewarm de cachés note/region | activo | ahora corre después de validar/materializar la carga |
| `voice_refresh_all_region_caches` | activo | bug real del multiplicador de pitch; se conserva |
| Cadencia del pump | activo | underruns 4.770 → 82, medido |
| Ciclo de worker separado | **activo rev. 40** | `worker_cycle()` por llamada indirecta; mismo código, deja entrar el código optimizado de V8; rollback `SSW_WORKER_CYCLE_TRAMPOLINE=0` |
| Culler visual | **no conectado** | `src/renderer/note_raster_compositor.*`, verificado celda por celda pero cuesta más de un frame sin un caché de tiles en segundo plano |

Si el colapso de CC por adyacencia altera el sonido, apagarlo con el define.

---

## 9. Método: lo que funcionó y lo que no

Esta sesión acumuló **ocho hipótesis falladas** y **cinco contadores que medían
algo distinto de lo que su nombre prometía**. El patrón fue siempre el mismo:
confirmar una idea con una lectura propia sin validarla contra un caso de
respuesta conocida.

**Todo lo que avanzó vino del usuario:** aislar que sin CC no lagea, que mejora
al repetir, que se rompe al cambiar de soundfont, que el problema desaparece al
recargar, las capturas en idle, y el perfilado del navegador. **Priorizar sus
observaciones sobre la instrumentación nueva.**

Reglas prácticas:

- Antes de sacar conclusiones de un contador nuevo, verificar que da el valor
  esperado en un caso conocido (idle, un MIDI trivial).
- Una sola variable por comparación.
- Leer el código original antes de proponer: el motor es byte por byte idéntico
  en `enqueue_event`, `steal_voice_fast` y `alloc_voice`.
- Tras cualquier inserción automática en un archivo grande, imprimir la región
  y confirmar la función que la contiene.

### La herramienta que más destrabaría

Un **build de diagnóstico con `-O1` o `-g2`** en lugar de `-O3`. Con `-O3` todo
se inlinea en una función y el perfilador del navegador no puede desglosar. Con
símbolos mostraría `collect_note_regions_cached`, `steal_voice_fast`,
`alloc_voice` y el resampleo por separado, y nombraría al culpable directamente.

Cómo perfilar en Brave: F12 → Performance → grabar 5 s → pestaña **Bottom-up** o
**Call tree**, ordenar por Self time, elegir el hilo del worker.

---

## 10. Cómo trabajar con el usuario

- Prueba en **Brave, Windows, Ryzen 9 5900X (24 hilos)**. Brave sub-reporta
  `navigator.hardwareConcurrency` (reporta 7) por defensa anti-fingerprinting;
  por eso un pedido explícito de workers no se recorta contra los cores
  detectados.
- Escribe en español. Manda capturas del panel y de la consola.
- Prefiere arreglos a mediciones, y tiene razón en presionar por eso. Pero
  acepta instrumentar cuando se explica qué decide la medición.
- Fija las reglas de fidelidad y hay que respetarlas: si el original suena así,
  el port también.
- Si no carga el SF2, primero verificar HTTPS, ventana no privada y Shields de
  Brave: los pthreads necesitan `crossOriginIsolated`, que se logra con
  `web/coi-serviceworker.js`.
- Con cada update, actualizar este documento.

---

## 11. Pendientes a largo plazo (sin tocar)

- **GUI:** selector de colores estilo BPFA con popup RGB/hex y paleta en
  cookies; quitar Post-buffer; cambiar "Black MIDI Player" por "WASM-MIDI
  Toolkit / Update 0 Revision 19 (Test Build)"; note speed por ticks; dejar un
  solo gráfico NPS y quitar Skipped vel; quitar la barra superior; fondo
  "neural network"; limpiar referencias a terceros. El usuario pidió dejar esto
  para cuando el rendimiento esté resuelto.
- **Soundfonts GM completos suenan mal** y los drums no siguen el estándar.
  Viene del SSv2 original.
- **Carga de MIDI** lenta comparada con BPFA (`StreamingMidiParser`,
  `PreprocessedMidi` sin revisar).
- **Prerender de frames visuales** con caché de tiles en hilos de fondo, para
  que el culler visual rinda.
- **Precisión de CC** respecto del original: el usuario reportó que el pitch y
  el release a veces no se aplican a tiempo. Dos causas candidatas documentadas
  en el historial: la coalescencia sin timestamp en `enqueue_event` combinada
  con el agendado por lotes, y el gobernador colapsando a un render por bloque
  bajo carga.

---

## 12. Referencia técnica detallada

La reescritura de la sección 1-11 resumió varias cosas que no deben perderse.
Esta sección las conserva **sin editar**, tal como estaban en la revisión 37
antes de la reescritura. Si algo acá contradice las secciones 1-11, **mandan las
secciones 1-11**, que están actualizadas.

Incluye: política de workers y pool de voces, memoria y `memory.grow`,
cross-origin isolation, trabajo verificado con sus defines de rollback
(`SS_EVENT_SPIN_ITERATIONS`, `WORKER_FREELIST_KEEP_MAX`), el detalle del culler
visual, y los pendientes 7.1 a 7.10 con su análisis completo.

### Política de workers y pool de voces

- `Workers = 0` → todos los hilos que reporte el navegador.
- `Workers = N` → N hilos. **Un pedido explícito no se recorta contra el conteo
  de cores detectado**, porque Brave *farblea* `navigator.hardwareConcurrency`
  por defensa anti-fingerprinting (una máquina de 24 hilos reporta 7).
- El pool de voces se **divide** entre los workers.
- `WORKER_FREELIST_REFILL` es un reparto proporcional, no 512 fijo. Motivo:
  `free_push()` devuelve la voz al stack **local** del worker, nunca al pool
  global, así que con lote fijo los primeros workers se quedaban con todo y el
  resto no podía ni asignar ni robar. Con 1024 voces y 24 workers, 22 quedaban en
  cero, y las notas enrutadas ahí se caían sin sonar.
- **Rebalance por ciclo** (rev. 21): lo anterior arregló el arranque pero no el
  régimen. Ahora cada worker devuelve al pool global, al inicio de cada ciclo,
  las voces libres que sus eventos en cola no van a usar. Ver §0 y
  `PORTING_STATUS.md` → "Per-cycle freelist rebalance".
- `PTHREAD_POOL_SIZE` tiene piso de 34 y **no se deriva de
  `hardwareConcurrency`**, por lo mismo del farbling.
- `_beginthreadex` ahora **comprueba su retorno**. Antes lo ignoraba: si el pool
  se agotaba, `g_threads[w]` quedaba NULL pero `g_worker_count` seguía contándolo,
  y cada render esperaba un done event que nunca llegaba.

#### Memoria

El módulo del synth usa pthreads, así que su heap es un `SharedArrayBuffer` y
`memory.grow` **sincroniza a todos los workers**: con 16–24 los congela a todos a
la vez. Se ve como un stall sin carga.

Lo que lo dispara escala con el conteo de workers: `CH_EVENT_QUEUE_SIZE` son
16384 eventos por *(worker, canal)*, o sea >150 MB de colas con 24 workers contra
~44 MB con 7. Por eso `INITIAL_MEMORY` está en 1 GB con paso de crecimiento
lineal de 256 MB, y `DEFAULT_PTHREAD_STACK_SIZE` en 256 KB.

#### Cross-origin isolation

Los pthreads necesitan `crossOriginIsolated=true`, que en GitHub Pages se logra
con `web/coi-serviceworker.js`. Sin HTTPS no hay Service Worker, sin Service
Worker no hay COI, sin COI no hay `SharedArrayBuffer`, y el SF2 no carga. Si el
usuario reporta "no cargan las soundfonts", verificar HTTPS, ventana no privada y
Shields de Brave **antes** de mirar código.

---

### 12.b Trabajo hecho y verificado (no tocar sin motivo)

**Gobernador de segmentación** (`ssw_render_queued_into` en
`snappy_wasm_core.c`). Las fronteras de render caen solo en una grilla múltiplo
de 8 frames, con tope adaptativo por carga medida. Los eventos selector
(bank/RPN/data entry/program change) toman frontera **exacta** cuando hay una
nota admitida en ese canal desde la última frontera — esto es correctitud, no
optimización (ver 3.1). Test: `tools/ssw_schedule_harness.c`, 14 grupos.

**Cadencia del pump** (`snappysynth-worker.js`). Rellena hacia un objetivo de
ocupación del 75% en tajadas de 12 ms, en vez de ráfagas de 16 bloques. Bajó la
ráfaga sincrónica de 116 ms a 20 ms. Underruns 4.770 → 82 medido en navegador.
Simulación: `tools/pump_slice_sim.mjs`.

**Culler visual** (`src/renderer/note_raster_compositor.{hpp,cpp}`). Port
horizontal del compositor de `NoteMeshCache` de BPFA. Devuelve **notas, no
geometría**, para que el shader original quede intacto — BPFA hornea bordes,
sombreado por velocity y anchos distintos para sostenidos, y eso cambiaría la
estética. Usa el orden de capas de BPFA (gana la que empieza después). El test
compara celda por celda contra un modelo del depth rule: **cero diferencias en 9
casos**. A 1M notas por viewport saca 76% de las instancias en 21,8 ms.

> **NO está conectado a `gl_renderer.cpp`.** 21,8 ms es más de un frame, y a 10k
> notas saca solo 2,6% de instancias por 0,5 ms, o sea que ahí es pérdida. Solo
> rinde con el caché de tiles en hilos de fondo (7.3). Al conectarlo: el pase de
> notas necesita depth test apagado o z constante, porque el orden lo impone el
> culler.

**Spin acotado antes del futex** (`compat/win_compat.h`). 384 iteraciones antes
de parkear. Correctitud verificada con 20.000 ciclos × 6 workers sin wakeups
perdidos. **El beneficio no está medido** — no se puede fuera del navegador.
Rollback: `-DSS_EVENT_SPIN_ITERATIONS=0`.

**Rebalance de freelists por ciclo** (`rebalance_worker_freelist` en
`voice.c`). Ver §0. Test: `tools/worker_freelist_borrow_check.c`, 5 grupos,
incluye el bloque real extraído. Rollback: `-DWORKER_FREELIST_KEEP_MAX=16384`
convierte `keep()` en "quedate con todo" y anula el efecto sin tocar código.

---

### 12.c Pendiente a largo plazo

Ordenado por lo que el usuario priorizó.

**7.1 — Notas que no suenan con múltiples workers.** **Hecho en rev. 21** (ver
§0), pendiente de captura del usuario. La hipótesis era correcta: un worker sin
voces libres solo roba las propias y descartaba notas teniendo voces libres en
otros workers. En vez de robar de la pila de otro (que obliga a hacer la pila
concurrente y toca el camino caliente), cada worker devuelve al pool global lo
que no va a usar, y el pool ya es lock-free. Test de host incluido. Si la
captura muestra `DROPPED > 0`, seguir la guía de §0.

**7.2 — Gráficos en la barra lateral.** El usuario pidió carga, latencia, voces y
steals/s como gráficos junto a los que ya existen (NPS, POLYPHONY), y el resto
como contadores. Hoy están todos como contadores de texto. Los datos ya están
expuestos como `Q_PROPERTY`. **Falta ubicar el componente de gráfico
reutilizable** — no está en `Controls.qml` ni en `StatsPanel.qml`.

**7.3 — Prerender de frames visuales.** La otra mitad de `NoteMeshCache`: 2–32
hilos de fondo, hasta 64 pantallas por adelantado, caché por (firma de settings,
tick de inicio), invalidación por contador de generación en el seek.

**7.4 — Bloque de mezcla desacoplado.** BPFA usa 8192 frames para la mezcla del
synth y 1024 para los buffers del dispositivo, con este comentario en
`SynthAudio.cpp`: *"Larger blocks amortize worker barriers and API transitions
while the separate 1024 frame WinMM buffers retain responsive playback/seek
behavior."* WASMIDI usa un solo `blockFrames` para ambas cosas.
**No es cambiar una constante:** `preferredRenderFrames()` ya tiene una nota de
que se intentó y se revirtió porque un render largo sincrónico dejaba sin atender
la entrega de eventos. Requiere sacar el render del hilo de mensajes primero.

**7.5 — Soundfonts GM completos suenan mal.** Con bancos e instrumentos distintos
se reproducen los samples incorrectos, y los drums no siguen el estándar MIDI.
**Viene del SnappySynthV2 original**, no del port. Sin revisar. Es selección de
preset/banco/región, no rendimiento.

**7.6 — Carga de MIDI lenta y con consumo alto comparada a BPFA.**
`StreamingMidiParser` y `PreprocessedMidi` de BPFA sin revisar.

**7.7 — GUI.** Todo pendiente, y el usuario pidió dejarlo para cuando el
rendimiento esté resuelto:
- Selector de colores como BPFA: eliminar el botón "Per Track", listar los
  colores en Colors, popup al hacer click con RGB y hex en tiempo real, paleta
  guardada en cookies.
- Quitar el botón "Post-buffer".
- Cambiar "Black MIDI Player" por "WASM-MIDI Toolkit" y debajo "Update 0
  Revision 19 (Test Build)".
- Note speed totalmente basado en ticks, no en tiempo. Si empeora el rendimiento,
  avisar.
- "NPS Live" y "NPS" son lo mismo: dejar "NPS". Sacar "Skipped vel".
- Quitar la barra superior (Dekxtopia, Home, MIDI Player WebGL2).
- GUI más responsiva a los colores activos del teclado. Falta el fondo "neural
  network" que antes estaba.
- Limpiar referencias a proyectos de terceros y a funcionamiento interno
  avanzado, para que sea apto para producción.

**7.9 — RESUELTO en la rev. 23.** Ver §0.

**7.10 — Los CC no se aplican como en el SnappySynthV2 original.** Reportado por
el usuario: las canciones se deforman, como si el pitch bend y el release no se
aplicaran correctamente o a tiempo. **Investigado, no arreglado.** Hay dos
causas candidatas y son independientes:

*Causa A: coalescencia de CC contra agendado por lotes.* `enqueue_event()` en
`voice.c` fusiona un CC con el evento inmediatamente anterior de la misma cola
cuando coinciden canal y número de CC:

```c
if (prev->type == EVT_CONTROL_CHANGE && prev->ch == e.ch && prev->key == e.key) {
    prev->value = e.value;
    prev->timestamp_qpc = e.timestamp_qpc;
    return;
}
```

Notar que **no compara timestamps**, a diferencia de la fusión de notas, que
exige `prev->timestamp_qpc == e.timestamp_qpc`. Quedan excluidos los CC 6, 38,
98, 99, 100, 101, 120, 121 y 123, pero **no** los continuos: 1, 7, 10, 11, 64,
71-74, 91, 93.

En el motor nativo esto es inofensivo, porque los eventos llegan en tiempo real
y el hilo de render drena la cola entre llegadas: los valores intermedios de una
rampa sobreviven. En WASMIDI los eventos de un bloque entero se despachan de
golpe **antes** de un único `voice_render_float()`, así que una rampa de CC11 de
0 a 127 a lo largo de 512 frames se colapsa a 127 aplicado al inicio del bloque.
La automatización continua pierde su forma. Esto es estructural del agendado por
lotes, no un bug puntual.

Verificar primero si el pitch bend sufre lo mismo: existe una rama
`prev->type == EVT_PITCH_BEND` en el mismo enqueue y hay que leer sus
condiciones. Si también fusiona sin comparar timestamp, explica directamente la
deformación reportada, porque un bend colapsado salta en vez de deslizarse.

*Causa B: el gobernador de segmentación colapsa a un solo render bajo carga.*
`ssw_render_queued_into()` reduce el presupuesto a 1 cuando la carga medida
pasa el umbral, y con presupuesto 1 **ningún** CC abre frontera: todos se
aplican al inicio del bloque. Con bloque de 512 eso es 11,6 ms de granularidad
justo cuando el material es denso. Además, las fronteras se cuantizan hacia
abajo en la grilla, así que un CC se aplica hasta un cuanto **antes** de su
posición real; el nativo lo aplica *después*, en el chunk siguiente. Temprano y
tarde no suenan igual.

*Cómo separarlas:* forzar `g_render_budget` a su máximo y ver si la deformación
persiste. Si desaparece, es B y se ataca con el presupuesto o con el bloque de
mezcla desacoplado (7.4). Si persiste, es A y hay que hacer que la fusión de CC
respete el timestamp como ya lo hace la de notas — con el costo de que la cola
se llena más, ver `CH_EVENT_QUEUE_SIZE`.

*Advertencia:* tocar esto cambia el sonido por definición, así que es el tipo de
cambio que hay que comparar contra el nativo antes y después, no solo medir.

**7.9 (histórico) — Rendimiento malo hasta cambiar de soundfont una vez.** La primera carga de SF2 parece dejar algo en un estado
peor que el que deja una recarga. Candidatos: layout o conversión de samples,
preprocesado de regiones, el caché de steal score, o el conteo de workers al
momento de la primera carga. Comparar la ruta de `ssw_load_sf2` en primera
carga contra recarga.

**7.8 — Frames del renderizador se detienen** en pasajes de 3M+ notas por
segundo. El ring visual arranca en `1 << 23` notas y **duplica**: a 12 bytes por
`VisualNote` eso es un VBO de 100 MB y el siguiente escalón 201 MB. Hipótesis sin
confirmar: una de esas duplicaciones falla en la GPU y el ring queda inservible.
Para confirmarlo hay que loguear en `ensureRingCapacity()` y `allocateRing()` el
tamaño pedido, el resultado de `glBufferData` y el error de GL.

---

---

## 13. Apéndice A: la revisión 21 de GPT (restaurada)

**Esta sección se había perdido.** En la revisión 22 se reemplazó el bloque de
la revisión 21 en lugar de agregarse encima, y el trabajo documentado de GPT
desapareció del handoff durante varias revisiones sin que nadie lo notara. Se
restaura acá **tal como GPT la escribió**, desde su copia original.

Algunas de sus conclusiones fueron revisadas después (por ejemplo, el rebalance
de freelists sigue activo y verificado; la lectura de `SIMD` que corrigió era
correcta y quedó incorporada). Ante contradicción con las secciones 1-11, mandan
esas.

### Revisión 21 (GPT)

**7.1 resuelto en el motor, con prueba de host. Sin verificar en navegador.**

- `voice.c`: `rebalance_worker_freelist()` corre al inicio de cada ciclo de
  worker. El worker se queda con tantas voces libres como eventos tiene en
  cola (tope 128) y devuelve el resto al pool global en **un solo CAS**
  (encadena el sobrante por `g_next_free` primero). El camino caliente
  (`free_pop`) no cambia. El lote de refill y el pre-refill del ciclo quedan
  acotados por la misma cifra para que los tres no se peleen.
- Invariante que compra, por ciclo: **ninguna voz sonando se roba mientras
  exista una libre en cualquier worker que no la vaya a usar**. Es exactamente
  lo que pedía 7.1.
- Contadores nuevos: `REBAL/s` (voces devueltas al pool por segundo) y
  `DROPPED` (note-ons que se cayeron sin sonar, acumulado). Antes la caída
  era invisible en todo stat. Van por los cinco archivos de la ruta de
  telemetría (§5), en el `Flow`, no en el `RowLayout`.
- Test: `tools/worker_freelist_borrow_check.c` incluye el bloque **real**
  extraído de `voice.c`. Reproduce el bug con rebalance apagado (1792 notas
  caen a robo/drop en 4096/8), luego prueba cero caídas en 1024..16384 voces ×
  2..24 workers, conservación (cada vid en exactamente un lugar), integridad
  de la cadena, y estrés de 8 hilos contra el pool CAS. Determinista, ~90 ms.
- CI: `.github/workflows/build-wasm.yml` ahora corre **todos** los tests de
  host de §2 antes del build de Emscripten. Un cambio de motor que rompa uno
  no llega a deploy.
- Arreglado `tools/ssw_schedule_harness.c`: le faltaban tres globals de
  telemetría de la rev. 20 y no compilaba. Pasa 14/14.

**Qué mirar en la próxima captura:** `DROPPED` debe quedarse en 0 en el
material que antes perdía notas melódicas. Si sube con `REBAL/s > 0` y
`FREE > 0`, el problema es otro (selector, 3.1). Si sube con `REBAL/s = 0`,
el rebalance no está corriendo y hay que mirar `worker_has_pending_events`.

---

---

## 14. Apéndice B: historial de revisiones 22 a 37 (sin editar)

Registro cronológico de cada revisión tal como se escribió en su momento.
**Contiene conclusiones que después se demostraron falsas** — por ejemplo, que
el presampleo estaba descartado (lo estaba por un contador roto, ver error 8 en
la sección 7), o que la rev. 23 resolvía el bug de primera carga (no lo hacía).
Se conserva porque muestra el razonamiento y qué se probó, y porque borrarlo
fue precisamente el error que obligó a restaurar el apéndice A.

**No usar este apéndice como fuente de estado actual. Para eso están las
secciones 1-11.**

### Rev. 37

#### Dónde está el tiempo, confirmado por dos vías independientes

`ALLOC` (tiempo de la fase previa al render, sumado entre workers) es 20-40x
`BUSY` (tiempo del bucle de consumo de la cola de render) en toda zona densa,
en cinco capturas distintas. Un perfilado del navegador lo confirma aparte:
`wasm-function[47]` se lleva el 70,4% del tiempo, todo *self time*, y está bajo
`invokeEntryPoint` -> `handleMessage` de `snappysynth-core.worker.js`, o sea el
punto de entrada de un pthread: es `worker_thread` con todo inlineado por -O3.

**El tiempo está dentro de `worker_thread`, en la fase previa al render:**
drenar colas de eventos, asignar voces, robar.

#### Descartado con medición. No volver a perseguirlo.

- **El DSP.** `BUSY` es despreciable frente a `ALLOC`, y `_emscripten_get_now`
  aparece con 5,5 ms en el perfilado, así que los contadores tampoco distorsionan.
- **Los CC.** El autor del motor y del MIDI de prueba lo zanjó: *"un CC es un
  evento, una nota son dos; los CC son la mitad de caros que las notas"*. Con
  3,99M de CC contra 44,7M de notas son una fracción mínima. El `CC#7 100%` que
  midió el perfilador por controlador era trivial: era el único CC presente.
- **El reparto de la cola de render.** `WACT 23` de 24 workers participando.
- **Vectorizar.** `MISS INTERP 100%` es el caso NORMAL, no una patología:
  `no_interp` exige velocidad exactamente 1.0, o sea solo la nota en el root key
  sin bend. Casi ninguna nota real califica.
- **El bloque de mezcla desacoplado (7.4).** No es costo fijo por llamada.
- **Voces por worker.** El hardware de referencia del repo de SSv2 es un 3700X
  (16 hilos) pero el usuario corre un 5900X (24), así que el nativo en su
  máquina usaría el mismo conteo de workers.

#### Intentos fallidos, con su causa

- **Colapso de ráfagas de CC por supersesión** (rev30). Bajó `ALLOC` de 32x a 8x
  pero la automatización salió escalonada y el usuario reportó la respuesta de
  CC como peor que el original. La fusión nativa exige adyacencia en el slot
  inmediatamente previo de la cola; supersesión no es la misma operación.
- **Colapso por adyacencia** (rev33), que sí es la regla nativa. Colapsó 1,78M
  de eventos y **no mejoró nada**, lo cual es consistente con que los CC no sean
  el costo.
- **Presampleo y `refresh_region_runtime_cache`** (rev23, rev29). Ninguno era la
  causa del bug de primera carga.
- **Prewarm de los cachés note/region** (rev36). No resolvió el bug de primera
  carga.

#### El bug de primera carga: abierto, intermitente

Primera carga de SF2 lenta, mejora al repetir el MIDI, se rompe al cambiar de
soundfont. **Desapareció tras recargar durante las pruebas y volvió después.**
Depende del estado de arranque. Sin causa identificada.

#### Plan acordado con el usuario para la próxima sesión

Atacar los tres componentes de la fase previa al render **más la frontera de
eventos**, todo a la vez, detrás de un define único para revertir de un tiro. Si
no mejora, la fase queda descartada entera.

**Regla que el usuario fijó y que manda sobre todo:** solo optimizaciones que no
puedan alterar qué voz se elige ni cuándo suena. Eso deja FUERA la política del
stealer y la selección de regiones, y deja DENTRO evitar trabajo repetido,
cachear recálculos y sacar cómputo invariante de los bucles.

Los cuatro frentes, en orden de margen real:

1. **Frontera de eventos.** `ssw_queue_events()` hace un `malloc` por lote y,
   por evento, un `llround` sobre `double` más la conversión a frames. Es
   aritmética pura: precalcularla da un resultado idéntico más barato.
2. **Drenado de eventos.** Buscar trabajo por evento que sea invariante del lote.
3. **Asignación.** Los cachés note/region ya existen; ampliar su cobertura no
   cambia qué región se elige, solo cuánto cuesta encontrarla.
4. **Robo.** Margen mínimo: casi todo lo tocable cambia qué voz muere. Limitarse
   a evitar recálculos dentro del bucle de sondeo.

#### Lo que más ayudaría antes de escribir código

Un build de diagnóstico con `-O1` o `-g2` en vez de `-O3`. Con -O3 todo se
inlinea en una función y el perfilador no puede desglosar; con símbolos
mostraría `collect_note_regions_cached`, `steal_voice_fast`, `alloc_voice` por
separado y nombraría al culpable directamente. El reparto relativo sigue siendo
válido aunque el build sea más lento.

#### Método, para quien continúe

Esta sesión acumuló ocho hipótesis falladas y cinco contadores que medían algo
distinto de lo que su nombre prometía (`GAP` invertido, `FREED` mal ubicado,
`RECYC` contaminado por el rebalanceo, `PRESMP` con el reset en otra función,
y una lectura errónea del árbol del perfilador). El patrón siempre fue el mismo:
confirmar una idea con una lectura propia sin validarla antes contra un caso de
respuesta conocida.

Las cosas que **sí** funcionaron vinieron del usuario: aislar que sin CC no
lagea, que mejora al repetir, que se rompe al cambiar de soundfont, y el
perfilado del navegador. Priorizar eso sobre la instrumentación propia.

---

#### Rev. Estado en la revisión 30 (leer primero)

#### Intento fallido: colapso de ráfagas de controlador

Se implementó un pase que, antes de despachar los eventos de un bloque,
descarta un valor de controlador que otro posterior del mismo canal y
controlador supersede. Idea: restaurar el agregado que el motor nativo recibe,
ya que allá los eventos llegan de a uno y la cola los colapsa contra el evento
previo mucho antes de acumular un bloque.

**Resultado numérico:** bueno. `CCOL 1.149.605` de 3.988.966 CC del archivo,
`ALLOC` bajó de ~32x `BUSY` a **8x**, y `UNDERRUNS 0` con NPS 687.872 y
CC/s 74.668.

**Resultado sonoro:** malo. El usuario reportó la respuesta de CC como
claramente menos precisa que la del SSv2 original. **Desactivado por defecto**
(`SSW_COLLAPSE_CONTROLLER_BURSTS` ahora es 0).

**Por qué falló, que es lo importante:** no es la misma operación que hace el
nativo. La fusión nativa exige que el CC anterior esté en el slot
**inmediatamente previo** de la cola, así que solo colapsa ráfagas realmente
consecutivas y **conserva los pasos intermedios de una rampa**. El pase
implementado escanea hacia adelante por toda la tanda y descarta cualquier valor
superseded, deteniéndose solo en una nota del mismo canal, así que una rampa de
expresión repartida a lo largo de un bloque pierde todos los pasos salvo el
último de cada tramo. Es un colapso a nivel **bloque** haciendo de sustituto de
uno a nivel **adyacencia**, y se oye como automatización escalonada.

**Para quien retome esto:** la dirección es correcta (8x contra 32x es una
mejora real y medida), el mecanismo no. Una versión fiel tendría que reproducir
**adyacencia**, no supersesión: colapsar solo cuando dos CC del mismo canal y
controlador son consecutivos en el flujo de despacho sin ningún otro evento de
ese canal en medio. Eso conserva las rampas y sigue matando el spam de valores
repetidos.

#### Lo que sigue abierto

- **Primera carga de SF2 lenta.** Sin causa identificada. `PRESMP 0/0` en ambas
  cargas descartó el presampleo, que era la única hipótesis viva.
- **Stall con CC densos.** El motor es byte por byte idéntico al original, así
  que la causa está en cómo el port entrega los eventos. El intento de esta
  revisión confirma que ahí está el costo, pero no encontró la forma fiel de
  reducirlo.
- `ALLOC` sigue en ~32x `BUSY` con el colapso apagado: el pool saturado y el
  robo fallido siguen dominando el bloque en material con CC.

---

#### Rev. Estado en la revisión 29 (leer primero)

#### Stall con CC densos: el motor NO es la causa

Se comparó el árbol original contra el port línea por línea. **La fusión de CC en
`enqueue_event()` y el break de VOR son byte por byte idénticos.** Mismo código,
mismas exclusiones (CC 6, 38, 98-101, 120, 121, 123), misma rotura de secuencia
por canal para todo evento que no sea nota.

Si el mismo código recibe el mismo flujo produce el mismo resultado, así que el
stall **se origina antes de `enqueue_event`**: el port le entrega al motor un
flujo distinto del que recibe el original. Esa es la parte que el port cambió —
el original recibe eventos uno por uno en tiempo real vía `SendDirectData`,
mientras el port agenda por lotes y despacha el bloque entero antes de renderizar.

Dos diferencias posibles y distinguibles:

- **Volumen:** que lleguen más eventos CC al motor de los que el archivo
  contiene, por duplicación o falta de filtrado en la ruta parser → worker.
- **Orden:** que el intercalado por timestamp rompa la adyacencia que la fusión
  necesita. Objeción a esta idea: si el original reprodujera el mismo archivo
  vería la misma secuencia, salvo que su player filtre antes de entregar.

**El número que decide:** eventos CC por segundo que entran a `enqueue_event`,
contra los que el archivo tiene por segundo. Iguales → el flujo es el mismo y
hay que buscar en otro lado. Órdenes de magnitud más → ahí está el stall, y
corregirlo es fiel por definición porque devuelve el flujo a lo que el original
recibe. **NO tocar el motor.**

El filtro de redundancia de VOR de la rev. 28 no cambió el cuadro
(`ALLOC 1.930 ms` contra `BUSY 66 ms`, 29x), lo cual confirma lo anticipado en
esa revisión: los CC de este archivo cambian de valor de verdad, no son
redundantes.

#### Primera carga de SF2: sigue sin resolverse

`PRESMP 0/0 skip` en **ambas** cargas, con `REGIONS 11`. Ni resampleadas ni
salteadas significa que el bucle de `sfz_apply_presampling()` no recorrió
ninguna región: o `num_regions` era 0 o `inst` era NULL en el momento de
llamar. Como `REGIONS 11` sí aparece en el panel, las regiones existen después.

**Descartado:** el presampleo no es la diferencia entre primera y segunda carga,
porque da 0/0 en las dos. La hipótesis del `cache_entry` sin poblar tampoco se
sostiene: habría dado `skipped > 0`, no 0/0.

Lo que sí está medido y es la pista firme: con las mismas voces activas,
`BUSY` 996 ms en primera carga contra 316 ms en segunda, y `BLOCK` 32,5 ms
contra 3,5 ms. **El trabajo real de render difiere ~3x con el mismo número de
voces**, así que las voces recorren otro camino de datos.

**Advertencia sobre esos números:** `BUSY` y `BLOCK` no cuadran aritméticamente
entre sí (316 ms sumados dentro de un bloque de 3,5 ms serían 90 workers con 24
configurados), así que uno de los dos no está midiendo el ciclo que se cree.
Verificar la sincronización de los snapshots antes de construir nada encima.

**Siguiente paso sugerido:** instrumentar `ssw_load_sf2()` directamente —
`instrument` nulo o no, y `instrument->num_regions` justo antes de llamar a
`sfz_apply_presampling()`, en ambas cargas. Hay dos call sites de esa función
(uno en `ssw_init_ex`, otro en la carga) y conviene saber cuál corre último,
porque el segundo pisa los contadores del primero.

---

#### Rev. Estado en la revisión 28 (leer primero)

#### La causa real, aislada por el usuario

Dos pruebas suyas delimitaron el problema mejor que toda la instrumentación:

- MIDI de millones de notas por segundo, 8192 voces saturadas, cientos de miles
  de robos por segundo: **no lagea**.
- MIDI con cientos de miles de **CC** por segundo: **lagea**.
- En ese MIDI, `ACTIVE` sube y baja siguiendo la música: **no hay fuga de voces**.

Eso descarta, definitivamente: el DSP, el stealer, las barreras, el bloque de
mezcla desacoplado, el pool de voces y la ruta de note-off. Todo lo que se
persiguió antes eran síntomas de un pool legítimamente saturado.

#### El mecanismo

```c
static inline uint16_t vor_current_event_token(int ch, int key) {
    return (uint16_t)(g_vor_key_generation[ch][key] ^
                      (uint16_t)(g_vor_channel_generation[ch] * 257u));
}
static inline void vor_break_channel_sequence(int ch) { ++g_vor_channel_generation[ch]; }
```

VOR apila note-ons idénticos en una sola voz, y es lo que permite que millones
de notas por segundo quepan en 8192 voces. Dos note-on solo se apilan si
comparten `vor_token`, y ese token mezcla la generación **del canal**.

En `enqueue_event()`, **todo** evento que no sea nota llamaba a
`vor_break_channel_sequence()`. Un solo CC entre dos note-on idénticos les da
tokens distintos y **el apilado no ocurre**. Con CC densos, VOR queda desactivado
de hecho: cada nota repetida se lleva su propia voz, el pool satura, el robo se
dispara y `ALLOC` domina el bloque.

#### El arreglo

Romper es correcto cuando el CC cambia algo: dos notas separadas por un cambio
real de volumen o pan no son idénticas y no deben apilarse. Es inútil cuando el
valor es el que el canal ya tenía.

`vor_event_is_redundant()` en `voice.c` suprime el break solo para CC y pitch
bend que repiten su valor actual. **Fiel por construcción:** si el estado del
canal no cambió, las notas realmente son idénticas y se apilan igual que sin el
evento. Los CC de secuencia (6, 38, 98-101, 120-127) nunca son redundantes,
porque repetirlos es una segunda acción real, no un no-op.

    python3 tools/extract_vor_redundancy_logic.py
    cc -O1 -Wall -o /tmp/vor tools/vor_redundancy_check.c && /tmp/vor

11 grupos, incluidos independencia por canal y por controlador, sustain on/off,
pitch bend y limpieza en reset. El test extrae el código real de `voice.c`, no
una copia.

#### Limitación, y cómo se mide

**Esto solo ayuda si los CC son redundantes.** El test 11 lo deja explícito: una
rampa de 128 valores distintos sigue rompiendo 128 veces. Si el archivo del
usuario tiene CC que cambian de verdad a esa densidad, el apilado se rompe igual
y hace falta otra respuesta — probablemente cuantizar el break a la grilla de
render en vez de por evento.

Si tras esta revisión el MIDI con CC sigue lageando, esa es la conclusión y no
hay que volver a mirar voces, stealer ni DSP.

---

#### Rev. Estado en la revisión 27 (leer primero)

#### El hallazgo firme: el costo está en asignar voces, no en el DSP

Zona densa, 8192 voces: `ALLOC 2.243 ms` contra `BUSY 68 ms`. **33× más tiempo
asignando que renderizando.** Con `FREE 95`, `STEALS/s 21.505` y
`DROPPED 282.666`, los workers pasan el bloque entero sondeando en busca de
víctimas de robo que la guarda de prioridad rechaza.

Esto descarta, con medición: vectorizar el DSP, el bloque de mezcla desacoplado
(7.4), y la hipótesis de voces-por-worker.

**El stealer NO se toca.** Se verificó en el árbol original que `alloc_voice()`
también descarta la nota cuando el robo falla, sin fallback. Agregar uno sería
apartarse del original, y el usuario pidió fidelidad.

#### Error corregido en esta revisión: el contador `FREED` mentía

`FREED 0%` fue un falso positivo. Estaba mal de dos formas:

- **Mal ubicado.** El contador vivía en `free_voice()`, que tiene una sola
  llamada en todo el archivo. La ruta **normal** de reciclado es el barrido
  posterior al render en `publish_worker_done()`, que llama a `free_push()`
  directamente. Las voces sí volvían, por un camino no instrumentado.
- **Mal planteado.** Una voz no se libera cuando llega su note-off, sino cuando
  la envolvente de release termina de caer, uno o varios bloques después, y más
  tarde aún con pedal de sustain. Un porcentaje por ciclo entre note-offs y
  liberaciones no significa nada en ningún bloque aislado.

El indicio que lo delataba estaba en la misma captura: `FREE 95` estable. Si de
verdad no se liberara ninguna voz, `FREE` habría caído a 0 en el primer segundo
y se habría quedado ahí.

**Es el segundo contador mal planteado de la sesión**, después de `GAP`
invertido. Lección repetida: una métrica nueva se valida contra un caso donde se
conozca la respuesta esperada, antes de sacar conclusiones de ella.

#### Reemplazo: `RECYC %`, acumulativo

Ahora cuenta en `free_push()`, o sea en **ambas** rutas de reciclado, y compara
acumulados: voces recicladas contra note-ons que consiguieron voz.

| RECYC | Conclusión |
|---|---|
| Justo bajo 100, estable | Las voces vuelven bien. El retraso es simplemente las voces sonando ahora. El pool no alcanza para el material y el nativo con el mismo cap haría lo mismo: no hay fuga. |
| Bajando sostenidamente | Las voces no vuelven. Ahí sí hay fuga, y el candidato es la coalescencia de eventos con agendado por lotes (7.10). |

**Comprobación más barata antes de instrumentar nada más:** `PK POLY` en el
panel de información del archivo dice cuántas voces simultáneas pide el material
de verdad. Si supera 8192, el pool simplemente no alcanza y no hay nada que
arreglar en el motor.

---

#### Rev. Estado en la revisión 26 (leer primero)

#### El costo NO está en el DSP. Está en asignar voces.

Medido en zona densa, 16 workers, 8192 voces:

| | |
|---|---|
| ALLOC | **1.735 ms** |
| BUSY | **56 ms** |
| BLOCK | 106,9 ms |
| WACT | 16 de 16 |
| FREE | 48 |
| STEALS/s | 21.218 |
| DROPPED | 192.736 |

**31× más tiempo asignando que renderizando.** 1.735 ms repartidos en 16 hilos
son ~108 ms cada uno, que es exactamente el `BLOCK`: los workers pasan el bloque
entero escaneando en busca de víctimas de robo.

Esto invalida tres líneas de trabajo que estuvieron sobre la mesa:

- **Vectorizar el DSP no serviría de nada.** El trabajo no está ahí.
- **El bloque de mezcla desacoplado (7.4) tampoco ataca esto**, porque no es
  costo fijo por llamada.
- La hipótesis de voces-por-worker ya se había caído: el hardware de referencia
  del repo de SSv2 es un 3700X (16 hilos), pero el usuario corre un 5900X (24),
  así que el nativo en su máquina usaría el mismo conteo de workers y las mismas
  voces por worker. La fragmentación no explica la diferencia.

#### El mecanismo del acantilado

`steal_voice_fast()` recorre `wd->active[]` con esta guarda:

```c
if (!low_cap_steal && v->env_state != ENV_RELEASE && !v->note_off_received &&
    (v->owner_channel != ch || v->key != new_note_key) &&
    victim_priority >= incoming_priority) { saltar }
```

En material denso casi todas las voces tienen volumen parecido, así que la
condición se cumple para casi todas y la función devuelve −1. Encima
`probe_cap` cae a 128 cuando la presión pasa el percentil 99. Cada note-on
fallido paga hasta 128 sondeos **para no encontrar nada**: con 21.218 robos por
segundo más 192.736 fallos, son millones de sondeos inútiles por segundo.

**El original hace exactamente lo mismo y también descarta la nota** — no tiene
fallback. Se verificó en `alloc_voice()`/`steal_voice_fast()` del árbol
original. Por lo tanto **agregar un fallback sería apartarse del original, no
corregirlo**, y el usuario pidió fidelidad. NO tocar el stealer.

#### Lo que falta explicar, y el instrumento de esta revisión

Si el stealer, el conteo de workers y las voces por worker son idénticos al
nativo, algo alimenta la presión más rápido acá. Es un lazo: menos throughput →
las voces no se liberan a tiempo → la presión queda sobre el percentil 99 →
`probe_cap` en 128 → el robo falla → más descartes → más presión.

Candidato directo, y es un problema introducido por el port: la coalescencia de
eventos con agendado por lotes (ver 7.10). Si los note-offs se fusionan o se
aplican al inicio del bloque en vez de a su offset, **las voces viven de más**.

**Instrumento agregado:** `FREED %`, voces efectivamente liberadas como
porcentaje de note-offs consumidos en el ciclo. Rojo bajo 90%.

| FREED | Conclusión |
|---|---|
| < 90% sostenido | Los note-offs no liberan voces. La fuga está en esa ruta, el stealer es víctima y no causa. Arreglar ahí y el acantilado desaparece solo. |
| ~100% | Las voces sí vuelven y la presión viene del volumen de notas puro. Entonces el techo es throughput general y hay que volver a §4. |

---

#### Rev. Estado en la revisión 25 (leer primero)

#### El reparto de la cola NO es el problema

`WACT 23` de 24 workers en zona densa. Mi sospecha sobre `pop_chunk` queda
descartada: el pool sí participa.

Pero eso hace que el resto de los números digan algo nuevo:

| | Zona densa, post-recarga |
|---|---|
| ACTIVE | 8.090 |
| WACT | 23 de 24 |
| BUSY | 63 ms |
| BLOCK | 85,2 ms |
| STEALS/s | 29.774 |
| DROPPED | 219.220 |
| FREE | 92 |

23 workers participan pero suman apenas 63 ms de trabajo mientras el bloque
tarda 85,2 ms de reloj. Repartidos en 23 hilos eso serían ~2,7 ms de pared.
**Faltan ~82 ms sin explicar.**

#### Dónde están esos 82 ms

`BUSY` arranca **justo antes del bucle de consumo de la cola de render**, o sea
que **no cuenta la fase de eventos**: drenar las colas de canal, asignar voces y
robar. Con el pool agotado (`FREE 92`) y 29.774 robos por segundo, esa fase hace
un trabajo enorme que es invisible para `BUSY` pero está entero dentro de
`BLOCK`.

**Instrumento agregado en esta revisión:** `ALLOC`, tiempo sumado entre workers
en esa fase previa. Se pone rojo cuando supera a `BUSY`. Lectura:

| ALLOC vs BUSY | Conclusión |
|---|---|
| ALLOC >> BUSY | El costo está en asignación de voces y robo, no en el DSP. Atacar el stealer y el agotamiento del pool, no vectorizar. |
| ALLOC ~ 0 | El tiempo está en las barreras y el trabajo fijo por llamada. Ahí sí entra el bloque de mezcla desacoplado (7.4). |

Si `ALLOC` resulta dominante, notar que **vectorizar el DSP no serviría de
nada**: el trabajo no está ahí. Sería la tercera pista que se descarta midiendo.

#### Sigue abierto

**El bug de primera carga NO está resuelto.** El arreglo de la rev. 23
(`voice_refresh_all_region_caches`) es correcto en sí mismo pero no era la
causa. Reportado por el usuario tras probar la rev. 24. Sospechoso vigente:
`sfz_apply_presampling`. Verificación pendiente: contador de regiones con
`is_resampled` verdadero tras cada carga; si la primera da 0 y la segunda da
todas, confirmado.

---

#### Rev. Estado en la revisión 24 (leer primero)

#### Corrección de un error de interpretación mío

`MISS INTERP 100%` **no es un bug, es el caso esperado.** `no_interp` exige una
relación de velocidad exactamente 1.0, o sea solo la nota en el root key del
sample sin pitch bend. En música real casi ninguna nota cumple eso, así que el
100% es normal. Lo interpreté como patología durante dos revisiones. El arreglo
de la rev. 23 (`voice_refresh_all_region_caches`) es correcto en sí mismo — el
multiplicador de pitch se aplicaba tarde y eso afectaba el sonido — pero **no
era la causa del bug de primera carga**, que sigue abierto.

#### El bug de primera carga, medido

Misma soundfont, mismo material, prácticamente las mismas voces activas:

| | 1ª carga | 2ª carga |
|---|---|---|
| ACTIVE | 5.209 | 5.022 |
| BUSY | **609 ms** | **5 ms** |
| BLOCK | 31 ms | 3,5 ms |
| LOAD | 269% | 31% |

Con el mismo conteo de voces, el trabajo real de los workers difiere ~100×.
Eso no puede ser planificación ni paralelismo: cada voz recorre un camino de
datos distinto. Sospechoso principal:
`sfz_apply_presampling(instrument, g_cfg.sample_rate)`. Si en la primera carga
no se aplica o se aplica con la tasa equivocada, `is_resampled` queda falso y
cada voz resamplea en tiempo de ejecución sobre el sample original; al cargar
otra soundfont la región ya quedó presampleada. **Verificación pendiente:** un
contador de cuántas regiones tienen `is_resampled` verdadero tras cada carga.
Si la primera da 0 y la segunda da todas, confirmado.

#### El paralelismo es malo siempre; el bug de primera carga lo camuflaba

`BUSY` no estaba roto: lee 0 cuando el bloque es barato. Ahora que funciona,
paralelismo efectivo = BUSY / BLOCK.

**Cuidado con qué se compara contra qué.** Entre la captura de primera carga y
la de sesión densa cambian DOS variables a la vez (primera-vs-segunda carga, y
liviano-vs-denso), así que esa comparación no sirve para aislar el paralelismo.
La comparación válida es entre las dos capturas que son ambas post-recarga:

| Escenario | ACTIVE | BUSY | BLOCK | Workers efectivos (de 24) |
|---|---|---|---|---|
| post-recarga, liviano | 5.022 | 5 ms | 3,5 ms | **1,4** |
| post-recarga, denso | 8.182 | 65 ms | 22,8 ms | **2,9** |
| primera carga, liviano | 5.209 | 609 ms | 31 ms | ~20 (ver abajo) |

O sea: **después de recargar, el paralelismo es malo en los dos casos.** El ~20
de la primera carga NO es buen paralelismo, es un artefacto del bug de primera
carga: al encarecer cada voz ~100×, los chunks duran lo suficiente como para que
todos los workers alcancen a tomar uno. Cuando el trabajo por voz vuelve a su
costo real, el reparto no llega a involucrar al pool.

Son dos problemas separados, y el segundo estaba camuflado por el primero:

1. **Primera carga:** ~100× de costo por voz. Sospechoso `sfz_apply_presampling`.
2. **Siempre:** 24 workers rinden como ~3. Este es el techo real de throughput y
   el que bloquea la meta de 8192 voces con 8 workers.

En la sesión densa además `FREE 0`, `STEALS/s 87.793` y `DROPPED 309.544`: el
pool está agotado encima de todo lo anterior.

**Instrumento agregado en esta revisión:** `WACT`, cuántos workers distintos
consumieron al menos un chunk de la cola en el último ciclo. Se pone rojo bajo
la mitad del conteo configurado. Junto a `BUSY` separa las dos lecturas:

| WACT | Lectura |
|---|---|
| ~24 con BUSY bajo | Todos participan pero hay poco trabajo; el costo está en las barreras y el trabajo fijo por llamada. |
| ~3 con BUSY alto | El reparto de la cola no involucra al pool. Mirar `pop_chunk` en `worker_thread`: se dimensiona como `max(base, render_size / g_worker_count)`, así que con chunks grandes los primeros workers en despertar se llevan todo antes de que los demás lleguen. |

---

#### Rev. Qué se hizo en la revisión 23 (leer primero)

**7.9 resuelto: el bug de la primera carga de soundfont.** Era la causa del
`SIMD 0%`, del `MISS INTERP 100%`, y de la aparente variación de rendimiento
entre soundfonts. Los tres eran el mismo bug.

`refresh_region_runtime_cache()` pliega `sample_rate / g_audio.sample_rate`
dentro de `cached_pitch_base_multiplier`, y tenía **una sola llamada en todo
voice.c**, dentro de `voice_init_with_count()`. En la primera soundfont las
regiones se crean *después* de esa llamada, así que nunca reciben la
corrección. Consecuencia: todas las voces reproducen a una relación distinta
de 1.0, `no_interp` es falso para todas, y **el 100% cae al bucle escalar**.
Cargar una segunda soundfont volvía a entrar a init con regiones presentes y lo
arreglaba en silencio: de ahí "el primer SF2 siempre carga mal, luego de cargar
otro mejora", y el 400% de carga en sesiones sin trabajo real.

Arreglo: `voice_refresh_all_region_caches()` en `voice.c`, llamada desde
`ssw_load_sf2()` justo después de `sfz_apply_presampling()`. El multiplicador de
pitch también fija la velocidad de reproducción, así que esto es tanto una
corrección de **sonido** como de rendimiento — antes de esto la primera
soundfont sonaba con el pitch mal.

**Qué mirar en la próxima captura:** con la primera soundfont recién cargada,
`MISS INTERP` debería dejar de ser 100% y `SIMD %` subir. Si eso pasa, la
decisión sobre el resampler vectorizado queda en suspenso hasta volver a medir,
porque la medición que la motivaba estaba contaminada por este bug.

**Sigue abierto y sin explicar:** `BUSY 0ms` con `BLOCK 101.5ms`. El reloj no es
el problema (`QueryPerformanceCounter` da nanosegundos con frecuencia 1e9, y la
aritmética no trunca ni desborda). O el contador tiene un camino no visto, o los
workers realmente no consumen la cola. Quedó a medias un contador de
participación de workers que lo separa: cuenta cuántos workers distintos
tomaron al menos un chunk. Si reporta 24 con BUSY en 0, el timer está mal; si
reporta 1 o 2, el pool no reparte la cola y ese es el bug real.

---

#### Rev. Qué se hizo en la revisión 22 (leer primero)

**La pregunta abierta de §4 quedó respondida: `SIMD 0%`.** Casi ninguna voz
entra al camino vectorizado, y por eso el DSP corre escalar. Medido en
navegador junto con `BUSY 63ms` contra `BLOCK 87.9ms` con 24 workers: si el
render de voces sumara 63 ms entre 24 hilos mientras el bloque tarda 87,9, el
tiempo no está ahí, está en el trabajo fijo y las barreras. Pero la causa raíz
de que el bloque sea tan caro es que nada se vectoriza.

Las guardas del camino SIMD por voz exigen, **todas a la vez**:

```c
vor_fast_ok && f_start == 0 && pending_note_off_samples < 0 &&
channels == 1 && s_ch == 1 && no_interp && env_state == ENV_SUSTAIN &&
!filter_enabled && !loop_active
```

Tres son muy restrictivas en material real, y explican las tres observaciones
del usuario:

- `no_interp` exige velocidad exactamente 1.0, o sea solo la nota en el root
  key sin pitch bend.
- `!loop_active`: la mayoría de los samples sostenidos de un SF2 tienen loop.
  Esto es el **"depende de la soundfont y de los samples, no de los efectos"**.
- `!filter_enabled`: cualquier CC que active el filtro echa a todas las voces
  del camino de golpe. Esto es el **"algunos CC provocan lag"**.

**Instrumentación agregada:** contador `MISS <razón> <pct>` que atribuye cada
voz que falla a la primera guarda que la bloquea, en el orden en que se
evalúan, así las cuentas particionan en vez de solaparse. Razones: `INTERP`,
`LOOP`, `FILTER`, `OTHER`. Va por los cinco archivos de la ruta de §5 y en el
`Flow`.

**Por qué importa la razón y no solo el número:** cada bloqueo necesita un
kernel distinto. Interpolación pide un resampler vectorizado; el loop pide que
el wrap entre en el bucle vectorial; el filtro pide un biquad vectorizado. La
próxima captura dice cuál escribir primero en vez de adivinar.

**Pendiente nuevo, reportado por el usuario y sin investigar:** el rendimiento
es malo al arrancar **hasta cambiar de soundfont al menos una vez**. Eso
sugiere que la ruta de primera carga deja algo en un estado distinto del que
deja una recarga (layout de samples, preprocesado de regiones, o algún caché).
Es un bug concreto y aparte del tema SIMD. Ver 7.9.

**Revisión de la revisión 21, hecha y aprobada.** Verifiqué manifiesto 41/41,
compilación de C, `node --check`, llaves de QML, y corrí los seis tests de
host: todos pasan. Confirmé además que `tools/voice_rebalance_logic.inc` se
regenera idéntico desde `voice.c`, o sea que el test usa el código real y no
una copia que se pueda desincronizar. El rebalance corre en el tope del ciclo
del worker, donde la pila es owner-only, así que `free_pop()` ahí es seguro.

Riesgo anotado sobre esa revisión: con 0 eventos pendientes `keep = 0` y el
worker devuelve **todas** sus voces libres de a una. `REBAL/s` es el número que
lo delata; si sale alto y `BLOCK` no mejora, conviene acotar el `give` por
ciclo. En la captura actual `REBAL/s` es 0 porque el pool está agotado
(`FREE 79`), así que el dato todavía no se pudo leer.

---

---

## 15. Apéndice C: secciones base de la revisión 37 anterior (sin editar)

Las secciones 1-11 de este documento reescriben con otras palabras las
secciones base que había antes. Este apéndice conserva las originales tal cual,
para garantizar que ningún dato se perdió en la reformulación. **Ante
contradicción, mandan las secciones 1-11.**

### Anterior §1. Qué es el proyecto

WASMIDI es un reproductor de Black MIDI que corre en el navegador, desplegado en
`https://midi.dekx.cc` (GitHub Pages). Dos mitades:

- **Síntesis de audio.** Un port a WebAssembly de **SnappySynthV2**, un motor de
  voces escrito en C para Windows. El original vive en
  `third_party/snappysynthv2/` y se compila con Emscripten. Usa pthreads reales
  (Web Workers) para repartir el render de voces.
- **Visualización.** Un piano roll **horizontal** en OpenGL ES 3 / WebGL2
  (`src/renderer/gl_renderer.cpp`), dentro de una app Qt6 compilada también a
  WASM. El tiempo corre en el eje X y el pitch en el eje Y.

El objetivo declarado: **igualar el rendimiento del SnappySynthV2 nativo lo más
posible, sin alterar su sonido**, y **sin alterar la estética del renderizador
visual**. Esas dos restricciones mandan sobre cualquier optimización. Una mejora
que cambie el sonido o el dibujo no sirve.

Meta concreta pendiente: **8192 voces estables con 8 workers** (1024 por worker).
Hoy se estrangula desde ~4096.

#### Proyectos de referencia

El usuario provee dos árboles de código como referencia. **No son dependencias,
son fuentes de ideas.** Cualquier técnica portable a WASM vale la pena intentarla.

- **SnappySynthV2 original** (nativo, Windows). Referencia de *sonido* y de
  *rendimiento*. Si el port suena distinto al original, el port está mal.
- **BPFA** (Better Piano From Above, fork de PianoFromAbove). Referencia de
  *arquitectura de rendimiento*. Ya usa SnappySynthV2 como motor, así que sus
  decisiones son directamente comparables.

---

### Anterior §2. Restricciones del entorno de trabajo (crítico)

**En el entorno donde se edita este código NO hay Qt6 ni emsdk.** Esto define qué
se puede afirmar y qué no:

- `src/renderer/gl_renderer.cpp`, `src/mainwindow.cpp`, `src/mainwindow.hpp` y
  todo `src/qml/` **no se pueden compilar ni una sola vez**.
- El build de Emscripten completo tampoco.
- Todo cambio en esos archivos llega al usuario **sin una sola verificación**.

Esto ya costó recompilaciones perdidas. La regla que salió de ahí: **cambios de
motor se verifican con tests; cambios de GUI se verifican con una captura del
usuario.** Ir en tandas chicas y pedir captura después de cada una.

#### Lo que sí se puede verificar

```
# C del motor (compila standalone fuera de Emscripten)
cd third_party/snappysynthv2
gcc -fsyntax-only -I. -DSNAPPYSYNTH_WASM=1 Voice/voice.c
gcc -fsyntax-only -I. -DSNAPPYSYNTH_WASM=1 snappy_wasm_core.c

# JavaScript
node --check web/snappysynth-worker.js
node --check web/snappysynth_bridge.js

# Tests de host, sin toolchain
python3 tools/extract_ssw_schedule_logic.py
cc -O1 -Wall -o /tmp/h tools/ssw_schedule_harness.c -lm && /tmp/h
cc -O1 -o /tmp/s tools/worker_freelist_share_check.c && /tmp/s
python3 tools/extract_voice_rebalance_logic.py
cc -O1 -Wall -pthread -o /tmp/b tools/worker_freelist_borrow_check.c && /tmp/b
node tools/pump_slice_sim.mjs
c++ -O2 -std=c++17 -Isrc/renderer -o /tmp/c \
    tools/note_raster_compositor_check.cpp \
    src/renderer/note_raster_compositor.cpp && /tmp/c

# QML: solo balance de llaves. No es compilación.
```

En Windows con MinGW, `snappy_wasm_core.c` necesita
`-include ../../tools/host_posix_env_shim.h` (declara `setenv`/`unsetenv`, que
esa libc no tiene). Emscripten y glibc no lo necesitan. Solo declaraciones.

**Todos estos checks corren en GitHub Actions** (paso "Host tests") antes del
build de Emscripten. Nada se compila localmente: el flujo es editar, subir,
y leer el log del workflow.

`MANIFEST.sha256` cubre 41 archivos y **hay que regenerarlo después de cada
cambio** con `python3 tools/regen_manifest.py` (la lista de archivos vive ahí;
agregar los nuevos). CI lo verifica con `--check` y con `sha256sum -c`, así que
un manifiesto viejo rompe el build en vez de pasar desapercibido.

---

### Anterior §3. Errores ya cometidos. No repetirlos.

Cada uno costó una recompilación. También están en `PORTING_STATUS.md` y
`third_party/snappysynthv2/UPSTREAM_NOTES.md`.

**3.1 — Quitar la frontera de render para bank select / RPN / program change.**
Parece seguro: "la cola de eventos se drena en orden de envío". Es falso entre
clases de evento. `enqueue_event()` enruta notas con `g_note_worker_map[ch][key]`
(hash por tecla) y eventos de canal con `g_channel_worker_map[ch]`. Con más de un
worker están en **colas distintas** consumidas concurrentemente en un mismo ciclo
de render. Sin la frontera, los note-on resuelven contra banco/programa
equivocado, el selector de regiones no encuentra nada, y **la nota se cae sin
sonar y sin error**. Se ve como robo agresivo de voces en material no denso y no
lo es. Rollback: `SSW_FORCE_SELECTOR_BOUNDARIES=1`.

**3.2 — Fragmentar el render por cada evento de estado.** Un bloque de 512 frames
se convertía en ~100 llamadas a `voice_render_float()`. El costo de esa llamada
está dominado por trabajo fijo: dos barreras de futex, rebuild de la render
queue, setup por voz de *todas* las voces activas, y pérdida de los caminos SIMD.

**3.3 — Punto fijo en el mapeo de columnas del culler visual.** Redondea distinto
que el renderizador y descarta notas que sí ocupaban un píxel. Debe usar el mismo
`floor`/`ceil` en `double`, en el mismo orden de operaciones. Comentado en el
código fuente.

**3.4 — La métrica `GAP` invertida.** Medía el tiempo entre invocaciones de
`pump()`. Con poca carga el ring se llena, el pump duerme, y el gap era enorme
*porque todo estaba bien*. Se encendía en rojo justo cuando no pasaba nada.
Corregido a `LATE` = `gap − cobertura del ring`, con piso en cero. **Lección:
validar toda métrica nueva contra un caso donde se conozca la respuesta.**

**3.5 — Agregar elementos a un `RowLayout` lleno.** No hace wrap: al desbordar
empujó fuera de pantalla los controles de SnappySynth. La telemetría vive ahora
en un `Flow`, que reflota.

**3.6 — Comparar dos MIDIs distintos como si fueran el mismo workload** y sacar
conclusiones sobre el conteo de workers. Inválido. Siempre fijar el archivo.

---

### Anterior §4. Estado del diagnóstico de rendimiento

Última medición (navegador, rev. 21): `LOAD 494%`, `BLOCK 56.2ms`, `EVT 0.1ms`,
`STEALS/s 2220`, `FREE 40`, `RING 26%`, `LATE 45ms`, `UNDERRUNS 841`,
16 workers, 8192 voces, bloque 512, bufs 48. Confirma que el rebalance de
freelist no era una mejora de throughput: arregla admisión justa, pero el DSP
sigue tardando casi cinco veces el presupuesto real. `SIMD %` y `BUSY ms`
quedaron fuera de la captura por el ancho del panel; ahora están en la primera
fila siempre visible. Son los próximos números obligatorios antes de optimizar.

#### Descartado con medición. No volver a perseguirlo.

- **No es cadencia del pump.** Resuelto: underruns 4.770 → 82.
- **No es el pool de voces por sí solo.** Con el pool sano igual da 475%.
- **No es el despacho de eventos.** `EVT` es 0,3 ms de 55,6 ms, o sea 0,5%. Fue
  una hipótesis explícita y **resultó falsa**.

#### La cuenta abierta

8.058 voces × 512 frames ÷ 55,3 ms = **74,6 M voice-samples/s** en total. El
nativo sostiene ~361 M/s en 16 hilos, o sea **22,6 M por hilo**. Con 24 workers
se obtiene el equivalente a **poco más de 3 hilos nativos**. O casi nada se
vectoriza, o los workers no se solapan.

#### Los dos números que lo resuelven (ya instrumentados)

Visibles en el panel de SnappySynth:

- **`SIMD %`** — porción de voces que toma el camino vectorizado. **Medido: 0%.**
  Ver §0 para las guardas exactas y qué las bloquea.
- **`MISS <razón> <pct>`** — cuál guarda rechaza a la mayoría. Es el número que
  decide qué kernel escribir.
- **`BUSY ms`** — tiempo ocupado sumado entre workers en el último ciclo. Con 24
  workers y `BLOCK 55ms`, paralelismo real daría del orden de 1.300 ms sumados.
  Si da ~55 ms, están serializados.

| SIMD | BUSY | Conclusión |
|------|------|-----------|
| bajo | alto | DSP en escalar. Relajar condiciones de entrada o agrupar voces compatibles antes de renderizar. |
| alto | bajo | Los workers no se solapan. Mirar la barrera y el reparto de la render queue. |
| bajo | bajo | Ambos. Empezar por SIMD. |
| alto | alto | El DSP es genuinamente así de caro en WASM. Revisar la meta de 8192 voces. |

#### Nueva captura: corrección de la interpretación anterior

Captura recibida: `LOAD 509%`, `BLOCK 58ms`, `EVT 0.1ms`, `SIMD 0%`,
`BUSY 873ms`, 16 workers, 8192 voces activas, 512 frames, 44100 Hz,
`STEALS/s 3120`, `DROPPED 0`, `RING 2%`, `LATE 48ms`, `UNDERRUNS 802`.

**La tabla anterior no basta para elegir una optimización.** La inspección de
`Voice/voice.c` confirma que `g_path_fast_voices` sólo cuenta admisiones al batch
estéreo sustain; `g_path_scalar_voices` se incrementa antes de los otros caminos
SIMD por voz (incluidos mono → estéreo). Por eso 0% NO prueba DSP completamente
escalar. El panel ahora dice `BATCH` para describir el contador real, conservando
los nombres internos/API por compatibilidad.

El batch WASM exige sustain, muestra estéreo, pitch casi unitario, sin loops,
sin filtros ni transiciones pendientes. No se deben quitar esas condiciones sin
implementar sus semánticas. No sabemos cuáles dominan en este SoundFont/captura.

`873 / 58 ≈ 15` es consistente con workers solapados, no con serialización total.
Son tiempos de pared: no prueban utilización CPU efectiva ni scaling ideal.
Siguiente paso: perfilar los caminos fuera del batch (envolvente, interpolación,
loops y filtros), o instrumentar sus causas de rechazo y cobertura SIMD real.
No hay todavía una mejora de throughput validada en navegador.

---

### Anterior §5. Arquitectura, por si hay que tocarla

#### Cadena de audio

```
midi-parser-worker.js  (parsea el MIDI, agenda eventos con lookahead)
        | scheduleBatch
snappysynth-worker.js  (Worker; hospeda el modulo WASM del synth)
        |  pump() -> ssw_render_queued_into() -> voice_render_float()
        |                                        \-> N worker pthreads
        | ring SharedArrayBuffer
snappysynth-audio-worklet.js  (AudioWorklet, consume en quantums de 128)
        |
snappysynth_bridge.js  (estado compartido, leido por Qt via EM_JS)
```

**Detalle que importa:** `pump()` corre en **el mismo hilo** que recibe
`scheduleBatch`. Un render largo y sincrónico deja sin atender la entrega de
eventos. BPFA no tiene este problema porque renderiza en un hilo dedicado con un
ring en medio. Esto bloquea portar el bloque de mezcla grande de BPFA (ver 7.4).

#### Telemetría

Los números viajan en el payload de stats, **no por consola** — imprimir desde un
worker a esa frecuencia le cuesta tiempo real al hilo principal, y fue un
problema reportado por el usuario.

Ruta: `snappy_wasm_core.c` (exports `ssw_*`) → `snappysynth-worker.js` →
`snappysynth_bridge.js` (`state.*`) → `mainwindow.cpp` (`EM_JS`) →
`mainwindow.hpp` (`Q_PROPERTY`) → `Controls.qml`.

Para agregar un contador hay que tocar los cinco. El patrón a copiar es
`synthSteals`.

Campos actuales: `LOAD %`, `BLOCK ms`, `EVT ms`, `SIMD %`, `BUSY ms`, `RING %`,
`LATE ms`, `REBAL/s`, `DROPPED`, más `WORKERS`, `VOICES/WKR`, `STEALS/s`,
`UNDERRUNS`.

### Anterior §8. Cómo trabajar con el usuario

- Prueba en **Brave, en Windows, en un Ryzen 9 5900X (24 hilos)**. Brave
  sub-reporta `hardwareConcurrency` como 7.
- Manda capturas del panel y de la consola. Esa es la única fuente de verdad
  sobre el navegador.
- El flujo que funciona: **instrumentar antes que arreglar.** Cuando se adivinó,
  se falló (3.1, 3.4, 3.6, y la hipótesis del despacho de eventos). Cuando se
  midió, se resolvió en una iteración.
- Los archivos se editan en el árbol del usuario y se suben a GitHub tal cual;
  el build corre en Actions. Hacer los cambios completos directamente en los
  archivos, no entregar diffs ni parches para aplicar a mano.
- Con cada update, actualizar este documento: qué se hizo, qué sigue, qué falta a
  largo plazo, y qué necesita saber quien continúe.

---

## 15. Revisión 38 — materialización de SF y costo agregado por el port

Esta revisión es el primer pase de arreglo sobre el estado descrito en §5. No se
reclama mejora de navegador hasta recibir una A/B del usuario; Qt6 y emsdk no
están disponibles en el entorno de edición.

### 15.1 Soundfont: no entrar a realtime con trabajo pendiente

Se corrigió la interpretación de `PRESMP`: `resampled == 0` no implica que no se
hayan recorrido regiones. La UI ahora expone `seen`. Además se corrigió el caso
en que un fallo de reserva durante `resample_wav_data()` quedaba cacheado como si
hubiera producido datos a la tasa destino.

`ssw_load_sf2()` presamplea y valida la capa entrante antes de mezclarla;
`ssw_init_ex()` valida también el instrumento existente al reinicializar el audio.
Una región sólo pasa como materializada si tiene PCM/cache válidos y, cuando la
tasa difiere, un buffer resampleado real. Fallar esa comprobación devuelve error
al worker en lugar de publicar `SF2 ready`.

### 15.2 Frontera de eventos

`ssw_queue_events()` reutiliza slabs en un pool acotado en lugar de reservar y
liberar un bloque por mensaje. Para timestamps no negativos, la conversión a frame
usa `x + 0.5` + conversión entera, la misma regla de `llround()` en ese dominio.
Ambas optimizaciones se revierten juntas con:

```text
-DSSW_REV38_EVENT_HOTPATH=0
```

No cambian el orden de eventos, la frontera de selector ni el timestamp resultante.

### 15.3 Telemetría que estaba dentro del hot path

La instrumentación había reintroducido trabajo global por voz incluso con Debug
apagado. `g_notes_started_total` se incrementaba por cada voz admitida y el
reciclaje se contaba en `free_push()`, donde además se mezclaban refill/rebalance
con liberaciones reales. Los contadores de cobertura SIMD tenían el mismo patrón.

Ahora `worker_thread` toma `g_debug_metrics` una sola vez por ciclo. Los atómicos
de diagnóstico sólo se ejecutan cuando Debug está activo, y `RECYC` cuenta la
retirada real de `ENV_OFF` en el barrido post-render. Al prender Debug se reinicia
el baseline de lifecycle para que el porcentaje siga siendo interpretable.

### 15.4 Archivos tocados

- `third_party/snappysynthv2/snappy_wasm_core.c`
- `third_party/snappysynthv2/Parser/sfz_parser.c`
- `third_party/snappysynthv2/Voice/voice.c`
- `web/snappysynth-worker.js`
- `src/mainwindow.cpp`
- `src/mainwindow.hpp`
- `src/qml/Controls.qml`
- `tools/ssw_schedule_harness.c`
- `HANDOFF.md`
- `MANIFEST.sha256` (regenerar al cerrar la revisión)

### 15.5 Validación local de rev. 38

Disponible y ejecutada: sintaxis C de `voice.c`, `snappy_wasm_core.c` y
`sfz_parser.c`; sintaxis Node de worker/bridge; harness de scheduler; harness de
rebalance; harness de redundancia VOR; freelist share; simulación del pump.

No disponible aquí: build Qt6, build Emscripten completo, ejecución en Brave ni
prueba audible. La prueba del usuario debe comprobar primero `PRESMP seen ==
REGIONS`, `skip == 0` y después comparar primera vs segunda reproducción del mismo
MIDI/SF con Debug apagado. Para el caso CC, comparar `ALLOC`, `BLOCK`, underruns y
respuesta audible con exactamente el mismo MIDI/configuración.



---

## 16. Revisión 38.1 — diagnóstico reproducible del manifest en CI

GitHub Actions reportó `MANIFEST.sha256 is stale` después de que todos los tests
de host pasaran. El ZIP de rev. 38, ejecutando localmente el bloque exacto
`Host tests (engine logic, no Emscripten)` del workflow, sí pasa
`regen_manifest.py --check` y `sha256sum -c`. Eso demuestra que el fallo aparece
cuando el árbol que llega al commit no coincide byte por byte con el drop
empaquetado (por ejemplo, un archivo subido selectivamente o una versión previa
del workflow).

`tools/regen_manifest.py --check` ahora imprime cada archivo cuyo SHA difiere,
mostrando hash registrado y hash actual, además de entradas faltantes, extra o
malformadas. No se cambió código de runtime, audio, MIDI, renderer, VOR ni
stealing en esta revisión. `MANIFEST.sha256` se regeneró después de este cambio.


---

## 17. Revisión 39 — hot path del stealer bajo saturación CC

La prueba real de rev. 38 confirmó dos cosas distintas. Primero, la preparación de
la SF2 ya recorre las 11 regiones (`PRESMP seen 11`) sin skips en idle. Segundo,
el caso CC sigue siendo el cuello principal: con 24 workers y 8192 voces la
captura mostró `ACTIVE 8112`, `FREE 80`, `ALLOC 1974 ms`, `BUSY 59 ms`,
`STEALS/s 18285`, `DROPPED 310290`, `BLOCK 89.3 ms`, `LOAD 751%`, `RING 6%` y
`LATE 77 ms`. Es decir, el tiempo sigue concentrado en la fase de admisión/robo,
no en el DSP.

### 17.1 Workers=0 en navegador

Se encontró un bug independiente: en algunas configuraciones Chromium/Brave el
`GetSystemInfo()` emulado dentro del módulo devolvía un procesador, por lo que
`Workers=0` terminaba creando un único worker (`WORKERS 1`, `VOICES/WKR 8192`).
El worker JS ahora resuelve Auto en el límite del navegador usando
`navigator.hardwareConcurrency` y entrega ese valor explícitamente a
`ssw_init_ex()`. Un valor manual continúa pasando sin recortarse, porque Brave
puede farblear el conteo reportado.

### 17.2 Adyacencia de CC en O(n) por lote

La reproducción browser-side del colapso nativo de CC por adyacencia buscaba el
siguiente evento de canal haciendo un scan hacia delante por cada controlador.
Con corridas largas de notas entre CC eso revisita repetidamente los mismos
eventos. Ahora, al admitir el slab, un pase inverso construye el índice del
siguiente evento no-nota de cada canal. La consulta durante render queda O(1).

La relación es idéntica a la anterior: ignora note-on/off (van a otra cola), no
cruza límites de slab, no cruza el final del bloque y sólo absorbe cuando el
siguiente evento de ese canal es el mismo CC. Se verificó además por comparación
aleatoria contra el algoritmo anterior; no aparecieron diferencias de decisión.

### 17.3 Espejo compacto de campos estáticos del stealer

`steal_voice_fast()` es byte por byte equivalente al SSv2 nativo en rev. 38, pero
cada intento fallido puede sondear 128--512 voces. En Shared-WASM, leer para cada
probe `key`, `voice_age` y el volumen de prioridad desde un `voice` grande obliga
a tocar líneas de memoria dispersas aunque muchas candidatas se descarten antes
de necesitar el resto del estado.

Rev. 39 añade un arreglo compacto por voz con exactamente esos campos:
`priority_volume`, `voice_age`, `key` y la clase de prioridad derivada. Se llena al
admitir una voz y se actualiza en todas las rutas VOR que modifican
`original_volume`. El loop de probes usa ese espejo para los guards tempranos y
sigue leyendo del `voice` para estado dinámico, loudness, envelope y la decisión
final.

No cambia el orden del scan, `probe_cap`, cursor, scores, protección de bass,
prioridades, guards, víctima elegida ni qué note-on se descarta. Si el arreglo no
se puede reservar, el accessor cae automáticamente a los campos originales. Se
puede compilar sin el camino nuevo con `-DSSW_STEAL_STATIC_CACHE=0`.

### 17.4 Validación disponible

Pasan sintaxis C de `voice.c`, `snappy_wasm_core.c` y `sfz_parser.c`; sintaxis
Node de worker/bridge; scheduler 14/14; freelist rebalance; VOR redundancy;
freelist share y simulación del pump. Falta, como siempre en este entorno, el build
Qt/Emscripten completo y la prueba real en Brave.

La próxima comparación debe usar 24 workers explícitos primero para aislar el
cambio del stealer. En el mismo punto CC de la captura anterior comparar
`ALLOC`, `BLOCK`, `STEALS/s`, `DROPPED`, `RING`, `LATE` y respuesta audible. La
expectativa de esta revisión no es cambiar `STEALS/s`/`DROPPED` por política, sino
bajar el costo temporal de llegar a exactamente las mismas decisiones.

---

## 18. Estado al cierre de la sesión — rev. 39 / CC hot path pendiente

### 18.1 Qué quedó implementado

La revisión 39 es el último árbol de código disponible al cierre de esta sesión.
No se debe confundir el resultado del CI del workflow con un fallo del runtime:
el problema anterior de `MANIFEST.sha256` se debió a que la subida selectiva omitió
`.github/workflows/build-wasm.yml`; el workflow del repositorio quedó desalineado
con el árbol empaquetado. El usuario confirmó que ya corrigió esa subida.

Cambios funcionales acumulados desde rev. 37:

- La preparación de SF2 espera a que las regiones requeridas estén materializadas
  antes de publicar `SF2 ready`; el warmup limpia sus propios `CC120` antes de
  devolver el control al playback.
- `PRESMP` ahora permite distinguir regiones vistas, resampleadas y omitidas.
- El camino de eventos reutiliza slabs y evita `malloc/free` por lote, además de
  evitar trabajo innecesario de redondeo en la conversión timestamp->frame.
- La telemetría Debug fue sacada del hot path cuando está desactivada; `RECYC`
  cuenta retirada real de voces `ENV_OFF` y no movimientos internos de freelist.
- `Workers=0`/Auto se resuelve en el worker JS usando el límite del navegador
  (`navigator.hardwareConcurrency`) para no caer accidentalmente a un solo worker.
- La detección de adyacencia de CC se cambió de scans hacia delante repetidos a
  un pase inverso O(n) por slab, conservando la decisión anterior.
- Rev. 39 añadió un espejo compacto de campos usados en los guards tempranos del
  stealer (`priority_volume`, `voice_age`, `key` y clase de prioridad). El orden
  del scan, `probe_cap`, cursor, scores, guards y víctima elegida permanecen sin
  cambios; existe fallback al acceso original y el camino puede desactivarse con
  `SSW_STEAL_STATIC_CACHE=0`.

### 18.2 Validación real aportada por el usuario

**SF2 recién cargado / primera reproducción:**

- `PRESMP 11 seen / 0 resmp / 0 skip`.
- En la captura: `ACTIVE 4864`, `FREE 3328`, `ALLOC 25 ms`, `BUSY 537 ms`,
  `BLOCK 30.1 ms`, `LOAD 260%`, `STEALS/s 0`, `DROPPED 0`, `RING 6%`,
  `LATE 18 ms`, `UNDERRUNS 88`, con 24 workers efectivos.
- Esto indica que la preparación de las 11 regiones y el contador de presampleo
  funcionan, pero la primera reproducción todavía tiene un costo alto en `BUSY`.
  No se verificó aún si una segunda reproducción del mismo MIDI reduce ese costo.

**Zona con mucho CC por delante:**

- Con 24 workers y 8192 voces la reproducción sigue saturando el pool.
- Última captura: `ACTIVE 8085`, `FREE 107`, `ALLOC 2,387 ms`, `BUSY 635 ms`,
  `LOAD 829%`, `BLOCK 30.1 ms`, `STEALS/s 45,558`, `DROPPED 259,130`,
  `RING 6%`, `LATE 93 ms`, `UNDERRUNS 884`, `REBAL/s 1,521`.
- *Nota rev. 40:* ese `BLOCK 30.1 ms` es idéntico al de la captura de primera
  carga y no cuadra con `LOAD 829%` (con el presupuesto de ~11.6 ms que implica la
  otra captura serían ~96 ms). Probable error de transcripción; confirmar contra la
  captura original antes de usarlo.
- Por tanto, rev. 39 **no resolvió el cuello de botella CC**. El espejo compacto
  del stealer y la optimización O(n) de adyacencia no redujeron suficientemente
  el costo de admisión/robo bajo esta carga.
- El síntoma sigue siendo `ALLOC >> BUSY`, con el pool casi completamente lleno.

### 18.3 Qué se quería hacer y quedó sin terminar

La siguiente investigación prevista era seguir bajando el costo del hot path de
admisión sin cambiar la semántica musical del SSv2 nativo. En particular:

1. Perfilar `alloc_voice()`/`steal_voice_fast()` a nivel de suboperación para
   separar tiempo de selección de víctima, sincronización, acceso al `active[]`,
   CAS/free-list y actualización de estado.
2. Verificar si el gran `ALLOC` viene realmente del scan del stealer o de la
   fragmentación/contención de las freelists por worker. La métrica `REBAL/s`
   alta en la última captura hace que esta segunda hipótesis siga abierta.
3. Revisar la ruta de note admission después de que los CC hayan roto VOR, sin
   cambiar `probe_cap`, orden de víctimas, scores, protección de bass ni reglas
   de drop hasta tener evidencia de que una modificación es semánticamente
   equivalente.
4. Medir si el `REBAL`/borrow de voces libres puede hacerse más barato. No se
   debe asumir que más rebalance siempre ayuda: hay que medir `ALLOC`, `REBAL/s`,
   `STEALS/s`, `DROPPED` y tiempo de bloque juntos.
5. Para la primera carga, reproducir el mismo MIDI una segunda vez sin recargar
   SF2 y comparar `BUSY`, `BLOCK` y `LOAD`. Si la segunda pasada cae mucho, localizar
   el cache/materialización que todavía ocurre dentro de `voice_render_float()` y
   mover únicamente esa preparación a la fase de carga.

### 18.4 Qué NO se debe hacer sin una prueba de equivalencia

No cambiar todavía:

- la política de VOR;
- `steal_voice_fast()` para reducir arbitrariamente el número de probes;
- `probe_cap`, cursor, scores o prioridad de víctimas;
- protección de bass;
- orden temporal de eventos;
- reglas de CC que cambien el significado musical;
- DSP/render del `voice.c` solo para mejorar el benchmark.

La razón es que el objetivo sigue siendo fidelidad de sonido/comportamiento con el
SSv2 nativo, no simplemente reducir `ALLOC` o `UNDERRUNS` a costa de cambiar qué
voz sobrevive.

### 18.5 Próximo punto exacto de continuación

Continuar desde **rev. 39**, no desde rev. 37 ni desde una rama intermedia.
Primero perfilar la ruta de admisión/rebalance con el MIDI CC que produjo la última
captura. Mantener **24 workers explícitos** durante la comparación para aislar el
problema de Auto. Luego repetir el mismo caso con Auto una vez validado el hot path.

Última revisión entregada al usuario antes de este handoff:
`wasmidi-main-rev39-cc-steal-hotpath.zip`.

---

## 19. Revisión 40 — hilos de síntesis atrapados en código sin optimizar

### 19.1 Cómo se llegó

El usuario pidió no agregar telemetría y encontrar en el código qué hace que el
rendimiento mejore al **recargar**, aun sin haber reproducido una nota ni cargado
un MIDI. Se compararon los dos caminos:

- **Primera carga:** `initCore()` al arrancar la página (`ssw_init_ex()` sin
  instrumento, crea los 24 hilos) → `loadSoundfont` → `ssw_load_sf2()` →
  presampleo + validación → `voice_refresh_all_region_caches()` →
  `voice_prewarm_note_caches()` → `ssw_warmup()`.
- **Recarga** (`reinitializeCore()` o `ssw_clear_soundfonts()`): `ssw_init_ex()`
  con instrumento → `voice_shutdown()` → `voice_init_with_count()` →
  presampleo + validación → refresh → prewarm.

En datos de C terminan iguales: lo único de `voice_init_with_count()` que lee el
instrumento es `refresh_region_runtime_cache()`, que `ssw_load_sf2()` ya repite, y
`rebuild_channel_render_cache()` depende solo del estado de canal. **La única
diferencia real es que la recarga destruye y recrea los hilos de síntesis.**

### 19.2 El mecanismo

1. `worker_thread()` era un `for (;;)` que se ejecuta una sola vez por hilo y
   nunca retorna. Con `-O3 -flto` todo el hot path (eventos, admisión, robo,
   render) queda inlineado ahí: es la `wasm-function[47]` con 70% de self time
   del perfilado de rev. 37.
2. V8 compila cada función wasm primero con Liftoff (rápido de compilar, código
   sin optimizar) y la recompila con TurboFan cuando se calienta.
3. **V8 no tiene OSR para WebAssembly:** el código optimizado se usa en la
   *próxima llamada* a la función. Una función que nunca retorna no la recibe.
4. Los hilos creados al abrir la página entran a `worker_thread` cuando solo
   existe Liftoff y se quedan ahí para siempre. Los hilos recreados por una
   recarga entran cuando TurboFan ya existe (por uso previo o por la caché de
   código de V8 entre visitas).

Explica: lentitud de primera carga, que recargar la cure sin tocar notas, que
dependa del "estado de arranque" y que haya sido intermitente (la caché de código
de V8 se invalida con cada build nuevo). **No explica** por sí solo "mejora al
repetir el MIDI" (repetir no recrea hilos) ni "cambiar de soundfont lo
reintroduce". Esas dos observaciones venían de sesiones con varias variables
cambiando a la vez (§7, error 6); re-observarlas con rev. 40.

### 19.3 El cambio

`third_party/snappysynthv2/Voice/voice.c`: el cuerpo del `for (;;)` pasó **tal
cual** a `static int worker_cycle(int widx, worker_data *wd)`. `worker_thread()`
queda como un bucle que la llama una vez por bloque. Solo se mapeó el control de
flujo del nivel del bucle externo, identificado por el compilador (gcc marca los
`continue`/`break` que quedan fuera de un bucle) y revisado a mano en las ramas
que gcc no compila:

| Línea original (rev. 39) | Antes | Ahora |
|---|---|---|
| chequeo de `g_quit_flag` al inicio del ciclo | `break;` | `return SSW_WORKER_CYCLE_QUIT;` |
| worker sin voces activas, tras publicar | `continue;` | `return SSW_WORKER_CYCLE_CONTINUE;` |
| `#ifdef GPUMIX`, timeout de consumers | `continue;` | `return SSW_WORKER_CYCLE_CONTINUE;` |
| salidas por quit dentro de bucles internos | `return 0;` | sin cambio (`0 == QUIT`) |

Los `continue` dentro de bucles internos (incluido el de la rama
`__wasm_simd128__` del batch estéreo, que gcc no compila) quedan igual. El
`static __thread int s_last_active` sigue siendo uno por hilo. Los `goto` y sus
etiquetas quedaron todos dentro de `worker_cycle`.

La llamada es **indirecta, por un puntero `volatile`** (`g_worker_cycle_fn`). Con
una llamada directa, LLVM LTO o el inliner de Binaryen (`wasm-opt -O3` inlinea
funciones con un solo llamador sin límite de tamaño) podían volver a meterla en
`worker_thread` y deshacer el arreglo. Costo: una llamada indirecta por bloque
por worker.

- Default: activo en Emscripten, apagado en nativo.
- Rollback: `-DSSW_WORKER_CYCLE_TRAMPOLINE=0` (llamada directa, forma anterior).
- No cambia orden de eventos, admisión, VOR, stealer, `probe_cap`, protección de
  bass, DSP ni telemetría. Cero contadores nuevos.

### 19.4 Observación sobre `ALLOC` (documentada, sin tocar código)

El cronómetro de `ALLOC` va de después de `g_start_events` hasta el inicio del
consumo de la cola de render, y en medio está `WaitForSingleObject(
g_render_producers_ready, 500)`. Por eso incluye la espera al worker más lento: con
desbalance, `ALLOC` sumado se acerca a N × el camino crítico. La relación
"`ALLOC` 20-40× `BUSY`" puede estar inflada por eso. Además, **todas las capturas
del Problema B se tomaron probablemente con hilos del arranque**, o sea sobre
código Liftoff. Hay que repetirlas con rev. 40 antes de seguir optimizando
admisión o robo. Por pedido del usuario no se agregó telemetría para separarlo.

### 19.5 Validación

Ejecutado en este entorno:

- Sintaxis C de `voice.c` en variantes: default, `GPUMIX`, `VOICEDEBUG`, `-mavx2`,
  `-mavx512f -mavx512bw`, `-UVOR`. `snappy_wasm_core.c` y `sfz_parser.c` limpios.
- Diff sin indentación contra rev. 39: solo cambian las 3 líneas de la tabla más
  la envoltura de la función.
- **Nuevo** `tools/worker_cycle_smoke.c`: enlaza el motor real en nativo, crea 24
  workers, renderiza 200 bloques, destruye/recrea el pool 3 veces y limpia. Pasa
  con el trampolín activo y con rollback. Se verificó que detecta el error: con el
  quit mal mapeado, el test se cuelga (el join del shim POSIX ignora el timeout),
  por eso se corre con `timeout`.
- Harnesses de host de siempre: scheduler, rebalance, VOR, freelist share, pump.

No disponible: build Emscripten, Brave, prueba audible.

### 19.6 Prueba del usuario (sin telemetría)

1. **Confirmar la causa, con el build rev. 39:** cerrar Brave por completo y
   abrirlo con `brave.exe --js-flags="--no-liftoff"` (todo compilado optimizado
   desde el inicio). Si la primera carga ya rinde como tras una recarga, la causa
   queda confirmada. Volver a abrir Brave normal después.
2. **Validar rev. 40, Brave normal:** página nueva, cargar SF2, reproducir el mismo
   MIDI. La primera reproducción debería acercarse a la de después de una recarga
   en los primeros segundos (V8 necesita que la función se caliente y compilarla).
3. **Repetir la captura de la zona con CC** con 24 workers explícitos, y compararla
   con una tomada después de una recarga. Si ahora coinciden, las capturas previas
   del Problema B estaban medidas sobre código sin optimizar.

### 19.7 Estado y continuación

- **Hecho:** separación de `worker_cycle()`; smoke test; correcciones en §4, §5,
  §8 y §18.2.
- **Próximo:** esperar el resultado de §19.6. Si se confirma, re-evaluar el
  Problema B con capturas nuevas antes de tocar admisión/robo (§18.3 sigue válido
  como lista, pero sus números base cambian).
- **Si no mejora:** revertir con el define y buscar otra diferencia entre hilos
  del arranque e hilos recreados (orden de reserva de memoria del pool de voces,
  afinidad de pthreads del pool de Emscripten).
- **Largo plazo:** sin cambios respecto de §11 y §12.c, más:
- **Pendiente: limpieza de comentarios del código.** Recortar los comentarios que
  repiten el handoff (p. ej. el bloque de `worker_cycle()` en `voice.c` y la
  cabecera de `tools/worker_cycle_smoke.c`) a una línea con qué hace y el define de
  rollback. Regla del usuario: el razonamiento va en el handoff, no en el código.
- **Para quien continúe (GPT o Claude):** continuar desde rev. 40. No volver a
  meter el cuerpo del ciclo dentro de `worker_thread` ni convertir la llamada en
  directa: el objetivo es que la función del hot path **retorne** en cada bloque.
  La misma regla aplica a cualquier bucle infinito nuevo en un hilo wasm.

### 19.8 Resultado de la prueba del usuario

- **Primera carga: resuelta.** Todo funcionó bien desde la primera reproducción,
  sin recargar.
- **Zona con CC: sigue con bastante lag.** El Problema B no dependía (o no solo)
  del código sin optimizar. Falta una captura del panel con rev. 40 en el mismo
  punto CC y 24 workers explícitos para tener números base nuevos: las capturas de
  §17 y §18.2 se tomaron con hilos del arranque y no sirven como referencia.

Próximo trabajo: Problema B, partiendo de esa captura nueva y leyendo el código de
admisión/robo antes de agregar instrumentación (preferencia del usuario).

Revisión anterior entregada: `wasmidi-main-rev40-worker-cycle.zip`.

---

## 20. Revisión 40.1 — el botón Debug rompía la configuración del synth

**Síntoma (usuario, rev. 40):** todo suena bien hasta prender Debug; desde ahí
suena mal, y sigue mal al apagarlo hasta reiniciar el programa.

**Causa:** `setDebugMetrics()` del bridge mandaba `{type: "configure",
debugMetrics}`. El handler de `configure` en `snappysynth-worker.js` lee todos los
campos sin valores por defecto del estado actual, así que un mensaje con solo
`debugMetrics` dejaba `maxVoices` 1, `minVoices` 0, `blockFrames` 1,
`numBuffers` 1, workers Auto, `noteSharding` 0, y después llamaba
`reinitializeCore()` y mandaba `engineConfig` al AudioWorklet con esos valores.
Apagar Debug repetía lo mismo; solo un reinicio volvía a mandar la configuración
completa. El comentario del bridge decía que ya no pasaba por `configure()`, pero
el tipo de mensaje seguía siendo `configure`.

**Arreglo:**

- `web/snappysynth_bridge.js`: el toggle manda `{type: "debugMetrics", enabled}`.
- `web/snappysynth-worker.js`: rama nueva para `debugMetrics` que solo llama
  `ssw_set_debug_metrics()` y retorna, sin tocar configuración ni reinicializar.
  Además acepta un `configure` sin `maxVoices` pero con `debugMetrics` como toggle,
  para el caso de un bridge viejo en caché. Los dos `configure` reales del bridge
  siempre incluyen `maxVoices`, así que no entran en esa rama.

**Validación:** `node --check` de worker y bridge; manifest. Sin prueba en
navegador desde acá.

**Consecuencia para las mediciones:** cualquier captura tomada después de prender
Debug en caliente, con este bug presente, no describe el motor configurado. Las
capturas del Problema B con rev. 40 deben tomarse con rev. 40.1.

**Próximo:** el usuario prueba rev. 40.1 (Debug on/off en reproducción, debe seguir
sonando igual) y manda la captura de la zona CC con 24 workers explícitos.

Última revisión entregada: `wasmidi-main-rev40.1-debug-toggle.zip`.

### 20.1 Captura del usuario con rev. 40.1 (zona CC, Debug on)

Debug ya no altera la configuración (8192 voces se mantienen). **Corrección del
usuario:** esta captura es de **otro hardware** (no el 5900X; de ahí `WORKERS 16`) y
de **otra sesión/parte del MIDI**. No es comparable con las capturas de §17/§18.2.

`ACTIVE 8,144`, `FREE 48`, `ALLOC 928 ms`, `BUSY 54 ms`, `WACT 13`, `LOAD 564%`,
`BLOCK 50.4 ms`, `STEALS/s 64,930`, `DROPPED 705,083` (acumulado), `RING 6%`,
`LATE 39 ms`, `REBAL/s 0`, `UNDERRUNS 4,043` (acumulado), `CCOL 1,747,328`,
`CC#7 66%`, `MISS INTERP 100%`, `PRESMP 11/0/0`. Gráficos: NPS ~3,75M, CC/s ~111k.

Lectura:

- `BUSY` 54 ms: ~3,4 ms por worker por bloque para 8,1k voces; dentro de esta
  captura el render es una fracción menor del bloque. (No comparar con los 635 ms de
  rev. 39: otro hardware y otra parte del MIDI.)
- `ALLOC` / 16 ≈ 58 ms ≈ `BLOCK`: cada worker pasa todo el bloque en la fase previa
  o esperando en la barrera (§19.4). El bloque lo fija el worker más lento en
  admisión/robo. Todo el exceso sobre el presupuesto (11,6 ms) está ahí.
- Pool a >99% (`FREE 48`), así que `probe_cap` = 128 por intento de robo; cada note-on
  que termina en `DROPPED` pagó el escaneo completo sin resultado.
- Análisis independiente de esa captura (sin comparar con otras):
  - Demanda: NPS ~3,75M → ~43,5k note-ons por bloque de 512 frames (11,6 ms), más
    sus note-offs. Bloque real 50,4 ms ≈ 4,3× el presupuesto.
  - Descartados por la propia captura: DSP (`BUSY` 54 ms sumado), manejo de CC (solo
    11 CC llegaron a los workers en la última llamada; `CCOL` absorbe el resto),
    preparación de SF (`PRESMP 11/0/0`).
  - `RECYC 3%`: casi ninguna voz termina su release; se reutilizan por robo. Voces
    iniciadas ≈ `STEALS/s` / 0,97 ≈ 67k/s → ~98% de los note-ons no toman voz nueva
    (se apilan por VOR o se descartan).
  - **El stealer no alcanza a explicar el tiempo:** ~750 robos por bloque (más los
    drops), cada uno ≤128 probes con el pool >99%, son del orden de 10^5 probes por
    bloque repartidos en 16 workers: décimas de ms por worker, no 50 ms.
  - Con 8192 voces y 16 workers el sharding es por hash, que acá equivale a
    `worker = key % 16`: reparto razonable salvo que la sección use pocas teclas.
  - Si el reparto es parejo, son ~2,7k note-ons (+ note-offs) por worker por bloque
    en ~50 ms: varios µs por evento, demasiado para el camino de apilado VOR.
    **Dónde buscar:** el trabajo por evento del camino que toma el ~98% de las notas
    (selección de región, `vor_find_overlap_voice`, emparejado de note-off y colas
    por tecla), no el stealer.
- **Dato del usuario (corregido):** el SSv2 **nativo reproduce todo bien, incluso con
  más voces**. Es la versión **WASM** la que no corre bien esta sección en ninguno de
  los hardwares probados. El costo extra es propio del port y, según esta captura,
  está en la fase previa al render de los workers (no en DSP, CC, stealer ni
  segmentación: `ALLOC`/16 ≈ `BLOCK` y `BUSY` de un render completo indican un solo
  segmento por bloque). Próximo paso: diff de la fase previa (`worker_cycle` hasta la
  barrera de producers) contra el `voice.c` nativo; cada diferencia es candidata.

---

## 21. Comparación con el `voice.c` nativo (sin cambios de código)

El usuario aportó el `voice.c` nativo de SSv2 (6.183 líneas; el `voice.c_` adjunto
es una versión anterior de 2.084 líneas y no se usó). Se compararon función por
función, normalizando espacios y comentarios: 136 funciones idénticas, 10 distintas.

**Fase previa de los workers (`worker_thread` nativo vs `worker_cycle`) — única
diferencia además de telemetría y del mapeo de control de flujo de rev. 40:**

- el port llama `rebalance_worker_freelist(wd)` cada ciclo (devuelve al pool global
  lo que exceda los eventos pendientes); el nativo no rebalancea;
- relleno de freelist: el nativo apunta a 128 libres por ciclo y
  `refill_worker_freelist` pide 512; el port apunta a la cantidad de eventos
  pendientes y limita el refill a eso + 1;
- `++wd->drops_local` al descartar;
- `update_voice_steal_static_meta` (espejo de rev. 39) al admitir y en
  `vor_merge_voice*`.

**Otras diferencias:** `enqueue_event` (filtro de redundancia VOR, solo evita
breaks), `steal_voice_fast` (espejo de rev. 39; `steal_protect_release_tail` se
refactorizó a `_with_priority` y es equivalente), `voice_render_float`
(sincronización de fin de ciclo propia de wasm), `setup_workers`,
`voice_init_with_count`, `voice_shutdown`.

**Conclusión (inferencia de lectura, NO medida):** el trabajo por evento de la fase
previa es el mismo código que el nativo y las diferencias del port en esa fase
parecen baratas. Que "el mismo trabajo corre más lento en el navegador" es una
hipótesis: nunca se ejecutó el núcleo del port fuera del navegador con una SF2 y un
MIDI reales. Lo único ejecutado en nativo es `tools/worker_cycle_smoke.c` (Linux,
sin SF2, sin notas que suenen: solo control de hilos). Además, el build no-navegador
del app Qt no tiene sintetizador ni carga de archivos (todo está bajo
`#ifdef __EMSCRIPTEN__` / `if(EMSCRIPTEN)`).

**Próximo paso propuesto:** banco de prueba en host (sin telemetría en el producto):
compilar el motor del port en nativo, como `tools/worker_cycle_smoke.c`, y
alimentarlo con un flujo sintético con la densidad de la captura de §20.1 (~3,75M
note-ons/s con duplicados VOR, ~111k CC/s, 8192 voces, 16 workers). Si en nativo
el bloque entra en presupuesto, el problema es del entorno wasm; si no, se perfila
con `perf` en host para nombrar la función.

---

## 22. Banco de prueba en host y lectura de la captura de §20.1

`tools/host_bench/`: `make_sf2.py` genera una SF2 de 11 regiones a 44,1 kHz (como la
de la captura: sin resampleo) y `bench.c` alimenta al núcleo del port por la misma
API que el worker del navegador (`ssw_queue_events` + `ssw_render_queued_into`),
bloques de 512, 8192 voces. Flujo sintético: 172 ticks/s, 16 canales, K teclas por
canal y tick, D pistas duplicadas (VOR), CC7 por pista, notas de 20 ms. Con los
valores por defecto (K=24, D=56) da ~3,7M NPS y reproduce los números de la
captura: `STEALS/s` ~62k (captura: ~65k), pool lleno, drops altos.

Resultados en el contenedor (1 núcleo, CPU desconocida, build nativo -O3):

| Config | CPU por bloque | Fase previa | Render |
|---|---|---|---|
| 1 worker | ~39 ms | ~17 ms | ~20 ms |
| 16 workers (en 1 núcleo) | ~31-32 ms total | ~0,5 ms por worker, parejo | ~22 ms total |

- En nativo la fase previa se reparte **pareja** entre los 16 workers (~0,5 ms cada
  uno). El trabajo total es ~30 ms de CPU por bloque: en una máquina de 8+ hilos
  entraría holgado en los 11,6 ms si se paraleliza.
- En el navegador (§20.1) el render sumado fue 54 ms (≈2,5× el nativo de acá, razonable
  para wasm SIMD128 sin FTZ), pero `ALLOC` por worker ≈ el bloque entero. Con la fase
  previa pareja y chica, eso indica que **algún worker tarda ~50 ms en llegar a la
  barrera o la barrera tarda en liberarse**: no es trabajo repartido.
- En este flujo sintético los CC no cambian nada (misma cifra con y sin CC), porque
  todas las pistas mandan el mismo valor y el filtro de redundancia/colapso los
  absorbe. El MIDI real debe tener CC que sí rompen el apilado VOR; hace falta el
  archivo real para reproducirlo.

**Hipótesis principal para el navegador: sobresuscripción de hilos.** El diseño
sincroniza todos los workers en barreras en cada bloque. En el navegador los workers
compiten con el hilo de UI (piano roll WebGL a millones de NPS), el parser de MIDI, el
caché visual, el AudioWorklet y el hilo que despacha eventos. Con Workers = hilos
lógicos, basta que el SO desaloje un worker para que los demás esperen en la barrera.
El SSv2 nativo corre sin esa competencia. **Prueba sin código:** misma máquina y
sección, Workers explícitos 4 / 8 / 12 contra Auto; si el lag baja con menos workers,
es esto.

**Otros costos propios de wasm, por orden de peso esperado:** render escalar
(`MISS INTERP 100%`, ~2/3 del CPU total en host; un kernel de interpolación con
SIMD128 sobre 4 voces/muestras es la mayor ganancia de CPU disponible), sin FTZ/DAZ
(denormales en filtros/colas), cadena de despertares de futex en las barreras
(`SetEvent` re-señalado por cada worker), y el costo por evento de admisión (~200 ns
nativo por evento, ~85k eventos por bloque en esta densidad; cada nota duplicada se
procesa por separado porque las pistas duplicadas no quedan adyacentes en la cola).

---

## 23. Revisión 41 — Hypernova real en host: el cuello son dos teclas

**Material de prueba del usuario:** `Hypernova.mid` (498 MB, 26 pistas, 960 TPQN) +
`MatheryLSD.sf2` (11 regiones). No van en el repo. El `.7z` usa LZMA2 sin filtros;
sin `7z` se extrae con `lzma` de Python (formato RAW, `dict_size = 32 MiB`, datos
desde el byte 32; CRC verificado `0xeb442912`).

**Herramienta:** `tools/host_bench/midibench.c` (fusiona pistas en streaming con mapa
de tempo, precarga CC/programas antes de `start`, renderiza desde `start-2`). Uso:
`./build.sh && DBG=1 ./midibench MatheryLSD.sf2 hypernova.mid <workers> 101.5 1.5 [wav]`.

**Diagnóstico (build nativo, contenedor de 1 núcleo, zona 101,5-103 s, ~2,9M NPS):**

- Las teclas **0 y 127** llevan ~7% de las notas cada una (y 1/126, 2/125...), 61% en
  el canal 15. Con 16 workers el reparto `worker = tecla % 16` manda la tecla 0 al
  worker 0 y la 127 al worker 15.
- CPU de la fase previa por worker y bloque (rev. 40): **~31 y ~33 ms** en esos dos,
  **<1 ms** en los otros 14. Eso explica la captura de §20.1: todos esperan en la
  barrera al worker de una tecla caliente (×~2,5 en wasm ≈ el bloque entero).
- Dentro de esos workers, con el pool saturado (~650k-960k drops/s): el emparejado de
  note-off (~65%) y el escaneo del stealer (~30%).
- Causa del note-off: `keyqueue_append_voice` agrega por la **cabeza** (más nuevo), el
  sondeo rápido recorre desde la **cola** (más viejo) 128 voces que en teclas calientes
  son todas colas de release, falla, y cae al recorrido completo de la lista de esa
  tecla (~400 voces, casi todas liberadas). ~24k recorridos completos por bloque.
- Más voces empeoran: con 32768 voces las listas por tecla son más largas.

**Cambio (rev. 41), `Voice/voice.c`, `SSW_NOTEOFF_OPEN_LIST` (default 1):** lista
intrusiva por (canal, tecla) solo con voces que un note-off todavía puede emparejar
(se agregan al admitir, se podan perezosamente al encontrarlas liberadas, muertas,
fuera de la cola o reasignadas). El recorrido completo de respaldo del note-off
recorre esa lista en vez de la cola completa; aplica el mismo predicado y los mismos
efectos por voz. Arreglos compactos aparte del struct `voice`.

- **Audio bit-idéntico con 1 worker** (WAV comparado byte a byte contra rev. 40 en la
  zona densa). Con 16 workers el motor ya era no determinista entre corridas (dos
  corridas de rev. 40 difieren tanto como rev. 40 vs rev. 41), así que la prueba de
  equivalencia es la de 1 worker: el cambio es lógica por tecla.
- Worker caliente: **~31-33 ms → ~12-14 ms** por bloque. CPU total con 16 workers:
  150 → 95 ms por bloque.

**Experimento, `SSW_NOTEOFF_FIFO` (default 0):** reemplaza también el sondeo desde la
cola por "la voz abierta más vieja con orden ≤ corte" sobre la lista abierta. Worker
caliente ~8 ms. **Cambia decisiones** (el original, al fallar el sondeo de 128, libera
todas las voces que califican; esta variante solo la más vieja): la diferencia de audio
es clara en la zona densa. Se entregaron WAVs A/B de 96-105 s (`..._rev41.wav` y
`..._fifo_experimental.wav`, 1 worker) para juzgar de oído.

**Qué falta en el worker caliente (~12 ms nativos):** el sondeo desde la cola del
note-off (128 probes por note-off) y `steal_voice_fast`. Hacerlos exactamente más
baratos es difícil porque el sondeo cuenta entradas obsoletas de la cola principal y
sus desenlaces dependen de ese conteo. Las ganancias grandes que quedan cambian
decisiones bajo saturación y hay que aprobarlas de oído.

Validación: sintaxis C en variantes (default, GPUMIX, FIFO=1, OPEN_LIST=0, AVX2),
smoke de workers, harnesses de host, `node --check`.

Revisión anterior entregada: `wasmidi-main-rev41-noteoff-openlist.zip` (tiene el bug de §24).

---

## 24. Revisión 41.1 — regresión de rev. 41: bloques de 2 s en zonas livianas

**Captura del usuario (rev. 41, zona liviana):** NPS ~143k, polifonía 126, pero
`ACTIVE 8,179`, `FREE 13`, `BLOCK 2,000.9 ms`, `LATE 1,897 ms`, `LOAD 17,205%`,
`ALLOC 6,510 ms`, `BUSY 55 ms`. `BLOCK` ≈ 2000 ms es el timeout con que el hilo
principal espera a los workers: un worker no termina su ciclo. Las voces no se
liberan porque ese worker deja de procesar note-offs, por eso el pool se llena con
poca música.

**Causa:** la lista abierta de rev. 41 era intrusiva (enlaces `next/prev` por voz y
cabeza/cola por tecla). `open_append()` desenganchaba la voz de su lista anterior. Una
voz que vuelve al pool global puede ser readmitida por **otro** worker, que entonces
modificaba la lista de una tecla ajena mientras el dueño la recorría: lista corrupta,
ciclo, bucle infinito. Con 1 núcleo (contenedor) no aparece.

**Arreglo (`Voice/voice.c`):** cada (canal, tecla) tiene un arreglo propio de
entradas `{vid, gen}` que solo toca el worker dueño de esa tecla (el que admite y
procesa sus note-offs). `g_voice_gen[vid]` se incrementa en cada admisión; una entrada
cuyo `gen` no coincide es de una admisión anterior y se descarta sin tocar nada ajeno.
`keyqueue_clear_corrupt` solo incrementa un contador atómico por tecla; el dueño vacía
su arreglo al verlo cambiado. La compactación al llenarse conserva las entradas que el
recorrido original todavía tendría que ver (incluidas las voces en `ENV_OFF`, cuyo
desenganche de la cola principal forma parte del comportamiento).

**Validación:** audio **bit-idéntico a rev. 40 con 1 worker** en la zona densa; 16
workers: 82 ms de CPU total por bloque en la zona densa (rev. 40: ~150); zona liviana
20-30 s igual que rev. 40 (~4 ms por bloque). No se pudo reproducir el cuelgue de
rev. 41 en el contenedor, así que la corrección se apoya en el diseño (un solo
escritor por arreglo), no en una prueba. Falta la prueba en navegador.

Revisión anterior entregada: `wasmidi-main-rev41.1-openlist-owner.zip`.

---

## 25. Revisión 42 — lentitud durante la precarga: competencia de hilos

**Reporte del usuario (rev. 41.1):** el rendimiento mejoró claramente, pero sigue
ralentizándose en las mismas partes, y **ocurre mientras la app hace la precarga**.

**Lectura del código:** mientras reproduce, el worker del parser (`midi-parser-worker.js`)
arma en segundo plano un horizonte de 64 pantallas de páginas visuales y el barrido del
renderer; el `visual-cache-worker` y el hilo de UI también trabajan. Todo eso compite
por núcleos con los workers del synth. Con Workers en Auto = `hardwareConcurrency`, el
motor tiene un worker por hilo lógico, así que durante la precarga hay más hilos
ocupados que núcleos. El motor sincroniza todos sus workers en barreras en cada bloque:
basta que el sistema desaloje uno (en Windows, un quantum es ~15 ms) para que el bloque
entero espere. Cuando termina la precarga, esos hilos quedan libres y deja de pasar.
Además, los lotes de eventos del synth salen del mismo hilo del parser que arma las
páginas visuales; una página es un trabajo que no se interrumpe, así que un lote puede
esperar a que termine (el código ya prioriza el synth entre páginas, no dentro de una).

**Cambio:** Auto ahora deja hilos libres: `workers = reportados − 4` (−2 con menos de 8,
nada con menos de 4). Un valor explícito sigue ganando sin cambios. En la zona densa
esto no cuesta: el camino crítico es el worker de la tecla caliente (§23) y el render
es barato. Tooltip de Workers actualizado.

**Pruebas pedidas al usuario:**
1. Misma parte con Auto (rev. 42) durante la precarga.
2. Dejar que la precarga termine, volver antes de la parte y reproducir. Si ya no se
   ralentiza, el problema restante es de planificación entre hilos, no del motor.

Lo que queda del motor en la zona más densa (101-104 s): el worker de la tecla caliente
hace ~12 ms nativos por bloque (§24), que en wasm siguen superando los 11,6 ms.

Revisión anterior entregada: `wasmidi-main-rev42-auto-reserve.zip`.

---

## 26. Revisión 43 — el synth tiene su propia fuente de MIDI

**Pedido del usuario:** audio y visualización deben ir cada uno a su propio ritmo.
Hasta rev. 42 los lotes de eventos del synth salían del worker del parser, el mismo
hilo que arma las 64 pantallas de precarga y el barrido del renderer.

**Diseño:**

- `web/synth-feeder-worker.js` (nuevo): worker dedicado que recibe el `File` del MIDI,
  lo lee entero (`FileReaderSync`) y hace una pasada de indexado (tempo, selectores,
  SysEx y checkpoints por pista cada 1024 eventos). Luego fusiona pistas en streaming
  con un heap (tick, pista, orden en la pista). Habla con el worker del synth por un
  `MessagePort` directo, sin pasar por el hilo principal.
- La clase `SmfSynthSource` replica exactamente `buildEventBatch` del store mapeado y
  el post-proceso de `midi-parser-worker.js`: note-on vel 0 → `0x80` con d2=64,
  running status (lo cancelan F0-F7 y FF), agrupado de note-ons idénticos
  consecutivos en la misma tick hasta 256 (bits 24-31), piso de velocidad, SysEx con
  byte de estado y `safeExclusive`, `safeUntil`, mapa de tempo (orden de pista,
  duplicados por tick → gana el último, 0 → tempo previo), y en el seek: SysEx
  histórica + estado de banco/programa/bend posterior al último reset GM/GS/XG.
- Worker del synth: al recibir `play(reset)` o `seek` con `local: true`, resetea el
  alimentador por tiempo (mismo `floor(secondsToTick)`) y pide lotes por su cuenta
  (cobertura 0,75-1,5 s por delante). Mientras está en modo local ignora
  `schedule`/`scheduleBatch`/`sysexBatch` del camino viejo. Generación por reset para
  descartar lotes viejos.
- Bridge: crea el alimentador al cargar el MIDI, le entrega el puerto al worker del
  synth cuando está listo y activa el modo local recién en el siguiente play/seek con
  reset (nunca a mitad de reproducción). `state.localSynthReady/Active`.
- `mainwindow.cpp`: entrega el `File` al bridge al instalar el MIDI y lo limpia al
  cerrarlo; `wasmidi_mapped_synth_reset/pump` no le piden nada al parser si el modo
  local está activo; el piso de velocidad se avisa antes de cada play/seek con reset
  (`wasmidi_snappy_local_floor`).
- **`?synthsource=parser` en la URL** vuelve al camino anterior: sirve para comparar
  en la misma zona si el lag viene del visualizador.
- CI: `node --check`, copia a `site/` y `test -s` del worker nuevo. Manifest: 42.

**Verificación (`tools/feeder_check/`):** `ref.cpp` compila el store mapeado real en
nativo y produce el flujo del synth (post-proceso incluido) en forma expandida;
`jsref.mjs` hace lo mismo con `SmfSynthSource`; `proto.mjs` maneja el archivo real del
worker por `MessagePort` con resets y una generación vieja que debe ignorarse.
Resultados: **byte a byte idénticos** en Hypernova (0 s, 33,3 s, 77,7 s con piso 40,
101,5 s, 128 s) y en un MIDI sintético con resets GM/GS/XG, SysEx F0/F7, tempos en la
misma tick, running status, aftertouch y seek más allá del final. Por el protocolo real
también iguales (comparando mensajes y SysEx por separado, porque los cortes de lote
difieren). `./check.sh` repite la parte sintética.

**Costo:** Hypernova 498 MB: carga ~1,4 s; en el segundo más denso, ~0,5 s de CPU por
segundo de audio, en su propio hilo. Memoria: el archivo entero (~500 MB) vive en el
alimentador además del store del parser.

**Diferencias conocidas con el camino viejo:** el agrupado de notas idénticas se corta
en otros puntos porque los lotes tienen otro tamaño (el synth recibe las mismas notas;
solo cambia cómo se reparten las pilas entre lotes).

**No probado:** nada del lado del navegador (el worker del synth y el bridge no se
pueden ejecutar acá). Pruebas pedidas: reproducir Hypernova normal y con
`?synthsource=parser` en la misma zona; seek en pausa y en reproducción; cambiar el
piso de velocidad durante la reproducción; cargar otro MIDI.

Revisión anterior entregada: `wasmidi-main-rev43-synth-feeder.zip`.

---

## 27. Revisión 43.1 — resultado del A/B: el lag no viene del visualizador

**Prueba del usuario (rev. 43, Hypernova):**

- Con el alimentador propio (default de rev. 43): **peor** que las builds anteriores.
- Con `?synthsource=parser` (camino viejo): mejor, pero **el mismo lag en la misma
  parte**.

**Conclusión:** separar la entrega de eventos del hilo de precarga no elimina el lag,
así que el cuello en esa parte no es la competencia con el visualizador por ese hilo:
es el cómputo del motor (el worker de la tecla caliente, §23-24). Que el alimentador
empeore es coherente con que suma un hilo ocupado (~0,5 núcleo en lo más denso) y
~500 MB de RAM sin quitar trabajo del camino crítico.

**Cambio:** el camino del parser vuelve a ser el default. El alimentador queda como
opción con `?synthsource=feeder` (el código y la verificación de §26 se conservan).

**Próximo:** volver al motor. Lo que queda en el worker caliente (~12 ms nativos por
bloque en 101,5-103 s, ~2,5× en wasm) es el sondeo desde la cola del note-off y el
stealer bajo saturación. Las reducciones grandes cambian decisiones de voz y
requieren aprobación de oído (§23, WAVs A/B).

Revisión anterior entregada: `wasmidi-main-rev43.1-parser-default.zip`.

---

## 28. Revisión 44 — note-off FIFO activado por defecto

**Decisión del usuario:** activar `SSW_NOTEOFF_FIFO` (§23). Ahora es `1` por defecto;
`-DSSW_NOTEOFF_FIFO=0` vuelve al emparejado exacto de rev. 41.1.

**Qué cambia:** cada note-off libera la voz abierta más vieja de esa tecla con orden
≤ corte, buscándola en la lista de voces abiertas del worker dueño. El original sondea
128 voces desde la cola de la lista completa y, si no encuentra, libera **todas** las
que califican; en teclas saturadas eso último pasaba casi siempre. En pasajes no
saturados el resultado suele coincidir; en los saturados cambia qué voces se liberan.

**Verificación:** el audio de esta build con 1 worker es bit-idéntico, en la parte
común, al WAV `hypernova_96-105s_fifo_experimental.wav` entregado en rev. 41 (hecho
entonces con la lista enlazada), así que ese WAV sigue siendo la referencia de oído.

**Costo del worker caliente (nativo, contenedor, 101,5-103 s):**

| Workers | Rev. 40 | Rev. 41.1 (exacto) | Rev. 44 (FIFO) |
|---|---|---|---|
| 16 | ~31-33 ms | ~12-14 ms | ~10 ms |
| 12 (Auto de rev. 42 en 16 hilos) | — | — | ~6,4 ms |

Con 12 workers cada uno tiene más voces propias y satura menos. En wasm (~2,5×) el
worker caliente queda cerca de ~16 ms con 12 workers: todavía por encima de los
11,6 ms, pero mucho más cerca que al principio (~80 ms).

**Próximo:** el stealer en el worker caliente, y medir en el navegador con Auto.

Revisión anterior entregada: `wasmidi-main-rev44-noteoff-fifo.zip`.

---

## 29. Revisión 45 — límite de tiempo de render al 100%

**Pedido del usuario:** que SnappySynth tenga un límite de 100% de tiempo de render,
para que cuando se atrasa no arrastre todo lo demás (la transporte visual sigue el
reloj del PCM entregado, así que un synth lento frena también el piano roll).

**Diseño (estilo "CPU limit" de BASSMIDI, pero cortando admisión):**

- `snappy_wasm_core.c`: después de cada bloque compara el tiempo real que tardó
  `ssw_render_queued_into` con `g_render_limit_percent` (default **100**) del tiempo
  que dura el bloque. Si se pasa, fija/reduce un presupuesto de admisión por ciclo
  (empieza en 50% del bloque, ×0,75 por bloque lento, mínimo 200 µs). Si el bloque
  tarda <70% del objetivo, lo sube (+12,5% + 50 µs) y al pasar 4× el bloque lo quita
  (0 = sin límite). `ssw_set_render_limit_percent(0)` lo desactiva.
- `Voice/voice.c` (`worker_cycle`): cada worker mide desde el inicio de su ciclo; cada
  16 note-ons que no se apilaron por la vía rápida de VOR, mira el reloj; pasado el
  presupuesto, los note-ons restantes de ese ciclo se descartan (cuentan en `DROPPED`
  y en `ssw_limited_notes()`). Los contadores de orden por tecla se incrementan igual
  que en un drop normal, así que los note-offs siguen emparejando bien. Note-offs, CC y
  apilados VOR no se limitan.
- Es el camino crítico real: el bloque espera al worker de la tecla caliente, y lo que
  ese worker gasta es admisión.

**Validación (host):**
- Zona liviana (20-24 s, 1 worker): audio **bit-idéntico** a rev. 44; el límite no
  cortó ninguna nota (el presupuesto puede activarse brevemente sin llegar a cortar).
- Zona densa (100-105 s, 16 workers en 1 núcleo): 55,9 → 37,7 ms por bloque. No llega a
  11,6 ms acá porque con un solo núcleo el render de ~8k voces ya pasa el bloque; en el
  navegador el render se reparte entre núcleos y la admisión del worker caliente es lo
  que manda, que es justo lo que se corta.

**Límites conocidos:** si el render mismo (no la admisión) supera el bloque, cortar
admisión no alcanza; el siguiente paso sería matar voces en release (más cambio de
sonido). El límite no es configurable desde la UI todavía (default fijo 100%).

Revisión anterior entregada: `wasmidi-main-rev45-render-limit.zip`.

---

## 30. Revisión 46 — el límite de 100% descarta primero las notas más suaves

**Reporte del usuario (rev. 45):** mejora mucho todo, pero cuando el límite corta se
nota mucho. Pidió que corte empezando por las notas de menor velocity.

**Por qué rev. 45 sonaba mal:** cortaba por tiempo dentro de cada ciclo: pasado el
presupuesto, todas las notas restantes de ese ciclo se perdían sin importar su
volumen, y siempre las del final del bloque.

**Dato de Hypernova (100-105 s):** el 99,1% de los note-ons tiene velocity ≤ 16. Un
piso de velocity bajo alcanza para sacar casi toda la carga y conservar lo que se oye.

**Cambio:**

- `Voice/voice.c`: `g_admit_vel_floor` (0 = apagado). En `worker_cycle`, justo después
  de incrementar el contador de orden de la tecla (para que los note-offs sigan
  emparejando), un note-on con velocity de entrada menor al piso se descarta (cuenta en
  `DROPPED` y `ssw_limited_notes()`). Es una comparación entera: casi no cuesta.
- `snappy_wasm_core.c`: el controlador ahora usa un promedio móvil del tiempo por bloque
  (EMA 0,25, para ignorar picos aislados que el ring absorbe). Sobre el 100%: sube el
  piso de 1 a 4 por bloque según cuánto se pasa. Entre 85% y 100%: lo mantiene. Bajo
  85%: lo baja de a 1 por bloque. Solo con el piso en 128 (ya no entra ninguna nota
  nueva) vuelve al corte por tiempo de rev. 45 como último recurso. `ssw_reset()`
  (seek/play desde cero) reinicia piso, presupuesto y promedio.
- `ssw_admit_vel_floor()` exportado para depuración (no está en el panel).

**Validación (host, 1 núcleo — el peor caso posible, con 16 workers compartiendo un
núcleo):**

- Zona densa 98-106 s: el bloque promedia ~10 ms (<11,6 ms) contra ~56 ms sin límite;
  el piso se mueve entre ~65 y ~100 en este entorno, donde el render solo ya casi llena
  el bloque. En una máquina real el render se reparte y el piso debería quedar mucho
  más bajo.
- Inicio de la canción: audio **bit-idéntico** a rev. 44.
- Zona 20-24 s con 1 worker: en este contenedor hay un tramo (22 s) donde el render de
  ~5k voces pasa el bloque y el límite actúa (≈1,4k notas suaves). Es el
  comportamiento esperado en una máquina que no llega; en una que llega, no actúa.
- WAV entregado: `hypernova_96-106s_rev46_limit_worstcase_1core.wav`, renderizado con
  el límite en este contenedor: es el peor caso, sirve para oír qué tipo de notas se
  pierden, no cuántas se perderán en tu PC.
- Harness del scheduler (en el CI) actualizado con los stubs nuevos.

Revisión anterior entregada: `wasmidi-main-rev46-velocity-limit.zip` (criterio equivocado, ver §31).

---

## 31. Revisión 47 — el límite descarta por sonoridad estimada, no por velocity

**Reporte del usuario (rev. 46):** desaparecían las notas más audibles en vez de las
menos audibles.

**Causa:** la velocity de entrada no mide cuánto se oye una nota. En Hypernova lo que
se oye son notas de velocity baja apiladas (VOR: la ganancia sube con la cantidad de
pistas duplicadas) y canales con CC7/CC11 altos. Además, el controlador de rev. 46
subía el piso a ciegas mientras el render no bajaba (las voces ya sonando siguen
costando), así que se pasaba de largo hasta 65-120.

**Cambio:**

- `Voice/voice.c`: por cada note-on que no se apila por la vía rápida de VOR se estima
  su sonoridad = `midi_vel_to_amp(vel) × stack × g_channel_render[ch].gain_mul` (lo
  mismo que ya usa el stealer para priorizar), en bins de 1,5 dB (`log2 × 4 + 96`,
  96 = ganancia unitaria). Cada worker acumula un histograma del ciclo y lo suma al
  global al terminar. Si el bin es menor que `g_admit_floor_bin`, la nota se descarta
  (mismo tratamiento de contadores que antes). Los apilados VOR nunca se descartan.
- `snappy_wasm_core.c`: sobre el 100% (promedio móvil), el controlador fija cuántas
  notas admitir por bloque (`allow`, empieza en lo admitido y baja ×0,8 por bloque
  lento) y elige el piso como el bin más bajo que deja entrar solo las `allow` notas
  más sonoras del histograma del bloque. Bajo 85% sube `allow` (×1,1 + 8) hasta
  quitar el límite. Con `allow` en el mínimo (8) vuelve al corte por tiempo como último
  recurso. `ssw_reset()` reinicia todo.

**Medición (host, 1 núcleo, 16 workers, 98-106 s; referencia = mismo motor sin
límite):**

| Versión | Nivel medio vs sin límite | Peor ventana de 50 ms | Correlación de envolvente |
|---|---|---|---|
| rev. 46 (piso de velocity) | −2,5 dB | 41,5 dB más baja | −0,03 (no se parece) |
| rev. 47 (sonoridad) | +0,5 dB | 1,5 dB más baja | 0,67 |

Confirma el reporte y el arreglo: rev. 46 hacía desaparecer tramos enteros audibles;
rev. 47 conserva el nivel y la forma aun en el peor caso posible (en este contenedor el
límite corta muchísimo porque el render solo ya casi llena el bloque).

- Inicio de la canción: bit-idéntico a rev. 44. Zona 20-24 s con 1 worker: el límite
  actúa solo en el tramo de 22 s donde este contenedor no llega.
- WAV: `hypernova_96-106s_rev47_limit_worstcase_1core.wav` (peor caso).
- Harness del scheduler actualizado con los stubs nuevos.

Revisión anterior entregada: `wasmidi-main-rev47-loudness-limit.zip`.

---

## 32. Nombre del fork: WasmiSynth

**Decisión del usuario:** el fork del motor se llama **WasmiSynth**, con el slogan
**"SnappySynthV2 for WASMIDI"**. Usar ese nombre de acá en adelante (también en
sesiones con GPT).

- Panel: el título de la sección pasó de "SNAPPYSYNTH V2" a "WASMISYNTH" (tooltip con
  el slogan). README actualizado.
- `third_party/snappysynthv2/UPSTREAM_NOTES.md`: nuevo encabezado y una sección con
  el comportamiento que difiere a propósito del original y cómo volver a él (el texto
  anterior decía que los cambios se limitaban a compatibilidad, ya no era cierto).
- **Sin renombrar identificadores:** rutas (`third_party/snappysynthv2/`), archivos
  (`snappysynth-worker.js`, `snappysynth_bridge.js`), el prefijo `ssw_*`,
  `WasmidiSnappyBridge` y los targets de CMake quedan igual; cambiarlos rompe
  cachés, el CI y referencias cruzadas sin beneficio. Si alguna vez se renombran,
  hacerlo en una revisión dedicada.

Revisión anterior entregada: `wasmidi-main-rev47.1-wasmisynth.zip`.

---

## 33. Pendiente evaluado: qué del lado JS convendría pasar a wasm

**Pregunta del usuario:** ¿quedan cosas en JS que, pasadas a wasm, mejoren el
rendimiento? Revisión de los bucles por evento/muestra en `web/*.js` (sin cambios de
código). Lo pesado ya está en wasm (motor, store del parser, páginas visuales en C++).
**Nada de esto cambia el lag de la zona densa**: ese cuello es el worker de la tecla
caliente, que ya es wasm (§23-31).

1. **Candidato (pendiente, prioridad baja-media):** post-proceso del synth-pump en
   `midi-parser-worker.js` (el `while (i < count)`): piso de velocidad, unión de
   pilas de note-ons idénticos y copia a dos arreglos nuevos por lote, evento por
   evento en JS, en el mismo hilo que la precarga visual. Moverlo dentro de
   `MidiMappedStore::buildEventBatch` (o una función C++ vecina) que escriba directo
   los arreglos finales de mensajes y tiempos. Ganancia estimada, no medida: decenas de
   ms de CPU por segundo de audio en lo más denso, en el hilo del parser; no acorta el
   camino crítico del synth. Verificación: `tools/feeder_check/` ya emula ese
   post-proceso byte a byte; el flujo debe seguir idéntico.
2. **Descartado por ahora:** puntuación y armado de páginas en
   `visual-cache-worker.js`. Solo visual; bucles sobre arreglos tipados enteros donde
   V8 queda cerca de wasm. Ganancia chica.
3. **Descartado:** worker del synth (solo `HEAPU32.set`/`HEAPF64.set` = memcpy, SysEx
   escaso) y AudioWorklet (~88k muestras/s de copia).

**Prioridad real para el lag:** el stealer en el worker caliente y el costo del render
con miles de voces.

---

## 34. Revisión 48 — renderer: pendiente 7.8 (frames que se frenan a 3M+ NPS)

**Pedido del usuario:** empezar con los pendientes del handoff que tocan el renderer
del visualizador. Pendientes del renderer: **7.8** (frames que se detienen en pasajes
de 3M+ NPS), **7.3** (prerender de frames con caché de tiles en hilos de fondo) y el
**culler visual** sin conectar, que depende de 7.3. Se empezó por 7.8: es un bug que
afecta justo las zonas densas y su causa se puede leer en el código.

**Causa encontrada en el código (hipótesis de 7.8 precisada):**
`calculateSharpView()` decide cuántas pantallas preparar por delante con
`estimatedPerScreen = noteCount / maxTick × windowTicks`, o sea la densidad
**promedio de toda la canción**. En un Black MIDI no uniforme como Hypernova, el
promedio es bajo, así que pide hasta 32 pantallas; al entrar en la zona de ~3M NPS esas
32 pantallas son decenas de millones de notas. El ring (2^23 = 8,4M notas, 100 MB)
tiene que duplicarse a 16M/32M (201/402 MB de VBO más el mismo tamaño en el vector de
CPU, más la copia del anterior y la subida completa): congelamiento o falla de
asignación en WebGL. Además `residentBudget` crecía con `ringCapacity_`: una vez que
el ring crecía, el presupuesto permitía todavía más.

**Cambio (`src/renderer/gl_renderer.cpp`):**

- `estimatedPerScreen` = máximo entre la densidad promedio y la densidad **residente
  ahora** (notas entre `sharpTail_` y la cabeza preparada, sobre los ticks entre el
  inicio de la vista y `sharpRemoteSafeThrough_`). Al entrar en una zona densa el
  horizonte se acorta solo, en cuanto llegan sus primeras notas.
- `residentBudget` fijo en 0,55 × 2^23, sin depender del ring actual.
- `allocateRing()` escribe un `console.warn` si el ring crece. Es un evento raro (no
  es telemetría por frame): si aparece en la consola durante Hypernova, 7.8 sigue
  abierto y hay que agregar control de flujo al instalar lotes remotos
  (`flushRemoteSharpBatches`).
- No cambia qué se dibuja ni cómo: solo cuánto se prepara por delante. Mínimo 4
  pantallas como antes.

**Herramienta nueva:** `tools/renderer_check/check.sh` compila `gl_renderer.cpp` y
`note_raster_compositor.cpp` en host con stubs de GLES3/emscripten generados desde
los símbolos que usan. Prueba que compila, no que dibuja bien. Hasta ahora el renderer
no se podía compilar fuera del CI. Único warning: `-Wclass-memaccess` en la línea
~1674, previo a este cambio.

**Prueba pedida:** Hypernova en la zona de 1:41 con la consola del navegador abierta:
¿se siguen frenando los frames? ¿aparece "WASMIDI renderer ring grows"?

**Siguientes pendientes del renderer (sin empezar):**
- **7.3 — prerender con caché de tiles** en hilos de fondo (la otra mitad de
  `NoteMeshCache` de BPFA): trabajo grande de diseño; necesario para que el culler
  rinda (hoy 21,8 ms por viewport de 1M notas).
- **Conectar el culler** después de 7.3, con depth test apagado o z constante en el
  pase de notas (el orden lo impone el culler). Regla vigente: no cambiar cómo se ve.

Revisión anterior entregada: `wasmidi-main-rev48-renderer-horizon.zip` (empeoró, ver §35).

---

## 35. Revisión 48.1 — se revierte el horizonte de rev. 48

**Reporte del usuario (rev. 48):** el rendimiento empeoró y empezó a perder frames
mucho antes.

**Lectura:** el cambio acortaba el horizonte de precarga. La densidad "residente" se
calculaba dividiendo las notas entre la cola del ring y la cabeza preparada por los
ticks desde `viewStart`, pero la cola conserva notas largas que empezaron antes de la
vista: la densidad salía inflada, el horizonte caía a 4 pantallas en zonas normales y
el renderer quedaba sin frames preparados ante cualquier demora del worker.

**Cambio:** `calculateSharpView()` vuelve exactamente a rev. 47 (densidad promedio de
la canción, presupuesto 0,55 × max(ring, 2^23)). Se conserva solo el `console.warn`
de `allocateRing()` cuando el ring crece, que no cambia nada del dibujo ni del flujo.
Diff contra rev. 47: únicamente esas líneas.

**Cómo seguir con 7.8:** no achicar el horizonte. La prueba pendiente es la misma:
Hypernova en 1:41 con la consola abierta.
- Si aparece "WASMIDI renderer ring grows" justo cuando se frenan los frames, la
  causa es el crecimiento del ring. El arreglo va del lado de la instalación
  (`flushRemoteSharpBatches`): no instalar lotes lejanos que obliguen a crecer, sin
  tocar cuántas pantallas se piden.
- Si no aparece, la hipótesis de 7.8 es falsa y hay que buscar en otro lado.

Revisión anterior entregada: `wasmidi-main-rev48.1-renderer-revert.zip`.

---

## 36. 7.3 + culler: análisis antes de implementar (sin cambios de código)

**Decisión del usuario:** dejar 7.8 de lado y pasar a los otros pendientes del renderer
(7.3 prerender con caché de tiles y conectar el culler).

**Lo que muestra el código actual:**

1. **Regla de capas.** `drawSharpRing()` dibuja con depth test `GL_LESS` y
   `z = (end - start) / 2^24`: la nota **más corta queda arriba**; a igual duración gana
   la primera dibujada. El culler (`note_raster_compositor`) usa la regla de BPFA: la
   que **empieza después queda arriba**. Su cabecera dice que la regla de WASMIDI "se
   dejó a pedido", pero el renderer nunca se cambió. Conectarlo cambia qué color se ve
   donde se superponen notas.
2. **Exactitud del prerender.** El culler es exacto a resolución de píxel para *un*
   frame (inicio de vista y ancho exactos). En tiempo real los ticks de cada frame no
   se conocen de antemano, así que un caché calculado con otro alineamiento puede
   diferir en astillas de un píxel. Por frame, en cambio, cuesta 21,8 ms por 1M notas
   (más que un frame).
3. **Hilos.** La app Qt no se compila con pthreads (solo el core del synth). Un
   "hilo de fondo" sería un Web Worker sin acceso al ring del hilo principal: habría que
   copiarle las notas o hacerlo en el worker del parser (que ya compite con el synth).
4. **Transparencia:** blending apagado y `uTransparencyEnabled = 0`: las notas son
   opacas, así que quitar notas ocultas no cambia píxeles.

**Alternativa exacta medida:** eliminar notas **idénticas** a una anterior (misma
tecla, mismo inicio, mismo fin, cualquier pista/canal). Con la regla actual, a igual
duración gana la primera dibujada, y dos notas idénticas cubren exactamente los mismos
píxeles a cualquier resolución: la segunda nunca se ve. En Hypernova, de las 12,7M
notas que empiezan entre los ticks 600000 y 680000 (zona densa), **7,2M (56,8%) son
duplicados exactos**. No depende del frame, no necesita culling por frame ni hilos, y
se puede hacer en el worker del parser al armar el barrido (también achica la
transferencia y el ring). Complicación: el barrido estilo SharpMIDI agrega notas
abiertas y las cierra después, así que el duplicado recién se conoce al cerrar.

**Pregunta abierta al usuario:** (A) eliminación exacta de duplicados, sin cambio
visual; o (B) el plan original de 7.3 + culler, aceptando cambiar la regla de capas y
diferencias de un píxel.

**Decisión del usuario (§36):** hacerlo como lo maneja BPFA: su regla de capas (la que
empieza después queda arriba; a igual inicio, la de mayor orden de fuente) y su
esquema de `NoteMeshCache` (hilos de fondo, hasta 64 pantallas por delante, caché por
firma de settings + tick de inicio, invalidación por generación en el seek). Se
mantiene la salida del culler como **notas** (mismo shader, paleta, glow) salvo la
regla de capas.

**Bloqueo:** el repo solo tiene el compositor ya portado; el código de BPFA
(`linux/NoteMeshCache.cpp` y lo que agenda sus frames) no está disponible en esta
sesión, y el resumen de `PORTING_STATUS.md` no alcanza para replicarlo con exactitud
(en particular, cómo elige el tick de inicio de cada frame en reproducción en tiempo
real). Pedido al usuario: esos archivos.

---

## 37. Revisión 49 — BPFA: cómo lo hace y fase 1 (regla de capas)

**Fuente:** el usuario aportó el repo de BPFA (`BPFA-master-latest.7z`; extraído con un
lector 7z propio en Python porque el contenedor no tiene `7z`). Archivos clave:
`linux/NoteMeshCache.{h,cpp}` y `PianoFromAbove/PreprocessedMidi.{h,cpp}`.
**Licencia de BPFA: "All Rights Reserved" (EULA de Piano From Above).** No copiar su
código a WASMIDI; implementar el diseño.

**Cómo funciona `NoteMeshCache` (lo que antes faltaba):**

- **Tiles alineados a una grilla fija**, no por frame: `base = floor(pos / span) ×
  span`; cada tile cubre `[k·span, (k+1)·span)` completo, con `rasterHeight` filas.
  El frame visible cae entre dos tiles y se dibujan desplazados. Así resuelve que en
  tiempo real no se sepa el tick de cada frame: la exactitud es por tile, no por frame.
- `RequestWindow(pos, settings, screens=64)`: pide los tiles de `base` a
  `base + 64·span`, al frente de la cola; descarta pedidos y resultados fuera de esa
  ventana (salvo los "difíciles"). Con firma de settings nueva: generación nueva y
  cola vacía. Al cambiar de firma siembra también los rangos **difíciles**
  (`GetDifficultTimeRanges`) con ±10 tiles, al final de la cola.
- Workers: `max(2, min(32, hw>4 ? hw·3/4 : hw−1))` hilos con prioridad baja
  (`setpriority(…, 3)`), cada uno toma pedidos del frente.
- `Build`: rasteriza visitando notas visibles **en reverso**
  (`VisitVisibleReverse`); cada celda queda con la nota de mayor `(start,
  sourceOrder)`: **empieza después = arriba; a igual inicio, mayor sourceOrder**.
  `sourceOrder` se asigna en orden de note-on del flujo fusionado. `fixInvisibleNotes`
  fuerza al menos una fila. Después emite quads por corridas de celdas iguales.
- `PopPrepared` entrega resultados de la generación vigente; `Release` al desalojar un
  tile permite volver a pedirlo; `Invalidate` en el seek.

**Fase 1 hecha (rev. 49):** `drawSharpRing()` dibuja con depth test **apagado**. El ring
guarda las notas en orden de note-on del flujo fusionado (el mismo orden que
`sourceOrder` de BPFA), así que "la última dibujada queda arriba" es exactamente la
regla de BPFA. Antes: depth `GL_LESS` con `z = duración`, la más corta arriba. Cambia
qué color se ve donde se superponen notas; es la parte visible de adoptar BPFA y lo
que el culler asume. `tools/renderer_check/check.sh`: compila.

**Plan de las fases siguientes (sin empezar):**

- **Fase 2 — caché de tiles.** Tiles de `sharpStableWindowTicks_` alineados a la
  grilla; raster de ancho = píxeles del área de notas y 128 filas de teclas (el roll de
  WASMIDI es horizontal). Un tile se arma cuando está completo en el ring
  (`sharpRemoteSafeThrough_ ≥ fin del tile`; las notas aún abiertas en ese punto
  realmente siguen más allá). Salida: notas sobrevivientes (`note_raster_compositor`,
  sin la geometría de BPFA, para no cambiar el shader), en orden de ring, a un VBO por
  tile. Dibujo: por cada tile visible, scissor a su rango de x y draw instanciado con el
  mismo shader; si un tile no está listo, se dibuja ese rango desde el ring como hoy.
  Hasta 64 tiles por delante + 2 de historia; invalidación en seek, cambio de note
  speed, ancho, colores por pista.
- **Dónde corre el armado:** BPFA usa hilos; la app Qt de WASMIDI no tiene pthreads.
  Primer paso propuesto: en el hilo principal con presupuesto por frame (un tile de
  ~600k notas cuesta ~13 ms y se usa durante varios frames); si no alcanza, moverlo a
  Web Workers.
- **Fase 3 — rangos difíciles** sembrados al cargar, como BPFA.

**Prueba pedida (fase 1):** mirar zonas con notas superpuestas; el color que queda
arriba ahora es el de la nota que empieza después.

Revisión anterior preparada: `wasmidi-main-rev49-bpfa-layering.zip` (no se llegó a entregar; su contenido está en rev. 50).

---

## 38. Revisión 50 — fase 2: caché de tiles culleados estilo BPFA

**Pedido del usuario:** continuar con BPFA después de la fase 1 (§37, incluida acá).

**Cambios:**

- `src/renderer/gl_renderer.{hpp,cpp}`:
  - `updateCullTiles()` (al inicio de `drawSharpRing`): solo en modo `remoteIndexed`.
    Tiles de `windowTicks` alineados a la grilla (`k·W`). Firma: `W`, ancho del
    framebuffer, colores por pista, `sharpRingEpoch_` (se incrementa en los 3 resets
    del ring, o sea en cada seek) y documento; si cambia, se liberan todos. Se
    desalojan los tiles detrás de la vista o a más de 64 por delante. Arma como mucho
    2 tiles por frame o hasta 4 ms, en orden, y solo tiles completos
    (`(k+1)·W ≤ sharpRemoteSafeThrough_`).
  - `buildCullTile()`: toma del ring (desde `sharpTail_` hasta la cabeza preparada) las
    notas con inicio < fin del tile y que no terminaron antes de su inicio; las abiertas
    se tratan como abiertas hasta el fin del tile (`viewEndTick`), que es cierto porque
    el tile está completo. Con menos de 50k notas el tile queda "crudo" (se dibuja
    desde el ring como antes, porque culling no conviene). Si no, `CullViewport` con
    raster = ancho del framebuffer y 128 teclas; los sobrevivientes van a un VBO propio
    del tile (`GL_STATIC_DRAW`), en orden de ring.
  - Dibujo: si algún tile visible está culleado, cada tile visible se dibuja con
    scissor a sus columnas (una columna pertenece al tile donde cae su centro) con
    `cullVao_`; los crudos o no listos, con el dibujo del ring de siempre
    (`drawSharpRingRange`) bajo su scissor. Sin tiles culleados, igual que fase 1.
  - `cullVao_` se crea con los 3 atributos instanciados y se borra en `destroy()`.
- `src/renderer/note_raster_compositor.cpp`: cobertura por **centro de píxel**
  (`ceil(x − 0,5)` en ambos bordes), igual que rasteriza la GPU (el contexto no usa
  MSAA) y que BPFA (`floor(x + 0,5)`). Antes usaba `floor`/`ceil` "por contacto": una
  nota de arriba reclamaba columnas que en pantalla no cubría y ocultaba notas
  visibles. Una nota que no cubre ningún centro no emite fragmentos y se puede
  descartar. `tools/note_raster_compositor_check.cpp` (en el CI) actualizado a ese
  modelo: TODO OK.
- `CMakeLists.txt`: `note_raster_compositor.{cpp,hpp}` ahora se compilan en la app.

**Verificación (`tools/renderer_check/`):**

- `check.sh`: `gl_renderer.cpp` y el compositor compilan (stubs de GL).
- `cull_tile_sim.cpp`: modelo en CPU del dibujo (centro de píxel, última dibujada
  gana), flujo sintético denso con duplicados, 1000 px para 1920 ticks:
  - frames alineados a la grilla de tiles: **0 píxeles distintos** contra el dibujo
    crudo (el culling es exacto);
  - frames desalineados (el caso normal al reproducir): **0,135%** de píxeles con otro
    color. Es la aproximación de la grilla fija de BPFA (el tile se decide en su
    grilla, el frame cae corrido); BPFA además dibuja el raster mismo, así que su
    diferencia es mayor.
  - Los tiles conservan el 42,7% de las notas.

**No probado:** nada en navegador. El armado corre en el hilo principal (la app no
tiene pthreads): un tile de ~100k notas cuesta unos pocos ms nativos, más en wasm, y un
armado no se puede partir; si se notan tirones, el siguiente paso es moverlo a un Web
Worker.

**Siguiente (fase 3):** rangos difíciles sembrados al cargar, como BPFA.

Revisión anterior entregada: `wasmidi-main-rev50-bpfa-tiles.zip` (CI falló por el manifest, ver §39).

---

## 39. Revisión 50.1 — CI: manifest desactualizado por README.md

**Falla del CI (rev. 50):** `MANIFEST.sha256 is stale ... MISMATCH: README.md`. El
README del repo del usuario no coincide con ninguna versión entregada (ni la de rev. 39
ni la de rev. 47.1+, ni sus variantes con CRLF), así que en el repo tiene contenido
propio. El resto de los chequeos pasó (scheduler, freelists, VOR, pump, compositor con
la regla nueva). El `#pragma message` de `voice.c` es solo una nota de compilación.

**Cambio:** `README.md` sale de la lista de `tools/regen_manifest.py`: es documentación
que el usuario edita directo en el repo y no tiene sentido que frene el build. El
manifest ahora cubre 41 archivos. El README del zip no se aplica encima del del repo.

Revisión anterior entregada: `wasmidi-main-rev50.1-manifest.zip`.

---

## 40. Revisión 51 — el límite recorta solo en los workers que marcan el tiempo

**Reporte del usuario (rev. 50, "Axley - Quarantine Project 2", 69M notas, pico 5,4M
NPS, 24576 voces, 24 workers):** la velocidad mejoró, pero el búfer de frames se sigue
agotando en las zonas fuertes (en la captura el roll queda vacío con el teclado
encendido) y WasmiSynth sigue comiéndose notas audibles importantes.

**Causa del audio:** el tiempo del bloque lo fija el worker más lento (el de las teclas
calientes, §23), pero rev. 47 bajaba el piso de sonoridad para **todos** los workers.
Las notas de melodía en teclas atendidas por workers ociosos se descartaban sin ahorrar
nada en el camino crítico.

**Cambio (`Voice/voice.c`, `snappy_wasm_core.c`):**

- Histograma de sonoridad, piso y tiempo de trabajo **por worker**
  (`g_admit_hist[w][128]`, `g_admit_floor_bin[w]`, `g_admit_work_us[w]`; hasta 256
  workers). Cada worker mide su fase previa (QPC al empezar el ciclo y antes de la
  barrera de producers) y la publica al terminar.
- Controlador: sobre el 100% (promedio móvil), solo un worker "cuello" recorta
  (trabajo ≥ 60% del más lento y > 25% del bloque). Los demás tienen piso 0 y liberan
  su `allow` si lo tenían. El corte por tiempo sigue como último recurso cuando todos
  los que recortan llegaron al mínimo. `ssw_render_limit_clear()` en init y en reset.
- Harness del scheduler: stubs actualizados.

**Medición (host, 1 núcleo, 16 workers, 98-106 s de Hypernova, contra el mismo motor
sin límite):**

| Versión | Nivel medio | Peor ventana 50 ms | Correlación de envolvente |
|---|---|---|---|
| rev. 46 | −2,5 dB | 41,5 dB más baja | −0,03 |
| rev. 47 | +0,5 dB | 1,5 dB más baja | 0,67 |
| rev. 51 | +0,4 dB | 0,9 dB más baja | **0,82** |

Solo los workers 0 y 15 (teclas 0 y 127) recortan; el resto queda con piso 0. En este
contenedor el bloque no baja de ~40 ms porque con 1 núcleo el cuello es la CPU total,
no el camino crítico; en una máquina real el recorte ataca justo el camino crítico.

**Frames (sin cambio de código, análisis):** los tiles de rev. 50 solo aligeran el
dibujo en la GPU. Lo que se agota es la preparación: el worker del parser genera el
barrido de todas las notas (5,4M/s), el hilo principal las copia al ring y las sube
enteras a la GPU, y recién ahí se cullean. En BPFA el culling ocurre en hilos de fondo
sobre el MIDI preprocesado y el hilo principal solo sube tiles terminados. El paso
equivalente acá: armar los tiles culleados fuera del hilo principal, donde está el
MIDI, y mandar solo sobrevivientes (en Hypernova denso se descartan >57% solo por
duplicados). Es un cambio de arquitectura; pendiente de aprobación.

Revisión anterior entregada: `wasmidi-main-rev51-limit-per-worker.zip`.

---

## 41. Revisión 52 — el límite corta notas repetidas por tecla, no por sonoridad

**Reporte del usuario (rev. 51):** sigue comiéndose muchas notas audibles. Aprobó el
cambio de arquitectura de frames (§40).

**Causa:** el worker cuello atiende varias teclas (con 24 workers, el de la tecla 0
también atiende 24, 48, 72, 96, 120: los Do de la melodía). El piso de sonoridad
comparaba la melodía con el spam de la tecla 0, que por venir apilado estima más
fuerte, y la melodía perdía.

**Cambio (`Voice/voice.c`, `snappy_wasm_core.c`):** en vez del piso de sonoridad, los
workers cuello ponen un **tope de note-ons por (canal, tecla) y ciclo**. El primer
note-on de cada tecla en el ciclo siempre entra; se descartan solo los que se repiten
en la misma tecla más veces que el tope. Una tecla con notas esparcidas (melodía) nunca
llega al tope; una tecla de spam sí, y ahí cada nota extra aporta casi nada porque la
enmascaran las demás. Cada worker publica el máximo de note-ons en una sola tecla del
ciclo; el controlador arranca el tope en la mitad de ese máximo, lo baja ×0,8 por bloque
lento hasta 1 y lo sube +25% cuando sobra tiempo, hasta quitarlo. Los apilados VOR
por la vía rápida nunca cuentan. El corte por tiempo sigue como último recurso con
todos los topes en 1.

**Medición (host, Hypernova 98-106 s, contra sin límite):**

| Versión | Nivel medio | Peor ventana 50 ms | Correlación de envolvente | Notas cortadas por el límite |
|---|---|---|---|---|
| rev. 47 | +0,5 dB | 1,5 dB | 0,67 | — |
| rev. 51 | +0,4 dB | 0,9 dB | 0,82 | ~1,3M |
| rev. 52 | −0,1 dB | 2,0 dB | **0,95** | ~0,24M |

**Otro hallazgo:** con la config del usuario (24576 voces, 24 workers) y sin límite, en
1:41-1:44 el pool se llena igual (quedan ~200-300 voces libres) y el stealer descarta
~450-580k notas/s porque no encuentra víctima. Ese descarte es del algoritmo de SSv2 (el
nativo hace lo mismo con esas voces) y es independiente del límite.

**Plan de frames aprobado (siguiente revisión):** en modo remoto el hilo principal usa
solo el ring Sharp (el constructor de páginas visuales del parser está desactivado a
propósito). `MidiMappedStore::buildVisualPage(start, end)` ya arma en C++ la lista de
notas de un rango: se reusa para construir en el worker del parser los tiles de
`k·W` y pasarles `CullViewport` (el compositor es C++ puro) antes de transferirlos.
El renderer dibuja tiles primero y usa el ring solo donde falte un tile, lo que
permite reducir el barrido Sharp. Así el hilo principal deja de copiar y subir todas
las notas.

Revisión anterior entregada: `wasmidi-main-rev52-limit-per-key.zip`.

---

## 42. Revisión 52.1 — "lagea donde antes no": interruptores A/B

**Pregunta del usuario:** por qué WasmiSynth lagea en lugares donde originalmente no
lageaba.

**Medido en host (Hypernova, 24576 voces, 1 worker, rev. 40 vs 44 vs 52):**

| Zona | rev. 40 | rev. 44 (FIFO) | rev. 52 |
|---|---|---|---|
| 40-44 s | 3,78 ms/bloque, 1354 voces | 4,06 ms, 1354 | 3,74 ms, 1354 |
| 70-74 s | 96,2 ms, pool lleno | 93,5 ms, pool lleno | 89,4 ms, pool lleno |

El costo del motor por bloque no subió en zonas normales, y FIFO no deja voces colgadas
(mismas voces activas). Lo que cambió desde el original y puede sentirse como lag en el
navegador, no medible acá:

1. **Tiles en el hilo principal (rev. 50).** Armar un tile es una operación que no se
   corta; en wasm y con millones de notas puede tomar decenas de ms. Con Workers
   explícito en 24 en un CPU de 24 hilos, el hilo principal ocupado suma competencia
   con los workers del synth, que se sincronizan en barreras (§25).
2. **El límite de render (rev. 45+).** No agrega lag, pero en zonas que ya estaban justas
   corta notas que antes sonaban tarde.

**Cambio (solo para diagnosticar):**

- `?tiles=0` en la URL apaga la caché de tiles (EM_JS `wasmidi_tiles_disabled` en
  `gl_renderer.cpp`, leído una vez).
- `?synthlimit=0` (o un porcentaje) llega al worker del synth como parámetro de su URL y
  se aplica con `ssw_set_render_limit_percent` en `applyCoreSettings()` (exportado en
  CMake).

**Prueba pedida:** en un lugar que antes no lageaba, probar normal, `?tiles=0`,
`?synthlimit=0` y ambos juntos, y además Workers en Auto en vez de 24.

Revisión anterior entregada: `wasmidi-main-rev52.1-ab-switches.zip`.

---

## 43. Revisión 53 — parser de BPFA, fase A (carga + flujo del synth)

**Pedido del usuario:** reemplazar el parser actual por el de BPFA manteniendo todas
las funciones. **El usuario tiene permiso del autor de BPFA** para reutilizar su código
(esto reemplaza la restricción de licencia anotada en §37).

**Qué es el cargador de BPFA (`PianoFromAbove/PreprocessedMidi.{h,cpp}`):** archivo
entero en RAM ("Disk to RAM"), `ScanTrack` por pista (checkpoints cada 256 eventos con
offset/tick/running status, tempos, eventos de estado no-nota, SysEx), notas como
`PackedNote` de 8 bytes (tick de inicio + `durationAndKey`: 12 bits de duración,
`0x0fff` → tabla `LongNote`, velocity, nota, canal) emparejadas por pila por
canal/tecla (la más nueva primero) y parcheadas en el lugar al note-off, `NoteBlock` de
1024 notas por pista con fin máximo, cursores por pista sobre los bytes originales para
reproducir. En BPFA además: compresión zstd de bloques con deduplicación, hilos (PPL /
`std::async`), modo look-ahead con límite de memoria, buckets de densidad.

**Estrategia:** una clase nueva con el diseño de BPFA detrás del **mismo contrato** que
`MidiMappedStore` (mismos tipos y firmas), para que `midi_worker_core.cpp`, el worker
JS, `mainwindow` y el synth no cambien. Salidas idénticas byte a byte a las actuales;
lo que cambia es cómo se carga y se guarda el MIDI.

**Fase A hecha — `src/midi/bpfa_midi_store.{hpp,cpp}` (todavía no conectado al
build):**

- `index()`: lee el archivo entero por `readAt` en bloques de 8 MB, valida cabecera y
  chunks, `ScanTrack` estilo BPFA (decodificador `ReadNextEvent` de BPFA; tempos con
  la regla de WASMIDI —`len ≥ 3`, tempo 0 = mantener el anterior— para que el mapa de
  tiempo sea idéntico), metadatos (`noteCount`, `controlEventCount`, rango de alturas,
  `activeChannelMasks`, `maxTick`, duración), `PackedNote` + `LongNote` + `NoteBlock`.
- Synth: `resetEventCursor` (checkpoint previo + avance), `buildEventBatch` (heap por
  tick/pista sobre los bytes originales, vel 0 → note-off d2=64, agrupado de note-ons
  idénticos ≤256, `safeExclusive` de SysEx), `buildHistoricalSysEx`,
  `buildHistoricalSelectorState` (resets GM/GS/XG).
- SysEx: se guardan todos con el formato de WASMIDI (`[estado] + payload`), no con el
  filtro/normalización de BPFA, porque el synth es otro.

**Diferencias de decodificación heredadas de BPFA (casos raros):** un dato ≥ 0x80 o un
running status inválido termina la pista en lugar de rechazar el archivo; los meta
eventos no cancelan el running status. Archivos que el parser actual rechazaba pueden
cargar.

**Verificación (`tools/feeder_check/store_compare.cpp`, en `check.sh`):** flujo del
synth expandido (SysEx histórica, selectores y lotes) **idéntico byte a byte** al
parser actual en el MIDI sintético (6 posiciones) y en Hypernova (0 s, 33,3 s,
101,5 s).

**Medición (host nativo, 1 hilo, Hypernova 498 MB):** carga **~2,5 s** contra
~6,7 s del parser actual (18,8 s en frío). Memoria del store BPFA: ~842 MB (archivo
completo + 44,75M notas × 8 bytes; 721 notas largas).

**Fases siguientes:**
- **B — funciones visuales** sobre `PackedNote`/`NoteBlock`: `buildRenderSweep` (con
  las reglas SharpMIDI de color por pista/primera nota del parser actual),
  `buildVisualPage`, `buildKeySnapshot`, `buildLiveSnapshot`, tablas de color. Cada
  una verificada contra el parser actual.
- **C — conectar** en `midi_worker_core.cpp` (alias del tipo del store) y CMake.
- **D — zstd** de los bloques del archivo (baja la RAM) y **E — hilos** (pthreads en el
  módulo del parser).

Revisión anterior entregada: `wasmidi-main-rev53-bpfa-parser-a.zip`.

---

## 44. Revisión 54 — parser de BPFA, fases B y C (visual + conectado)

**Pedido del usuario:** aplicar el resto de las fases, salvo las que necesiten pruebas
suyas.

**Fase B — funciones visuales en `BpfaMidiStore`:**

- Cursor fusionado genérico (`MergeCursor`: heap tick/pista sobre los bytes originales
  con los checkpoints de BPFA), con filtro de solo notas para el renderer.
- Colores: el escaneo registra, por pista y canal, la primera nota, su orden y su orden
  de cierre (incluidas las huérfanas al fin de pista en orden de tecla) y
  `buildColorTables()` reproduce la paleta global y por pista (MPWGL2) del parser
  actual.
- `resetRenderCursor`/`buildRenderSweep`: barrido SharpMIDI (contador por canal/tecla,
  reemplazo de dueño, IDs de ring, cierres).
- `buildLiveSnapshot`/`buildKeySnapshot`: estado visual FIFO por (pista, canal, tecla)
  reconstruido reproduciendo cada pista desde su inicio (como hacía el parser actual,
  que nunca llenaba los snapshots de sus checkpoints), cola diferida de note-offs en
  el tick exacto, ventanas de NPS/CC. **Detalle crítico:** la elección del color dueño
  de cada tecla depende del orden de iteración del `unordered_map`, así que hay que
  repetir exactamente las mismas operaciones; incluida la del original de borrar la
  cola cuando queda vacía. Sin eso, mismos conteos pero colores distintos en empates.
- `buildVisualPage`: en rev. 54 devolvía `false` (la app no lo pide); **error**: el
  smoke test del CI sí lo ejercita. Portado completo en rev. 54.1 (§45).

**Verificación (`tools/feeder_check/`, en `check.sh`):** idéntico byte a byte al
parser actual en
- barrido del renderer, colores globales y por pista: sintético (0 y 5,5 s) y Hypernova
  (0 y 101,5 s, ~1M appends y cierres);
- snapshots en vivo con 120 frames a 60 fps por posición y seeks adelante y atrás:
  sintético (5 posiciones) y Hypernova (10, 101,5, 60 y 125 s).
Costo de los snapshots en Hypernova (3 seeks + 360 frames): 6,3 s contra 5,1 s del
parser actual; el seek reproduce desde los bytes en vez de un arreglo residente.

**Fase C — conectado:** `midi_worker_core.cpp` usa `BpfaMidiStore` por defecto
(`WASMIDI_PARSER_BPFA=1`; con `0` vuelve el store anterior). `CMakeLists.txt` compila
`bpfa_midi_store.cpp` en `midi_parser_core`. Compila en ambos modos (con stub de
emscripten en host).

**Fases no aplicadas (necesitan algo que no hay acá o pruebas del usuario):**
- **D — zstd:** el contenedor no tiene fuentes ni cabeceras de zstd ni red. Opción: en
  CMake, `FetchContent` de zstd (BSD) en el CI y comprimir bloques de 64 KB como BPFA
  con deduplicación. Baja la RAM (hoy el archivo completo vive en el módulo del parser).
- **E — hilos:** requiere compilar el módulo del parser con pthreads + Memory64
  compartida y probar en navegador.

**Riesgo a vigilar:** memoria. El store BPFA guarda el archivo completo + 8 bytes por
nota (Hypernova: ~842 MB). El parser anterior no guardaba el archivo pero sí 8 bytes por
evento de canal en arreglos residentes. Con MIDIs de varios GB conviene la fase D.

**Prueba pedida:** cargar MIDIs grandes (Hypernova, Axley) y comprobar tiempo de carga,
que el teclado, colores por pista, estadísticas y el audio se vean/oigan igual, y seeks.

Revisión anterior entregada: `wasmidi-main-rev54-bpfa-parser.zip` (CI falló, ver §45).

---

## 45. Revisión 54.1 — CI: `buildVisualPage` del store BPFA

**Falla del CI (rev. 54):** `tools/midi_parser_bootstrap_smoke.cjs` →
"Mapped parser could not build a visual page after dense indexing". El smoke test
exige que `_wmp_build_visual_page_js(0, 1)` devuelva notas; rev. 54 lo había dejado
devolviendo `false` porque la app no lo usa. Lección (error nº 14): "mantener todas las
funciones" incluye las que solo ejercita el CI.

**Cambio (`bpfa_midi_store.{hpp,cpp}`):** port completo de `buildVisualPage` del
parser anterior: estado rodante entre páginas consecutivas, checkpoints visuales
(`ensureCheckpoint`: reconstrucción directa si el salto supera media grilla, si no
avance con checkpoints de grilla), notas "carry" ordenadas por inicio/pista/orden de
apertura, cierre de huérfanas en el fin de pista dentro de la página, duración mínima
de 15 ms para notas de largo cero, desempate de inicios iguales de una misma pista por
orden de cierre. Para eso el cursor ahora lleva el **índice de evento de canal por
pista** (el "orden" del parser anterior) y los checkpoints de BPFA guardan ese índice.

**Verificación (`tools/feeder_check/page_compare.cpp`, en `check.sh`):** 64 páginas
seguidas de 0,1 s desde varias posiciones (calentamiento secuencial + seeks):
**idéntico** al parser anterior en el sintético (5 posiciones, ~400k notas) y en
Hypernova (0, 101,5 y 40 s; 14M notas de página). `check.sh` completo: synth, barrido,
snapshots y páginas, todo IDENTICAL.

**Nota para la memoria del navegador:** el smoke test recuerda que un fallo histórico
(Pass 12.8/12.9) fue crecer el heap de 64 MiB a ~500 MiB para guardar el archivo
crudo. El store BPFA hace exactamente eso (archivo completo en RAM). El CI en Node lo
pasa con el MIDI virtual de 480 MB, pero si en el navegador aparecen fallos de memoria
con MIDIs grandes, esta es la primera sospecha y la fase D (zstd) el remedio.

Revisión anterior entregada: `wasmidi-main-rev54.1-bpfa-visual-page.zip`.

---

## 46. Revisión 55 — parser de BPFA, fase D (fuente comprimida con zstd)

**Pedido del usuario:** continuar con lo que falta.

**Cambio (`bpfa_midi_store.{hpp,cpp}`), como `PreprocessedMidi` de BPFA:**

- Al terminar la carga, el archivo se parte en bloques de 64 KiB, cada uno con hash
  FNV-1a; los bloques idénticos se deduplican (comparación byte a byte en colisión) y
  el resto se comprime con `ZSTD_compress(..., 1)` (nivel 1, como BPFA; si no achica,
  se guarda crudo). Los bloques viven en slabs de 16 MiB y **el archivo crudo se
  libera**.
- Lectura: el decodificador pide bytes por posición; los bloques comprimidos se
  descomprimen en una caché LRU de 128 bloques (8 MiB). Cada cursor recuerda su bloque
  y valida que el slot no haya sido reutilizado por otro cursor.
- El escaneo, el emparejado de notas y la tabla de SysEx siguen usando el archivo crudo
  durante la carga (pico de memoria = archivo + estructuras, como BPFA).
- `CMakeLists.txt`: `FetchContent` de zstd 1.5.6 (release oficial de GitHub, **sin
  hash fijado todavía**; conviene agregar `URL_HASH SHA256=...` cuando se verifique),
  compilado como `wasmidi_zstd` (`lib/common`, `lib/compress`, `lib/decompress`,
  `ZSTD_DISABLE_ASM`, `-sMEMORY64=1`) y enlazado a `midi_parser_core`. El CI necesita
  red en el configure (GitHub Actions la tiene).
- `tools/feeder_check/check.sh`: en hosts sin cabeceras de zstd genera un `zstd.h`
  mínimo y enlaza `libzstd.so.1` del sistema.

**Verificación:** todas las comparaciones con el parser anterior siguen **idénticas**
(flujo del synth, barrido del renderer con colores globales y por pista, snapshots en
vivo con seeks, páginas visuales), en el sintético y en Hypernova (101,5 s y más).

**Medición (host, Hypernova):**

| | rev. 54.1 (crudo) | rev. 55 (zstd) | parser anterior |
|---|---|---|---|
| Memoria del store | ~842 MB | **~461 MB** | — |
| Carga | ~2,5 s | ~5,4 s | 6,7-21 s |
| Seek lejano (teclado) | — | 0,08 s (60 s), 2,2 s (101,5 s), 7,8 s (125 s) | ~1,7 s promedio |

**Pendientes y decisiones abiertas:**

1. **`PackedNote`/`NoteBlock` (358 MB en Hypernova) no los usa ninguna función
   todavía**: todas las salidas reproducen el parser anterior desde la fuente. O se
   usan, o conviene no construirlos.
2. **Seeks lejanos más lentos:** `rebuildVisualStateAt` reproduce cada pista desde el
   inicio (igual que el parser anterior, que tampoco llenaba snapshots), ahora
   decodificando y descomprimiendo. Bloquea el worker del parser (y la alimentación del
   synth) durante el seek. Opción: reconstruir el estado desde `PackedNote` emparejado
   FIFO por (pista, canal, tecla) usando los `NoteBlock` (requiere guardar el orden de
   apertura por nota para las páginas). Cambiaría solo el desempate de color entre
   canales de una misma pista en la misma tecla y tick (depende del orden interno del
   `unordered_map`), así que la comparación byte a byte dejaría de servir para eso.
3. **E — hilos:** sin hacer; requiere pthreads + Memory64 compartida en el módulo del
   parser y pruebas en navegador.

Revisión anterior entregada: `wasmidi-main-rev55-bpfa-zstd.zip`.

---

## 47. Revisión 55.1 — por qué rev. 55 carga lento, y arreglo

**Pregunta del usuario:** por qué rev. 55 carga tan lento.

**Medición por fase (host nativo, Hypernova 498 MB, 1 hilo):**

| Fase | rev. 55 | rev. 55.1 |
|---|---|---|
| Archivo a RAM | 80 ms | 103 ms |
| Escaneo de pistas (checkpoints, tempo, selectores, SysEx, colores) | 2232 ms | 1900 ms |
| Almacén de notas `PackedNote`/`NoteBlock` | 1514 ms | **no se construye** |
| Compresión (hash de deduplicación + zstd nivel 1) | 1773 ms | 1371 ms |
| **Total** | **5886 ms** | **3656 ms** |
| Memoria del store | 461 MB | **118 MB** (68 MB de fuente comprimida) |

En el navegador cada fase corre más lenta (wasm, un solo hilo, lectura del `File` por
`FileReaderSync`); BPFA reparte escaneo y compresión entre hilos.

**Causas y cambios:**

1. **Tres pasadas completas en un hilo**, y una de ellas inútil: el almacén de notas
   de BPFA no lo usa ninguna salida todavía (§46). Ahora se compila solo con
   `-DWASMIDI_BPFA_NOTE_STORE=1` (default 0): −1,5 s y −343 MB en Hypernova. El código
   queda para cuando se use (seeks rápidos, §46 punto 2).
2. **Hash de deduplicación byte a byte** (FNV-1a sobre 498 MB): ahora mezcla palabras de
   8 bytes. La igualdad se sigue confirmando con `memcmp`, así que la deduplicación es
   la misma.
3. **zstd:** probados niveles 1, −1, −3, −7: 1036/1016/844/761 ms para 68/84/98/115 MB.
   Se mantiene nivel 1 (BPFA): los negativos casi no ganan velocidad.

**Verificación:** `check.sh` completo IDENTICAL (synth, barrido, snapshots, páginas).

**Lo que queda del tiempo de carga:** escaneo (~1,9 s) y compresión (~1,4 s) nativos.
Bajarlos de verdad requiere la fase E (hilos), como hace BPFA.

Revisión anterior entregada: `wasmidi-main-rev55.1-bpfa-faster-load.zip`.

---

## 48. Revisión 56 — seeks lejanos rápidos y visual independiente de WasmiSynth

**Pedido del usuario:** arreglar los seeks lejanos y asegurar que los gráficos, el
teclado y el visualizador no puedan ser ralentizados por WasmiSynth.

### Seeks lejanos (`bpfa_midi_store.{hpp,cpp}`)

El estado visual de un seek se reconstruía reproduciendo cada pista desde el
principio (heredado del parser anterior, cuyos checkpoints nunca guardaban notas
activas). Ahora el escaneo mantiene por pista las colas FIFO de notas abiertas por
(canal, tecla) y, cada 131072 eventos de canal, guarda un **snapshot** (posición para
retomar + notas abiertas con inicio, velocity y orden de apertura), salvo que haya más
de 65536 notas abiertas. `rebuildVisualStateAt` arranca de la última instantánea
anterior al destino y reproduce solo desde ahí.

| Seek en Hypernova | rev. 55.1 | rev. 56 |
|---|---|---|
| 60 s | 77 ms | 18 ms |
| 101,5 s | 2214 ms | 143 ms |
| 125 s | 7772 ms | 49 ms |

Prueba de snapshots (3 seeks + 360 frames): 11,2 s → 0,94 s (el parser anterior:
5,2 s). Carga sin cambio (~3,4 s), memoria +5 MB.

El contenido de las colas es exacto; solo podría cambiar el orden interno del
`unordered_map` y con él el desempate de color de una tecla entre canales de una misma
pista con mismo inicio y misma cantidad. En las comparaciones con Hypernova (10, 101,5,
60 y 125 s) y el sintético, teclado, estadísticas y páginas siguen **idénticos**.

### Visual independiente de WasmiSynth

1. **Reloj de presentación (`mainwindow.cpp`, `updateCurrentTime`).** Antes, con el
   synth cargado, roll/teclado/gráficos seguían el reloj del PCM entregado por el
   AudioWorklet: si el synth se atrasaba, todo se atrasaba. Ahora manda el reloj de
   pared. Al arrancar o hacer seek se espera a que el audio empiece (como mucho 1 s) y
   se fija la latencia de salida (`presentationOffsetSeconds_`, 0-1 s); después la
   latencia solo puede **achicarse** (si el audio va adelante), nunca crecer. Si el synth
   se atrasa, el audio queda detrás del visual en vez de frenarlo. Pendiente a observar:
   tras varias faltas de audio, el desfase puede acumularse (no se resincroniza el synth
   a propósito: resetearlo por atraso causó el bug de "suena un segundo y muere").
2. **Worker del parser (`midi-parser-worker.js`, `drainSharpRenderer`).** El barrido del
   renderer cedía siempre ante los lotes del synth. Ahora, si falta el frame visible
   (`sharpRenderSafeThrough < sharpRenderUrgentThrough`), barre sin esperar; la prioridad
   del synth solo aplica al trabajo especulativo.

**Verificación:** `check.sh` completo IDENTICAL; `node --check` de todos los workers.
`mainwindow.cpp` no se puede compilar acá (Qt).

**Prueba pedida:** seeks lejanos en Hypernova/Axley; en una zona donde WasmiSynth
lagea, el roll, el teclado y los gráficos deben seguir a velocidad normal.

Revisión anterior entregada: `wasmidi-main-rev56-fast-seek-visual-clock.zip`.

---

## 49. Revisión 57 — MIDIs de miles de millones de notas: la fuente ya no se carga entera

**Reporte del usuario:** al cargar un MIDI de varios miles de millones de notas:
"Background MIDI parser failed while BPFA: compressing source: Cannot enlarge memory,
requested 17184825344 bytes, but the limit is 17179869184 bytes!" (16 GiB, el
`MAXIMUM_MEMORY` del módulo del parser).

**Causa:** el store BPFA leía el archivo crudo completo a RAM ("Disk to RAM") y recién
al final lo comprimía: pico = archivo + bloques comprimidos + estructuras. Con un MIDI
de varios GB pasa el techo de 16 GiB. (Es el mismo riesgo que anotaba el smoke test
sobre Pass 12.8/12.9.)

**Cambio (`bpfa_midi_store.{hpp,cpp}`):**

- `ingestSource()`: lee el archivo por `readAt` en ventanas de 8 MiB y cada bloque de
  64 KiB va directo a hash → deduplicación → zstd nivel 1 → slab. **El archivo crudo
  nunca está entero en memoria**: pico = fuente comprimida + una ventana. La
  confirmación de duplicados descomprime el bloque candidato (ya no hay bytes crudos
  para `memcmp`).
- Cabecera, tabla de pistas, escaneo y SysEx se leen desde la fuente comprimida
  (`copySource`, decodificador con caché de bloques). Tras copiar un SysEx se invalida el
  bloque del decodificador (la copia puede reciclar su slot de la LRU).
- Decodificador: ventana con un solo compare sin signo en el camino rápido.
- Snapshots del §48 con presupuesto total de 16M notas (~192 MB) para que MIDIs gigantes
  no los multipliquen; pasado el presupuesto, los seeks en esa zona reproducen más.
- `midi_worker_core.cpp`: export `wmp_memory_growth_probe_js(megas)` (reserva, toca y
  libera). `tools/midi_parser_bootstrap_smoke.cjs` lo usa si el heap no superó 64 MiB:
  el store nuevo ya no hace crecer el heap con los MIDIs de prueba, y la prueba de
  regresión de Memory64 (crecimiento con cantidad de páginas fraccional) necesita que
  crezca. Exportado en `CMakeLists.txt`.

**Medición (host, Hypernova, misma sesión; la CPU del contenedor varía entre corridas):**

| | rev. 56 | rev. 57 |
|---|---|---|
| Carga | ~5,4 s | ~6,3 s |
| Pico de memoria del store | archivo (498 MB) + ~68 MB | **~100 MB** |
| Memoria final | 123 MB | 123 MB |

La carga cuesta ~0,9 s más (descompresión durante el escaneo: ~0,67 s de las 7,6k
lecturas de bloque). Escalado: un MIDI de 20 GB con una relación como la de Hypernova
(7×) quedaría en ~3 GB comprimidos, bajo el techo de 16 GiB.

**Verificación:** `check.sh` (sintético) y Hypernova: flujo del synth, barrido del
renderer, teclado/estadísticas con seeks y páginas visuales **idénticos** al parser
anterior.

**Prueba pedida:** el MIDI de miles de millones de notas que falló.

Revisión anterior entregada: `wasmidi-main-rev57-bpfa-streaming-source.zip`.

---

## 50. Revisión 58 (provisional) — leer el MIDI bajo demanda en vez de guardarlo comprimido

**Pedido del usuario:** implementarlo de forma provisional, medir y descartarlo si no
mejora. Idea de fondo: el MIDI ya está en disco y el worker conserva el `File` después
de cargar (`mappedFileReady`, Pass 13), así que no hace falta tenerlo en RAM, ni crudo
ni comprimido.

**Implementación (`bpfa_midi_store.{hpp,cpp}`):** `setOnDemandSource(true)` (default):
sin `ingestSource`; `sourceBlock()` lee páginas por `readAt` a una caché LRU. Tamaño
automático: ~64 MiB con al menos 2 páginas por pista (página de 1 MiB a 64 KiB según la
cantidad de pistas). `setOnDemandSource(false)` vuelve a la fuente comprimida de rev. 57.
`tools/midi_parser_bootstrap_smoke.cjs`: la fuente queda instalada tras una carga
exitosa, como hace el worker del navegador (antes se desinstalaba y el store nuevo ya
no tenía de dónde leer).

**Benchmarks (host, Hypernova, `tools/host_bench/ondemand_bench.cpp`, lecturas con
`pread` reales; caché del sistema caliente, la CPU del contenedor varía ±10%):**

| | Comprimido (rev. 57) | Bajo demanda (rev. 58) |
|---|---|---|
| Carga | 4,8-5,1 s | **3,1-3,3 s** (−35%) |
| Memoria del store | 123 MB | **99 MB** |
| 2 s de la zona densa (synth + renderer) | ~1,2-1,3 s | ~1,1-1,4 s; 88 MB leídos en 88 lecturas |
| Seeks 101,5 / 60 / 125 s | ~185 / 27 / 60 ms | ~175 / 25 / 53 ms |

En el navegador hay una ganancia extra: para archivos chicos y medianos el worker ya
guarda una copia contigua del archivo en JS (`fastSourceLimitBytes`, hasta 1/8 de la
RAM). Con la fuente comprimida esa copia y la del wasm coexistían; bajo demanda se lee
directo de ella. Para archivos más grandes se lee del `File` con `FileReaderSync`,
como el parser original.

**Caché vs. pistas (riesgo de thrash):** con 16 páginas de 1 MiB para 26 pistas se
leyeron 368 MB en 2 s densos (cada pista expulsa la página de otra). Por eso el tamaño
automático garantiza 2 páginas por pista. 128 × 256 KiB: 74 MB en 296 lecturas. Con
MIDIs de miles de pistas las páginas bajan a 64 KiB.

**Carga progresiva (medido por partes, no implementado):** cabecera, tabla de pistas y
la primera página de cada pista: **2,6 ms**. Pero el tempo de Hypernova está en la
pista 1 (0,1 MB, 19844 cambios), no en la 0: no se puede asumir que esté en la primera.
Una pasada mínima solo para el tempo decodifica el archivo entero en **~0,5 s** nativos.
O sea, se podría empezar a sonar con el tempo exacto en ~0,5 s (contra ~3,2 s) y armar
el resto del índice detrás.

**Verificación:** `check.sh` completo y Hypernova (synth, renderer, teclado con seeks,
páginas): **idénticos** al parser original.

**Decisión pendiente del usuario:** mantener bajo demanda (recomendado) y, si se quiere,
seguir con la carga progresiva.

Revisión anterior entregada: `wasmidi-main-rev58-ondemand-source.zip` (descartada, ver §51).

---

## 51. Revisión 58.1 — se descarta la lectura bajo demanda

**Prueba del usuario en el navegador (rev. 58):** todo carga más lento. Según la regla
acordada ("si no mejora o empeora, lo descartamos"), vuelve la fuente comprimida de
rev. 57 como default (`onDemand_ = false`). El código queda, apagado, por si se retoma.

**Por qué el host engañó:** en el host cada lectura es un `pread` barato desde la caché
del sistema. En el navegador cada página pasa por `wmp_read_file_slice` (EM_JS): copia
desde la copia contigua en JS (`HEAPU8.set`) o, para archivos grandes,
`File.slice` + `FileReaderSync.readAsArrayBuffer` por página, con costo fijo por
llamada. La fuente comprimida lee el archivo una sola vez en ventanas de 8 MiB y
después todo ocurre dentro del wasm. **Lección (error nº 15):** no decidir con
benchmarks de host algo cuyo costo dominante es el cruce JS↔wasm o la API de archivos
del navegador.

Se mantiene el cambio del smoke test (la fuente queda instalada tras una carga exitosa,
como en el navegador); con la fuente comprimida no se usa, es inofensivo.

**Verificación:** `check.sh` completo IDENTICAL con la fuente comprimida.

Revisión anterior entregada: `wasmidi-main-rev58.1-compressed-again.zip`.

---

## 52. Revisión 59 — revisión de Kimrashi (carga y parseo de MIDI)

**Pedido del usuario:** revisar la carga y el parseo de MIDI de Kimrashi
(`Kimrashi-main.zip`, Rust, reproductor Black MIDI para Windows) y tomar lo útil.

**Licencia: GPLv3** (el shader de notas viene de Wasabi, también GPLv3). WASMIDI no tiene
`LICENSE` en el repo: copiar código de Kimrashi obligaría a que WASMIDI sea compatible
con GPLv3. Se toman ideas, no código; el bosquejo de medición
(`tools/host_bench/kimrashi_pack_estimate.cpp`) es una reimplementación propia del
formato descrito y no se usa en la app.

**Qué hace Kimrashi (`src/parser.rs`):**

- Cada pista se re-codifica en registros: `vlv(delta<<2 | flags) [status] key [vel]
  [vlv(dur+1)]`; los note-off se pliegan en la duración (emparejado FIFO por
  canal+tecla con listas enlazadas de ordinales), velocity y duración "corridas"; CC,
  programa, pitch bend y aftertouch se conservan; metas salvo tempo y SysEx se
  descartan; velocity cuantizable.
- Los bytes de cada pista se cortan en chunks definidos por contenido (gear hash,
  ~256 B, 64-1024) y cada chunk distinto se guarda una vez (hash por shards + comparación
  de bytes). En MIDIs "merge" llega a 0,02-0,14 del tamaño del archivo.
- Carga en paralelo por pista (rayon): cada hilo lee una pista con su propio handle, la
  empaqueta y la descarta; un presupuesto de bytes limita lo crudo en vuelo. También
  carga desde archivos comprimidos/zip/7z, y salta notas cuando el synth se atrasa.

**Mediciones propias sobre Hypernova (host, 1 hilo):**

| | Bytes/nota | Tamaño | Tiempo |
|---|---|---|---|
| Fuente zstd de WASMIDI (rev. 57/58.1) | 1,52 | 68 MB | escaneo + compresión ~5 s |
| Formato Kimrashi antes de dedup | 3,04 | 130 MB | |
| Formato Kimrashi con dedup por contenido | **1,30** | 55,5 MB | 2,7 s (solo empaquetar) |
| Dedup por contenido sobre bytes crudos (256 B) + zstd por chunk | 2,0 | 85,5 MB | |

Hypernova no es un "merge" muy repetido: el formato de Kimrashi ahorra solo ~19% contra
la fuente zstd actual. Su gran ventaja aparece en merges (tabla del README), que no
tenemos para medir.

**Qué se tomó:**

- **Emparejado FIFO con listas enlazadas** para las notas abiertas del escaneo (en
  lugar de un `std::deque` por canal+tecla), con un pool de nodos reciclados para que la
  memoria dependa de las notas abiertas y no del total de la pista. Escaneo de Hypernova
  ~3,9-4,3 s → ~3,6 s (−10-15%), carga total ~−6%. Resultados idénticos (`check.sh`,
  teclado con seeks y páginas en Hypernova).

**Qué no se tomó y por qué:**

- **Formato empaquetado como almacenamiento:** pliega los note-off, así que el flujo del
  synth y el barrido SharpMIDI (que dependen del orden original de on/off entre pistas)
  dejarían de ser idénticos; en Hypernova el ahorro es chico.
- **Cuantizar velocity:** cambia sonido y opacidad.
- **Dedup por contenido sobre bytes crudos:** en Hypernova queda peor que zstd en
  bloques de 64 KiB.

**Lo que vale la pena después (necesita navegador):**

- **Carga en paralelo por pista con varios Web Workers**, como los hilos de Kimrashi:
  cada worker lee sus pistas del `File` en lecturas grandes (no por páginas, que fue lo
  que falló en §51), arma su parte del índice y la fuente comprimida, y la transfiere al
  worker del parser. El escaneo por pista ya es independiente salvo tempo, colores y
  SysEx, que se combinan al final.
- **Probar el formato de Kimrashi con un MIDI "merge" real** antes de descartarlo para
  siempre.
- **Cargar desde .zip/.7z/.gz/.zst** como función nueva.

Revisión anterior entregada: `wasmidi-main-rev59-kimrashi-review.zip`.

---

## 53. Revisión 60 — zstd solo para archivos grandes

**Pregunta del usuario:** ¿zstd está empeorando todo?

**Medición (host, Hypernova 498 MB, dos corridas):**

| | Carga | Memoria del store |
|---|---|---|
| zstd siempre (rev. 57-59) | 4,5-5,1 s (1,3 s comprimiendo + descompresión durante el escaneo) | 123 MB |
| crudo en RAM | **3,35-3,74 s** (−25-30%) | 511 MB |

Respuesta: para la carga, sí: zstd cuesta ~1,1-1,4 s en Hypernova. Pero es lo que
permite cargar MIDIs de varios GB (§49) y baja la memoria 4×.

**Cambio:**

- `bpfa_midi_store`: `setRawSourceLimit(bytes)`, default **512 MiB**. Hasta ese tamaño
  el archivo se lee entero (ventanas de 8 MiB) y se decodifica directo; más grande, la
  fuente zstd en streaming de §49. `setRawSourceLimit(0)` = siempre zstd.
- `midi-parser-worker.js`: al terminar una carga exitosa se descarta la copia contigua
  del archivo en JS (`__wasmidiMidiParserWholeFile`, hasta 1/8 de la RAM). El store BPFA
  nunca vuelve a leer el archivo después de indexar (ni crudo ni zstd), así que esa
  copia solo duplicaba memoria. El `File` se conserva (está en disco).

**Verificación:** `check.sh` y Hypernova en modo crudo (synth, renderer, teclado con
seeks, páginas): **idénticos** al parser original. El modo zstd no cambió desde rev. 59.

**Prueba pedida:** tiempo de carga de Hypernova (crudo) y de un MIDI de más de 512 MiB
(zstd).

Revisión anterior entregada: `wasmidi-main-rev60-zstd-only-large.zip`.

---

## 54. Revisión 61 — zstd solo cuando el MIDI crudo no entraría

**Pedido del usuario:** que zstd se active solo cuando el MIDI pueda exceder los límites
o usar más que la RAM del sistema, no con un umbral fijo.

**Cambio:**

- `midi-parser-worker.js`, `rawSourceLimitBytes()`: límite = mínimo entre
  - el heap del parser (`MAXIMUM_MEMORY` 16 GiB) menos 2 GiB para índice y estructuras
    → 14 GiB, y
  - el 75% de la RAM según `navigator.deviceMemory` (si no hay dato, se asumen 8 GiB);
    a la mitad si durante la carga coexiste la copia contigua del archivo en JS
    (solo pasa con archivos de hasta 1/8 de la RAM).
  Se pasa antes de cada parseo con el export nuevo `wmp_set_raw_source_limit_js(bytes)`
  (exportado en CMake). Ejemplos: `deviceMemory` 8 → crudo hasta 6 GiB; 4 → 3 GiB.
- `midi_worker_core.cpp`: `g_rawSourceLimit` (default 512 MiB si el worker no lo fija,
  como en CI/Node) aplicado a `BpfaMidiStore::setRawSourceLimit` antes de `index()`.

**Límite conocido:** `navigator.deviceMemory` está topado en 8 por especificación (y
Brave lo puede alterar), así que en máquinas con más RAM el límite real queda en 6 GiB:
archivos más grandes van por zstd aunque entrarían crudos. Es el lado seguro.

**Verificación:** `node --check` de workers y smoke test; `midi_worker_core.cpp`
compila con y sin `WASMIDI_PARSER_BPFA`. El store no cambió (§53 verificado).

Revisión anterior entregada: `wasmidi-main-rev61-zstd-by-budget.zip`.

---

## 55. Revisión 62 — "BPFA: track scan" más rápido

**Reporte del usuario:** la etapa "BPFA: track scan" es demasiado lenta. También pidió
benchmarks con un Chromium en el entorno de trabajo en vez de en host: **no es posible
acá** (no hay Chromium ni Playwright, no hay red para instalarlos, y no hay emsdk para
compilar el wasm). Alternativa propuesta: un paso del CI (que sí tiene red y emsdk) que
corra el sitio compilado en Chromium headless con un MIDI sintético y escriba los
tiempos de carga en el log.

**Causa (gprof, modo crudo):** `readEvent` era una llamada no inlineada por evento
(124,6M en Hypernova) que además copiaba el decodificador entero de ida y vuelta (la
"copia local" agregada en rev. 57 contra un aliasing que no resultó ser el problema):
~65% del escaneo. El resto, la contabilidad por evento.

**Cambio (`bpfa_midi_store.cpp`):**

- `readEventImpl` es plantilla; `readEvent` se inlinea sin copiar el decodificador.
- `RawDecoder`: decodificador sobre bytes contiguos sin ventana de bloques, usado por el
  escaneo en modo crudo. El cuerpo del escaneo es una lambda genérica que corre con
  `RawDecoder` (crudo) o `Decoder` (zstd). La reproducción (`decodeNext`) también se
  beneficia del inline.

**Medición (host, Hypernova, dos corridas):**

| | Escaneo antes | Escaneo ahora | Carga total ahora |
|---|---|---|---|
| Crudo | 2,84-2,91 s | **1,73-1,93 s** | **~2,3 s** |
| zstd | 3,27-3,34 s | **2,17-2,23 s** | **~3,5 s** |

**Verificación:** `check.sh` (16 IDENTICAL, 0 DIFFER) y Hypernova en modo crudo (synth,
renderer, teclado con seeks, páginas) y en modo zstd (synth, teclado con seeks):
**idénticos** al parser original. `store_compare` y `live_compare` aceptan
`BPFA_ZSTD=1` para forzar la fuente zstd.

Revisión anterior entregada: `wasmidi-main-rev62-faster-track-scan.zip`.

---

## 56. Revisión 63 — MIDIs con miles de pistas: cada cursor decodifica su propio bloque

**Reporte del usuario:** con `The_Nuker_3_Ultimate_F4` la carga total tarda casi una
hora (no los ~2 s medidos con Hypernova).

**El archivo:** 16,66 GB de MIDI (7z de 83,5 MB), **9679 pistas**, PPQ 7680,
2079 millones de notas, 867 s. Supera el límite crudo → fuente zstd. Benchmark en host
leyendo el 7z en streaming por stdin (`tools/host_bench/large_midi_bench.cpp`; el disco
del entorno no alcanza para extraerlo).

**Causa:** la caché LRU compartida tenía 128 bloques de 64 KiB. Con 9679 pistas, los
cursores fusionados (synth, renderer, teclado) leen una pista distinta casi en cada
evento y se expulsan entre sí: **cada pocos eventos se descomprimía un bloque de 64 KiB
entero**. El índice en sí no era el problema.

| Rev. 62, host | Tiempo | Bloques descomprimidos |
|---|---|---|
| Índice (ingesta + escaneo) | 118 s | — |
| 1 s de synth a mitad de canción (43,9M eventos) | **60,9 s** | 1.129.882 |
| 1 s de renderer a mitad de canción | **60,1 s** | 1.129.885 |
| Reconstrucción del teclado a mitad de canción | **82,8 s** | 1.308.829 |

**Cambio (`bpfa_midi_store.{hpp,cpp}`):**

- Cada `Cursor` tiene su propio búfer de bloque (`own`, reservado al primer uso) y
  descomprime ahí; ya no compite con los demás. La LRU compartida queda para lecturas
  sueltas (cabecera, SysEx, escaneo).
- Tamaño de bloque según las pistas, elegido con la cabecera en la primera ventana de la
  ingesta: ~256 MiB / (3 cursores × pistas), entre 8 y 64 KiB. Hypernova (26 pistas):
  64 KiB como antes; Nuker: 8 KiB.
- Contextos de zstd reutilizados (`ZSTD_compressCCtx`, `ZSTD_decompressDCtx`) en vez de
  crear uno por bloque (con bloques chicos dominaba el costo: 4 KiB tardaba 17 s en
  Hypernova).
- `MergeCursor` reinicia sus cursores con `resize` (los cursores ahora no son copiables).
- `check.sh`: el `zstd.h` mínimo incluye las funciones de contexto.

**Después (rev. 63, host):**

| | Tiempo | Bloques descomprimidos |
|---|---|---|
| Índice | 129 s (ingesta 48 s, limitada por la descompresión del 7z en Python; escaneo 73 s) | — |
| 1 s de synth a mitad de canción | **1,56 s** (−97%) | 32.683 |
| 1 s de renderer a mitad de canción | **1,81 s** (−97%) | 32.683 |
| Reconstrucción del teclado a mitad de canción | **15,0 s** (−82%) | 150.111 |
| Memoria del store | 1,94 GB (fuente 1,26 GB con bloques de 8 KiB; era 0,70 GB con 64 KiB) | |

**Lo que queda:**

- **1 s de música cuesta ~1,6 s de parseo a mitad de canción:** son 43,9 millones de
  eventos por segundo (≈22M de notas/s); no entra en tiempo real en un solo hilo con
  ninguna representación. Ahí deciden el límite de render y los descartes.
- **Teclado tras un seek lejano: 15 s.** El presupuesto de snapshots (16M notas) se
  agota temprano en un MIDI de 2.000 millones de notas y más adelante se reproduce desde
  el inicio de cada pista. Siguiente candidato.
- **En el navegador** puede sumar el crecimiento del heap: `MEMORY_GROWTH_GEOMETRIC_STEP=0`
  pide exactamente lo necesario en cada crecimiento (cientos de crecimientos hasta
  ~2 GB). No se puede medir acá.

**Verificación:** `check.sh` (16 IDENTICAL, 0 DIFFER) y Hypernova en modo zstd (synth y
teclado con seeks): **idénticos** al parser original.

Revisión anterior entregada: `wasmidi-main-rev63-per-cursor-blocks.zip`.

---

## 57. Revisión 64 — página de benchmark del parser en Chromium y crecimiento del heap

**Reporte del usuario (rev. 63):** con The Nuker 3 la carga todavía puede tardar media
hora. En host el índice tarda ~129 s; la diferencia está en el navegador y no se puede
medir desde acá (sin Chromium, sin red, sin emsdk).

**Cambios:**

1. **`web/parser-bench.html`** (desplegada por el CI en `site/`): elige un MIDI, lanza el
   mismo `midi-parser-worker.js` que la app con el protocolo normal (`worker-ready` →
   `parse` → `progress` → `result-*`), y muestra cuánto dura cada etapa de progreso más
   la transferencia del documento, con un texto para copiar (archivo, navegador,
   `deviceMemory`, núcleos, etapas, total). No reproduce ni dibuja. Sirve para que el
   usuario mida en su Chromium real, y como base para un benchmark headless en el CI.
2. **Crecimiento del heap del parser:** `MEMORY_GROWTH_GEOMETRIC_STEP=0` pedía exactamente
   cada asignación: con un store de ~2 GB son cientos de crecimientos de Memory64, y
   cada uno recrea las vistas de JS y puede copiar la memoria. Ahora +20% por
   crecimiento con tope de 96 MiB de holgura (`MEMORY_GROWTH_GEOMETRIC_CAP`), así que el
   desperdicio que motivó el 0 queda acotado. No verificable en host.

**Prueba pedida:** abrir `parser-bench.html` del sitio, medir The Nuker 3 y pegar el
texto del resultado. Con eso se ve qué etapa se lleva la media hora (lectura del
archivo, compresión, escaneo o transferencia).

Revisión anterior entregada: `wasmidi-main-rev64-parser-bench.zip`.

---

## 58. Revisión 65 — la carga corría en código no optimizado de V8 (Liftoff)

**Medición del usuario con `parser-bench.html` (Chrome 155, 16 núcleos, rev. 64,
The Nuker 3, 15.884 MB):**

| Etapa | Navegador | Host (rev. 63) | Relación |
|---|---|---|---|
| Compresión de la fuente | 82,0 s | ~48 s (limitado por la descompresión del 7z en Python) | ≤1,7× |
| Escaneo de pistas | **608,8 s** | 73 s | **8,3×** |
| Total | 691,7 s | ~129 s | |

Lo normal entre wasm y nativo es 1,5-2,5×. El 8× apunta a la misma causa que §19
(rev. 40, `worker_cycle`): **V8 no tiene on-stack replacement para wasm**. Una función
que entra una sola vez y queda en un bucle de minutos nunca pasa de Liftoff (código
base, sin optimizar; con Memory64 todavía más lento) a TurboFan. `index()` entra una vez
y el escaneo de todas las pistas (lambda genérica, inlineada) corre dentro de ella. La
compresión llama a zstd (funciones separadas que sí se optimizan), por eso esa etapa
sufre poco.

**Cambio (`bpfa_midi_store.cpp`):** `runChunked(f)` llama a `f` una y otra vez a través
de un puntero a función `volatile` (el inlining, incluso con LTO, no lo atraviesa), así
el cuerpo es una función que se llama muchas veces y V8 la optimiza.

- Escaneo: cada llamada procesa hasta 2^18 eventos (`ScanChunkEvents`) de la pista
  actual y devuelve si terminó; `finalTick` se fija al terminar o al agotar el tramo.
- Ingesta: cada llamada procesa una ventana de 8 MiB (hash, deduplicación, zstd).

En host no cambia nada (Hypernova crudo ~2,1 s, zstd ~3,8 s): el beneficio es solo en
V8. Resultados **idénticos** (`check.sh` 16/16; Hypernova crudo: synth y teclado con
seeks; zstd: synth).

**Prueba pedida:** repetir `parser-bench.html` con The Nuker 3. Si el escaneo baja a
~2× del host (~150 s), la causa era esta.

Revisión anterior entregada: `wasmidi-main-rev65-v8-tierup.zip`.

---

## 59. Revisión 66 — una sola pasada en modo zstd (paso 1 de 2)

**Medición del usuario (rev. 65, otra PC: 24 núcleos, `deviceMemory` 32, The Nuker 4,
20.937 MB):** compresión 103,2 s (203 MB/s, igual que antes), escaneo 197,4 s
(106 MB/s; la rev. 64 en la otra PC daba 26 MB/s) → total 301 s. El arreglo de Liftoff
(§58) funcionó: el escaneo quedó ~2× del nativo, lo esperable. Nota: Chrome 155 informa
`deviceMemory` 32, no está topado en 8 como se asumió en §54.

**Pedido del usuario:** hacer las dos mejoras propuestas: una sola pasada y carga en
paralelo con varios Web Workers. Esta revisión es la primera.

**Cambio (`bpfa_midi_store.{hpp,cpp}`):** en modo zstd ya no se comprime todo el archivo
y después se lo vuelve a descomprimir para escanearlo:

- `beginIngest` prepara el estado (`IngestState`: ventana de 8 MiB, mapa de
  deduplicación, contexto de compresión) sin leer nada.
- `sourceBlock` ingiere ventanas a demanda: cuando el escaneo pide una posición más
  allá de la frontera, `ingestNextWindow` lee la ventana, deduplica y comprime sus
  bloques, y el escaneo lee **directo de la ventana cruda** (slot `0xfffd`, inválido
  para cursores persistentes). Lo que queda detrás de la frontera (un evento que cruza el
  borde, el running status que retrocede un byte) sale del bloque comprimido.
- Las cabeceras `MTrk` se leen dentro del bucle de escaneo, en orden de archivo, para no
  saltar adelante; `finishIngest` comprime lo que quede tras la última pista.
- Progreso: una sola etapa "BPFA: reading, compressing and scanning" (por bytes).
- Constructor y destructor fuera de línea (`IngestState` es un tipo incompleto en el
  header).

**Medición (host):** Hypernova en zstd sin cambio apreciable (~3,7-4,1 s en ambas
versiones: con bloques de 64 KiB la descompresión era barata). Nuker 3: 123 s contra
129 s, pero en host la descompresión del 7z en Python comparte el único núcleo, así que
la medición no es limpia. Lo que se elimina es descomprimir todo el archivo durante el
escaneo: para Nuker con bloques de 8 KiB son ~16 s nativos, más en wasm. Estimación
para el navegador: −10-20%. La ganancia grande la tiene que dar el paralelismo.

**Verificación:** `check.sh` 16/16 IDENTICAL; Hypernova en zstd (synth, teclado con
seeks) idénticos.

**Paso 2 (siguiente): carga en paralelo.** Diseño: el worker del parser lee las
cabeceras de pista, reparte pistas contiguas (balanceadas por bytes) entre N workers
auxiliares con el mismo módulo wasm; cada uno ingiere y escanea su rango (los bloques
cuyo inicio cae en su rango) y devuelve un blob con bloques comprimidos + hashes e
índice por pista (checkpoints, snapshots, primeras notas, tempos, selectores, SysEx,
contadores). El worker principal los incorpora en orden de pista y deduplica entre rangos
con el mismo criterio que hoy, así el resultado coincide con la carga de un hilo.

Última revisión entregada: `wasmidi-main-rev66-single-pass.zip`.
