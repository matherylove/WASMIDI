# WASMIDI — Handoff completo

**Estado al cierre: revisión 72 (16 de octubre de 2026).** Para retomar el proyecto
(incluido en Claude Code) leer primero **`CLAUDE.md`** en la raíz: resume el proyecto,
la forma de trabajo del usuario, el build, la verificación, el estado actual, los
interruptores de rollback y los pendientes. Después leer **§90** (lista de pendientes vigente; §67 es la
lista anterior) y, según el tema, las secciones de cada revisión:
parser §43-§60, synth §61-§63 y §66, WASMIDI Audio §64-§65, renderer §34-§38 y §48.

Las secciones 1 a 14 describen el proyecto hasta la rev. 40 y conservan su texto
original; **§5 ("estado actual") y §8 son históricas**: el estado vigente está en
`CLAUDE.md` y en las secciones finales. Este documento solo se edita agregando o
corrigiendo en el lugar (§7, error 10).

> Texto original de la cabecera (rev. 40): escrito para alguien que llega sin ningún
> contexto. Leé las secciones 1 a 5 antes de tocar código. La sección 5 es la más
> importante: dice dónde está el problema hoy. La sección 19 (rev. 40) es el cambio más
> reciente y corrige la lectura del Problema A y de todas las capturas previas.

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

> Nota rev. 72: esto describe el entorno de chat usado hasta ahora (1 núcleo, sin red,
> sin Qt6 ni emsdk, con Chromium de Playwright en `/opt/pw-browsers`, ver §65). En otro
> entorno con red (p. ej. Claude Code en la máquina del usuario o en la nube) puede
> instalarse emsdk 3.1.56 y Qt 6.8.3 y compilar/medir localmente.

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

## 5. (Histórico, rev. 40) Estado de entonces: dónde estaba el problema

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
16. **Afirmar que una herramienta no existe sin buscarla en todo el sistema** (rev. 71.1).
    Chromium estaba en `/opt/pw-browsers` y en la caché de puppeteer; `which` no lo
    encontraba.

---

## 8. (Histórico, rev. 40) Estado del código: qué estaba activo

> El estado vigente de los mecanismos y sus rollbacks está en `CLAUDE.md`.

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

Revisión anterior entregada: `wasmidi-main-rev66-single-pass.zip`.

---

## 60. Revisión 67 — carga en paralelo con Web Workers (paso 2 de 2)

**Medición del usuario (rev. 66, PC 1, The Nuker 3):** 277,3 s en una sola etapa
("reading, compressing and scanning"), contra 691,7 s de la rev. 64 (−60% entre §58 y
§59).

**Diseño:**

1. El worker del parser lee la cabecera y la posición de cada `MTrk`
   (`wmp_track_table_js`: lecturas de 8 bytes) y reparte las pistas en rangos contiguos
   balanceados por bytes: `min(núcleos − 2, 12, pistas)` helpers. Sin al menos 2 rangos,
   archivos < 128 MiB o < 4 núcleos, carga normal de un hilo.
2. Cada helper (`web/midi-parser-helper.js`, mismo módulo wasm) recibe el `File` y su
   plan, y corre `BpfaMidiStore::indexPartial`: la misma pasada única de §59 limitada a
   su rango. Un bloque de fuente pertenece al helper cuyo rango contiene el inicio del
   bloque; el helper lee hasta terminar el último bloque propio. En modo crudo los
   helpers solo escanean (sin guardar bloques; conservan la ventana anterior para los
   retrocesos de un byte) y el worker principal lee el archivo entero mientras tanto.
3. El helper exporta un blob (`exportPartial`): bloques propios (hash, tamaño, palabra
   de almacenamiento y bytes comprimidos), y por pista rango de bytes, `maxTick`, máscara
   de canales, notas, primera nota, checkpoints y snapshots; además controles, alturas,
   tempos, eventos de estado y SysEx. Presupuesto de snapshots proporcional a sus bytes.
4. El worker principal fusiona los blobs **en orden de pista** (`mergePartial`):
   deduplica cada bloque contra todos los anteriores con el mismo criterio que la carga
   de un hilo (hash + comparación de bytes descomprimidos), copia los bytes comprimidos
   sin recomprimir, y agrega tempos, estados y SysEx; `finishMerge` hace el mismo cierre
   que `index()` (`finalizeIndex`, compartido).
5. Si algo falla en cualquier helper: se terminan todos y se carga en un hilo.

**Archivos:** `bpfa_midi_store.{hpp,cpp}` (`PartialPlan`, `indexPartial`,
`exportPartial`, `beginMerge`, `mergeReadRaw`, `mergePartial`, `finishMerge`,
`finalizeIndex`, `blockSizeForTracks`, base de bloques `chunkBase_`, ingesta acotada a
un rango); `midi_worker_core.cpp` (exports `wmp_track_table_js`, `wmp_partial_*`,
`wmp_merge_*`; agregados en CMake); `midi-parser-worker.js` (`parallelParse`, progreso
ponderado por bytes con la etapa "BPFA: parallel load (N workers)"); CI copia el helper.

**Verificación (host, `tools/feeder_check/bpfa_parallel_sim.hpp` simula la partición,
los parciales y la fusión en un proceso):**

- `check.sh`: 24 IDENTICAL, 0 DIFFER, incluidos synth, renderer, teclado y páginas con 3
  helpers en modo crudo y zstd contra el parser original.
- Hypernova con 6 helpers, crudo y zstd: synth, renderer y teclado con seeks idénticos.
- Fuente comprimida idéntica en tamaño (68,21 MB) con 1, 4 y 8 helpers: la
  deduplicación entre rangos equivale a la de un hilo.
- Costo total de CPU de la división (simulada en serie): +15-25% sobre un hilo
  (bloque de borde repetido, serialización, fusión). En paralelo el tiempo de pared
  esperado es ~(un hilo / helpers) + fusión.

**No verificable acá:** el arranque real de Workers y el wasm en cada helper, la memoria
pico (cada helper tiene sus bloques comprimidos hasta que el principal los copia), y la
ganancia. Medir con `parser-bench.html`.

**Medición del usuario (rev. 67, PC 1: 16 núcleos, `deviceMemory` 32, The Nuker 3):**

| Etapa | Tiempo |
|---|---|
| Opening MIDI as a paged source (incluye la tabla de pistas: 9679 lecturas de 8 bytes en serie con `FileReaderSync`) | 2,98 s |
| BPFA: parallel load (12 workers) | 38,3 s |
| BPFA: merging partial indexes | 0,17 s |
| **Total** | **42,2 s** |

Evolución en la misma PC y archivo: 691,7 s (rev. 64) → 277,3 s (rev. 66) → **42,2 s**
(rev. 67): 16× menos. Pendiente menor: la tabla de pistas (~7% del total) es una
cadena de lecturas dependientes (cada cabecera dice dónde está la siguiente).

Revisión anterior entregada: `wasmidi-main-rev67-parallel-load.zip` (+ nota de medición).

---

## 61. Revisión 68 — ideas de Kestrel/SVMS, parte 1: admisión, tope de robos y salida

**Contexto:** el usuario pidió leer Kestrel (MPL-2.0, síntesis en GPU offline),
SuperVirtualMIDISynth V3 (GPL-3, synth realtime para Black MIDI) y Kasaria (GPL-2,
basado en Timidity) y luego implementar todas las ideas, con la GPU como alternativa
seleccionable a la CPU. Solo se toman ideas (licencias GPL). Orden acordado:

1. **Esta revisión:** admisión por bloque, tope de robos, limitador + bloqueador de DC.
2. Multinúcleo estilo SVMS: un hilo procesa eventos en orden, el render se reparte en
   tandas de 256 voces tomadas de un contador atómico, reducción en orden fijo
   (determinismo); umbral para no paralelizar bloques chicos.
3. Síntesis por WebGPU (estilo Kestrel) con interruptor GPU/CPU.

**Cambios (`snappy_wasm_core.c`, `Voice/voice.{c,h}`, `CMakeLists.txt`):**

- **Admisión por bloque (Kestrel, "admission decides who never starts"):** en
  `ssw_render_queued_into`, antes de despachar, se cuentan los note-ons del bloque con
  un histograma de velocity. Si superan `max_voices × g_admission_percent / 100`
  (default 25%), se fija un umbral de velocity: entran todos los de velocity mayor y,
  del umbral, los primeros que quepan en orden. Cada (canal, tecla) tiene una FIFO de
  "entró / no entró" en corridas (`ssw_note_fifo`, O(1) en el caso común); el note-off
  que el motor emparejaría (FIFO, como `SSW_NOTEOFF_FIFO`) con un note-on descartado
  también se descarta, así no quedan notas colgadas. Se limpia en `ssw_reset`.
  Exports: `ssw_set_admission_percent`, `ssw_admission_dropped`.
- **Tope de robos por ciclo (Kestrel `--steal-percent`):** en `worker_cycle`, cada
  worker puede robar como mucho `max_voices × 25% / workers` voces por ciclo; pasado eso
  la nota entrante se descarta en vez de cortar otra voz (evita la "lluvia de ataques
  de 85 ms" en zonas saturadas). `voice_set_steal_cap_percent`,
  `ssw_set_steal_cap_percent` (0 = sin tope).
- **Salida (Kestrel):** con "Soft clip" activado (el default), en lugar de la curva de
  soft clip de `voice.c` se aplica en el core un pasa-altos Butterworth de 2.º orden a
  15 Hz (bloqueador de DC) y un limitador brickwall con 2 ms de anticipación a
  −0,3 dBFS (mínimo deslizante de la ganancia necesaria, ataque que llega a destino
  antes del pico, release de 80 ms, clamp final). `voice_set_output_soft_clip_enabled`
  queda siempre en 0. Agrega 2 ms de latencia.

**Medición (host, Hypernova 100-104 s, 1024 voces, 1 worker, límite de render
apagado):**

| | Costo vs tiempo real | Bloque máx. | Robos/s | Descartes en el motor/s |
|---|---|---|---|---|
| rev. 67 | 1,57× | 144 ms | 77-259k | 0,7-1,1M |
| rev. 68 solo limitador (admisión y tope en 0) | 1,49× | — | — | — |
| **rev. 68** | **0,67×** | 24,8 ms | 19-62k | <1k |

La admisión descartó 6,58M note-ons en 4 s antes de que llegaran a las voces. Nivel
medio con admisión y tope: +0,7 dB respecto de sin ellos (las voces duran más); peor
ventana de 50 ms: 0,9 dB más baja. El pico de salida queda en 0,966 (antes saturaba a
1,0 en la conversión a 16 bits).

**Hallazgo:** el motor **no es determinístico ni con 1 worker**: el presupuesto de
cortes por bloque (`ssw_update_render_budget`) depende del tiempo medido, así que dos
corridas idénticas difieren. Es lo que la parte 2 (render con reducción en orden fijo)
y una opción de render determinístico deberían resolver para poder verificar audio
byte a byte como el parser.

**Sin cambios de UI:** el botón "Soft clip" ahora significa "limitador + DC blocker".

Revisión anterior entregada: `wasmidi-main-rev68-admission-limiter.zip`.

---

## 62. Revisión 69 — etapa 2: multinúcleo estilo SVMS y modo determinístico

**Lo que ya había (relevado al implementar):** el render de SSv2 ya repartía voces
dinámicamente (cola global `g_render_queue` + contador atómico `g_render_pop`), pero:
(a) cada worker tomaba ~1-2 tandas grandes (`pop_chunk` ≈ voces/workers), así que un
worker desalojado con su tanda frenaba a todos; (b) cada worker mezclaba en su propio
búfer y el orden de la cola dependía de qué worker llegaba primero, así que la suma de
punto flotante cambiaba con el timing; (c) los eventos se procesan repartidos por
tecla (`g_note_worker_map`), así que el worker de las teclas saturadas marca el tiempo
del bloque (§23, §40).

**Cambios (`Voice/voice.{c,h}`, `snappy_wasm_core.c`, UI):**

1. **Tiles fijos de 256 voces** (`RENDER_TILE_VOICES`): cada tanda tomada del contador
   mezcla en su propio búfer de tile (`g_tile_mix`, reservado en
   `ensure_worker_mix_capacity`), y `voice_render_float` suma los tiles **en orden de
   tile**. Un worker desalojado retiene a lo sumo 256 voces.
2. **Cola en orden fijo:** el último productor arma `g_render_queue` recorriendo los
   workers 0..W−1 (`build_render_queue_ordered`); antes cada productor reservaba su
   tramo con un `InterlockedAdd` en el orden en que llegaba.
3. **Reparto "single" (SVMS):** `SS_NOTE_SHARDING=single` / sharding 3: todos los eventos
   van al worker 0 y todos los workers comparten los tiles de render. **Auto lo elige con
   ≤ 4096 voces** (con la admisión de §61 entran como mucho 25% de las voces por bloque,
   así que un hilo alcanza). El tope de robos usa `g_event_worker_count` (1 en single).
   UI: el botón "Sharding" cicla Auto/Channel/Hash/Single; bridge, worker y
   `mainwindow.cpp` aceptan 0-3.
4. **Modo determinístico** (`ssw_set_deterministic`, exportado): un render por bloque
   (sin cortes según el tiempo medido), limitador de tiempo de render apagado, cola
   ordenada por `voice_age`. Pensado para verificar audio.

**Verificación (host, Hypernova 100-102 s, 1024 voces, sharding single + determinístico):**
dos corridas con 1 worker → **WAV idénticos**; 1 contra 4 y 1 contra 8 workers →
**WAV idénticos**. Es la primera vez que el audio de WasmiSynth se puede comparar byte a
byte. Sin modo determinístico (default) el nivel y los picos coinciden con rev. 68
(79,5-79,6 dB, pico 0,966) y el costo en host es el mismo (~0,64× con 4 workers en 1
núcleo; el host no mide paralelismo real).

Cómo reproducir: `tools/host_bench/midibench.c` con `SHARD=3 DET=1 VOICES=1024` y
distinta cantidad de workers; comparar los WAV con `cmp`.

**No hecho:** que el hilo que llama a `voice_render_float` también tome tiles (SVMS lo
hace). Requiere un `worker_data` extra para el coordinador; con tiles de 256 la ganancia
es un núcleo más, no la eliminación de esperas.

**Prueba pedida:** en el navegador, Hypernova y The Nuker 3 con 1024 voces y Sharding
Auto (= Single) contra Hash; ver si desaparecen los tirones por workers.

Revisión anterior entregada: `wasmidi-main-rev69-svms-multicore.zip`.

---

## 63. Revisión 70 — etapa 3: render de voces por WebGPU con interruptor CPU/GPU

**Modelo (híbrido, inspirado en Kestrel y en el `GPUMIX` original de SSv2):** el CPU sigue
siendo el dueño de las voces (eventos, admisión, robos, note-offs) y decide, voz por voz y
segmento por segmento, cuáles van a la GPU. Para esas, en vez de renderizar, escribe una
petición de 160 bytes (`wgpu_voice_request`) y **avanza su estado con la misma aritmética
por muestra** que usa el shader (posición con loop, envolvente ataque/decay/sustain/release,
note-off pendiente), así el CPU nunca espera a la GPU para decidir el bloque siguiente. La
GPU devuelve solo audio.

**Qué va a la GPU (`wgpu_try_offload`, `Voice/voice.c`):** voces con sample en el pool de
la GPU, salida estéreo, sample mono/estéreo, sin portamento en curso, sin `kill_now`, sin
fundido VOR, sin note-off pendiente sobre un apilado VOR (>1), con `startup` menor al
segmento, que no se salgan del sample sin loop en este segmento. Incluye ataque, release,
note-off pendiente (no-VOR) y **filtro**: el estado del biquad vive en la GPU por voz
(`filters[voice_id]`) y continúa entre bloques GPU consecutivos; si la voz no se renderizó
en la GPU en el bloque anterior, se reinicia con el estado del CPU (`filter_reset`).
Limitación conocida: si una voz filtrada vuelve al CPU (p. ej. por `kill_now`), el CPU usa su
último estado propio del filtro.

**Piezas:**

- `Voice/voice.{c,h}`: `wgpu_voice_request`, `wgpu_try_offload`, capacidad
  `max_voices × 8` peticiones, tope de voz-frames por bloque (`voice_wgpu_set_frame_cap`,
  para no exceder el scratch de la GPU), serial de bloque y número de segmento,
  `voice_wgpu_collect_samples` (tabla {dirección baja/alta, int16, offset} con offsets
  pares y 2 muestras de guarda), campo `wgpu_serial` en `voice`, `gpu_offset_plus_one` en
  `wav_data` (el `malloc` del resample de SFZ pasó a `calloc`). En modo GPU se salta el lote
  rápido WASM SIMD (para que las voces pasen por la descarga) y `mix_apply_gain_and_limit`
  aplica solo la ganancia global (el limitador del core corre después de sumar la GPU).
- `snappy_wasm_core.c`: base de frames por segmento (`ssw_wgpu_set_frame_base` + inicio del
  segmento), sin etapa de salida en C en modo GPU, exports `ssw_wgpu_*`,
  `ssw_global_mix_gain`, `ssw_output_stage_apply` (en CMake).
- `web/snappysynth-webgpu.js` (nuevo, desplegado por el CI): `WasmiSynthGpu`. Shader WGSL
  `renderVoices` (un hilo por petición, recorre sus frames con interpolación lineal,
  filtro, envolvente; escribe filas empaquetadas en `scratch`) y `reduceMix` (un hilo por
  muestra de salida, suma las peticiones en orden fijo). Una pasada por segmento (el filtro
  avanza en orden), luego la reducción, copia a un búfer de lectura y `mapAsync`. Pool de
  samples en un storage buffer (u32 con dos int16); si no entra en
  `maxStorageBufferBindingSize`, se informa y se queda en CPU. Errores de dispositivo
  (`lost`, `onuncapturederror`) apagan la GPU.
- `web/snappysynth-worker.js`: `setupGpu`/`disableGpu`, y `renderOneBlockGpu`: el bloque se
  renderiza en el ring pero **no se publica** hasta que llega su parte GPU; entonces se suma,
  se aplica la etapa de salida y se publica en orden (hasta 8 bloques en vuelo; el espacio
  libre descuenta los no publicados). Un seek (generación del ring) descarta los pendientes.
  Al volver a CPU se terminan primero los bloques GPU pendientes. Se rehace al cargar otro
  soundfont o reinicializar el core.
- Bridge (`setGpu`, estado `gpuStatus`, se muestra como "WasmiSynth GPU: …"),
  `mainwindow` (`synthGpu`, persistido, reaplicado cuando el synth queda listo) y botón
  **CPU/GPU** en el panel WASMISYNTH (junto a "Stack gain").

**Verificación (host, `tools/host_bench/midibench.c` con `GPUEMU=1`, que emula el shader en
C con el mismo algoritmo):**

- Hypernova 40-44 s: 460.867 voz-bloques en la "GPU"; 100-102 s: 345.228.
- Nivel CPU vs CPU+GPU: 79,54/79,65 dB y 79,48/79,49 dB.
- Forma de onda: correlación **1,000** (zona liviana) y **0,987** (zona densa) alineando
  **127 muestras**: en modo GPU se saltea el limitador con anticipación de `voice.c`, así que
  la salida sale ~2,9 ms antes que en modo CPU.
- Modo determinístico: idéntico con 1 y 4 workers, tanto en CPU como con la GPU emulada.
- Arneses del CI (scheduler con stubs nuevos, rebalanceo, worker cycle) OK.

**No verificable acá:** que el WGSL compile en Chrome, el pipeline WebGPU real, la latencia
del `mapAsync` y la ganancia de rendimiento. Probar con el botón GPU y mirar el estado del
panel ("active (N MB of samples on the GPU)" o el motivo de "unavailable") y la consola.

Revisión anterior entregada: `wasmidi-main-rev70-webgpu.zip`.

---

## 64. Revisión 71 — WASMIDI Audio (reproductor solo de audio)

**Pedido del usuario:** un reproductor HTML estilo Winamp, con el estilo visual y de GUI
de dekx.cc y sus programas, que use el sistema completo de WASMIDI pero solo para audio
con WasmiSynth, para aislar los problemas de audio de los del piano roll, el teclado y
los gráficos. Nombre: **WASMIDI Audio**.

**Archivos nuevos (`web/`, desplegados por el CI junto al sitio):**

- `audio.html`: dos ventanas apiladas con el kit de ventanas de Dekxtopia (barra de
  título degradada, botones de caption, barra de menú con Home / WebAssembly MIDI Player /
  WASMIDI Audio, barra de estado, group boxes). Ventana principal estilo Winamp: LCD con
  reloj VT323, estado (PLAY/PAUSE/STOP), marquesina con el nombre del MIDI, lecturas de
  voces activas, carga de render, llenado del búfer y underruns, medidor; barra de
  posición; transporte (reinicio, play, pausa, stop, abrir MIDI, abrir SF2, CPU/GPU,
  volumen). Ventana "Info": datos del MIDI (archivo, tamaño, formato, pistas, notas, PPQ,
  duración, SoundFont), configuración de WasmiSynth (voces, workers, bloque, búferes,
  sharding, limitador, Apply) con lecturas (workers, motor, notas descartadas, robos) y un
  registro con los tiempos de cada etapa de carga. Arrastrar y soltar `.mid`/`.sf2`;
  espacio = play/pausa.
- `audio.css`: la base de `dekx.css` copiada (variables, `.win`, caption, menú, botones,
  campos, status bar, group) más los estilos del reproductor. Se copia en vez de enlazar
  `dekx.cc/dekx.css` porque la página corre con COEP `require-corp` (pthreads) y una hoja
  de otro origen sin CORP no cargaría. Las fuentes de Google se piden con `crossorigin`.
- `wasmidi-audio.js`: orquesta lo mismo que `mainwindow.cpp` para el audio, sin Qt:
  worker del parser (`parse`, progreso, `doc-info`), `WasmidiSnappyBridge`
  (`ensureBackend`, `configure`, `loadSoundfontFile`, `play/pause/stop/seek`,
  `scheduleBatch`, `getAudioClock`, `setVolume`, `setGpu`) y el mismo protocolo de
  alimentación del synth: `synth-reset` con generación, espera del ACK
  `synth-cursor-reset`, `synth-pump` con el mismo horizonte (2-4 s según bloque y
  búferes), cola del pedido más lejano, `synth-more`, `synth-pump-deferred`,
  `synth-batch` → `scheduleBatch`. Configuración guardada en `localStorage`
  (`wasmidiAudio`).

**Cambios para que funcione sin el documento de Qt:**

- `midi_worker_core.cpp`: exports `wmp_seconds_to_tick_js` y `wmp_document_info_js`
  ([maxTick, duración, PPQ, notas, pistas, formato]); en CMake.
- `midi-parser-worker.js`: mensaje `doc-info`; `synth-reset` acepta `seconds` y
  `synth-pump` acepta `endSeconds` (se convierten a ticks en el worker).
- CI: copia los tres archivos y reemplaza `?v=dev` en `audio.html` por la versión del
  build (igual que en `index.html`).

**Qué aísla:** usa el mismo parser (BPFA, carga paralela incluida), el mismo bridge,
worker, AudioWorklet y motor que la app, y el mismo protocolo de alimentación; no crea
el renderer, ni pide barridos visuales, teclado o páginas al worker del parser, que
queda dedicado al synth. No usa el límite dinámico de velocity de la app (piso 0).

**No verificable acá:** todo corre en el navegador (aislamiento cross-origin, AudioWorklet,
workers). Archivos JS con `node --check`; exports C++ compilados con stub.

Revisión anterior entregada: `wasmidi-main-rev71-wasmidi-audio.zip`.

---

## 65. Revisión 71.1 — WASMIDI Audio: arreglos visuales y Chromium en el entorno

**Reporte del usuario (captura):** visualmente bugeado: el reloj del LCD ("00:00" a
46 px más el total en la misma línea) no entraba en su columna de 132 px y el total se
cortaba contra la marquesina; la barra de estado decía "SnappySynthV2 idle"; el aviso de
aislamiento cross-origin quedaba en el registro aunque la página ya se recargara.

**Cambios:** columna del reloj de ancho automático, total en su propia línea ("/ 00:00"),
estado abajo; lecturas con valor grande en VT323 y etiqueta chica; la marquesina solo se
desplaza si el texto no entra (medido); "SnappySynthV2" se muestra como "WasmiSynth"; el
aviso de aislamiento solo aparece si a los 3 s la página sigue sin aislamiento, como error
y pidiendo recargar; iconos de transporte al doble de tamaño.

**Corrección importante (error nº 16):** el entorno de trabajo **sí tiene Chromium**:
`/opt/pw-browsers/chromium-1194` y `chromium_headless_shell-1194` (Playwright, con
`PLAYWRIGHT_BROWSERS_PATH=/opt/pw-browsers`) y `~/.cache/puppeteer/chrome/linux-131…`.
Antes se le dijo al usuario que no había (§55, §57): solo se había buscado con `which`.
Con esto se verificó esta página renderizándola (servidor local + captura). Lo que sigue
faltando es emsdk y red, así que no se puede compilar el wasm acá; con el sitio ya
compilado por el CI (artefacto `site/`) subido, se puede correr `parser-bench.html`,
WASMIDI Audio y la app en Chromium headless para medir (servidor con COOP/COEP).
WebGPU sin GPU real probablemente no esté disponible en headless.

Revisión anterior entregada: `wasmidi-main-rev71.1-audio-visual-fix.zip`.

---

## 66. Revisión 72 — más voces = más trabajo; Auto pasa a Single siempre

**Reporte del usuario (WASMIDI Audio, Hypernova):** mucho más lag con 12 workers y 12288
voces que con 1 worker y 1024 voces.

**Medición (host, 1 núcleo, Hypernova 100-102 s; con 1 núcleo el tiempo de bloque es el
CPU total que necesita la configuración, no el que tardaría en paralelo):**

| Workers | Voces | Reparto | CPU por bloque de 11,6 ms | Fase de eventos (ALLOC) |
|---|---|---|---|---|
| 1 | 1024 | Auto (Single) | 5,6-7,6 ms | 0,5 ms |
| 12 | 1024 | Auto (Single) | ~6,9 ms | — |
| 1 | 12288 | Auto (rev. 71: Hash) | 49-61 ms | 3,1 ms |
| 12 | 12288 | Hash | 53-65 ms | eventos repartidos y desbalanceados |
| 12 | 12288 | Single | 48-57 ms | 3,1 ms en un hilo |
| 12 | 24576 | Hash / Single | 95 / 76 ms | — / 3,2 ms |

Conclusión: **los workers casi no agregan costo** (1 vs 12 workers con 1024 voces: igual),
pero **12288 voces son ~8× más trabajo de render** que 1024: en Hypernova el pool queda
lleno (12.279 voces activas) y se roban ~480k voces/s. Para que 12 workers lo absorban
habría que repartir ~50 ms de CPU por bloque con eficiencia casi perfecta, mientras el
navegador también corre el parser, el AudioWorklet y el hilo principal; cualquier worker
desalojado frena el bloque. La comparación justa es misma cantidad de voces con 1 y 12
workers.

**Cambio (`Voice/voice.c`):** Auto usa **Single** con cualquier cantidad de voces (antes
solo hasta 4096). Single fue más barato que Hash en todos los casos medidos (48-57 vs
53-65 ms con 12288; 76 vs 95 ms con 24576) y su fase de eventos es ~3 ms en un hilo.
Determinístico: con 12288 voces, 1 y 12 workers dan el mismo WAV.

Revisión anterior entregada: `wasmidi-main-rev72-auto-single.zip`.

---

## 67. Cierre de sesión (rev. 72): pendientes abiertos y paso a Claude Code

Sin cambios de código en esta sección. El usuario pidió **no hacer nada todavía**,
actualizar el handoff para seguir el proyecto en Claude Code y saber qué archivos darle
como contexto. Se agregó `CLAUDE.md` en la raíz (resumen operativo vigente).

### Reportes del usuario sin resolver (en orden de llegada)

**A. WebGPU con 12 workers va mucho más lento que con 1 worker; con 1 worker rinde
exactamente igual que CPU.** (Este mensaje quedó sin respuesta en el chat.) Primero
confirmar qué muestra el panel ("WasmiSynth GPU: active (N MB…)" o "unavailable: …"): si
con 1 worker la GPU no estaba activa, "rinde igual" se explica solo. Hipótesis a medir,
en este orden:

1. **Contención de atómicos en el camino de descarga** (`wgpu_try_offload`, rev. 70):
   cada voz descargada hace `InterlockedAdd(&g_wgpu_frames)` e
   `InterlockedIncrement(&g_wgpu_count)` desde todos los workers de render; con 1 worker
   no hay contención. Arreglo probable: reservar tramos de peticiones por worker (un
   atómico por tile de 256 voces, no por voz).
2. **False sharing de las peticiones**: 160 bytes por petición, escritas por distintos
   workers en ranuras contiguas que comparten líneas de caché. Arreglo: tramos por
   worker o padding a múltiplos de 64 bytes.
3. **`reduceMix` es O(frames × 2 × peticiones)**: cada muestra de salida recorre todas
   las peticiones leyendo su struct. Con miles de voces son decenas de millones de
   iteraciones por bloque. No depende de los workers, pero puede dominar. Arreglo:
   reducción en árbol por tiles de voces (como Kestrel) o acumular por segmento.
4. **Costo que el CPU no se ahorra**: el avance de estado por muestra de cada voz
   descargada (posición, envolvente) sigue en el CPU, y en modo GPU se desactiva el lote
   SIMD rápido de voces en sustain.
5. JS: `submit()` copia todas las peticiones con `HEAPU8.buffer.slice` por bloque y
   espera `mapAsync`; con hasta 8 bloques en vuelo la latencia de la GPU puede vaciar el
   ring.

Medición sugerida: modo determinístico (`ssw_set_deterministic(1)`), mismas voces,
1 vs 12 workers, tiempo de `renderQueuedWithSysex` y de `submit` por separado, en
Chromium. El emulador de host (`GPUEMU=1` en `midibench`) sirve para la parte de CPU.

**B. "¿Dónde están los controles?"** Sin contexto suficiente (puede referirse a WASMIDI
Audio, al panel de WasmiSynth de la app o a otra cosa). Preguntar antes de actuar.

**C. "Una vez minimizadas no deben estar usando CPU o GPU, pero deben poder reanudarse
lo antes posible con el menor costo posible."** Aclarar qué se minimiza: la pestaña o
ventana del navegador (hoy `requestAnimationFrame` se frena solo, pero los workers
siguen: prefetch del parser, armado de tiles, render del synth por adelantado), paneles
de la app, o las ventanas de WASMIDI Audio. Si es la pestaña: con audio sonando el synth
debe seguir; lo visual (tiles, barridos del renderer, snapshots del teclado/gráficos)
debería pausarse con `visibilitychange` y reanudar desde el estado guardado (caché de
tiles y snapshots de §48) sin reconstruir todo.

**D. "Optimízalos para que usen lo menos posible en recursos, pero que no se vean
choppy, con bajos frames o de mala calidad."** Probablemente sobre lo mismo que C (lo
visual). Aclarar alcance y medir antes (fps, CPU por hilo, GPU) en Chromium.

**E. Bug: "algunas veces, luego de cambiar de synth, el programa descarga la soundfont y
no puede cargar otra."** Sin investigar. Puntos a revisar: el cambio de backend del synth
en `mainwindow.cpp` y `snappysynth_bridge.js` (reinicialización del worker, estado
`soundfontLoaded`, montajes WORKERFS que no se desmontan, `configure` que reinicia el
core y descarta el SF sin volver a cargarlo, carreras entre `configure` y
`loadSoundfontFile`). En rev. 70 también se agregó `setupGpu` tras `initCore` y tras la
carga del SF: verificar que no interfiera. Reproducir primero con los pasos exactos del
usuario.

### Otros pendientes vigentes

- Confirmar en el navegador que el shader WGSL compila y que el pipeline WebGPU anda
  (rev. 70, no verificado).
- Objetivo pedido por el usuario: 60 fps mínimo y 1024 voces constantes en The Nuker 3
  sin colgarse. Plan propuesto (aceptado en parte): benchmark de reproducción en
  Chromium; teclado y gráficos sin decodificar (conteos precalculados y barrido del
  roll); decodificación repartida durante la reproducción usando los Workers de la carga;
  tiles culleados repartidos (el culling por partes es exacto con la regla de capas).
  La admisión y el tope de robos (§61) ya cubren la parte del synth.
- Reconstrucción del teclado en seeks lejanos de The Nuker (~15 s; presupuesto de
  snapshots agotado).
- Que el coordinador de `voice_render_float` también tome tiles de render (§62).
- Montar en el CI un paso de benchmark con Chromium headless.
- Largo plazo: GUI cosmética (§11), mapeo GM de instrumentos y drums, precisión de CC,
  carga desde archivos comprimidos (.zip/.7z/.gz/.zst), hash de zstd fijo en
  `FetchContent`.

### Qué darle a Claude Code

1. **El repositorio** (este árbol, idealmente como el repo de GitHub clonado, con el
   workflow de Actions). Contiene `CLAUDE.md`, `HANDOFF.md`, el código, las herramientas
   de verificación y los bancos de prueba.
2. **Archivos de prueba** (no están en el repo por tamaño/licencia), en una carpeta fuera
   del repo o en `.gitignore`: `Hypernova.mid` (26 pistas, 960 TPQN; zona densa
   100-104 s), `MatheryLSD.sf2` (11 regiones), y para carga/escala The Nuker 3 / 4
   (15,9 / 20,9 GB) si hay disco. El MIDI sintético de `check.sh` se genera solo.
3. **Opcional, como referencia de solo lectura** (fuera del repo): el SnappySynthV2
   original, BPFA, Kimrashi, Kestrel, SuperVirtualMIDISynth, Kasaria y el sitio
   `dekx_cc-main` (estilo visual de dekx.cc). Recordar las licencias (`CLAUDE.md`).
4. **Si se quiere medir en el navegador**: el sitio compilado por el CI (`site/`, artefacto
   de Pages) o un entorno con emsdk 3.1.56 + Qt 6.8.3 para compilar.

Los transcripts del chat no hacen falta: todo lo relevante está en este documento.

Última revisión entregada: `wasmidi-main-rev72-handoff-claude-code.zip`.

---

## 67. Revisión 73 — GPU útil, SF2 que no recarga, y WASMIDI Audio con ventanas

**Reportes del usuario:** (1) 12288 voces con 12 workers van más lento que 1024 con 1
worker; (2) el modo GPU va mucho más lento con 12 workers y con 1 worker rinde igual que
CPU; (3) a veces, después de cambiar el synth, se descarga la soundfont y no se puede
cargar otra; (4) WASMIDI Audio: ventanas de letras/marcadores, gráficos estilo Winamp y
visualizador WebGL2 de espectro en espejo; todas movibles y minimizables menos la
principal; minimizadas sin usar CPU/GPU. Meta declarada: The Nuker 3 y 4 en tiempo real
a 1024 voces de principio a fin, sin quedarse sin frames, en un Ryzen 9 5900X.

### GPU (`Voice/voice.c`, `web/snappysynth-webgpu.js`)

Causas: el CPU avanzaba el estado de cada voz descargada **muestra por muestra**
(costo del mismo orden que renderizarla) y reservaba cada petición con dos atómicos
compartidos (contención entre 12 workers).

- **Avance en forma cerrada** (`wgpu_env_advance`): ataque/decay lineales, release
  exponencial con `powf`/`logf`, note-off pendiente partiendo el bloque, posición
  `pos + pitch·n` con `fmodf` para el loop. O(1) por voz.
- El shader y el emulador también calculan la posición en forma cerrada (contador de
  muestras avanzadas), así CPU y GPU coinciden.
- **Reserva por tile:** cada worker escribe las peticiones de su tile en un búfer propio
  (`g_wgpu_local[widx]`, 256 entradas) y las copia con un solo `InterlockedAdd` al final
  del tile (`wgpu_flush_tile`); el lugar se verifica al tomar el tile
  (`wgpu_tile_has_room`, margen de 256 × (workers+1)).
- Al reinicializar el core, el JS apaga la descarga hasta que `setupGpu` la reactiva
  (antes el motor podía seguir descargando voces que nadie renderizaba).

Medición (host, Hypernova 100-102 s, 1 worker, costo del lado CPU sin contar la GPU):
1024 voces 6,24 → **2,84 ms**; 12288 voces 56,9 → **9,27 ms** por bloque de 11,6 ms.
Audio con la GPU emulada: mismo nivel (79,48 dB); correlación 0,97 con el modo CPU (la
posición en forma cerrada es más exacta que la suma sucesiva del CPU y difiere en fase
en notas largas). Determinístico con 1 y 4 workers.

### SF2 que no se puede recargar (`snappysynth-worker.js`, `snappysynth_bridge.js`)

Camino encontrado: si una reconfiguración (voces, workers, bloque…) hace fallar
`ssw_init_ex` (p. ej. sin memoria para tantas voces), `initCore` lanzaba, `coreReady`
quedaba en `false` y **toda** carga de SF2 posterior fallaba con "core is still
starting"; el bridge ya había marcado la soundfont como descargada.

- El worker guarda la última configuración aceptada (`lastGoodConfig`); si una nueva
  falla, restaura la anterior, reinicializa y avisa con un error claro.
- Un `loadSoundfont` que llega mientras el core se reinicia se encola y se procesa al
  terminar.
- `loadSoundfontFile` del bridge ahora espera la respuesta real del worker y devuelve si
  cargó (antes devolvía `true` al enviar el mensaje).

### Letras, marcadores y densidad de notas (parser)

- `bpfa_midi_store`: el escaneo del índice pide al decodificador los metas de texto
  (0x01, 0x05, 0x06) con `wantText` (los demás decodificadores no los ven, así que no
  cambia `order`); hasta 200.000 eventos de 256 bytes. Cuenta note-ons por cubetas de
  `PPQ/8` ticks (`density_`). Ambos van en el blob de la carga paralela (versión 2) y se
  suman/ordenan al unir. Verificado: Hypernova (6 helpers) densidad idéntica y suma =
  44.750.713 notas; MIDI sintético con letras/marcadores idéntico en un hilo y paralelo.
- Exports `wmp_text_events_js`, `wmp_text_events_size_js`, `wmp_density_js` (densidad
  agrupada a ≤ 262.144 cubetas con sus segundos de inicio); mensajes `text-events` y
  `density` en el worker del parser (texto UTF-8 con respaldo latin1).

### WASMIDI Audio (`audio.html`, `audio.css`, `wasmidi-audio.js`, bridge `getOutput`)

- **Escritorio de ventanas:** todas se arrastran por la barra de título (pointer
  capture); las no principales se minimizan con los botones de caption y se reabren con
  los conmutadores INFO / LYR / GRAPH / VIS de la ventana principal (como EQ/PL de
  Winamp). Posiciones y estado en `localStorage` (`wasmidiAudioLayout`). Con < 900 px de
  ancho, apiladas.
- **Bucle bajo demanda:** `requestAnimationFrame` solo mientras se reproduce o el
  visualizador está decayendo; si no, un temporizador de 4 Hz para lecturas. Cada
  ventana solo trabaja si está abierta; al reabrirla se reconstruye desde el estado
  actual (búsqueda binaria), sin historial acumulado mientras estaba cerrada.
- **Letras:** línea actual con la parte cantada resaltada y la siguiente; `.kar` (texto
  0x01, `/` y `\` como saltos, `@` encabezados) o letras 0x05 (`\r`/`\n`). Marcadores
  en lista, el actual resaltado, clic = seek. El DOM solo cambia cuando cambia la sílaba
  o el marcador.
- **Gráficos:** NPS (de la densidad del parser, ventana de 0,25 s), voces, carga y
  búfer; canvas 2D que se desplaza por copia (`drawImage` sobre sí mismo) a 20 Hz y solo
  dibuja la columna nueva.
- **Visualizador:** `AnalyserNode` (FFT 4096) conectado a la salida del AudioWorklet solo
  mientras la ventana está abierta (desconectado no trabaja en el hilo de audio); 72
  barras logarítmicas 30 Hz-18 kHz con picos que caen; WebGL2 con un quad instanciado,
  un `bufferSubData` y 3 draws por cuadro; reflejo inferior más oscuro.
- Verificado en Chromium headless (`/opt/pw-browsers`): sin errores de página; arrastre,
  minimizado y persistencia; letras, gráficos y visualizador con datos simulados
  (`globalThis.__wasmidiAudioTest`).

### Meta The Nuker en tiempo real — estado

WasmiSynth a 1024 voces no es el cuello: en la zona más densa de Hypernova cuesta
0,58× del tiempo real en un núcleo. El cuello es **alimentarlo**: The Nuker 3 a mitad de
canción tiene ~44 M eventos/s; el worker del parser los decodifica y envía todos, y el
synth los encola y recorre todos aunque la admisión deje entrar ~22 mil note-ons/s. Lo
pendiente para la meta: admisión en el parser (descartar ahí los note-ons que el synth
no admitiría, con sus note-offs), y decodificación repartida entre los helpers
persistentes de la carga paralela.

Revisión anterior entregada: `wasmidi-main-rev73-gpu-audio-windows.zip`.

---

## 68. Revisión 73.1 — paso a Claude Code

**Pedido del usuario:** mover todo el proyecto a Claude Code y recibir lo necesario.

**Cambios (sin tocar código de la app):**

- `CLAUDE.md` actualizado al estado de la rev. 73 (lo lee Claude Code al empezar):
  proyecto, forma de trabajo, build, verificación, archivos de prueba, estado, pendientes
  (la meta de The Nuker en tiempo real queda primera, esperando respuesta del usuario),
  errores a no repetir y licencias.
- Herramientas para compilar y medir fuera de GitHub Actions con las mismas versiones:
  `tools/dev_setup.sh` (emsdk 3.1.56, Qt 6.8.3 por aqt, Playwright/Chromium),
  `tools/dev_env.sh`, `tools/local_build.sh` (pruebas de host + configure + build +
  validación + `site/`), `tools/run_ci_step.py` (ejecuta cualquier paso `run:` del
  workflow por nombre, así lo local no se desincroniza del CI),
  `tools/serve_site.py` (servidor con COOP/COEP/CORP) y `tools/browser_bench.py`
  (Chromium headless: `parser` corre `parser-bench.html`, `audio` carga MIDI+SF2 en
  WASMIDI Audio y registra voces/carga/búfer/underruns por segundo, `shot` captura).
- `.gitignore`: `build/`, `site/`, `tools/data/` (archivos de prueba), `*.wav`.

**Verificado acá:** `run_ci_step.py` corre el paso de pruebas de host del CI completo
(exit 0); `serve_site.py` devuelve los tres encabezados; `browser_bench.py shot` captura
`audio.html` en Chromium headless. `dev_setup.sh` y `local_build.sh` no se pudieron
correr (sin red).

Última revisión entregada: `wasmidi-main-rev73.1-claude-code.zip`.

## 69. Revisión 73.2 — primer build local en Claude Code

**Pedido:** comprobar si el entorno de Claude Code puede correr `tools/dev_setup.sh`
(el host anterior no podía).

**Resultado:** contenedor de 4 núcleos, 15 GB, con red. `dev_setup.sh` instala emsdk
3.1.56, Qt 6.8.3 (`gcc_64` y `wasm_singlethread`) y Playwright; Qt necesitó permitir
`download.qt.io` en la red del entorno. `local_build.sh` compila los 79 objetos y arma
`site/` (≈5 min con las pruebas de host).

**Causas y cambios:**
- La validación del parser (`midi_parser_bootstrap_smoke.cjs`) fallaba con
  `Cannot convert a BigInt value to a number` al crecer la memoria: el `node` del PATH
  era Node 22 y el CI usa Node 24 (Memory64). `tools/dev_env.sh` antepone al PATH el
  Node 24 que trae emsdk.
- `browser_bench.py` no arrancaba: el Playwright de pip (1.64) pide un build de Chromium
  distinto del preinstalado. Ahora usa `$WASMIDI_CHROMIUM`, o `/opt/pw-browsers/chromium`
  si existe, o el de Playwright.
- Bit de ejecución en `dev_setup.sh`, `local_build.sh` y los `.py` de `tools/` con shebang.

**Verificado:** build completo y validaciones del CI en verde con Node 24;
`browser_bench.py shot` captura `index.html` y `audio.html` y ambas páginas cargan.

**No verificado:** benchmarks con `Hypernova.mid` y `MatheryLSD.sf2` (no están en
`tools/data/`); WebGPU (Chromium headless sin GPU real).

---

## 70. Revisión 74 — The Nuker 4: el parser alimenta al synth, copias de echo y workers que sí renderizan

**Pedido (Dekx):** que *The Nuker 4 F2.mid* (20,9 GB) suene en tiempo real sin problemas
en WASMIDI y en WASMIDI Audio con MatheryLSD; WasmiSynth a 1024 voces por worker en el
Ryzen 9 5900X + RX 9070XT y en el Ryzen 7 7435HS + RTX 2050; roll, gráficos y teclado en
tiempo real; modo GPU lo mejor posible (referencia: Kestrel); la calidad no puede
empeorar de forma audible frente a un MIDI sin robos ni descartes. Dato del usuario: en
este tipo de MIDI casi todas las notas vienen de *echo channels*, copias que empiezan y
terminan en el mismo punto, apiladas como un sándwich.

**El archivo (índice en host, `tools/host_bench/nuker_bench.cpp`):** 30 pistas, 960 PPQ,
1223 s, **2.743 millones de notas**; índice 208-232 s en host con fuente zstd (455 MB de
fuente comprimida, 1,3 GB de store). Pico **51,6 M note-ons/s en 553 s**; por décimos de
la canción, el máximo pasa de 15 M/s en 6 de 10 tramos. Para medir se usa un recorte de
550-556 s (`tools/host_bench/midi_slice.c`, 491 MB, 64 M notas, pico en 3 s del recorte).

**Causa:** el synth no es el cuello. A 51 M note-ons/s, el camino anterior
(`buildEventBatch` → JS → worker del synth → admisión en el synth) tenía que decodificar
y mover ~103-163 M eventos/s por un solo hilo, aunque después el synth descarte el 96%.

**Cambios:**

1. **El parser alimenta al synth directamente** (`BpfaMidiStore::buildSynthFeed`,
   exports `wmp_feed_*`, `web/midi-parser-worker.js`). El bridge crea un `MessageChannel`
   y le da un extremo al Worker del parser (`feeder-port`) y el otro al worker del synth;
   el synth pide `{fill, target}` y el parser responde lotes `{batch, messages, times,
   sysex…, safeUntil}`. Sin pasar por el hilo principal. `?synthsource=feeder` vuelve al
   feeder JS de §26-27 y `?synthsource=relay` al relevo por la página.
2. **Admisión en el parser** (la propuesta (a) de §67): la canción se corta en bloques
   de admisión de 512 frames a 48 kHz y en cada bloque entra como mucho el 25% de las
   voces (umbral por velocidad con histograma, igual que la admisión del synth de §61).
   Los note-offs se emparejan FIFO por (pista, tecla); el de un note-on descartado se
   descarta también. No quedan notas colgadas ni note-offs huérfanos (verificado: las
   cuentas coinciden con las del propio archivo).
3. **Copias de echo plegadas:** en cada pista, los eventos idénticos (mismo tick, mismo
   mensaje) se juntan en una unidad con la cantidad en el byte alto (hasta 256), para
   note-ons y note-offs. VOR ya renderiza una pila como una sola voz, así que la
   admisión cuenta **unidades**: con el mismo presupuesto de voces entran ~37× más
   notas distintas. El synth acepta note-offs con cantidad
   (`ssw_note_fifo_push/pop` con `count`) y su FIFO de admisión cuenta las copias.
4. **Decodificación repartida** (propuesta (b) de §67): Workers auxiliares
   (`web/midi-parser-helper.js`, `feed-init` / `feed-decode`) con su propia instancia
   del parser leen los slabs comprimidos de 16 MiB por `SharedArrayBuffer`
   (`wmp_copy_feed_slab`). Cada pedido es un tramo de `chunks × 4` bloques con puntos de
   reanudación por pista (24 B por pista); el coordinador cose los resultados en orden
   (`stitchFeedResult`) resolviendo los note-offs que cruzan tramos. Memory64 + pthreads
   no sirve: la API JS limita la memoria i64 compartida a 65536 páginas (4 GB). Se
   activan con ≥ 6 núcleos (`floor(cores/3)`, máx. 8); con menos, decodificación en un
   hilo.
5. **WasmiSynth: en Single solo renderizaba un worker.** Los workers sin voces propias
   (todos menos el 0 en Single, el modo de Auto desde la rev. 72) salían del ciclo antes
   de tomar tiles de render. La rev. 69 decía que compartían los tiles, pero el host de
   1 núcleo no podía verlo (1 contra 4 workers daba WAV idénticos porque renderizaba
   uno solo). Ahora todos toman tiles, y la compactación de la lista de voces de cada
   worker (`compact_worker_voices`) pasa al inicio del ciclo siguiente, cuando ningún
   otro worker sigue renderizando esas voces.
6. `wmp_feed_reset_js` limpia `complete` y `safeSeconds` (al volver al principio después
   del final de la canción, el feed decía "completo" sin mandar nada).
7. `tools/browser_bench.py`: `--log` (toda la consola) y `--voices=N --workers=N`.

**Medición (contenedor de Claude Code: Xeon 2,8 GHz, 4 hilos, más lento que ambas PCs):**

| Prueba | Antes | Después |
|---|---|---|
| Host, feed de 1 s del pico | 1,17 s (1 helper, tramos de 16 bloques) | 0,81 s (1 helper, tramos 4×8); **0,24 s con 4 helpers** (2×4) |
| Host, render 1024 voces, recorte del pico, 1/2/4 workers | 0,78× / 0,80× / 0,77× | **0,80× / 0,50× / 0,36×** |
| Chromium, WASMIDI Audio, recorte del pico, 1024 voces, workers auto | underruns 16 → 142 en 6 s, carga 108% | **0 underruns**, carga 62-107% |
| Igual con 3 workers | 204 underruns, carga hasta 160% | **0 underruns**, carga 44-56% |
| Chromium, 256 voces | — | 0 underruns, carga ≤ 47% |

En el pico el parser descarta el 96% de los note-ons (contando copias) a 1024 voces; el
synth descarta además ~1300 en 3 s por su propia admisión.

**Verificación:** salida de los helpers idéntica a la del feed secuencial
(`tools/host_bench/feed_parallel.cpp`, recorte del pico y MIDI sintético; y en el CI,
`synthFeedSmoke` en `tools/midi_parser_bootstrap_smoke.cjs` con una segunda instancia del
módulo como helper). `feed_check.cpp` compara el feed sin admisión contra el stream de
eventos (multiconjunto por tick, expandiendo las pilas). Modo determinístico: 1 contra 4
workers da **WAV idénticos** ahora que los 4 renderizan (`pipeline_bench`, `DET=1`).
Pruebas de host del CI: todas pasan.

**No verificado / pendiente:**
- El archivo completo de 20,9 GB en Chromium (solo el recorte del pico) y en las PCs del
  usuario; los helpers del feed no se activan en este contenedor (4 hilos).
- **El roll, el teclado y los gráficos siguen con su propia decodificación** (etapa 2):
  en host el barrido del renderer cuesta ~3,9× tiempo real en el pico. Falta repartirlo
  entre helpers igual que el feed.
- Modo GPU estilo Kestrel (etapa 3); WebGPU sigue sin probarse con GPU real.
- Comparación de calidad a oído: los WAV de `pipeline_bench` (`WAV=prefijo`) sirven
  para escuchar el pico con y sin admisión en el parser (`FEED=0`).

## 71. Revisión 75 — The Nuker 4, etapa 2: roll, teclado y contadores desde un flujo plegado que decodifican los helpers

**Pedido (Dekx, el de §70):** que en WASMIDI el roll horizontal, los gráficos y el teclado
también vayan en tiempo real con *The Nuker 4 F2.mid*. Durante el trabajo pidió además que
Claude fusione sus PRs cuando las pruebas pasen, sin esperar su aprobación.

**Causa:** el Worker del parser recorría el MIDI completo dos veces más, con el heap de
fusión de pistas (`mergePop`, un evento por vez): el barrido del roll y el estado del
teclado/contadores. En el recorte del pico (§70), en host: barrido **2,6-2,7 s** y estado
del teclado **4,7-5,0 s** por segundo de canción, en el mismo hilo que además cose el feed
del synth. Las copias de echo están casi todas en la misma pista (el 83% de los bytes del
recorte es una sola pista), así que plegarlas por pista reduce ~100 M eventos a ~2,3 M
palabras en 1,6 s del pico.

**Cambios:**

1. **Flujo visual plegado** (`BpfaMidiStore`, `visualDecodeSpan` / `visualAppend` /
   `visualTrim`): las palabras de nota y de controlador (sin 0xA0) de todas las pistas en
   orden (tick, pista), con las copias idénticas de una pista y un tick juntas en una
   palabra con la cantidad en el byte alto, como el feed del synth. Plegar dentro de una
   pista y un tick es exacto para el roll y para el teclado: entre dos copias no hay un
   evento opuesto de la misma tecla. Es el modo por defecto; `?visualstream=0` vuelve al
   heap de fusión.
2. **El barrido del roll** (`buildRenderSweepStream` + `renderStep`) consume el flujo con
   cantidades: el contador de 16 bits de cada tecla y el dueño del color avanzan igual que
   con una palabra por evento (incluido el desborde a 65536).
3. **El teclado y los contadores** (`buildLiveSnapshotStream`) leen el mismo flujo, detrás
   del barrido. Por tecla (pista, canal, nota) basta guardar cuántas notas están abiertas y
   la más nueva abierta (los note-offs cierran la más vieja, así que la más nueva solo
   cambia con un note-on o cuando la tecla se vacía); tabla plana si
   `pistas × 2048 ≤ 1 M` claves, si no un mapa. El desempate entre dueños con el mismo
   rango (dos canales de una pista) ahora es la clave más alta y no el orden del hash, en
   los dos stores (el original también), para que el flujo y la fusión den lo mismo.
4. **Retención:** `visualTrim` no descarta lo que el teclado todavía no leyó, y hasta que
   el teclado se une al flujo después de reiniciarlo no descarta nada (tope 8 M palabras).
   Ese era el error que hacía que el teclado cayera siempre a la fusión: el roll va
   pantallas adelante y recortaba el flujo antes de que el teclado llegara.
5. **El reset del teclado** (seek) toma las densidades del último segundo y los eventos de
   su tick desde el flujo cuando lo cubre, sin decodificar con la fusión. La reconstrucción
   de notas abiertas sigue saliendo de los snapshots por pista (cada 131072 eventos).
6. **Si el flujo no llega a la posición del teclado:** sin helpers, el teclado lo extiende
   él mismo (`decodeVisualLocal`; el barrido reutiliza esas palabras); con helpers, lee
   hasta donde llegó y el teclado se atrasa unos milisegundos en vez de decodificar por su
   cuenta.
7. **Helpers** (`web/midi-parser-worker.js`, `midi-parser-helper.js`): los mismos Workers
   del feed (§70) decodifican tramos del flujo visual (`visual-decode`, exports
   `wmp_visual_*`); los tramos del feed tienen prioridad. El coordinador los agrega en
   orden (`appendVisualResult`), y el barrido se detiene cuando le falta el próximo tramo y
   sigue cuando llega (`sharpRenderStarved`). El pool también arranca con el primer pedido
   del roll, no solo con el synth.
8. **Overrides en la URL** (la página le pasa su query al Worker): `?helpers=N` fuerza la
   cantidad de helpers (Brave puede reportar pocos núcleos), `?rawlimit=MiB` el límite de
   fuente cruda (`0` fuerza zstd; los helpers solo existen con la fuente comprimida),
   `?visualstream=0`, `?visualdebug=1` (cada segundo, en la consola: tiempo de barrido,
   teclado y agregado de tramos del Worker del parser).
9. **`web/visual-bench.html`** (y `browser_bench.py visual`): carga un MIDI con el Worker
   del parser y reproduce un tramo a 60 cuadros/s sin dibujar ni sonar, pidiendo el roll y
   el teclado como la app. Mide por segundo cuántos cuadros tuvieron el roll atrasado, el
   adelanto mínimo del roll y la latencia del teclado. `browser_bench.py app` maneja la app
   real (clics en el layout de 1280×900), pero en Chromium sin GPU el hilo principal no
   llega a pedir cuadros y no sirve para medir.
10. Herramientas: `tools/host_bench/visual_check.cpp` (fusión contra flujo local contra
    tramos de helper, IDENTICAL/DIFFER y tiempos por segundo de canción) y un
    `visualStreamSmoke` en `tools/midi_parser_bootstrap_smoke.cjs` (tres pistas con copias
    de echo; barrido por fusión, flujo local y tramos de una segunda instancia; estado del
    teclado del flujo contra la fusión).

**Probado y descartado en esta revisión:** un Worker propio para el teclado (con el estado
pasado del coordinador al Worker, y repartido por pistas). Con el flujo bien retenido el
teclado cuesta 0,03-0,05 s por segundo de canción en el hilo del parser, así que se quitó.

**Medición (host, contenedor de 4 hilos, recorte del pico, 60 cuadros/s, roll 2 s adelante):**

| Camino, por segundo de canción en el pico | Fusión (antes) | Flujo local | Flujo con helpers |
|---|---|---|---|
| Barrido del roll | 2,5-3,2 s | 0,72-0,90 s | 0,05 s en el coordinador + 0,8-1,0 s en los helpers |
| Teclado y contadores | 4,8-4,9 s | 0,031-0,045 s | 0,032-0,039 s |
| Reset del teclado en 2,5 s | 0,21-0,25 s | 0,22-0,27 s | 0,20-0,22 s |

**Medición (Chromium headless, contenedor de 4 hilos, `visual-bench.html` con el recorte del
pico, 1,5 s + 4 s, roll 2 s adelante, 60 cuadros/s):**

| Query | Roll atrasado | Adelanto mín. | Teclado | Latencia del teclado |
|---|---|---|---|---|
| `?helpers=3&rawlimit=0` (flujo + helpers) | 0 de 243 cuadros | 0,07 s | 56/s de media, 60/s pasado el arranque | media 2,8 ms, máx. 225 ms (el primer reset) |
| `?helpers=0` (fuente cruda, flujo local) | 132 de 244 | se atrasa en el pico | 25/s | — |
| `?visualstream=0` (fusión, como antes) | 232 de 244 | segundos atrás | 0,5/s | hasta 1,4 s |

WASMIDI Audio con helpers y 1024 voces: 0 underruns (el synth y los
tramos visuales comparten el pool sin pisarse).

**Verificación:** `visual_check` en el recorte del pico (2,5 s + 1,6 s, y la canción entera
del recorte): altas, cierres y estados del teclado **IDENTICAL** entre fusión, flujo local y
tramos de helper. `tools/feeder_check/check.sh`: 24 IDENTICAL, 0 DIFFER (con el desempate
nuevo en los dos stores). Smoke del parser del CI: OK. Pruebas de host del CI: pasan.

**No verificado:** el archivo completo de 20,9 GB en Chromium; las PCs del usuario (con 16
y 24 hilos hay 5-8 helpers, acá 3 forzados en 4 hilos); el roll dibujado con GPU real.
Pendiente aparte: con un MIDI de menos de ~3-6 GB la fuente queda cruda y no hay helpers
(ni para el synth ni para el roll); compartir la fuente cruda con los helpers es lo
siguiente si hace falta.

## 72. Revisión 76 — El teclado y el roll se congelaban al empezar a mitad de The Nuker 4

**Pedido (Dekx):** en `visual-bench.html` con *The Nuker 4 F2.mid* completo (Chrome 155,
24 hilos, 8 helpers) desde 0 todo iba en tiempo real, pero desde 548 s el roll se quedaba
quieto y el teclado no respondía nunca en 15 s.

**Causa:** el primer pedido del teclado después de un salto reconstruye qué notas están
abiertas (`rebuildVisualStateAt`) desde el último snapshot de cada pista. El presupuesto de
notas de los snapshots (16 M) era global: en The Nuker 4 cada copia de echo es una nota
abierta, así que cada snapshot tiene miles de notas y la pista densa agotaba el presupuesto
en los primeros minutos. En 548 s había que decodificar desde muy atrás: **45 s** en host
(un hilo), y en Chromium más, con el Worker del parser bloqueado (ni roll ni teclado).

**Cambios** (`BpfaMidiStore`):

1. **Presupuesto de snapshots por pista**, proporcional a sus bytes (mínimo 4096 notas).
   Cuando una pista lo agota se queda con uno de cada dos snapshots y duplica el intervalo,
   así que siguen cubriendo toda la canción. Mismo total de memoria.
2. **Snapshots compactos para el teclado** (`liveSnapshots`): por (canal, tecla) con notas
   abiertas solo la cantidad y el inicio de la más nueva (12 bytes por tecla, como mucho
   2048 por pista, en vez de una entrada por copia). Presupuesto propio de 4 M teclas
   (~48 MB) con el mismo reparto y raleo. Viajan en el blob de la carga paralela.
3. **`liveRebuildAt`:** el reset del teclado en modo flujo siembra las teclas desde el
   snapshot más cercano (compacto o de notas) y decodifica el resto plegado
   (`decodeTrackUntil`) directo a la tabla plana de teclas, sin el mapa de listas de notas.
4. **Densidades del reset sin la fusión:** cuando el flujo visual no cubre el último
   segundo, cada pista se decodifica plegada desde su checkpoint y los puntos se ordenan por
   tick; los eventos del tick actual también salen de ahí.

La reconstrucción de notas del roll (`rebuildVisualStateAt`, páginas y fusión) no cambió;
solo usa snapshots repartidos de otra manera.

**Medición:**

| Reset del teclado en 548 s, The Nuker 4 completo | Antes | Después |
|---|---|---|
| Host, un hilo (`reset_probe`, carga simple o paralela simulada) | 45,3 s | 0,13 s |
| Chromium, contenedor de 4 hilos, `?helpers=3` | > 15 s (nunca respondió) | 0,19 s |

En ese Chromium, desde 548 s: el teclado responde 61 veces por segundo desde el primer
segundo; el roll va en tiempo real hasta 552 s y se atrasa entre 553 y 558 s (el pico),
porque 3 helpers en 4 hilos no alcanzan a decodificar el pico completo; se recupera solo en
559 s. En las PCs del usuario hay 8 helpers.

En el recorte del pico, un salto justo después del pico (4,2 s, densidades sobre el segundo
más denso) pasa de 2,0 s (fusión) a 0,6 s en host.

**Verificación:** `visual_check` en el recorte del pico desde 0,3 s, 2,5 s, 3,3 s y 4,2 s:
altas, cierres y estados del teclado IDENTICAL entre fusión, flujo y helpers.
`tools/feeder_check/check.sh`: 24 IDENTICAL (incluida la carga paralela simulada con zstd).
Smoke del parser del CI: OK (en el build local).

**No verificado:** las PCs del usuario; saltos justo después del pico en el navegador (las
densidades todavía decodifican ese segundo en el Worker del parser: ~0,6 s en host).

## 73. Revisión 77 — Las notas muy cortas dejaban huecos en el roll

**Pedido (Dekx):** "las notas muy pequeñas desaparecen dejando un hueco", en todo The Nuker 4.

**Causa:** los tiles culleados (§38) se cullean sobre su propia grilla de píxeles, pero se
dibujan corridos una fracción de píxel según dónde cae el inicio del tile en el cuadro. El
culling decidía por centros de píxel: una nota corta que cubría el centro de una columna la
reclamaba entera y descartaba las notas de atrás en esa columna. Corrida la grilla, la nota
corta ya no cubre la muestra de esa columna (o cubre otra) y lo que había atrás no se dibuja:
queda el fondo. Al revés también: una nota que no cubría ningún centro se descartaba aunque en
el cuadro corrido sí pinta. En `tools/renderer_check/cull_tile_sim.cpp` (flujo sintético
denso, tiles y cuadros desalineados) el 0,135% de los píxeles salía distinto del dibujo sin
tiles.

**Cambio** (`CullViewport`, `src/renderer/note_raster_compositor.cpp`): una columna queda
tapada solo cuando las notas de adelante cubren todos sus ticks. Las columnas que una nota
cubre enteras se marcan en el bitset de siempre. En las columnas de los bordes se guarda el
rango de ticks cubierto (bitset `partial` + `partialRange`, sin limpiar el arreglo de rangos;
los rangos que se tocan se unen y una columna pasa al bitset cuando el rango la cubre entera).
Una nota sobrevive si alguna columna que toca no está tapada en su tramo. La comparación es en
ticks enteros, no en píxeles: la GPU calcula los bordes de cada nota con la misma función
monótona del tick, así que si los ticks de una nota están dentro de la unión de las de
adelante, cualquier muestra que pinte también la pinta alguna de adelante, con cualquier
corrimiento y cualquier redondeo. La regla de capas no cambia.

**Medición:**

| | Antes | Después |
|---|---|---|
| `cull_tile_sim`: píxeles distintos (362 cuadros desalineados) | 0,1351% | **0** |
| `cull_tile_sim`: notas que quedan en los tiles | 42,7% | 43,6% |
| Tiles de 1920 columnas, mismo flujo: notas que quedan / tiempo | 2,82 M / 315 ms | 3,09 M / 348 ms |

En los conjuntos sintéticos del CI (`tools/note_raster_compositor_check.cpp`, notas de
fracción de píxel) se descarta menos (crashpoint 1M: 76% → 57%; raster 640: 73% → 25%) porque
ahora se conservan las notas que no cubren un centro en la grilla del tile y sí pintan corridas.
El check cambió en eso: la nota sin centro debe sobrevivir, y se agregaron dos casos (una nota
que tapa media columna no esconde a la de atrás; dos que entre las dos la cubren sí).

**Verificación:** check del compositor TODO OK, `cull_tile_sim` 0 píxeles distintos,
`tools/renderer_check/check.sh` compila, build local completo (pruebas de host del CI + wasm +
validación del sitio) OK.

**No verificado:** en las PCs con The Nuker 4 (si queda algún hueco con `?tiles=0` también,
es otra cosa: sin tiles cada nota de menos de un píxel que no cubre un centro no se dibuja,
igual que antes).

## 74. Revisión 78 — WasmiSynth renderiza cuatro voces a la vez; benchmarks y WASMIDI Audio en segundo plano

**Pedido (Dekx):** "aun se acaba rapido el buffer en algunas partes del midi, recuerda nuestra
meta"; "el player solo sigue sin trabarse con 6144 voces en el ryzen 9, la meta eran 24576, y
aun falta el gpu synth con kestrel". Y: "No hay forma de que al menos en benchmarks se pause el
test cuando cambio de ventana o pestaña? y en wasmidi audio que suene asi esté minimizado o en
otra pestaña pero que todo lo visual quede congelado mientras?"

**Causa (synth):** con callgrind sobre `pipeline_bench` (recorte del pico de §70, 24576
voces, 4 workers) casi todo el costo está en el bucle genérico por muestra de `worker_cycle`
(biquad, interpolación, envolvente): ~7-9 ns por voz y muestra, escalar. Los eventos, la
reserva de voces y los robos son marginales ("alloc" suma esperas de todos los workers, no CPU).

**Cambio (synth, `Voice/voice.c`):**

1. Las voces estéreo que llegan al bucle genérico por muestra (`vor_fast_ok`, salida a 1 o 2
   canales, sin medio rate) se **estacionan** (`voice_park`, hasta `RENDER_TILE_VOICES` por
   tile, en almacenamiento `__thread`) con todo el estado que ese bucle lee, y al final del
   tile `voice_park_flush` las renderiza **de a cuatro** con vectores de 4 carriles
   (`vq_f4`/`vq_i4`, extensiones vectoriales de GCC/clang: SSE en host, SIMD128 en wasm).
2. Cada carril hace la misma aritmética por muestra que el bucle escalar. La envolvente se
   escribe uniforme como `env*m + a` (ataque, decay, sustain y release con su `m` y `a`).
3. **Tramo apretado sin máscaras:** mientras ningún carril tiene que salir, el bucle no
   selecciona por carril; los carriles vacíos tienen una muestra muda, ganancia 0 y
   envolvente constante. El tramo se acota por los cuadros de inicio y fin, por el margen al
   final de la muestra (`2 + frames*endpos/2^23`) y se corta cuando la envolvente cruza sus
   límites (`en >= hi` o `en <= lo`). La constante "sin límite" es `FLT_MAX` y no `INFINITY`
   (el wasm compila con `-ffast-math`).
4. Un carril sale al camino escalar (`voice_run_scalar`, el mismo bucle con
   `advance_voice_sample_state`) desde el cuadro en que necesita algo que los vectores no
   cubren: zona de diezmado (< 0,01), fin de una muestra sin loop, o un note-off pendiente en
   ese bloque. `voice_park_finish` hace la cola de siempre (`voice_done_ws`, compactación).
5. La suma de los cuatro carriles es un árbol fijo `(l0+l1)+(l2+l3)`, así que el resultado es
   determinístico y no depende de cuántos workers haya.
6. Rollback: `-DSSW_VOICE_QUAD=0`.

**Cambio (páginas):**

- `web/visual-bench.html`: con la pestaña oculta (`visibilitychange`) el test se pausa
  ("En pausa (pestaña oculta)"), el tiempo oculto no cuenta, las muestras de latencia del
  teclado que cruzan una pausa se descartan, y el informe final dice cuántos segundos estuvo en
  pausa. En la corrida de la canción entera que mandó el usuario (cuadros raros en 889-1033 s)
  la pestaña había quedado oculta.
- `web/wasmidi-audio.js`: el feed del parser al synth y el render ya viven en Workers (§70) y
  el audio sale por el AudioWorklet, así que la página suena en segundo plano. Lo que dependía
  del hilo principal era el transporte del camino de respaldo (`scheduleAhead` con
  `?synthsource=relay/feeder`) y el fin de la canción, que solo corrían en
  `requestAnimationFrame` (no corre con la pestaña oculta). Ahora `transportTick` hace eso y,
  con la página oculta, lo llama el timer de 250 ms sin tocar el DOM: reloj, ventanas, letras,
  gráficos y visualizador quedan congelados y vuelven al mostrarse. Chrome no frena los timers
  de una pestaña que suena.
- `tools/browser_bench.py audio ... --hide=S`: desde el segundo S la página se reporta oculta;
  imprime el reloj que muestra la página (`shown`) junto al del audio.

**Medición (synth):**

| | Antes | Después |
|---|---|---|
| Host, núcleo del render por voz y muestra (tramo apretado) | 1× | ~1,5× más rápido |
| Host, `pipeline_bench` pico, 24576 voces, 4 workers: render (veces tiempo real) | 2,97-3,28× | 2,16-3,14× (worker busy 106-120 → 74-110 ms/bloque; el contenedor es ruidoso) |
| Chromium headless, WASMIDI Audio, recorte del pico, 6144 voces, 3 workers: carga máx. | 162% | 128% |
| idem, underruns | 368 | 128 |

Diferencia de salida contra el render anterior (`DET=1`, recorte del pico, 3 s): máximo 1 LSB,
SNR 105,4 dB. Con distintas cantidades de workers en modo determinístico: IDÉNTICO.

**Medición (páginas, Chromium headless):** `visual-bench` con la pestaña "oculta" 3 s de 5 s:
305 cuadros, informe "en pausa 3.0 s con la pestaña oculta". WASMIDI Audio, recorte del pico,
1024 voces, 3 workers, oculta desde el segundo 2: el reloj de la página quedó en 00:01, el audio
siguió hasta el final con 0 underruns y el fin de la canción se detectó estando oculta.

**Verificación:** `gcc -fsyntax-only` de `voice.c` con y sin `SSW_VOICE_QUAD`, arneses del CI
del synth (`ssw_schedule_harness`, `worker_freelist_borrow_check`, `worker_cycle_smoke`),
`node --check` de `web/*.js`, build local completo (pruebas de host del CI + wasm + validación
del sitio) OK.

**No verificado / pendiente:**

- Las PCs del usuario. SIMD por CPU da ~1,3-1,5×: no alcanza para 24576 voces (hoy 6144 en el
  5900X). Lo que falta es la etapa 3, el synth por GPU estilo Kestrel (pasos por bloque en la
  GPU: robo/alta/render/reducción/compactación, pool SoA, fase 32.32, suma en árbol fijo,
  note-offs por tabla canal×tecla). El híbrido de las rev. 70-73 sigue sin probarse en una GPU
  real.
- Una pestaña realmente oculta (en headless se simula `document.hidden`; el freno de timers de
  Chrome no se emula).
- En Chromium, el contador de notas descartadas del recorte del pico salió más alto con el
  render nuevo (98.597 contra 22.621 en corridas distintas); el modo determinístico en host da
  la misma salida, así que parece depender del tiempo del navegador. Revisar si se repite.
- El roll atrasado en 830-858 s de la canción entera en la PC del usuario (pierde ~0,28 s por
  segundo con 8 helpers; en host los helpers decodifican ese tramo a 1,18 s por segundo de
  canción entre todos). Hacen falta las líneas `[WASMIDI visual]` de la consola en ese tramo.

## 75. Revisión 79 — Modo GPU hacia 24576 voces: todas las voces a la GPU, suma por cuadro y note-offs apilados en una pasada

**Pedido (Dekx, el de §74):** 24576 voces (1024 × 24 workers) y el synth por GPU estilo Kestrel.

**Medición de partida (host, contenedor de 4 hilos, recorte del pico de §70, 24576 voces,
4 workers, `DET=1`):** con `pipeline_bench GPUSIDE=1` (nuevo: las voces van a la lista de
peticiones de WebGPU y no se renderizan, o sea el costo del lado CPU del modo GPU) el modo GPU
costaba **3,0×** tiempo real y el modo CPU 4,4×. Callgrind del lado CPU del modo GPU:

- `keyqueue_apply_noteoff_cutoff`: 22,7 M llamadas en 1 s de canción. Un evento de note-off
  plegado (copias de echo, `stack_count`) hacía una llamada por copia y cada una recorría la
  lista abierta de la tecla. Todo en la fase de eventos, que es serial (reparto Single).
- 27% de los bloques de voz no iban a la GPU: `wgpu_try_offload` rechazaba las voces apiladas
  por VOR con un note-off pendiente (`vor_stack_count > 1`), que son casi todas en The Nuker 4,
  y se renderizaban en la CPU por el camino escalar.
- `wgpu_try_offload` también rechazaba las voces de muestras sin loop que llegaban al final
  dentro del bloque.

**Cambios:**

1. **Note-offs apilados en una pasada** (`keyqueue_apply_noteoff_batch`, `Voice/voice.c`): los
   n note-offs de un evento plegado se aplican con una sola compactación de la lista abierta y
   un puntero que avanza. Si la lista está ordenada por `key_order_id` (lo normal: se agrega en
   orden de note-on) es O(lista + n); si no, cae a buscar por cada cutoff como antes. Resultado
   igual al de n llamadas del camino FIFO: WAV **idéntico** en modo determinístico.
2. **Voces VOR con note-off pendiente a la GPU:** el avance en forma cerrada aplica
   `vor_apply_pending_note_offs` en el cuadro del note-off como `advance_voice_sample_state`;
   si la voz conserva copias sigue sonando y la petición va con `pending_off = -1` para que el
   kernel no la suelte.
3. **Muestras sin loop que se acaban:** se descargan igual. El kernel deja de sonar la voz
   cuando la posición llega a `playback_end` y no interpola más allá del final; la CPU la da
   por terminada (`ENV_OFF`) cuando su posición pasa el final. En la CPU esa voz seguía en un
   release mudo ocupando un lugar del pool.
4. **Suma de voces en la GPU** (`web/snappysynth-webgpu.js`): antes cada voz escribía su fila
   en un scratch empaquetado y `reduceMix` recorría, por cada muestra de salida, todas las
   peticiones del bloque (structs de 160 bytes). Ahora el scratch es una matriz por cuadro con
   una columna por voz (`(cuadro × columnas + voz)`, se limpia con `clearBuffer`), así las
   escrituras de un workgroup y las lecturas de la suma son contiguas, y la suma son dos pasadas
   sin barreras: tramos de 256 columnas por cuadro y después los tramos de cada cuadro, siempre
   en el mismo orden. Una voz tiene a lo sumo una petición por segmento y los segmentos no se
   pisan en cuadros, así que la columna por voz no choca (el estado del filtro por voz ya lo
   suponía).
5. `tools/host_bench/pipeline_bench.cpp`: `GPUSIDE=1`. `midibench.c` (`GPUEMU`): el emulador
   del kernel sigue los cambios 3.

**Medición (host, 24576 voces, 4 workers, `DET=1`, pico, 2 s):**

| | Antes | Note-offs en una pasada | + todas las voces a la GPU |
|---|---|---|---|
| Modo CPU, render | 4,39× | 3,08× | — |
| Modo GPU, lado CPU (`GPUSIDE=1`) | 3,02× | 1,73× | **0,90×** |
| Fase de eventos (`alloc`, suma de los 4 workers), 2.º segundo | 155 ms/bloque | 45 ms/bloque | 39 ms/bloque |
| Bloques de voz que van a la GPU | 73% | 73% | 100% |

Sin `DET` (con el límite de render, como en el navegador) el lado CPU del modo GPU ya estaba en
~0,95× porque el límite descarta notas. A 6144 voces en modo CPU determinístico: 0,91× → 0,76×.

**GPU emulada (SwiftShader, Chromium headless del contenedor):** la suma nueva da **2×** más
cuadros por segundo con 1024 y con 6144 voces. Una página de prueba con los dos kernels sobre
las mismas peticiones sintéticas (1 o 2 segmentos, filtros, loops, note-offs): diferencia
máxima relativa 5·10⁻⁶ (orden de la suma) y 24576 voces en 668 contra 1319 ms por bloque.
SwiftShader no representa una GPU real.

**Audio del modo GPU** (`midibench GPUEMU=1`, 1024 voces, recorte del pico, 3 s): igual que en
la rev. 73, la forma de onda no se puede comparar muestra a muestra con el modo CPU (la posición
en forma cerrada corre la fase y en un MIDI tan denso las señales se descorrelacionan), pero el
nivel por segundo queda dentro de 2-3% y el espectro a 0,55 dB de media del modo CPU, igual que
antes de esta revisión (0,56 dB).

**Verificación:** arneses del CI del synth y build local completo (pruebas de host del CI +
wasm + validación del sitio) OK; WASMIDI Audio en Chromium con GPU (SwiftShader): estado "active", sin errores de
validación.

**Pendiente / no verificado:**

- Una GPU real (la RX 9070XT y la RTX 2050 del usuario): cuánto tarda el kernel con 24576 voces
  y si el pipeline de hasta 8 bloques alcanza.
- Lo que queda en el lado CPU del modo GPU es la fase de eventos y reserva de voces (serial con
  el reparto Single): robos (`steal_voice_fast`), un ordenamiento por edad de voces, la
  preparación de las peticiones. Siguiente paso de la etapa 3.

## 76. Revisión 80 — Auto reparte los eventos con Hash por encima de 4096 voces; búsqueda de regiones y note-offs más baratos

**Pedido:** seguir hacia 24576 voces estables (1024 por worker en el Ryzen 9 5900X); hoy el
usuario solo llega a 6144.

**Causa:** con todas las voces en la GPU (rev. 79) lo que queda en la CPU es la fase de eventos
(note-ons, robos, note-offs). Con el reparto Single (Auto desde la rev. 72) esa fase corre en un
solo hilo mientras los demás esperan, y con 24576 voces cada robo recorre hasta 512 voces del
pool. En el contenedor (4 hilos) son ~8 ms por bloque de 10,7 ms en el pico de The Nuker 4: con
24 workers el render se reparte, pero esa fase no, así que es el techo también en modo CPU.

**Cambios:**

1. **Auto = Single hasta 4096 voces, Hash por encima** (`Voice/voice.c`, selección del reparto).
   Con Hash cada worker procesa los eventos de sus teclas (canal × 128 + tecla, módulo
   workers) con su propio pool: 24576 voces con 24 workers son 1024 por worker, y cada robo
   recorre un pool chico. La rev. 72 había elegido Single con mediciones de 1 núcleo en modo CPU
   (Hash costaba más CPU total con el render del pool lleno); con el render en la GPU y los
   arreglos de la rev. 79 eso se invirtió: Hash es más barato incluso en CPU total.
   Channel/Hash/Single en el botón siguen igual.
2. **Búsqueda de regiones SFZ** (`Parser/sfz_parser.c`): `sfz_ensure_selector_cache` ponía en
   cero dos tablas de conteo (~200 KB en la pila) en cada búsqueda aunque la caché estuviera al
   día. Ahora compara la revisión primero y solo reconstruye si cambió.
3. **Note-offs apilados en una pasada, sin escaneo previo** (`keyqueue_apply_noteoff_batch`): la
   lista abierta por tecla lleva si se agregó fuera de orden (`last_order`, `unsorted`) en vez de
   comprobarlo recorriéndola en cada evento; la compactación y los cutoffs avanzan juntos en un
   solo recorrido.
4. `tools/host_bench/pipeline_bench.cpp`: `SHARD` (0 auto, 1 channel, 2 hash, 3 single).

**Medición (host, recorte del pico de The Nuker 4 con 1,5 s de arranque, 2 s, 4 workers,
`DET=1`, tiempo de render / tiempo real):**

| | Antes | Regiones | + note-offs | Single → Hash |
|---|---|---|---|---|
| 24576 voces, modo GPU (lado CPU) | 0,87× | 0,78× | 0,81× (ruido) | **0,62×** |
| 24576 voces, modo CPU | 2,96× | 2,76× | 2,73× (3 s) | 2,73× (contra 2,87× Single) |
| 24576 voces, modo GPU, 1 núcleo (CPU total) | — | — | 0,91× | **0,74×** |
| 6144 voces, modo CPU / GPU | — | — | 0,78× / 0,22× | 0,76× / 0,18× |

Espera en la fase de eventos (`alloc`, suma de los workers que esperan) con 24576 voces en modo
GPU: 24,6/34,9 → 15,6/22,8 ms por bloque. Regiones y note-offs: WAV **idéntico** en modo
determinístico (Single) a 6144 y 24576 voces.

**Audio con Hash:** con Hash el resultado depende del momento en que cada worker toma voces
libres de otro (préstamo de la lista libre), así que dos corridas no dan el mismo WAV ni con
`DET=1`; el modo determinístico byte a byte sigue siendo Single. Contra Single (24576 voces):
nivel por segundo dentro de 0,4% y espectro a 0,38-0,45 dB de media, del orden de lo que varían
dos corridas de Hash entre sí.

**Verificación:** arneses del CI del synth (`worker_cycle_smoke` con 8192 voces y 24 workers
ahora corre en Hash), build local completo.

**Pendiente / no verificado:**

- En las PCs del usuario: WASMIDI Audio y la app con 12288 y 24576 voces, modo CPU y GPU, con
  Sharding en Auto (ahora Hash) contra Single. El contenedor tiene 4 hilos: con 24 workers la
  fase de eventos se reparte 24 veces, cosa que acá no se puede medir.
- Si el reparto por teclas queda desbalanceado en algún MIDI (todas las notas en pocas teclas),
  ese worker frena el bloque; Single no tiene ese problema.

## 77. Revisión 81 — Las copias de eco plegadas suman su volumen ("Stack gain" encendido y barato)

**Reporte del usuario (rev. 80, Ryzen 9 5900X):** 12288 voces estables en WASMIDI y WASMIDI
Audio (antes 6144); a 24576 el audio cruje. En WASMIDI el roll se queda sin cuadros en varias
partes mientras el audio va bien. Las notas combinadas por el plegado de eco suenan más bajo que
en el original.

**Causa del volumen:** una voz VOR que junta N note-ons idénticos suena con el volumen de **una**
nota cuando "Stack gain" está apagado (`ssw_set_vor_mode(1)`, el default heredado de
SnappySynthV2). Con el plegado de la rev. 74 casi todas las copias de The Nuker 4 llegan como una
pila, así que una nota con 50 copias suena igual que una sola, cuando en un synth sin VOR suena
como 50 voces en fase (N veces la amplitud). Antes del plegado parte de las copias no llegaba a
apilarse y sonaba por separado.

Con "Stack gain" encendido (modo 0) la pila ya sumaba, pero salía cara: cuando una copia recibe
su note-off, la ganancia de la pila baja hacia la nueva cantidad de copias a la velocidad del
release (`vor_gain_scale` → `vor_gain_target`), y mientras dura esa bajada la voz no iba a la
GPU ni al render de 4 voces. En el pico de The Nuker 4 con 24576 voces el modo GPU mandaba 0,8 M
peticiones en vez de 4,5 M y el lado CPU pasaba de 0,6× a 3,3×.

**Cambios:**

1. **La bajada de ganancia de la pila va a la GPU** (`wgpu_try_offload`, `snappysynth-webgpu.js`):
   la petición lleva `vor_scale` y `vor_target` (antes `pad3`/`pad4`); el kernel multiplica cada
   cuadro por la escala y la acerca al objetivo con `release_alpha`, como el bucle de la CPU, y la
   CPU avanza la escala en forma cerrada (`target + (scale − target)·alpha^n`). Las voces que no
   bajan mandan 1 y 1. `midibench GPUEMU` sigue el kernel.
2. **La bajada de ganancia en el render de 4 voces** (`voice_quad_render`, `voice_run_scalar`):
   un vector por carril con la escala, el objetivo y el alpha; los carriles que no bajan llevan
   1 (multiplicar por 1 es exacto, así que el modo 1 da el mismo WAV byte a byte). Las voces que
   bajan no usan el diezmado del release, igual que el bucle genérico.
3. **"Stack gain" encendido por defecto** en WASMIDI (`synthOverlapGain_`, guardado ahora con la
   clave `synthStackGain` para que el default nuevo llegue aunque se hubiera tocado el botón
   antes), en el bridge y en el worker. WASMIDI Audio tiene la casilla "Stack gain" (Echo).
4. **Workers en Auto dejan lugar a los helpers del parser** (`snappysynth-worker.js`): con ≥ 8
   núcleos se reservan los helpers de decodificación (núcleos / 3, hasta 8) + 2, en vez de 4
   fijos. 24 hilos: 14 workers (antes 20); 16: 9 (antes 12). Con el reparto Hash (rev. 80) todos
   los workers trabajan a la vez y competían con los helpers que alimentan el roll. Un valor
   explícito de Workers sigue mandando.
5. `pipeline_bench` y `midibench`: `VORMODE` (0 suma, 1 una nota).

**Medición (host, pico de The Nuker 4, 2 s, 4 workers, `DET=1`):**

| | Modo 1 (una nota) | Modo 0 antes | Modo 0 ahora |
|---|---|---|---|
| 24576 voces, modo GPU (lado CPU) | 0,58-0,60× | 3,30× (0,8 M peticiones) | 0,88× (4,5 M peticiones) |
| 24576 voces, modo CPU | 2,27-2,68× | 3,51-3,87× | 2,93× |
| 6144 voces, modo CPU, Single | 0,73-0,85× | 1,29× | 1,00× |

Lo que queda del costo del modo 0 está en los robos: `steal_voice_fast` cuesta ~8,5 k
instrucciones por llamada contra ~3 k en modo 1 (callgrind, mismas llamadas). Con la suma, las
pilas viejas son tan fuertes como las nuevas y el robo tiene que recorrer más voces; en modo 1
la pila entrante contaba N veces y la víctima una sola, así que robaba enseguida.

**Audio:** modo 1 con los cambios: WAV **idéntico** al de la rev. 80 (6144 voces, Single, `DET`).
Modo 0, GPU emulada contra CPU (1024 voces): nivel dentro de 1,5% y espectro a 0,49 dB de media,
lo mismo que el modo GPU sin pilas (§75).

**Pendiente / no verificado:**

- En las PCs del usuario: si el eco suena como el original, y los crujidos a 24576 con Workers
  en Auto (ahora menos workers) contra 24.
- El roll sin cuadros: faltan las líneas `[WASMIDI visual]` (`?visualdebug=1`) y saber si era
  modo CPU o GPU (en GPU, el kernel del synth y el WebGL del roll comparten la GPU).
- Abaratar el robo en modo 0.

## 78. Revisión 82 — El roll ya no se atrasa en 829-858 s de The Nuker 4 y el teclado no se congela ahí

**Pedido:** el log de `visual-bench.html` de la canción entera (24 núcleos, 8 helpers, modo GPU,
24 workers) muestra el roll atrasado solo en 829-858 s (hasta 8,9 s detrás). El audio iba bien.

**Causa del roll:** los helpers tardaban el doble en decodificar cada tramo del flujo visual con
el archivo entero que con un recorte de la misma zona (host: 1,30 contra 0,62 s por segundo de
canción). Cada tramo reposiciona las 30 pistas desde su checkpoint, y los checkpoints se ponen
cada 256 eventos de canal. La pista 0 de The Nuker 4 es la de tempo: 1,9 MB sin un solo evento
de canal, así que su único checkpoint es el inicio y `decodeNext` la leía entera (con ~30
bloques zstd) en **cada** tramo buscando un evento de canal que no existe. En la zona densa
hay ~250 tramos por segundo de canción. El recorte tiene una pista de tempo corta y no lo
sufría.

**Causa del teclado (apareció al arreglar el roll):** el flujo visual se recortaba a 8 M palabras.
El teclado lo lee en el tiempo de reproducción y el roll ~2 s adelante; a ~4,3 M palabras por
segundo (830 s) la ventana entre los dos pasa de 8 M, el recorte le quitaba al teclado sus
palabras y el teclado caía al camino lento (merge sin plegar): ~1,2 s congelado cada ~3 s. Antes
no se veía porque el roll iba atrasado y el flujo nunca crecía tanto.

**Cambios:**

1. `TrackStorage::channelEnd`: byte siguiente al último evento de canal de la pista (se anota en
   el escaneo, viaja en la imagen del feed y en la carga paralela). Todos los cursores que leen
   eventos de canal (`mergeReset`, feed, tramos visuales, reconstrucciones del teclado) terminan
   ahí (`cursorEnd`): una pista solo de tempo queda terminada al instante y una cola sin notas
   no se relee. La salida no cambia.
2. Flujo visual: se guardan hasta 48 M palabras (antes 8 M) y no se planea otro tramo mientras
   haya 32 M palabras detrás del teclado (`visualRoomAhead`, también en
   `wmp_visual_span_end_js` y en la decodificación local). Así el recorte forzado no le quita
   palabras al teclado; si una zona necesitara más, el roll se adelanta menos en vez de congelar
   el teclado.

**Medición:**

| | Antes | Ahora |
|---|---|---|
| Host, decode de los helpers en 832-836 s, archivo entero | 1,30 s por s de canción (posicionar 3,3 s de 6,6) | **0,63 s** (igual que el recorte) |
| Host, 826-840 s, 2 s de adelanto: caídas del teclado al merge / recortes forzados | 156 / 4169 | **0 / 0**; teclado 0,14 → 0,06 s por s |
| Chromium (4 hilos, `?helpers=3`), archivo entero, 826-840 s: cuadros con el roll atrasado | 846 de 852 (hasta 10,5 s) | **0 de 860**, adelanto ~1,97 s |
| Igual, teclado | 59,4/s (el roll atrasado) / 36,6/s solo con el arreglo 1 | **60,1/s**, latencia media 2,9 ms |
| Host, feed del synth con 3 helpers, 826-834 s | 1,03 s | 1,05 s (sin cambio: sus pedidos son 32× más largos) |

**Verificación:** `feeder_check/check.sh` 24 IDENTICAL, ningún DIFFER; `visual_check` con el
archivo entero: roll, cierres y teclado IDENTICAL entre merge, flujo local y helpers;
`feed_parallel` IDENTICAL; pruebas de host del CI y compilación local pasan.

**Pendiente / no verificado:**

- En las PCs del usuario con la canción entera (24 núcleos, 8 helpers). Memoria: el flujo puede
  llegar a ~480 MB en zonas que lo necesiten (antes ~80 MB); a 4,3 M palabras/s usa ~100 MB.
- Siguen abiertos los crujidos a 24576 voces y el robo caro en modo "Stack gain".

## 79. Revisión 83 — Línea `[WASMIDI app]` por segundo en WASMIDI con `?visualdebug=1`

**Pedido:** el usuario aclara que `visual-bench.html` y WASMIDI Audio rinden mucho mejor que
WASMIDI, porque WASMIDI corre a la vez WasmiSynth, el roll, los gráficos y el teclado, y que todo
junto tiene que andar sin lag ni cuadros perdidos. Lo bueno en los bancos sueltos no prueba nada
sobre WASMIDI.

**Por qué medir primero:** el Chromium del contenedor no tiene GPU. El roll de WASMIDI corre ahí a
~13 cuadros/s aun con un MIDI liviano (los primeros 30 s de The Nuker 4, 1024 voces, casi sin
voces activas), y el hilo principal pasa el 88% del tiempo en código del navegador (WebGL por
software), no en wasm ni JS. Con el recorte de 828-838 s la página deja de responder al dar play.
Nada de eso dice qué pasa con una GPU real, así que hace falta la medición en las PCs del usuario.

**Cambio:** `web/wasmidi-app-debug.js` (lo inyecta el CI en `index.html`, como el bridge). Con
`?visualdebug=1` escribe cada segundo, junto a las líneas `[WASMIDI visual]` del Worker del parser:

- cuadros (requestAnimationFrame), cuántos tardaron más de 25 ms y el mayor hueco;
- tareas largas del hilo principal (PerformanceObserver `longtask`): cantidad y ms;
- teclado: respuestas/pedidos de `key-state` y su latencia media y máxima;
- adelanto del roll en ticks (`safeThrough` de los deltas contra el tick del teclado);
- synth: voces, carga de render, llenado del ring, underruns nuevos, workers y si la GPU está
  encendida.

Sin el parámetro no hace nada (ni envuelve `Worker`).

**Verificación:** compilación local y pruebas de host del CI; WASMIDI en Chromium con
`?visualdebug=1` escribe la línea cada segundo y sus números coinciden con los de
`browser_bench.py app`.

**Pendiente:** el log de WASMIDI del usuario con `?visualdebug=1` en los tramos con lag o
cuadros perdidos (en GPU y en CPU) para decidir qué compite: hilo principal, GPU compartida
entre el synth por WebGPU y el roll, o núcleos entre workers del synth y helpers.

## 80. Revisión 84 — WasmiSynth alcanza al roll después de un underrun y el roll mantiene su último cuadro

**Pedido:** dos reglas del usuario. (1) Si WasmiSynth se queda atrás del piano roll, siempre
tiene que alcanzarlo para no desincronizarse. (2) Lo mismo para el roll: si se queda sin cuadros,
se queda en el último que tiene hasta que llegue el nuevo, y si WasmiSynth va adelante, salta
hasta él ignorando los cuadros que ya quedaron atrás.

**Causa del desfase del audio:** el worklet solo contaba como tiempo reproducido el PCM que
entregaba. En un underrun el dispositivo tocaba silencio, el reloj de audio no avanzaba y el
Worker seguía renderizando desde donde iba: cada underrun dejaba el audio atrasado para siempre.
El roll corre con el reloj de pared y solo acepta que el audio lo adelante
(`presentationOffsetSeconds_` solo baja, §48), así que el audio quedaba detrás del roll.

**Cambios del synth:**

- Worklet (`snappysynth-audio-worklet.js`): en un underrun suma los cuadros de silencio a la
  palabra 7 del encabezado del ring (`RING_STARVED`, antes libre) y cuenta el silencio como tiempo
  reproducido. `flushRing` la pone en 0 antes de subir la generación.
  Si el Worker reinicia el ring (seek, play con reinicio, stop, SF2 nueva) el worklet lo ve por el
  cambio de generación y vuelve a esperar el primer bloque: ese silencio es el comienzo de otra
  línea de tiempo, no un underrun que alcanzar.
- Worker (`snappysynth-worker.js`, `catchUpToClock`): antes de cada bloque (CPU y GPU) lee los
  cuadros de silencio nuevos, adelanta `renderSongTime` esa cantidad y llama a
  `ssw_skip_to(tiempo, 0.025)`. Una generación nueva del ring reinicia la cuenta.
- Core (`snappy_wasm_core.c`): `ssw_skip_to` mueve `g_song_frame` al destino y marca
  `g_drop_before_frame` = destino − 25 ms. En `ssw_render_queued_into` los note-on anteriores a
  esa marca no suenan (van a la FIFO de notas como descartados, así su note-off también se
  descarta) y no cuentan en el histograma de admisión; los cuentan `ssw_late_dropped()`. Los
  note-on de los últimos 25 ms y todos los demás eventos (CC, programa, pitch bend, SysEx,
  note-offs de notas que sí sonaron) se aplican en el cuadro 0 del bloque siguiente, como antes,
  así el estado del canal queda bien. `ssw_set_song_time` y `ssw_reset` borran la marca.
- Estadísticas: el Worker publica `catchUps`, `catchUpSeconds` y `lateDropped`; el bridge los
  guarda en `state`; la línea `[WASMIDI app]` muestra "caught up N ms (late notes M)" y
  `browser_bench.py audio` también.

**Cambios del roll (`gl_renderer.cpp`, `pianoroll.cpp`):**

- Si las notas del cuadro no están listas (`notesReady` falso: reconstrucción pendiente o
  `sharpRemoteSafeThrough_` antes del borde derecho), `renderRoll` no dibuja nada y devuelve
  falso: el FBO de Qt conserva el último cuadro completo. Antes dibujaba el fondo, el cursor y
  solo las notas ya preparadas (cuadro vacío o cortado). Si el tamaño cambió o nunca hubo un
  cuadro completo, dibuja como antes. Los cuadros mantenidos no cuentan en los FPS.
- Si todo lo que el Worker del parser preparó (instalado o en cola) termina antes del borde
  izquierdo de la pantalla, el roll pide una reconstrucción en la posición actual (la misma de un
  seek) en vez de terminar de preparar ticks que ya no se ven. Como mucho una por segundo, y no
  durante otra reconstrucción.
- `?visualdebug=1`: la línea `[WASMIDI app]` muestra "roll held N catch-ups M".

**Verificación:**

- Host: sintaxis del core, `node --check`, nuevo caso 15 en `tools/ssw_schedule_harness.c`
  (3 note-on atrasados descartados, 1 conservado, el CC atrasado se aplica, nada contado como
  admisión), `tools/local_build.sh` completo (pruebas de host del CI + wasm + `site/`). Prueba
  de host con MatheryLSD: 1000 notas en 1 s, render hasta 0,209 s, salto de 0,5 s: 475 note-on
  descartados (los de 0,209-0,684 s), 0 voces al final (ninguna nota colgada).
- Chromium (4 hilos, sin GPU), WASMIDI Audio, `nuker4_553.mid`:
  - 24576 voces (carga 360-650%, cientos de underruns): el reloj avanza 1,00 s por segundo
    (0,83 → 5,86 s en 5 s) y la canción de 6 s termina a los 6 s. Antes ese reloj avanzaba al
    ritmo del render (~0,3 s por segundo).
  - 1024 voces: 0 underruns, 0 ms de alcance, 0 notas atrasadas (sin cambio).
- Chromium, WASMIDI completo, `nuker4_000.mid` con `--seek=6` (nueva opción de
  `browser_bench.py app`): el seek mantiene 1 cuadro y el roll sigue dibujando bien (captura).

**No verificado:** con GPU real; que la reconstrucción por atraso del roll se dispare en las PCs
del usuario (en el contenedor el roll nunca se quedó atrás en el tramo liviano y el pesado
congela la página por WebGL por software). Una nota larga que empezó más de 25 ms antes del
destino de un salto no suena: se eligió así para no disparar de golpe todas las notas del tramo
perdido.

## 81. Revisión 85 — The Nuker 3 en WASMIDI: el roll ya no dibuja millones de notas sin cullear y el teclado no bloquea el Worker del parser

**Pedido:** el usuario probó la rev. 83 en el Ryzen 7 7435HS + RTX 2050 (16 hilos). WasmiSynth no
alcanzó a 12288 voces y con The Nuker 3 Ultimate F4 (15,9 GB, 9679 pistas, 2.080 M notas) el roll
perdía cuadros sin nada pesado en pantalla y WASMIDI terminó cerrándose ("application exit").

**Causa (Chromium del contenedor, 4 hilos, SwiftShader, The Nuker 3 completo):**

1. **Roll.** Los tiles culleados (§73) solo se armaban cuando `sharpRemoteSafeThrough_` pasaba el
   final del tile. Con poco adelanto del Worker (lo normal en The Nuker 3, donde cada tramo toca
   miles de pistas) los tiles visibles quedaban crudos y el roll dibujaba 0,3-1,7 M instancias por
   cuadro. El trace mostró el hilo de la página bloqueado en Commit → `GLES2::ReadPixels`
   esperando a la GPU hasta 7,5 s; con eso la página se congelaba 10-20 s aun con 1024 voces. En
   una GPU real el costo por instancia es mucho menor, pero 1,7 M instancias con mezcla por
   cuadro siguen siendo varias veces lo que dibuja un tile culleado.
2. **Reconstrucción del roll.** La regla de la rev. 84 (reconstruir si todo lo preparado quedó a
   la izquierda) podía no salir nunca de un roll mantenido: si el Worker preparaba más lento que
   la canción, cada reconstrucción en la posición actual volvía a quedar atrás.
3. **Teclado.** Después de que el roll reiniciaba el flujo visual más adelante, el teclado quedaba
   detrás del comienzo del flujo y caía en la ruta de fusión fuera del flujo: decodificaba las
   9679 pistas en todo el hueco, hasta 34 s por petición (18 s sin helpers). Mientras tanto el
   Worker del parser no atendía barridos del roll ni el feed del synth: el synth se quedaba sin
   eventos (también cuenta para los underruns a 12288 voces).
4. **Sin helpers** (Brave puede reportar pocos núcleos: helpers = ⌊núcleos/3⌋, máx. 8) el flujo
   visual se decodifica en el Worker del parser y una petición del teclado podía decodificar de
   una vez todo lo que faltaba hasta su tick.

**Cambios:**

- `gl_renderer.cpp`, tiles parciales: cada tile guarda `coverage` = min(fin del tile,
  `sharpRemoteSafeThrough_`). Se juntan las notas hasta `coverage + 1` y las notas abiertas se
  recortan en `coverage` al cullear (`CullViewport` con `viewEndTick = coverage`). Un tile sirve si
  su cobertura llega a min(borde derecho de la pantalla, fin del tile). Con `notesReady` los
  tiles visibles se arman (o se rehacen con más cobertura) en el mismo cuadro; el prefetch solo
  da por hecho un tile completo. Se reusa el slot y el VBO. Con menos de 50 k notas el tile sube
  las notas sin cullear. El piso del culling (§73: válido en cualquier desplazamiento sub-píxel)
  deja ~2,5 notas por celda (tecla, columna): ≤ ~750 k instancias por cuadro con dos tiles.
- Reconstrucción por atraso: se dispara si todo lo preparado quedó a la izquierda o si pasó ≥ 1 s
  de reproducción sin un cuadro completo (y el cursor avanzó ≥ W/4). La reconstrucción salta
  adelante 1, 2, 4, 8 ventanas (crece en cada intento seguido y vuelve a 0 tras 5 s de cuadros
  completos). `notesReady` exige además `sharpRemoteStartTick_ <= viewStart`; mientras el cursor
  no llega al salto el temporizador de "mantenido" se renueva (si no, se reconstruía cada segundo
  sin dibujar). El barrido queda pedido hasta el destino del salto (`sharpCatchUpFloor_`) para que
  no se lea como un seek hacia atrás.
- `bpfa_midi_store.cpp`, teclado detrás del flujo: si el flujo visual empieza después del próximo
  tick del teclado, `buildLiveSnapshotStream` devuelve la última instantánea (`liveHeld_`) hasta
  que el flujo cubra la ventana de densidades y haya decodificado el tick pedido; entonces fuerza
  un reset sobre el flujo (0,30-0,34 s en host). El teclado se queda quieto un momento en vez de
  bloquear el Worker.
- Sin helpers, cada petición del teclado decodifica como mucho 0,25 s de canción del flujo; si no
  alcanza, el teclado va un poco atrás.
- La tabla de teclas del teclado en vivo pasó de `std::unordered_map` a direccionamiento abierto
  (`liveTable_`, sondeo lineal, borrado con corrimiento). No era el cuello de botella (la parte en
  vivo cuesta 0,018 s por segundo de canción en The Nuker 3); se dejó porque da resultados
  idénticos y no reserva por nodo.
- Diagnóstico:
  - `?rolldraw=0`: el roll hace todo menos las llamadas de dibujo de notas (separa CPU de GPU).
  - `?visualdebug=1`: la línea `[WASMIDI app]` agrega "drawn max N notes (raw R, resident X)" y
    "app memory N MiB" (tamaño del heap del wasm de la app); la línea del Worker del parser agrega
    los mensajes que tardaron ≥ 20 ms ("tipo X ms/N (max M)"), incluidos `feed-reset`/`feed-fill`.
  - `browser_bench.py app --profile=S:D:OUT` (CPU profile por CDP, top 30 por tiempo propio) y
    `--trace=S:D:OUT` (trace de Chromium, eventos del hilo de la página; `WASMIDI_TRACE` agrega
    categorías).
  - `-DWASMIDI_PROFILE_FUNCS=ON` deja los nombres de funciones en `wasmidi.wasm` para el profiler
    (apagado por defecto y en el CI).

**Medición (Chromium del contenedor, The Nuker 3 completo, `?visualdebug=1`):**

- Dibujo: antes hasta 1,7 M instancias crudas por cuadro; ahora como mucho ~750 k culleadas.
  SwiftShader cuesta ~10 µs por instancia y ~66 ms por cuadro aun vacío, así que los cuadros/s del
  contenedor no dicen nada sobre una GPU real.
- Lado CPU con `rolldraw=0`, `helpers=3`: roll ~0,6 M ticks adelante, teclado 100-200 ms; en un
  tramo denso se atrasó, hizo 4 reconstrucciones y se recuperó.
- `rolldraw=0`, `helpers=0`: roll ~0,7 M ticks adelante, ninguna reconstrucción, teclado ~100 ms,
  petición del teclado más larga 72 ms (antes 18 s).
- Memoria de la app 130-182 MiB en todas las corridas; el cierre no se reprodujo.

**Verificación:**

- `tools/renderer_check/cull_tile_sim.cpp`: nueva pasada con tiles parciales, 0 píxeles distintos.
- `tools/host_bench/visual_check.cpp`: nuevo `playJump` (el barrido salta 1 s adelante a un tercio
  del recorrido; compara el teclado en vivo con la fusión de referencia desde 1 s después del
  salto). IDENTICAL en many9679, nuker4_553, many30 y The Nuker 3 en 55-61 s; petición más larga
  después del salto 0,304 s.
- Pruebas de host del CI, `node --check web/*.js`, `tools/local_build.sh` completo.

**No verificado:** con GPU real (cuánto mejoran los cuadros/s en la RTX 2050 y la RX 9070XT); el
cierre "application exit" (no se reprodujo; la memoria de la app no crece); WasmiSynth a 12288
voces en el 7435HS (16 hilos: probablemente limitado por CPU; el Worker del parser bloqueado por el
teclado le quitaba eventos y eso ya no pasa, pero no se midió en esa PC).

## 82. Revisión 86 — The Nuker 3 en visual-bench: tramos visuales más largos y teclado más barato en el Worker del parser

**Pedido:** el usuario mandó un log de `visual-bench.html?visualdebug=1` con The Nuker 3 Ultimate
F4 entero (867,1 s; 16 núcleos, 5 helpers, carga 44,1 s): 52043 cuadros, 12762 con el roll
atrasado (adelanto mínimo -73,8 s) en 122-135 s, 420-561 s, 710-717 s y 817-866 s; teclado
53,8/s de media pero 3-30/s en esos tramos, con latencia de hasta 484 ms.

**Causa (host y Chromium del contenedor, 4 hilos, `?helpers=3`):**

1. **Costo fijo por tramo del flujo visual.** Cada tramo que decodifica un helper
   (`decodeVisualRequest`) retoma cada pista desde su `checkpoint` (cada 256 eventos) con
   `decodeNext` hasta el tick de inicio. Con 9679 pistas eso cuesta ~0,1 s por tramo aunque el
   tramo tenga pocas palabras. En 420-440 s (14-31,7 M note-ons/s, ~43 M palabras/s casi sin
   plegar) los tramos de 512 K palabras duraban unos milisegundos de canción: host, decodificar
   un segundo de canción a 440 s costaba 7,3 s de helper.
2. **Orden del tramo.** Las palabras salen en orden (pista, tick) y se ordenaban por tick con
   `stable_sort`: n log n sobre 9679 tiras.
3. **Teclado en el Worker del parser.** Por segundo de canción a 440 s (host): anexar 0,14 s,
   barrido del roll 0,44 s, teclado 0,79 s. El teclado (`applyLiveEvent`) buscaba cada
   (pista, canal, tecla) en una tabla hash. Además el reset del teclado reproducía cada pista
   desde su último snapshot compacto por tecla, que se tomaba cada 131072 eventos de la pista:
   110 M eventos reproducidos a 430 s (2,44 s en host, 17,8 s en Chromium).

**Cambios:**

- `midi-parser-worker.js`: el tamaño de tramo visual crece desde 512 K palabras y se duplica
  con cada tramo pedido hasta `visualSpanMax` = mín(8 M, máx(512 K, pistas × 1024)), así el
  primer tramo tras un reinicio llega pronto y los siguientes amortizan la retoma de las
  pistas. `visualRestart` vuelve al tamaño inicial. Con tramos grandes la cola espera a lo sumo
  helpers + 1 tramos (antes 2 × helpers) para no acumular memoria.
- `bpfa_midi_store.cpp`, `visualDecodeSpan`: orden por conteo de ticks (estable dentro de cada
  tick, igual que `stable_sort`) cuando el rango de ticks es ≤ 4 × palabras + 65536; si no,
  queda el `stable_sort`.
- Teclado: cada (pista, canal) recibe 128 casillas planas en su primera nota (`liveSlot`,
  `livePairOf_` indexado por pista × 16 + canal) en vez de la tabla hash; `LiveKey` baja a
  16 bytes y la lista de casillas activas se borra por intercambio. Snapshots compactos por
  tecla cada 4096 eventos de pista (antes 131072); siguen limitados por
  `LiveSnapshotBudgetKeys`.

**Medición:**

| Host, 440 s, por segundo de canción | Decodificación en helpers |
|---|---|
| tramos de 512 K palabras | 7,3 s |
| 2 M | 3,6 s |
| 4 M | 2,8 s |
| 9,9 M | 2,2 s |
| 9,9 M + orden por conteo | 1,5 s |

| Snapshot por tecla cada | Reproducido a 430 s | Reset a 430 s (host) | Reproducido a 445 s | Memoria |
|---|---|---|---|---|
| 131072 eventos (antes) | 110 M | 2,44 s | 142 M | 12 MiB |
| 16384 | 20,7 M | 0,48 s | — | 37 MiB |
| 4096 (ahora) | 9,2 M | 0,26 s | 11 M | 59 MiB |

Microbenchmark del teclado sobre las palabras reales de 440 s: 35,9 ns por palabra con la tabla
hash, 7,5 ns con las casillas planas; 0 imágenes distintas.

Chromium del contenedor, `?helpers=3`, empezando en 415 s: reset del teclado 1,2-1,6 s (antes
17,8 s); 420-428 s casi en tiempo real; 429-455 s el roll se atrasa ~0,6 s por segundo. Ahí el
Worker del parser está saturado: por segundo, teclado ~550-700 ms, barrido ~100-250 ms, anexar
~100 ms (con 4 hilos los 3 helpers y el synth compiten por la CPU).

**Verificación:** `visual_check` IDENTICAL en many9679, many30 y nuker4_553;
`feeder_check` 24 IDENTICAL, 0 DIFFER; `tools/local_build.sh` completo; `browser_bench.py app`
con `many9679.mid` (12 s, sin errores).

**No verificado / pendiente:** en las PCs del usuario (16-24 hilos dejan más CPU al Worker del
parser que el contenedor); en 429-455 s el teclado sigue siendo lo más caro del Worker del
parser (siguiente paso: sacarlo a su propio Worker); un salto lejano todavía decodifica la
ventana de densidades desde los checkpoints (3,6 s en host a 830 s); la ráfaga de 830 s
(~121 M palabras/s) no se midió con estos cambios.

## 83. Revisión 87 — El teclado en su propio Worker

**Pedido:** lo que quedó pendiente de la rev. 86 (§82). En 429-455 s de The Nuker 3 el teclado
saturaba el Worker del parser, el mismo que anexa los tramos y barre el roll, y el roll se
atrasaba aunque los helpers alcanzaran.

**Causa (medida en host con las palabras reales de 440-442 s, 86,2 M palabras):** por segundo de
canción el Worker del parser hacía anexar + barrer + teclado, y el teclado era lo más caro.
Con las casillas planas de la rev. 86 seguía en 25 ns por palabra. Callgrind mostró que el
costo eran las llamadas por palabra (`liveConsume` → `liveAddDensity` → `applyLiveEvent` →
`liveSlot`) más que la tabla. El microbenchmark de §82 (7,5 ns) no tenía esas llamadas.
Anexar copiaba cada tramo dos veces dentro del wasm (al vector del resultado y después al
flujo): a 830 s son ~1,2 GB/s de palabras.

**Cambios:**

- **Worker del teclado.** Con los helpers activos, `midi-parser-worker.js` crea un Worker más
  con `midi-parser-helper.js` (mensajes `live-*`), que corre su propia instancia del parser.
  - `exportLiveImage` / `importLiveImage` le pasan solo lo que necesita: el fin de cada pista,
    los colores y `visualCheckpointSpan_`.
  - Al arrancar recibe el flujo tal como está (`exportVisualStream`). Después recibe cada tramo
    anexado (el mismo buffer del helper, transferido) y cada reinicio del flujo
    (`restartVisualStream`).
  - Responde los `key-state`. El Worker del parser los reenvía y devuelve las respuestas a la
    página sin cambiar el formato, así que WASMIDI y visual-bench no cambian.
  - Un reset (salto, primera petición, alcance del roll) necesita los snapshots y la fuente,
    que solo tiene el Worker del parser. El Worker del teclado pide `live-rebuild`. El parser
    arma las teclas con `liveRebuildAt` (`exportLiveRebuild`) y, si el flujo del teclado no
    cubre la ventana de densidades, también las densidades desde los checkpoints. El teclado
    las importa (`liveWorkerResume`) y sigue igual que antes.
  - Mientras espera la reconstrucción no procesa otros mensajes, así que los tramos no cambian
    el flujo a mitad de un reset.
- `buildLiveSnapshotStream` queda partido en `liveDecide`, `liveDensityFromStream`,
  `liveDensityDecode`, `liveAdvance` y `liveFinish`. El Worker del parser usa los mismos pasos
  que antes. El Worker del teclado usa `liveWorkerStep`, que devuelve `Rebuild` o
  `RebuildDensity` en vez de reconstruir, y fuera del flujo no avanza: espera a que llegue.
- Con el teclado afuera, el flujo del Worker del parser se recorta solo por el barrido
  (`liveExternal_`). La planificación de tramos sigue limitada por la posición del teclado,
  que llega con cada respuesta (`setLiveExternalPosition`), para que su copia no crezca sin
  límite. Si el Worker del teclado falla o los helpers se detienen, el teclado vuelve al Worker
  del parser y responde la petición pendiente. `?keyworker=0` lo deja en el Worker del parser.
- `liveStreamRun`: el recorrido del flujo del teclado hace densidad y teclas en un solo bucle
  sin llamadas. Solo las palabras del tick exacto de la petición (note-offs diferidos) siguen
  por `liveConsume`.
- `appendVisualResult` anexa desde el blob sin el vector intermedio (`PodReader::view`).
- `[WASMIDI visual]` con `?visualdebug=1` muestra "keyboard Worker X ms/N" (tiempo del teclado
  en su Worker). "live" pasa a contar solo las reconstrucciones.

**Medición:**

- Host, palabras de 440-442 s a 60 peticiones/s: teclado 25,1 → 11,2 ns por palabra (2,17 → 0,97 s
  para 2 s de canción). El barrido del roll sobre las mismas palabras cuesta 9,8 ns por palabra.
- Chromium del contenedor (4 hilos, SwiftShader), visual-bench, The Nuker 3 desde 415 s con
  `?helpers=3`:

| | rev. 86 (§82) | rev. 87 |
|---|---|---|
| Worker del parser por segundo en 435-454 s | teclado 550-700 ms + barrido + anexar (saturado) | barrido 200-400 ms + anexar 100-200 ms |
| Worker del teclado por segundo | — | 330-620 ms |
| roll atrasado a 454 s | -14,8 s | -9,5 s |
| roll en 417-433 s | atrasado desde 429 s | adelantado 0,8-1,3 s, 0 cuadros atrasados |

  Desde 434 s lo que limita en el contenedor son los 3 helpers: llegan ~180-240 MiB/s de
  tramos, y la canción necesita ~410 MiB/s (43 M palabras/s). Con 4 hilos compiten con el synth,
  el teclado y la página. El Worker del parser ya no está saturado.
- WASMIDI con `many9679.mid`, `?helpers=3&rawlimit=0`: el Worker del teclado responde. Los
  cuadros (~4/s) y la latencia del teclado (~250 ms) son iguales con `?keyworker=0`: los limita
  el hilo de la página con SwiftShader.

**Verificación:**

- `visual_check` (host) tiene dos casos nuevos: "keys" (todas las peticiones en una instancia
  de teclado con los tramos de un helper y las reconstrucciones del coordinador) y "keyjump" (lo
  mismo con un alcance del roll de 1 s). Dan IDENTICAL contra el camino de fusión en many30,
  many9679 y nuker4_553.
- `tools/midi_parser_bootstrap_smoke.cjs` (CI) prueba el contrato `wmp_live_*` con tres
  instancias wasm contra el camino de fusión.
- `feeder_check`: 24 IDENTICAL.
- `tools/local_build.sh` completo.

**No verificado / pendiente:**

- En las PCs del usuario: con 5-8 helpers y 16-24 hilos los helpers deberían alcanzar en
  420-450 s, pero no se midió.
- La ráfaga de 830 s (~121 M palabras/s): en host anexar 1,0 + barrer 1,2 s por segundo de
  canción. El Worker del parser no alcanza ahí ni sin el teclado (anexar ya tiene una copia
  menos, sin medir), y el teclado tampoco (~1,4 s/s en host).
- El salto lejano que decodifica la ventana de densidades desde los checkpoints (§82) sigue
  igual; ahora corre en el Worker del parser cuando el teclado pide `RebuildDensity`.

## 84. Revisión 88 — Los tiles culleados del roll se arman de a pedazos por cuadro

**Pedido:** el usuario probó la rev. 87 con The Nuker 3 (Chrome 155, 16 núcleos, 5 helpers).
En WASMIDI se siguen perdiendo cuadros en la zona densa antes de 2:13 y en otras partes, y
WasmiSynth volvió a cerrarse un momento mientras estaba en pausa. En visual-bench, con el mismo
archivo y la misma PC, los cuadros quedan en 58-61 por segundo durante todo el tema. El roll se
atrasa solo en 128-129 s (0,5 s), 435-454 s (hasta -5,6 s), 711 s y 828-841 s. En la rev. 86 se
atrasaba en 122-135, 420-561, 710-717 y 817-866 s.

**Causa:** visual-bench no arma tiles culleados; WASMIDI los arma en el hilo de la página
(`updateCullTiles`). Cada tile junta las notas del ring desde `sharpTail_` hasta su final y las
pasa por `CullViewport` de una vez. Con The Nuker 3 eso son 0,3-1 M notas.

- En host (nativo), 375 k notas cuestan 2-6 ms juntarlas y 22 ms cullearlas. Con 1 M notas son
  5-14 ms y 52-71 ms.
- El bucle de fondo armaba hasta 2 tiles por cuadro y solo miraba el tiempo después del primero.
  Un tile denso era, entonces, un cuadro de decenas de milisegundos, y llega uno nuevo cada ancho
  de pantalla de canción.
- El tile del borde derecho, mientras no estaba completo, se rearmaba en el mismo cuadro cada vez
  que el borde de la pantalla pasaba su cobertura.
- En el Chromium del contenedor, con `?tileslice=0` (el mismo armado, de una vez), hasta 140 s:
  54 segundos tuvieron algún tile de más de 16 ms, con un máximo de 102 ms.

**Cambios:**

- `note_raster_compositor`: `CullViewport` se parte en `CullBegin` / `CullStep` / `CullFinish`.
  `CullStep` visita a lo sumo N notas. `CullViewport` es las tres seguidas, así que el resultado
  no cambia.
- `gl_renderer.cpp`, `CullJob`: un tile por vez se arma de a pedazos, con 3 ms por cuadro (6 ms si
  el tile está en pantalla). Tiene su propia copia de las notas y su propio scratch.
  - Junta las notas en tramos de 16384 ids y cullea en tramos de 4096 notas, mirando el reloj
    entre tramos.
  - Retoma la juntada en `max(nextId, sharpTail_)`. Las notas antes de la cobertura ya no cambian:
    sus cierres llegaron antes que `sharpRemoteSafeThrough_`. Las que el tail deja atrás
    terminaron a la izquierda de la pantalla.
  - Un cambio de época del ring o de ancho descarta el trabajo, igual que los tiles.
- Qué tile se arma: el primero desde la izquierda que no cubre hasta `min(fin del tile,
  sharpRemoteSafeThrough_)`. Un tile incompleto se arma solo si es visible o el siguiente
  (`k ≤ lastTile + 1`), y se rehace cuando ganó 1/16 de tile de cobertura.
- Los tiles visibles siguen garantizados en el mismo cuadro, como en la rev. 85. Si el trabajo en
  curso es ese tile y ya cubre lo necesario, se termina; si no, se arma entero como antes. Esto
  pasa casi solo tras un salto o un alcance del roll.
- `?tileslice=0` arma cada tile en un cuadro (comparación y vuelta atrás).
- `?visualdebug=1`: la línea `[WASMIDI app]` agrega `tiles N (visible V, X ms, max Y ms)`. N son
  los tiles terminados, V los armados de una vez por estar en pantalla, X el tiempo del hilo de
  la página en tiles por segundo, e Y el pedazo más largo.
- `browser_bench.py app --pause=S:D`: pausa en el segundo S y reanuda D segundos después.

**Medición (Chromium del contenedor, 4 hilos, SwiftShader, The Nuker 3 desde 0 s hasta 140 s,
`?helpers=3&visualdebug=1&rolldraw=0`):**

| | `?tileslice=0` | rev. 88 |
|---|---|---|
| tiles terminados (armados de una vez por estar en pantalla) | 236 (4) | 93 (20) |
| tiempo de la página en tiles | 4164 ms | 1589 ms |
| pedazo más largo | 102 ms | 23 ms (un tile visible) |
| segundos con un pedazo > 16 ms | 54 | 4 (todos de tiles visibles) |

- Con `?tileslice=0` se terminan más tiles. Cada trabajo acaba en el cuadro, así que el tile
  incompleto del borde se rehace más seguido.
- Hay más tiles visibles armados de una vez (4 → 20): con poco adelanto, el tile del borde a veces
  no termina sus pedazos antes de entrar en pantalla. Cuestan ~7-24 ms cada uno en el contenedor.
  Una versión que armaba de a pedazos también los tiles incompletos lejanos dio 10 visibles, con
  un máximo de 36 ms.
- Los cuadros por segundo del contenedor (2-6) no sirven para medir esto. Con SwiftShader el
  hilo de la página está 92% en código nativo (`(program)` en el perfil) aun con
  `?rolldraw=0`; la parte wasm es ~1%.

**Pausa:** WASMIDI en pausa 80 s en 125 s de The Nuker 3 (`--pause=125:80`), con 4 workers y
16384 voces. No se cerró nada, la memoria de la app no cambió y al reanudar siguió sonando.
Mientras está en pausa, WasmiSynth no renderiza (`pump` sale con `!playing`), el worklet da
silencio sin contar underruns y el feed del parser no pide más. Hace falta el log de consola del
momento del cierre para saber qué fue.

**Verificación:** `tools/note_raster_compositor_check.cpp` compara el cull de a pedazos (de 7919
notas) contra el entero en todos los casos: mismas notas. `cull_tile_sim`: 0 píxeles distintos.
`tools/local_build.sh` completo.

**No verificado / pendiente:**

- Cuadros en las PCs del usuario. Con 60 cuadros por segundo hay 180 ms por segundo para tiles,
  ~10× lo que usaron hasta 140 s en el contenedor. Pero hay otras causas posibles de cuadros
  perdidos en WASMIDI que visual-bench no tiene: el teclado y los gráficos de Qt, las subidas al
  VBO y la GPU con 0,2-0,5 M instancias. La línea `[WASMIDI app]` con los tiles dirá si queda algo.
- El cierre de WasmiSynth en pausa no se reprodujo.
- Los tiles visibles que no llegaron a terminarse de a pedazos (tras un alcance, o con poco
  adelanto) se siguen armando en un cuadro (hasta 23 ms en el contenedor).

## 85. Revisión 89 — El feed del synth y el flujo visual se decodifican juntos

**Pedido:** con la rev. 88, en WASMIDI con The Nuker 3 los cuadros por segundo siguen llegando a
0 antes de 2:10 ("mejoró pero no lo suficiente") y sigue habiendo lag en los mismos lugares. El
usuario sospecha de la polifonía: usa 12288 voces en Auto con CPU (el modo GPU sigue siendo más
lento).

**Causa:** el contador de cuadros de WASMIDI cuenta solo los cuadros completos del roll; un
cuadro mantenido mientras faltan notas no cuenta. 0 cuadros por segundo es entonces el roll
esperando notas del flujo visual, no la página trabada. En visual-bench (sin synth) el roll de
la rev. 87 solo se atrasaba en 128-129 s. En WASMIDI los mismos helpers decodifican también el
feed del synth, y cada tramo de canción se decodificaba dos veces: una para el feed y otra para
el flujo visual (las dos parten de los checkpoints y llaman a `decodeTrackUntil`). Con 12288
voces en Auto, en 16 hilos, hay además 9 workers del synth ocupados, así que los helpers tienen
menos CPU que en visual-bench.

Medido en host (nativo) con The Nuker 3, segundos de helper por segundo de canción:

| tramo | feed | visual | feed + visual | conjunto |
|---|---|---|---|---|
| 118-132 s | 0,92 | 0,81 | 1,73 | 1,58 (-9%) |
| 432-446 s | 2,14 | 2,29 | 4,43 | 3,21 (-28%) |

El conjunto da exactamente los mismos bytes que los dos por separado (0 diferencias en 84
tramos). Ahorra menos que "el máximo de los dos" porque la parte visual también cuesta por
ordenar y copiar ~39 M palabras por segundo a 432 s. Probé ordenar en dos pasadas (11 bits bajos
del tick y después el resto): con datos sintéticos al azar es ~4× más rápido que el conteo por
tick, pero con The Nuker 3 no cambió nada (los ticks reales están agrupados y el conteo ya va en
caché), así que no quedó.

**Cambios:**

- `BpfaMidiStore::decodeFeedRequest(..., visual)`: con `visual`, después de decodificar cada
  bloque de 4 bloques del synth junta las palabras crudas de cada pista en `visRaw_` y al final
  arma el resultado visual de `[edge.front(), edge.back())` con el mismo orden que
  `decodeVisualRequest` (`visualOrderRaw`, `putVisualResult`, que ahora comparten los dos).
- `midi_worker_core.cpp`: `wmp_joint_decode_js` (helper; el buffer queda `[u64 tamaño del
  feed][resultado del feed][resultado visual]`), `wmp_feed_block_tick_js` y
  `wmp_feed_block_at_tick_js` (primer tick de un bloque del feed y primer bloque desde uno dado
  que empieza en o después de un tick). Agregados a `EXPORTED_FUNCTIONS`.
- `midi-parser-worker.js`, tramos conjuntos:
  - Los dos flujos se encuentran en un borde de bloque de 4 bloques del feed, contado desde
    `feedPlanned`. Los tramos solo visuales terminan en esos bordes. Los tramos solo del feed
    corren hasta la cabeza del flujo visual aunque pasen el horizonte del synth (1 s), porque
    el roll va varios segundos adelante y si el feed solo avanzara con el synth no lo alcanzaría
    nunca (la primera versión hizo 3 tramos conjuntos en 140 s).
  - Juntos, un tramo conjunto se planea cuando cualquiera de los dos lo pide. El tamaño sale del
    presupuesto de palabras del flujo visual, con tope de 0,75 s de canción, o el de un tramo
    del feed si el synth lo necesita ya. El feed guarda hasta 256 resultados adelantados.
  - Cada tramo conjunto llena una entrada de `feedQueue` y una de `visualQueue`, cada una
    validada con su propia secuencia: un reset del feed no tira la parte visual y un reinicio
    del roll no tira la del feed.
  - `?jointdecode=0` vuelve a los tramos separados.
  - `?visualdebug=1`: la línea `[WASMIDI visual]` dice cuántos tramos fueron conjuntos y
    `helpers N% busy` (tiempo de los helpers con un tramo encargado).
- `midi-parser-helper.js`: mensaje `joint-decode` → `joint-result` con los dos buffers.
- `tools/midi_parser_bootstrap_smoke.cjs`: barre el roll con el flujo armado solo con tramos
  conjuntos de 1-3 bloques de 4 y lo compara contra el merge; cada parte del feed debe ser
  igual byte a byte a `wmp_feed_decode_js` del mismo pedido; y `wmp_feed_block_at_tick_js`
  debe devolver el primer bloque de cada tick.

**Medición (Chromium del contenedor, 4 hilos, The Nuker 3 desde 0 hasta 140 s,
`?helpers=3&visualdebug=1&rolldraw=0`, 1024 voces para que el synth no sature los 4 hilos):**

| | `?jointdecode=0` | rev. 89 |
|---|---|---|
| helpers ocupados (promedio) | 49% | 34% |
| tramos anexados (conjuntos) | 35 (0) | 189 (186) |
| MiB del flujo visual anexados | 1764 | 1775 |

Con 12288 voces el synth usa ~246% de carga y hay underruns en el contenedor con o sin tramos
conjuntos: ahí no se puede medir el roll. El adelanto del roll quedó positivo en las dos corridas
de 1024 voces; los cuadros por segundo del contenedor no sirven (SwiftShader).

**Verificación:** `tools/feeder_check/check.sh`: 24 IDENTICAL, ningún DIFFER.
`tools/local_build.sh` completo, con la prueba del contrato del parser.

**No verificado / pendiente:**

- En las PCs del usuario. Si con la rev. 89 sigue habiendo 0 cuadros por segundo antes de 2:10,
  la línea `[WASMIDI visual]` dice si los helpers están saturados (`helpers N% busy` cerca de
  100%). Probar también con 6144 voces: si así no cae, el problema es CPU repartida entre el
  synth y los helpers (la sospecha del usuario), no la decodificación.
- Los tramos conjuntos son más cortos que los solo visuales (0,75 s contra hasta 8 M palabras),
  así que el Worker del parser y el del teclado reciben más tramos por segundo.
- La ráfaga de 830 s: el banco de host se quedó sin memoria ahí (cuestión del banco, que guarda
  todo).

## 86. Revisión 90 — Cada pista guarda su bloque descomprimido entre tramos

**Pedido:** la rev. 89 "no mejora nada": en WASMIDI con The Nuker 3 siguen los 0 cuadros por
segundo antes de 2:10 y el lag en los mismos lugares. El usuario mandó además el código de
sharkmidi (GPL-3, solo ideas) para ver qué sirve; la revisión está al final de esta sección.

**Causa:** reproducido en visual-bench, en el Chromium del contenedor (4 hilos, 3 helpers): el
roll se atrasa de 118 a 131 s de The Nuker 3. Cada pedido a un helper (tramo del feed, visual o
conjunto) trae la posición de cada una de las 9679 pistas y ponía `c.block = nullptr`, así que
cada pista volvía a descomprimir con zstd su bloque de 64 KiB aunque fuera el mismo bloque del
tramo anterior. Con tramos de 0,25-0,75 s de canción eso es casi siempre así: la mayoría de las
pistas avanza unos pocos bytes por tramo.

**Cambios:**

- `BpfaMidiStore::decodeFeedRequest` y `decodeVisualRequest`: el cursor de cada pista conserva
  su bloque si la posición pedida cae dentro de él. El bloque está en el buffer propio del cursor
  (`own`, ranura `0xfffe`), que solo escribe ese cursor, y `decodeNext` sigue validándolo con
  `sourceSlotValid` antes de usarlo, así que una ranura de la caché compartida que cambió no se
  reusa.
- Probé además dos cosas que no quedaron porque no cambiaron nada: que la posición del Worker del
  teclado no frene la planificación de tramos visuales (287 contra 293 cuadros atrasados, dentro
  del ruido) y subir el adelanto del flujo de 32 M a 96 M palabras (peor: 406).

**Medición:**

Host (nativo), The Nuker 3, 118-134 s, 3 helpers en ronda con tramos visuales: 0,64 → 0,52
segundos de helper por segundo de canción (-19%).

Chromium del contenedor, `browser_bench.py visual "The Nuker 3 Ultimate F4.mid" 118 20
--query=helpers=3&visualdebug=1` (la columna rev. 90 es el código publicado; la corrida tenía
además `keyplan=1`, el interruptor de prueba que dejaba la planificación como antes):

| | rev. 89 | rev. 90 |
|---|---|---|
| cuadros con el roll atrasado (118-137 s) | 782 | 293 |
| adelanto mínimo del roll | -2,60 s | -1,21 s |
| helpers ocupados (promedio) | 56% | 31% |
| teclado | 28,6/s | 40,0/s |

Lo que queda atrasado es 127-130 s (~70 M palabras visuales por segundo). Ahí los helpers ya no
están saturados; las líneas `[WASMIDI visual]` muestran a veces anexar en el Worker del parser
hasta ~0,8 s por segundo con el flujo en ~414 MiB (crecer y recortar el vector de palabras de
10 bytes).

**Verificación:** `tools/feeder_check/check.sh`: 24 IDENTICAL, ningún DIFFER.
`tools/local_build.sh` completo, con la prueba del contrato del parser (tramos conjuntos y
visuales comparados contra el merge).

**No verificado / pendiente:** en las PCs del usuario y en WASMIDI con synth (en el contenedor
el synth con 12288 voces satura los 4 hilos).

### Revisión de sharkmidi (ideas, sin código)

- **Renderizador:** mismo diseño que el roll de WASMIDI (los dos vienen de BPFA): instancia
  (inicio, fin, color/tecla/velocidad), shader con brillo y transparencia, contador por tecla que
  junta notas superpuestas, anillo con cola. Lo distinto no sirve aquí: escribe el fin de nota en
  un buffer mapeado persistente (OpenGL 4.4, no existe en WebGL2; WASMIDI ya sube los cierres en
  tramos seguidos), no cullea (en el navegador 1,7 M notas crudas por cuadro trababan la página,
  §81) y su "force cull" borra notas largas con más de 262 k en pantalla (rompe la regla de capas).
  Lo valioso es de dónde lee: barre en el mismo hilo que dibuja, directo de un arreglo de eventos
  ya ordenado por tick.
- **RSB:** en la carga arma un arreglo de todos los eventos de canal ordenado por tick. Cada grupo
  (tick, pista) son tramos `[estado][byte 3][tecla…]`, un byte por evento con el bit 7 en el último
  del tramo; el note-on con velocidad 0 se guarda como note-off con velocidad 64 para alargar
  tramos. Aparte: tamaño (u32) y pista (u16) por grupo con un ancla cada 64 grupos, y (tick,
  offset) por tick distinto. Una pasada cuenta los bytes exactos de cada grupo y la segunda escribe
  en paralelo: cada hilo es dueño de un pedazo de la salida y visita todas las pistas con cursores
  que parten de checkpoints.
- Medido con un contador propio sobre The Nuker 3: 4.158 M eventos de canal, 141 M grupos, 380 M
  tramos, 5,29 M ticks distintos → 4,92 GB de datos + 0,85 GB de grupos + 0,13 GB de ticks =
  **5,89 GB** (1,42 bytes por evento). `nuker4_830.mid`: 1,46 bytes por evento. Las palabras del
  flujo visual (10 bytes) serían 41,6 GB para la canción entera; la fuente zstd de hoy ocupa
  ~1,3 GB.
- Propuesta: armar ese flujo en la carga con los Workers que ya cargan en paralelo y que roll,
  teclado y feed del synth lean de él, en lugar de decodificar las 9679 pistas en vivo con helpers.

## 87. Revisión 91 — El flujo visual se guarda en pedazos

**Pedido:** seguir con lo que atrasa al roll en 127-130 s de The Nuker 3 después de la rev. 90.

**Causa:** probé en Chromium (visual-bench, 118-137 s, 3 helpers) tramos visuales más chicos (2 M
palabras: 283 cuadros atrasados, igual que la rev. 90) y más tramos en vuelo (8: 406; 2 M palabras
y 12 en vuelo: 813). Más trabajo en vuelo empeora: los helpers no son el límite. Las líneas
`[WASMIDI visual]` mostraban anexar tramos en el Worker del parser hasta 1,1 s por segundo
(3 tramos, 245 MiB). El flujo era un solo vector de palabras: anexar un tramo lo hacía crecer
(copiar todo el flujo al pasar la capacidad) y recortar borraba el frente (mover el resto,
cientos de MB). Lo mismo pasaba en la copia del flujo del Worker del teclado.

**Cambios:**

- `BpfaMidiStore`: el flujo visual es una lista de pedazos (`VisualPiece`: posición absoluta
  de su primera palabra, palabras y pistas), uno por tramo anexado. Anexar crea un pedazo del
  tamaño justo y recortar suelta pedazos enteros que todos los lectores pasaron; nada se mueve.
  Las posiciones siguen siendo absolutas (`renderPos_`, `livePos_`, `visBase_`, ahora con
  `visEnd_`), y `visualPieceAt` / `visualLowerBound` reemplazan el índice y la búsqueda por
  tick en el vector. El barrido del roll, el teclado (bucle sin llamadas de la rev. 87), las
  densidades y la exportación al Worker del teclado recorren los pedazos en orden.

**Medición (Chromium del contenedor, `browser_bench.py visual "The Nuker 3 Ultimate F4.mid" 118 20
--query=helpers=3&visualdebug=1`):**

| | rev. 90 | rev. 91 |
|---|---|---|
| cuadros con el roll atrasado (118-137 s) | 293 | 200 |
| adelanto mínimo del roll | -1,21 s | -0,99 s |
| teclado | 40,0/s | 47,0/s |
| peor segundo de anexar | 389 ms (1 tramo) | 149 ms (4 tramos) |

El arranque en 118 s se pone al día un segundo antes (atrasado hasta 119 s en vez de 120 s) y en
128-129 s quedan 102 cuadros atrasados. En el contenedor de 4 hilos ese tramo usa toda la CPU
(3 helpers, Worker del parser, Worker del teclado y la página).

**Verificación:** `tools/host_bench/visual_check.cpp` (barrido y teclado desde el flujo contra el
merge, también con tramos de helper y con el teclado en su Worker): `many9679.mid` 0-20 s y
`nuker4_553.mid` 0-8 s (1,23 M notas, con recortes), todo IDENTICAL. `tools/local_build.sh`
completo, con la prueba del contrato del parser.

**No verificado / pendiente:** en las PCs del usuario y en WASMIDI con el synth.

## 88. Revisión 92 — Flujo visual compacto, leído por corridas y decodificado por adelantado

**Pedido:** seguir con el atraso del roll en 127-130 s de The Nuker 3 (el usuario: "continua"). En
el mismo hilo preguntó por qué sharkmidi y BPFA cargan y reproducen más rápido, si pasar código a C
ayudaría y cómo hace BPFA para usar zstd sin problemas al reproducir; mandó el código de BPFA
(revisión al final de esta sección).

**Causa:** después de la rev. 91 el flujo visual seguía guardando 10 bytes por palabra (anexar
cientos de MiB por segundo en el Worker del parser y en el del teclado) y los helpers solo
planeaban tramos hasta el objetivo del roll, así que la zona densa se decodificaba justo cuando
hacía falta. Medido en host sobre la canción entera (contador propio fuera del repo con los helpers de
`decodeVisualRequest`, 4 hilos): 3.259 M
palabras plegadas, 141 M grupos (tick, pista) y 383 M corridas; decodificarlas cuesta 126 s de hilo
nativo (34 s de pared con 4 hilos) y ocupan 32,6 GB como palabras de 10 bytes.

**Cambios:**

- `BpfaMidiStore`: cada pedazo del flujo (`VisualPiece`) se guarda codificado como el RSB de
  sharkmidi (§86): cabecera de grupo `0xf1` (delta de tick y pista, LEB128) o `0xf0` (mismo tick,
  delta de pista); cabecera de corrida con estado y data 2 (bit 7: sigue un byte de copias de eco);
  un byte de data 1 por palabra, con el bit 7 en la última de la corrida. Una marca cada 65536
  palabras (`VisualMark`: palabra, byte, tick y pista antes de la cabecera) para buscar.
  The Nuker 3 entero: 4,78 GB (1,47 bytes por palabra) contra 32,6 GB.
- Lectores: cursores (`VisualCursor`) con posición absoluta y serie de pedazo; `visualSeek` sigue
  desde donde estaba o parte de la última marca; `visualLowerBound` busca por pedazo, por marca y
  después lee. Leer palabra por palabra costaba 2,5× más que el vector de la rev. 91 en wasm (barrido
  ~580 ms/s contra ~200 en la zona densa), así que `visualScanRuns` entrega corridas enteras: el
  tick se prueba una vez por grupo y los bytes de data 1 van directo al consumidor.
  `renderRun` (barrido del roll) y `liveStreamRun` (teclado) procesan cada corrida de una vez:
  estado, colores y par (pista, canal) se buscan una vez por corrida; las densidades suman la
  corrida entera en un punto (mismo resultado: los puntos del mismo tick se juntan).
- Resultado de helper `WVS2` y exportación al Worker del teclado `WVT2` con los bytes codificados.
- Decodificar por adelantado: `setVisualPlan` / `wmp_visual_plan_js` fija cuántas palabras puede
  tener el flujo delante del barrido y del teclado (`visualRoomAhead`); el recorte solo cuenta las
  palabras detrás del lector. En `midi-parser-worker.js` los tramos visuales ya no esperan al
  objetivo del roll: 32 M palabras (~47 MB por copia) por GiB de `deviceMemory`, entre 32 M y
  1024 M (`?visualplan=M` en millones; `?visualahead=0` vuelve a planear solo hasta el objetivo con
  32 M). Mientras suena el feed del synth y el plan va pasado el objetivo, un helper queda libre para
  el feed. Con adelanto los tramos conjuntos de la rev. 89 se apagan (el feed tendría que perseguir
  la cabeza visual; `?jointdecode=1` los fuerza). El Worker del teclado recibe el plan en
  `live-init`.

**Medición (Chromium del contenedor, 4 hilos, `browser_bench.py visual "The Nuker 3 Ultimate
F4.mid" START DUR --query=helpers=3&visualdebug=1`, cuadros con el roll atrasado):**

| | rev. 91 | compacto, palabra por palabra | + corridas, con adelanto | + corridas, sin adelanto | final (teclado y barrido por corridas) |
|---|---|---|---|---|---|
| 118-137 s | 200 | 435 | 164 | 150 | 143 |
| 105-136 s | — | — | 34 (todos en 105 s) | 99 (61 en 128-129 s) | 39 (todos en 105 s) |

- Anexar en el Worker del parser: ~137 → ~25-60 ms/s; los helpers entregan ~55 MiB/s con
  adelanto (230 MiB/s eran palabras de 10 bytes).
- Desde 118 s el adelanto no alcanza a juntar ventaja: los 3 helpers decodifican al 100% desde el
  arranque y comparten los 4 hilos con el barrido, el teclado y la página. Desde 105 s llegan al
  tope del plan antes de 127 s y en 127-130 s el roll no se atrasa ni un cuadro (sin adelanto: 61).
- Teclado en 128 s (corrida desde 105 s): 17 → 47 actualizaciones/s, latencia promedio 48 → 11 ms;
  Worker del teclado 793 → 521 ms en ese segundo.

**Verificación:** `tools/host_bench/visual_check.cpp` ahora también corre el Worker del teclado con
adelanto (plan de 256 M palabras, 4 tramos por cuadro sin esperar al objetivo) y compara notas,
cierres y teclado contra el merge: `many9679.mid` 0-20 s, `many30.mid` 0-30 s y `nuker4_553.mid`
0-8 s (1,23 M notas), todo IDENTICAL. `tools/local_build.sh` completo (ALL CHECKS PASSED, con la
prueba del contrato del parser).
WASMIDI con synth (`browser_bench.py app "The Nuker 3 Ultimate F4.mid" MatheryLSD.sf2 140 --voices=1024
--query=helpers=3&visualdebug=1`, desde 0 s): sin adelanto 0 underruns; con adelanto una corrida dio 7
underruns en un solo segundo (78 s) y la repetición 0 underruns y 0 notas tardías, con los helpers
ocupados 79% en promedio. En el contenedor de 4 hilos corren a la vez 3 helpers y 4 workers del synth,
así que ese segundo suelto no alcanza para culpar al adelanto; en las PCs los helpers tienen hilos
reservados (rev. 81). Una corrida anterior falló porque el servidor de prueba se cerró al crear los
helpers ("Synth feed helper crashed"): no es un error del código.

**No verificado / pendiente:** en las PCs del usuario y la ráfaga de 830 s (con 5-8 helpers el plan
de 1024 M palabras cubre ~8 s de déficit). Siguiente paso propuesto: armar el flujo de toda la
canción después de la carga, guardado en el Worker del parser, y pasarle al teclado solo lo cercano.

### Revisión de BPFA (código del usuario, permiso del autor; ver §43)

- **Modo normal** (`PreprocessedMidi::Load`): lee el archivo entero, escanea, comprime y deduplica
  los bloques (`BuildDeduplicatedSource`; en Windows con XPRESS_HUFF de la API de compresión de
  Windows, zstd solo con `BPFA_HAVE_ZSTD`) y después empareja todas las notas en paralelo en una
  tabla de `PackedNote` (8 bytes por nota, bloques de 1024 por pista). El renderizador lee esa tabla;
  la fuente comprimida solo alimenta el audio (cursores por pista con caché de bloques por hilo) y el
  salto rápido usa la tabla. The Nuker 3: 2.079 M notas × 8 B = 16,6 GB, más que el heap wasm de
  16 GiB de una pestaña.
- **Modo look-ahead** (`LoadLookAhead`, tope `LookAheadMemoryMB`, 1024 por defecto): mapea el
  archivo; un solo hilo (`LookAheadProducer`) mezcla las pistas con un heap y empareja notas en
  pedazos de 65536 hasta el tope, liberando los que ya sonaron; la carga espera a llenar el tope
  ("Prefilling look-ahead note cache"). Un seek fuera de lo armado reinicia el productor.
- Lo útil para WASMIDI: pagar la decodificación antes de reproducir. El flujo compacto es el
  equivalente de la tabla, 3,5× más chico (4,78 GB contra 16,6 GB para The Nuker 3), y lo arman
  varios helpers en vez de un hilo.

## 89. Revisión 93 — Dos modos del roll como BPFA: look-ahead con tope de RAM y canción entera

**Pedido:** después de la revisión del código de BPFA (§88) el usuario pidió "sacar lo mejor de cada
uno en modos separados": un modo que decodifica toda la canción al cargar (como el modo normal de
BPFA) y otro con tope de RAM y prellenado al cargar (como el look-ahead de BPFA).

**Causa / diseño:** el modo normal de BPFA guarda una tabla de notas de 8 bytes por nota (16,6 GB
para The Nuker 3, más que los 16 GiB del heap del parser); el flujo visual compacto de la rev. 92
guarda la misma información en 1,47 bytes por palabra (4,78 GB para The Nuker 3 entero), así que el
"modo normal" de WASMIDI es ese flujo decodificado entero y guardado. Hasta la rev. 92 el flujo se
llenaba solo mientras sonaba (los helpers compiten con el feed del synth en las zonas densas), cada
reset del roll lo tiraba y el Worker del teclado recibía una copia de cada pedazo apenas llegaba
(con adelanto, el plan entero dos veces en memoria).

**Cambios:**

- `BpfaMidiStore::resetRenderCursor`: si el tick de inicio cae dentro del flujo guardado
  (`visStart_ <= tick < visHead_`), el flujo se conserva: el barrido vuelve a empezar en
  `visualLowerBound(tick)` con el estado de notas vacío (mismo resultado que un flujo nuevo desde
  ese tick: los tramos no parten ticks y el plegado es por (pista, tick)). `visualKeptOnReset` /
  `wmp_visual_kept_js` lo informa; en `requestSharpRenderer` un reset conservado no llama a
  `visualRestart`, así que los tramos en vuelo siguen sirviendo.
- `setVisualKeepAll` / `wmp_visual_keep_all_js`: en el modo canción entera `visualTrim` no recorta
  nada (todo seek se conserva).
- Envío por demanda al Worker del teclado: `beginLiveForward` (`wmp_live_forward_begin_js`) exporta
  un flujo vacío donde empieza el envío y `liveForward` (`wmp_live_forward_js`) arma el próximo
  pedazo como resultado `WVS2` cuando el teclado está a menos de 32 M palabras de él (o el pedazo
  quedó más de 48 M palabras detrás del barrido), o la cabeza cuando ya no hay pedazos. El teclado
  acepta resultados que empiezan más adelante que su cabeza (los tramos sin palabras no se envían).
  En un reset el teclado reinicia en el pedazo del tick (`wmp_live_restart_at_js` con tick y base
  de `wmp_live_forward_tick_js` / `wmp_live_forward_base_js`). El recorte del coordinador no tira
  pedazos sin enviar. `VisualPiece` guarda `endTick`.
- `midi-parser-worker.js`: modo del roll desde el mensaje `parse` (`rollMode`, `lookAheadMB`) o
  `?rollmode=` (`1` look-ahead, `2` canción entera, `0` la rev. 92 sin prellenado) y
  `?lookahead=MB`. Antes de empaquetar el resultado, `visualPrefill` arranca los helpers y llena el
  plan desde el tick 0 (look-ahead: MB / 1,5 bytes por palabra; canción entera: todo, con
  `keepAll`) mostrando "Prefilling the roll look-ahead N%" / "Decoding the whole roll N%". Sin
  helpers (fuente cruda o pocos núcleos) decodifica en el Worker del parser
  (`wmp_visual_prefill_local_js`). Tope de memoria: lo que queda del heap (14,5 GiB menos lo que el
  heap tiene en uso, `wmp_heap_used_js` con `mallinfo`) y la mitad de `deviceMemory`; si la canción
  entera no entra, lo decodificado pasa a ser el look-ahead (aviso en consola). Al terminar escribe
  `[WASMIDI roll] … ready: N MiB, to S s, T s`. WASMIDI Audio y parser-bench no mandan modo: 0.
- WASMIDI: fila "Roll cache" en los controles (botón Look-ahead / Full song y campo de MB), guardada
  en `localStorage` (`rollMode`, `rollLookAheadMB`); se aplica al próximo MIDI. Por defecto
  look-ahead con 1024 MB.
- Tope del rellenado: con más de la mitad del plan ya decodificado por delante del barrido, los
  helpers rellenan de a un tramo por vez (`?refillcap=0` lo apaga; no se aplica durante el
  prellenado). Sin él, el look-ahead atrasaba el roll más que la rev. 92 en 128 s de The Nuker 3
  (35 cuadros contra 15); la causa es inferida: el Worker del parser anexaba los tramos del
  rellenado, más allá de 278 s, mientras barría la zona densa.
- El barrido del roll (`drainSharpRenderer`) volvía al bucle de eventos con `setTimeout(0)`, que
  Chromium frena a 4 ms después de unas pocas llamadas anidadas: como mucho ~250 pasadas por segundo
  en un Worker que no hace otra cosa. Ahora cede con un mensaje a sí mismo (`MessageChannel`), sin
  ese freno (`?sweepyield=0` vuelve a `setTimeout`). La pausa que deja lugar al synth no cambió.
- `visual-bench.html` no espera 8 s a los helpers si ya arrancaron al cargar.
- `browser_bench.py app`: los clics en "Load SF2" y en el campo de voces bajan 28 píxeles por la
  fila nueva.

**Medición:**

Chromium del contenedor (4 hilos, sin GPU), visual-bench de The Nuker 3 desde 0 s
(`browser_bench.py visual "The Nuker 3 Ultimate F4.mid" 0 140 "--query=helpers=3&visualdebug=1&rollmode=N"`):

| | rev. 92 (`rollmode=0`) | look-ahead 1024 MB | + tope del rellenado | + barrido sin el freno de 4 ms |
|---|---|---|---|---|
| cuadros con el roll atrasado (0-140 s) | 15 | 35 | 0 | 0 |
| adelanto mínimo del roll | -0,17 s | -0,37 s | 0,39 s | 1,78 s |
| adelanto mínimo en 126-128 s | -0,17 s | -0,31 s | 0,39 s | 1,96 s |
| teclado en 128 s (respuestas/s, latencia media) | 11/s, 86 ms | 13/s, 70 ms | 43/s, 14 ms | 44/s, 12,5 ms |

La tercera columna es la cuarta con `?sweepyield=0`. El prellenado de 1024 MB llenó 975 MiB, hasta
277,9 s de canción, en 17-20 s con 3 helpers. La carga total varió entre corridas por la caché de
páginas del contenedor (188-330 s para el mismo archivo), así que no se compara.

Canción entera (`rollmode=2`): `nuker4_553.mid` (5,9 s, el pico de The Nuker 4) 9 MiB en 0,9 s, roll
atrasado 0; `nuker4_830.mid` (10 s, 10,8 M notas) 79 MiB en 5,1 s; `many30.mid` (120 s) 8 MiB en 0,4 s.
WASMIDI con synth y un salto (`browser_bench.py app many30.mid MatheryLSD.sf2 30 --voices=1024
--seek=12`): mismos cuadros y latencias con `rollmode=2` que con `rollmode=0` (2-3 cuadros/s, el
SwiftShader del contenedor; el Worker del teclado responde en 0-1 ms), 0 underruns.

The Nuker 3 con `rollmode=2` en el contenedor (`deviceMemory` 8, tope 4,0 GiB; visual-bench desde
120 s): el prellenado llegó al final de la canción (867,1 s) con 4173 MiB en 95 s con 3 helpers y
avisó que no entra en 4,0 GiB, así que lo decodificado quedó como look-ahead (§88 estimaba 4,78 GB
para el flujo entero). Roll atrasado 45 cuadros solo en el primer segundo (el reset del teclado tardó
0,9 s); después 0 cuadros con adelanto de 1,98 s y teclado a 53-61/s, también en 126-129 s.

**Verificación:**

- `tools/host_bench/visual_check.cpp`: el Worker del teclado ahora recibe los pedazos por
  `liveForward` (keys, keyjump y ahead IDENTICAL) y una prueba nueva `kept` compara el barrido tras
  un reset conservado hacia atrás y hacia adelante con un flujo nuevo y con el camino de mezcla:
  `many9679.mid` 2-8 s y `nuker4_830.mid` 2-8 s (10,8 M notas) IDENTICAL.
- `tools/midi_parser_bootstrap_smoke.cjs` (CI): el Worker del teclado por envío por demanda, un reset
  conservado con reinicio del teclado, y el modo canción entera (prellenado local, reset al medio
  contra la mezcla). Las comprobaciones que querían un flujo nuevo lo reinician antes.
- `tools/feeder_check/check.sh`: todo IDENTICAL. `tools/local_build.sh` completo en verde; después
  del tope del rellenado y del cambio del barrido, `node --check` y la prueba del contrato del parser.

**No verificado:** en las PCs del usuario; la canción entera de The Nuker 3 guardada (unos 4,1 GiB en el
Worker del parser: con `deviceMemory` 32 el tope es lo que queda del heap de 14,5 GiB; que la pestaña
aguante esa memoria no está probado); el prellenado con 5-8 helpers; WASMIDI con synth en las zonas
densas con GPU real (el contenedor dibuja con SwiftShader). El bucle del Worker del synth también
vuelve con `setTimeout(pump, 0)` después de cada tramo de 12 ms. Probado con `MessageChannel` en una
copia del sitio (`browser_bench.py audio nuker4_553.mid MatheryLSD.sf2 7`): a 2048 voces (carga
~70%) 19 contra 28 underruns, todos en el primer segundo, y a 6144 voces (carga 204%) igual de
saturado; no se distingue del ruido entre corridas, así que no se cambió.

## 90. Lista de pendientes después de la rev. 93

**Pedido (Dekx):** "Actualiza el handoff con la lista de todo lo que haga falta". Sin cambios de
código. Esta lista junta lo abierto en §67 y en los "No verificado / pendiente" de §70-§89; reemplaza
como lista vigente a la de §67 (que queda como histórica). Cada punto dice dónde está el detalle.

### A. Meta: The Nuker 3 y 4 en tiempo real de principio a fin

Meta del usuario: WASMIDI (synth + roll + teclado + gráficos a la vez) y WASMIDI Audio, con
MatheryLSD, 1024 voces por worker (24576 en el 5900X), sin lag, sin cuadros perdidos y sin
diferencia audible frente a un MIDI sin robos ni descartes. Regla de sincronía (§80): nadie espera
a nadie, el que se atrasa alcanza.

1. **Probar la rev. 93 en las PCs** (5900X + RX 9070XT, 7435HS + RTX 2050, la de 16 núcleos):
   "Roll cache" en Look-ahead 1024 MB y en Full song. Con `deviceMemory` 32 la canción entera de
   The Nuker 3 (~4,1 GiB de flujo visual) debería entrar: falta ver que la pestaña aguante esa
   memoria y cuánto tarda el prellenado con 5-8 helpers (§89).
2. **Tiempo de carga.** El usuario dice que sharkmidi abre mucho más rápido. Hoy: 42 s la carga de
   The Nuker 3 en 16 núcleos (rev. 67) más el prellenado del roll de la rev. 93. Falta medir en las
   PCs cuánto suma el prellenado y decidir si se puede solapar con la carga (§88-§89).
3. **Ráfaga de 830 s de The Nuker 3** (~121 M palabras visuales/s): anexar + barrer en el Worker del
   parser y el teclado no alcanzan en host (§83, §88). Con Full song el flujo ya está hecho; falta
   medir si el barrido sólo alcanza ahí.
4. **Resto de 127-130 s de The Nuker 3**: picos de anexar de ~0,8 s/s con el flujo cerca de
   414 MiB (memoria del proyecto, rev. 93). En el contenedor ya da 0 cuadros atrasados; confirmar en
   las PCs.
5. **WASMIDI con 12288 voces y The Nuker 3**: el usuario veía 0 cuadros/s antes de 2:10 (§85-§86).
   Pedir la línea `[WASMIDI visual]` (`helpers N% busy`) y `[WASMIDI app]` con `?visualdebug=1`, y
   probar con 6144 voces para separar CPU del synth de decodificación.
6. **Crujidos a 24576 voces** en el 5900X (12288 estables, §77) y el robo caro con "Stack gain"
   (§77-§78). Abaratar el robo en modo 0.
7. **Synth por GPU estilo Kestrel (etapa 3)**: pasos por bloque en la GPU (robo, alta, render,
   reducción, compactación), pool SoA, fase 32.32, suma en árbol fijo, note-offs por tabla
   canal×tecla (§74-§75). Lo que queda del lado CPU del modo GPU es la fase de eventos (§75-§76).
8. **Saltos justo después del pico / saltos lejanos**: la ventana de densidades se decodifica desde
   los checkpoints en el Worker del parser (~0,6 s en host tras el pico de The Nuker 4, 3,6 s a
   830 s de The Nuker 3; §72, §82-§83). Una nota larga que empezó más de 25 ms antes del destino de
   un salto no suena (decisión de §80; revisar si el usuario lo nota).
9. **Helpers con fuente cruda**: un MIDI de menos de ~3-6 GB (según memoria) queda crudo y no tiene
   helpers ni para el synth ni para el roll (§70-§71); compartir la fuente cruda con los helpers.

### B. Verificaciones que necesitan las PCs del usuario o una GPU real

1. **Modo GPU de WasmiSynth (rev. 70-79)**: que el WGSL compile, que el pipeline de hasta 8 bloques
   alcance, cuánto tarda el kernel con 24576 voces, 1 contra 12-24 workers (§67 Rev. 73, §75).
2. **Auto (Hash) contra Single** a 12288 y 24576 voces, CPU y GPU; si el reparto por teclas se
   desbalancea en algún MIDI (§76).
3. **Cuadros del roll con GPU real** (tiles parciales y de a pedazos, rev. 85 y 88): con SwiftShader
   del contenedor los cuadros de WASMIDI no dicen nada (§79, §81, §84). Si en GPU el synth por
   WebGPU y el WebGL del roll compiten por la GPU (§77, §79).
4. **Cierre de WasmiSynth en pausa** ("application exit"): no se reprodujo (§81, §84); hace falta el
   log de consola del momento del cierre.
5. **Eco plegado con "Stack gain"**: si suena como el original (§77).
6. **WASMIDI Audio con audio real**: ventanas, letras, gráficos, visualizador y la SF2 que no
   recargaba (arreglada en la rev. 73, §67 Rev. 73), sin confirmar por el usuario.
7. **Pestaña oculta real**: el freno de timers de Chrome no se emula en headless (§74).
8. **Contador de notas descartadas** del recorte del pico más alto en Chromium con el render de 4
   voces (98.597 contra 22.621 en corridas distintas, §74): revisar si se repite.
9. **Huecos del roll en The Nuker 4** con `?tiles=0` (§73).

### C. Reportes viejos del usuario sin cerrar (§67)

1. **"¿Dónde están los controles?"** (§67, B): nunca se aclaró a qué se refería. Preguntar.
2. **Minimizado sin usar CPU/GPU** (§67, C y D): visual-bench se pausa y WASMIDI Audio suena con lo
   visual congelado (rev. 78), pero WASMIDI (`index.html`) no escucha `visibilitychange`: con la
   pestaña oculta `requestAnimationFrame` se frena solo, pero el barrido del roll, los tiles, el
   teclado y los helpers del flujo visual siguen (inferido del código, sin medir).

### D. Otros pendientes (largo plazo)

1. Que el coordinador de `voice_render_float` también tome tiles de render (§62).
2. Un paso de benchmark con Chromium headless en el CI (§67).
3. Abrir MIDIs comprimidos (.zip/.7z/.gz/.zst) directamente (§52, §67).
4. Hash fijo de zstd en `FetchContent` (`CMakeLists.txt` baja zstd 1.5.6 por URL sin `URL_HASH`).
5. Tabla de pistas de la carga: cadena de lecturas dependientes (~7% del total, §60).
6. GUI cosmética (§11), mapeo GM de instrumentos y drums, precisión de CC.

### Cerrados desde la lista de §67

- Reconstrucción del teclado en seeks lejanos de The Nuker (~15 s): snapshots por pista y por tecla
  (rev. 76, §72; rev. 86, §82).
- WebGPU más lento con 12 workers (§67, A): avance en forma cerrada y reserva por tile (rev. 73).
- SF2 que no se podía recargar después de cambiar el synth (§67, E): arreglado en la rev. 73.
- Roll, teclado y gráficos con su propia decodificación (etapa 2): flujo visual con helpers
  (rev. 75 en adelante).
- Admisión en el parser y decodificación repartida para el synth (rev. 74).

## 91. Revisión 94 — WasmiSynth carga carpetas SFZ; mockup de la GUI nueva

**Pedido (Dekx, hilo "GUI nueva"):** GUI nueva al estilo de dekx.cc (pantalla completa sin scroll,
modo celular vertical, notas y teclado con degradado y textura sin perder rendimiento, popups de
polifonía/NPS máximos y de lag, note speed en segundos, colores por pista en loop de 16 con
importación del config.xml, fondo neural, sin letreros innecesarios ni nombres de otros programas)
mostrando antes un mockup; y que WasmiSynth acepte una carpeta con un .sfz y lo que pida (samples)
para tener SFZ completo además de SF2.

### Mockup

Publicado y aprobado: https://claude.ai/artifact/6Rzqy1QJQrBaJq39comYMd (escritorio 1600×900, celular
390×844 con el roll vertical, diálogo de colores). Correcciones del usuario: el fondo neural tiene que
ser el de `dekx.js` (≤ 80 nodos, uno por 18.000 px², puntos de 2 px, líneas de menos de 140 px con
alfa (1 − d/140)·0,2, fondo #07071a, una sola red detrás de toda la página) y el kit visual el de
dekx.css y WASMIDI Audio (biseles, título con degradado horizontal, cajas de grupo, LCD en VT323),
no el de 0xDEADDEAD. Las notas nuevas tienen que rendir igual o mejor que las actuales: se mide antes y
después con visual-bench y `?visualdebug=1` cuando se cableen. El cableado de la GUI va en las
revisiones siguientes; esta revisión trae solo el SFZ.

### Causa (SFZ)

El parser SFZ heredado de SnappySynthV2 nunca se llamaba en el navegador (`ssw_load_sf2` solo abría
SF2) y estaba hecho para rutas de Windows: separaba carpetas solo con `\`, leía solo WAV de 16 bits,
un valor sin comillas terminaba en el primer espacio (`sample=Soft Layer\piano c4.wav` se cortaba),
`sample=` en `<group>`/`<global>` se ignoraba, una línea con dos cabeceras perdía la segunda, `<control>`
o `<effect>` dejaban el contexto en la última región, no había `#define`, y una sola muestra faltante
hacía fallar todo el instrumento (`ssw_instrument_is_materialized`).

### Cambios

- `Parser/sfz_parser.c`: rutas relativas a la carpeta del .sfz principal (también los `#include`,
  como sfizz), `\` → `/` fuera de Windows, búsqueda sin mayúsculas/minúsculas por cada parte de la
  ruta (librerías hechas en Windows), `#define $X` y su sustitución, comentarios `/* */`, la línea se
  parte en cada `<cabecera>` y cualquier cabecera desconocida cierra el contexto, `default_path` como
  opcode en cualquier lugar, `sample=` heredado de `<group>`/`<master>`/`<global>`, valores sin
  comillas que siguen hasta el próximo `opcode=`, alias `loopmode`/`loopstart`/`loopend`/`offby`/
  `polyphony_group`/`filtype`, generadores `*sine`, `*saw`, `*square`, `*triangle`, `*noise`,
  `*silence` (tabla de 2048 muestras = un ciclo de C4), `#include` hasta 16 niveles, líneas de hasta
  16 KB. Las regiones sin muestra se descartan y el resto carga. El bloque que aplica los opcodes quedó
  igual, movido a `sfz_apply_opcode`.
- `Parser/wav_loader.c`: WAV PCM de 8/16/24/32 bits, float de 32/64 bits y WAVE_FORMAT_EXTENSIBLE, más
  de 2 canales se quedan con los 2 primeros; sigue guardando int16 como antes.
- `snappy_wasm_core.c`: `ssw_load_sf2` abre con el parser SFZ si la ruta termina en `.sfz` (sin export
  nuevo).
- FLAC, Ogg Vorbis y Opus: el Worker del synth no tiene `AudioContext`, así que `snappysynth_bridge.js`
  los decodifica en la página con `OfflineAudioContext.decodeAudioData` a la frecuencia nativa (leída de
  STREAMINFO de FLAC o del encabezado de Vorbis; Opus 48 kHz) y los pasa como WAV de 16 bits con el
  mismo nombre. No se agregó código de terceros: descargar dr_flac/stb_vorbis al repo fue bloqueado en
  esta sesión y la decodificación del navegador alcanza.
- `snappysynth-worker.js`: `loadSoundfont` acepta `folder` (lista `{path, file}`) y `root`; monta
  la carpeta entera con WORKERFS (`blobs` con rutas, se lee bajo demanda) en `/soundfonts/layerN`.
- Bridge: `loadSoundfontFolder(files, sfz)`, `openSoundfontFolder()`, `sfzChoices(files)`. Si la
  carpeta tiene varios .sfz se usa el menos profundo y primero en orden alfabético (la GUI nueva va a
  dejar elegir).
- WASMIDI: botón "SFZ" al lado de "Load SF2" (`openSfzFolderPicker`, `webkitdirectory`).
  WASMIDI Audio: botón "SFZ…" y soltar una carpeta en la ventana principal.
- `tools/sfz_folder_check.c` (en el CI, paso "Host tests"): arma una librería con todo lo anterior,
  la carga con el parser y con el motor real y comprueba regiones, niveles y que una nota suene.

### Verificación

- Host: `tools/sfz_folder_check.c` OK (5 regiones, la faltante descartada, 24 bits al nivel correcto,
  `ssw_load_sf2` con .sfz, nota con pico > 0,01). Todo el paso "Host tests" del CI pasa.
- Compilación completa con `tools/local_build.sh` (Emscripten 3.1.56, Qt 6.8.3) OK.
- Chromium (WASMIDI Audio, `set_input_files` de la carpeta): librería con `Piano C4.flac` (estéreo
  44,1 kHz), `d4.wav` (24 bits), `e4.ogg` (48 kHz), `default_path=Samples\` y nombres con otras
  mayúsculas → "SFZ ready in 691 ms (3 regions)", y un MIDI de 6 notas suena (voces activas > 0).

### No verificado

- Librerías SFZ reales grandes: la memoria del Worker del synth (todas las muestras pasan a int16 al
  cargar) y el tiempo de decodificar miles de FLAC en la página.
- Opcodes que el motor no implementa (lorand/hirand, seq_*, sw_*, locc/hicc, curvas, efectos,
  `amp_veltrack`, `pitch_keytrack`…) se ignoran como antes; "soporte total" de opcodes queda pendiente.
- El botón "SFZ" de WASMIDI no se probó en el navegador (mismo bridge que WASMIDI Audio).

## 92. Revisión 95 — GUI nueva, parte 1: ventana dekx, LCD, panel, barra de estado y fondo neural

**Pedido:** cablear el mockup aprobado en §91 (https://claude.ai/artifact/6Rzqy1QJQrBaJq39comYMd) por
partes. Esta es la parte 1: la ventana con el kit de dekx.cc, el LCD, el panel de la derecha, la barra
de estado, el fondo neural de toda la página, el note speed con barra y segundos escritos, el diálogo
Advanced, el nombre "WebAssembly MIDI Player" y sin letreros innecesarios ni nombres de otros programas.
Quedan para las partes siguientes: notas y teclado con degradado y textura y teclado vertical (2), los
16 colores en bucle con el config.xml, la UI que sigue los colores y los avisos de polifonía, NPS y
lag (3), cuadros sin tope con prioridad (4) y el modo celular vertical (5).

### Cambios

- QML nuevo (`src/qml/`): `Dk.qml` (singleton con los colores de dekx.css, fuentes y formatos de
  números), `Glyphs.qml` (íconos pixel art del mockup), `DkBevel` (biseles raised/sunken/thin-in como
  bordes de 1 px), `DkButton` (botón y toggle VT323, presionado con el tramado 2×2 de dekx.css),
  `DkGroup` (caja con título en el borde), `DkField`, `DkSlider`, `DkReadout`, `DkCell`, `DkCaption`,
  `DkCapButton`, `DkDialog` (ventana hija), `PixelIcon`, `Dock.qml` (panel) y `AdvancedDialog.qml`.
  `Controls.qml` y `StatsPanel.qml` se borraron: su contenido quedó en el panel y en Advanced.
- Ventana: título con degradado y botón de pantalla completa (`MainWindow::toggleFullScreen`, API
  Fullscreen del navegador). LCD con reloj, total, estado (PLAY/PAUSE/STOP/LOAD), marquesina con
  archivo, pistas y PPQ, y lecturas Notes, NPS, Polyphony, BPM y FPS. Transporte con íconos pixel art,
  MIDI…, cerrar MIDI y VOL. Barra de estado: estado, FPS, WasmiSynth (voces, workers, carga), pistas y PPQ.
- Panel (304 px, sin scroll; solo el gráfico crece): Graphs, Note speed (barra 0,1-10 s y campo de
  0,1-60 s), Colors (TRACK/CHANNEL, los 16 colores, Edit… abre un diálogo con los 16 que lleva al
  selector de color), Neural background (Roll y Page), WasmiSynth (SoundFont, SF2…, SFZ…, Voices,
  CPU/GPU, STACK GAIN, SOFT CLIP, Advanced…, barra de carga) y Roll cache.
- Advanced: SoundFont (nombre, capas, estado del synth, Clear), Engine (Block, Buffers, Min voices,
  Workers, Rate, Vel ≥, Sharding, Channels, Bits, RT priority, Steal cache, Fast note-off, Validate,
  Debug), Live (los contadores de antes; los de diagnóstico solo con Debug), View (notas pasadas, el
  post-buffer de antes) y File (formato, PPQ, canales, cambios de tempo, CC, rango, picos).
- Letreros quitados: "Dekxtopia/Home/MIDI Player WebGL2 NEW", "BLACK MIDI PLAYER", "Format 0 / 1 ·
  Black MIDI ready", el FPS sobre el roll (pasó al LCD), las etiquetas de octava del teclado, "Parsing
  runs in a background worker / UI stays responsive", "Audio: AudioWorklet", la línea "Worker → original
  SnappySynthV2…", "Source-only desktop options…". Los cinco gráficos chicos (NPS, polifonía, BPM, CC/s,
  velocidad saltada) y el NPS LIVE quedaron en un gráfico; la velocidad saltada está en Live ("VEL ≥").
- Fondo neural (`src/neural_background.{hpp,cpp}`, `NeuralBackground`): el de `dekx.js` con el grafo de
  escena (líneas y cuadrados de 2 px con color por vértice), detrás de toda la página. Nodos
  min(80, ancho·alto/18000), velocidad ±0,08 px por cuadro de 60 Hz medida en tiempo (igual a
  cualquier tasa de cuadros), envolvimiento a 10 px del borde. El roll ya no dibuja su propia red: su
  fondo (el shader de tono y manchas) sale premultiplicado con alfa 1 − Roll·0,75 y deja ver la red de la
  página; el panel tiene alfa 1 − Page·0,5 (las mismas fórmulas del mockup). `neuralRoll`/`neuralPage`
  en `MainWindow`, guardados en localStorage.
- Gráfico (`src/live_graph.{hpp,cpp}`, `LiveGraph`): NPS relleno con degradado vertical (0,75 → 0,05 del
  acento) y línea, polifonía punteada (3 px sí, 2 no) y cuadrícula, los últimos 10 s de canción. Toma una
  muestra por cuadro mostrado (`QQuickWindow::afterAnimating`, como mucho 2048 por ventana) en vez del
  Timer de 33 ms y los seis Canvas de antes, y lo dibuja el grafo de escena. Un salto de posición
  empieza la historia de nuevo.
- Fuente VT323 (SIL OFL 1.1, `assets/fonts/VT323-Regular.ttf` y su licencia `VT323-OFL.txt`) en los
  recursos del módulo; Tahoma no existe en el wasm y cae en la fuente por defecto de Qt.
- CI ("Prepare Pages artifact"): `<title>WebAssembly MIDI Player</title>` y la ventana de carga de Qt
  (logo y "Qt for WebAssembly: wasmidi") cambiada por una ventana de dekx.cc; el paso falla si no la
  encuentra.
- `tools/browser_bench.py app`: coordenadas del layout nuevo a 1280×900 (SF2… 1176,710; Voices
  1067,739; play 906,88; pausa 943,88; barra de posición 963,60).

### Medición

Chromium del contenedor (4 hilos, sin GPU: WebGL por software), `browser_bench.py app` con un MIDI
sintético de 16 pistas y 60 s (26.880 notas) y MatheryLSD, 40 s con `?visualdebug=1`, la GUI de la
rev. 94 contra esta: 11,0 contra 10,7 cuadros/s y 991 contra 978 ms/s de tareas largas. El hilo de la
página está saturado por el WebGL por software en las dos, así que la diferencia es ruido; no sirve
para decidir entre una y otra. El costo propio del fondo es ≤ 80 nodos (≤ 3160 pares por cuadro) y
el del gráfico ≤ ~2048 muestras por cuadro.

### Verificación

- Compilación wasm local (Emscripten 3.1.56, Qt 6.8.3) y paso "Prepare Pages artifact" OK.
- Chromium: la página carga sin errores de QML, el MIDI y la SF2 cargan con los botones nuevos, suena,
  el roll, el teclado, el LCD, el gráfico y la barra de estado se actualizan; Advanced y Colors abren y
  cierran.

### No verificado

- Nada en una GPU real: cuadros por segundo y el costo del fondo en las PCs del usuario.
- Pantalla completa (necesita un clic real del usuario) y el selector de color dentro del diálogo.
- Ventanas más chicas que 840×600: el panel no tiene scroll y se recorta (el modo celular es la parte 5).

## 93. Revisión 96 — GUI nueva, parte 2: notas y teclado con degradado y textura, teclado vertical

### Pedido

Parte 2 del mockup aprobado (§91-§92): notas con degradado y textura de mapa de bits que rindan igual o
mejor que las de antes, el teclado optimizado y con el mismo estilo, a la izquierda del roll como en el
mockup.

### Cambios

- Notas (`noteVertex`/`noteFragment` en `gl_renderer.cpp`, el mismo programa para el anillo y los tiles
  culleados): degradado vertical a lo alto de la fila de la tecla (arriba el color aclarado 45%, al 45% el
  color, abajo oscurecido 45%), borde de 1 px con el color al 30% y textura de damero de 2 px (blanco al
  13%, alineada a la pantalla) por dentro, como el CSS del mockup. El borde solo se dibuja en un lado
  donde la nota mide ≥ 3 px (ancho o alto de fila); en las notas más chicas queda el relleno, para que las
  notas cortas del Black MIDI no se vuelvan solo borde. Los colores de cada nota (base con el brillo de
  nota sonando, aclarado, oscurecido, opacidad) se calculan una vez por vértice y llegan planos; por
  fragmento quedan las distancias a los bordes (interpoladas), un `mix` y el damero. La regla de capas no
  cambia (mismo orden de dibujo, sin profundidad ni mezcla).
- Fondo del roll: plano rgb(10,9,32) con una línea rgba(46,39,104,.45) en cada do, como el mockup, en
  vez del shader de tono y manchas (`hsl2rgb` y dos manchas por píxel). Sigue premultiplicado con alfa
  1 − Roll·0,75. `setNeuralVisual`/`updateBackgroundHue` se quitaron del renderer (el tono dominante
  sigue en `MainWindow` para la parte 3).
- Cursor de reproducción: barra de 2 px del acento (#6b55d4) con brillo suave (±15 px) en vez de la línea
  roja de 1 px; programa propio sobre el VAO vacío del fondo, mezcla premultiplicada, recortado con
  scissor. Misma posición que antes.
- Teclado (`src/keyboard.cpp`, `Keyboard.qml`): vertical, 62 px a la izquierda del roll, una fila por
  tecla con la misma altura y orden que las filas del roll (grave abajo). Filas blancas con degradado
  horizontal #a99fd6 → #efeaff → #d8d0f6, separador arriba de mi y si, barra negra de 37 px del lado del
  roll (#0b0922 → #2e2768 → #3e3080 con luz de 1 px arriba) y la raya corta de 24 px en las filas negras.
  Teclas apretadas con el color de la nota en degradado y el mismo damero. 181 instancias, se rearman
  solo cuando cambian las teclas o los colores; ya no depende del tono de fondo. El FBO ya no tiene un
  mínimo de 64 px (estiraba el teclado de 62 px).

### Medición

Chromium del contenedor (SwiftShader: WebGL por software, 4 hilos), página aparte con los shaders
reales de `origin/main` contra los nuevos, 1218×760, mediana de tres corridas alternadas. Por software el
costo por fragmento pesa mucho más que en una GPU, así que esto es el peor caso:

| Caso | Notas solas, antes → ahora | Cuadro entero (fondo + notas + cursor) |
|---|---|---|
| 750 k notas cortas (tope de un tile) | 14.617 → 14.435 ms (0,99×) | — |
| 200 k notas cortas | 3.921 → 3.937 ms (1,00×) | 3.946 → 3.913 ms (0,99×) |
| 20 k notas largas | 442 → 462 ms (1,05×) | 456 → 490 ms (1,07×) |
| 2 k notas a lo largo de toda la pantalla (sobredibujo máximo) | 67,5 → 76,6 ms (1,13×) | 78,0 → 64,9 ms (0,83×) |

La primera versión (colores calculados por fragmento) costaba 1,21× en el caso de sobredibujo; pasarlos
al shader de vértices la bajó a 1,13×. Con muchas notas (lo normal en Black MIDI) cuesta lo mismo: manda
el número de instancias, no el fragmento. En el cuadro entero el fondo plano compensa: más barato que
antes con sobredibujo, 7% más con notas largas por software.

### Verificación

- Compilación wasm local; captura en Chromium con un MIDI sintético sonando: notas, teclado, líneas de do
  y cursor como en el mockup, sin errores de shader.
- El primer intento usaba `active` como nombre de variable, palabra reservada en GLSL ES 3.00: el
  programa de notas no compilaba. Lo encontró la página de medición; corregido antes del PR.

### No verificado

- Cuadros por segundo con GPU real en las PCs del usuario (la medición de arriba es por software).
- Colores de las teclas apretadas con MIDIs reales de muchas pistas.

## 94. Revisión 97 — GUI nueva, parte 3: colores, config.xml, acento adaptable y avisos

### Pedido

Parte 3 del mockup aprobado (§91): selector de colores por pista o canal con 16 colores en bucle e
importación del config.xml; la interfaz se adapta a los colores dominantes de las notas; avisos arriba a
la derecha de polifonía máxima, NPS máximo y atraso, con los cuellos de botella (teclado, gráficos,
WasmiSynth, visualizador).

### Cambios

- `ColorsDialog.qml` (diálogo "Note colors" del mockup): Track/Channel, paleta de 16 con la textura de las
  notas (`NoteSwatch.qml`), editor del color elegido (R, G, B, hex), "Reset palette" (los 16 de fábrica),
  Import/Export config.xml, la lista de pistas (todas las del MIDI, mínimo 17 para mostrar la vuelta) o de
  canales con su color, y "Adapt the player to the colors playing most". OK conserva; Cancel devuelve
  colores, modo y "adapt" a como estaban al abrir. Reemplaza el diálogo de 16 cuadros + `ColorDialog`.
- config.xml: se lee `Visual/Colors/Color` con `R`, `G`, `B` (el formato de los reproductores tipo Piano From
  Above; el nombre de la raíz no importa), hasta 16 colores en orden; los que falten quedan como están
  (`wasmidi_browser_import_color_config`, `wasmidi_browser_colors_imported`). Export escribe el mismo
  formato con raíz `WebAssemblyMIDIPlayer` y solo los colores (`wasmidi_browser_download_text`).
- Acento: `MainWindow::accentColor` es el color de la paleta que más suena (el conteo de voces más los
  próximos 150 ms que ya existía para el tono del fondo), o #6b55d4 con "adapt" apagado (`adaptColors`,
  guardado). `MainWindow.qml` lo anima 600 ms y lo pone en `Dk.accent`: título, gráfico, NPS del LCD, barra
  de posición y el cursor del roll (`GLRenderer::setPlayheadColor`). `Dk.mix()` es el `mix()` del mockup;
  `accentLight` = mix 0,3. Se quitaron `dominantHue`/`neuralActivity` (ya no los leía nadie).
- Avisos (`Toasts.qml`), revisados una vez por segundo mientras suena:
  - Polifonía y NPS máximos: aparecen cuando el máximo nuevo lleva 1,5 s sin superarse (y pasados 3 s de
    canción), con el valor y su momento (`peakPolyphonyTime` nuevo); se van a los 8 s; cerrarlo los calla
    hasta el próximo MIDI. Son los máximos de lo ya tocado ("the most so far"): no hay un pico de todo el
    archivo calculado de antemano.
  - Atraso: cuadros de la ventana por segundo (`FrameAnimation`) y su mejor valor como tasa de la pantalla.
    ROLL atrasado si sus cuadros < 80% de esa tasa; KEYBOARD si va > 0,25 s detrás del cursor o hace menos
    de min(80%, 30) cuadros (`takeKeyboardSample`: estados de teclado aplicados y su atraso); GRAPHS si
    dibuja < 80% de lo que permiten la ventana y el reloj de reproducción (`LiveGraph::takeFrames`; el
    reloj se mueve cada 16 ms); WASMISYNTH si hubo underruns o carga > 100%. Aparece tras 2 s seguidos con
    algo atrasado, nombra lo atrasado y se va tras 4 s con todo a tiempo; cerrarlo lo calla 30 s.
- `DkCheckBox.qml`; `DkCaption.startColor` (el aviso de atraso usa el rojo del mockup); las muestras de la
  paleta del panel usan `NoteSwatch`; `assets/images/dither-light.png` (2×2, blanco al 13% en diagonal).

### Verificación

- Compilación wasm local y Chromium del contenedor con un MIDI sintético y MatheryLSD: avisos de NPS y
  polifonía con sus momentos, aviso de atraso (en el contenedor el teclado va a 4-6 cuadros/s por la
  latencia de ~70 ms del Worker y lo nombra; después "Caught up."), diálogo de colores, importación de un
  config.xml de 4 colores (los 4 primeros cambian en el roll, el teclado y el panel) y Cancel que los
  devuelve.

### No verificado

- Export (descarga del navegador) y el config.xml real de BPFA del usuario.
- Umbrales del aviso de atraso en las PCs del usuario (144 Hz, GPU real).

## 95. Revisión 98 — GUI nueva, parte 4: cuadros sin tope

**Pedido (raíz de §92):** que el framebuffer no se limite a 60 cuadros por segundo, sino que haga todos
los cuadros posibles, priorizando por orden lo que más le cuesta a la CPU.

### Causa

El roll ya pedía un cuadro en cada `requestAnimationFrame` (Qt para wasm dibuja con rAF, a la tasa de la
pantalla), pero la posición que dibujaba venía de `currentTime`, que movía un `QTimer` de 16 ms. En una
pantalla de 144 Hz el roll dibujaba 144 cuadros, pero solo ~60 posiciones distintas: el resto repetía la
anterior. Lo mismo el teclado, los gráficos y el LCD, que siguen a `currentTime`. Además, los tiles
culleados se armaban con un presupuesto fijo de 6 ms por cuadro (3 ms fuera de pantalla), pensado para
16,7 ms: a 144 Hz (6,9 ms por cuadro) ese pedazo solo ya llenaba el cuadro.

### Cambios

- `MainWindow::advanceFrame()` (Q_INVOKABLE): mueve el reloj en cada cuadro mostrado. Lo llama un
  `FrameAnimation` de `MainWindow.qml` mientras suena, antes de que la escena se sincronice, así que roll,
  teclado, gráficos y LCD ven una posición nueva en cada cuadro de la pantalla. El `QTimer` de 16 ms queda
  solo para cuando no hay cuadros (más de 40 ms sin uno): pestaña oculta, ventana minimizada o cuadros
  muy lentos; sigue haciendo el fin de canción y la sincronía con WasmiSynth.
- Presupuesto de los tiles por cuadro según la pantalla: `GLRenderer` toma el intervalo más corto entre
  cuadros de cada segundo como período de la pantalla (entre 4 y 16,7 ms) y le da al pedazo el 36% en
  pantalla y el 18% fuera (6 y 3 ms a 60 Hz, como antes; 2,5 y 1,25 ms a 144 Hz). El trabajo más caro, los
  tiles densos, recibe la misma parte de la CPU por segundo a cualquier tasa, y sigue el orden de
  siempre: primero los tiles en pantalla, después el que viene.
- `tools/browser_bench.py --uncapped`: Chromium con `--disable-frame-rate-limit --disable-gpu-vsync`.

Una página web no puede mostrar más cuadros que la tasa de la pantalla: rAF va atado al vsync y un cuadro
de más nunca llega al monitor. "Sin tope" es entonces la tasa del monitor (144, 165, 240 Hz…) en vez de 60.

### Verificación

- Compilación wasm local (Emscripten 3.1.56, Qt 6.8.3) OK.
- Chromium del contenedor (sin GPU, roll por software a ~13 cuadros/s) con `--uncapped`, un MIDI
  sintético y MatheryLSD: suena, el roll y el teclado avanzan; con cuadros más lentos que 40 ms el
  temporizador de respaldo sigue moviendo el reloj.

### No verificado

- Cuadros por segundo en las PCs del usuario con pantalla de más de 60 Hz (el contenedor no tiene GPU).
- Si a 144-240 Hz mover el reloj en cada cuadro (un pedido de teclado por cuadro al Worker) le pesa al
  Worker del teclado con The Nuker 3.

## 96. Revisión 99 — GUI nueva, parte 5: modo celular vertical, avisos en el roll y picos de todo el MIDI

**Pedido:** la parte 5 de §92 (un modo de celular funcional con el reproductor entero en vertical, del
mockup `Mobile.dc.html`) y tres defectos que el usuario vio en escritorio con la rev. 98: los avisos van
en la esquina del visualizador, no de la ventana; los máximos de NPS y polifonía son los de todo el MIDI,
no los de lo ya tocado; falta el contador de CC por segundo.

### Cambios

- Modo celular (`MainWindow.qml`, `phone`: ventana más alta que ancha y de menos de 760 px de ancho):
  título, LCD con reloj, NPS, Poly, CC/s y FPS; roll con el tiempo subiendo por la pantalla; teclado de
  46 px a lo ancho abajo; barra de posición; botones grandes (abrir, stop, play, pausa, note speed) y
  pestañas Song/Colors/Synth/View que abren los grupos del panel sobre la parte baja del roll (`Dock.section`,
  un segundo `Dock`; Synth suma el volumen). Tocar el roll vacío abre un MIDI. Ventana mínima 320×480.
- Roll vertical sin otro camino de dibujo: `GLRenderer::setPortrait` cambia x por y en el vertex shader
  de las notas (`uPortrait`), y el fondo y el cursor eligen el eje de `gl_FragCoord`. Todo lo demás (tiles
  culleados, `rasterWidth`, distancias del borde, tijeras con `scissorRoll`) trabaja en el eje del tiempo
  (`timePx()`) y el de las teclas (`keyPx()`), así que el culling y la regla de capas son los mismos.
  El teclado (`Keyboard.portrait`) hace lo mismo: una columna por tecla y las negras arriba.
- Avisos (`Toasts`) dentro del área del roll, arriba a la derecha del roll (en escritorio y en celular).
- Picos de todo el MIDI: el parser cuenta también los note-off que cierran una nota por cubeta de
  densidad (`noteOffDensity`, ppq/8 ticks, también en la carga en paralelo). `wmp_song_peaks_js` da el NPS
  máximo (note-ons de los últimos 0,25 s × 4, como el contador en vivo) y la polifonía máxima (note-ons
  menos note-offs acumulados al final de cada cubeta) con sus segundos. WASMIDI los pide al terminar la
  carga (`song-peaks`) y el aviso sale cuando la reproducción pasa por ese momento ("the most in the
  whole MIDI"), no después de un salto. La polifonía se mide al final de cada cubeta: una nota que empieza
  y termina dentro de una cubeta no suma.
- LCD: CC/s (`ccPerSecond`, que ya llegaba con el estado del teclado) en escritorio y celular.
- `Dock.takeGraphFrames()` da -1 con el gráfico oculto (pestaña cerrada en celular): GRAPHS no cuenta.

### Verificación

- Compilación wasm local (Emscripten 3.1.56, Qt 6.8.3) OK.
- Chromium del contenedor a 390×844 con un MIDI sintético y MatheryLSD: roll vertical, teclado iluminado
  abajo con los colores de las notas, pestañas, panel de Synth/Colors/View, gráfico en Song.
- Escritorio 1280×900, MIDI sintético con una ráfaga en 6,0-6,5 s (8 pistas, 120 notas/s cada una, notas
  de 1 s) y CC 7 en cada nota: avisos en la esquina del roll con 960 NPS a 0:06.3 y 488 notas a la vez a
  0:06.6 (lo esperado: 960 y 480 + las de fondo), CC/s 64.
- `tools/feeder_check/check.sh` (parser BPFA contra el original, también en paralelo).

### No verificado

- Un celular real (táctil, `devicePixelRatio` alto, pantalla completa) y el rendimiento del roll vertical
  con GPU.
- Los picos con The Nuker 3/4 (cuánto tarda `wmp_song_peaks_js` con millones de cubetas).

## 97. Revisión 100 — celular horizontal y roll vertical como Synthesia

### Pedido

Dekx, con una captura de un celular acostado: "Esta bugeado en movil desde el pov horizontal" (salía el
diseño de escritorio: encabezado cortado y grupos del panel encimados). Y: "el sintetizador vertical esta
al reves, debe hacer caer las notas como synthesia".

### Causa

- `phone` solo se activaba con la ventana más alta que ancha. Un celular acostado (~780×360) caía en el
  diseño de escritorio, que necesita ~900×600.
- En vertical el cursor iba centrado como en horizontal (la mitad de la ventana ya tocada) y el eje del
  tiempo quedaba hacia abajo en pantalla: las notas subían desde el teclado. Medido en Chromium: entre dos
  capturas a 0,86 s el roll se corría 254 px hacia arriba.

### Cambios

- `MainWindow.qml`: `compact` = no `phone` y ventana de menos de 540 px de alto o 860 px de ancho;
  `mobileUi` = `phone` o `compact`. Con `compact` se usa el encabezado del celular (reloj, NPS, Poly,
  CC/s, FPS), el panel por pestañas sobre el roll y una sola fila de transporte: abrir, stop, play, pausa,
  barra de posición, velocidad de notas y las cuatro pestañas como íconos. El roll y el teclado siguen
  horizontales. El panel de pestañas (celular y compacto) se desplaza si no entra (`Flickable`).
- `gl_renderer.cpp`, modo vertical: la ventana va de `currentTick` a `currentTick + windowTicks` (el cursor
  queda sobre el teclado y todo lo visible está por sonar), el eje del tiempo se invierte en el shader de
  notas (`vec4(y, -x, …)`), en `scissorRoll` y en el centro del cursor. Las notas caen hacia el teclado.
  Horizontal no cambia.

### Verificación

- Compilación wasm local OK.
- Chromium 390×844 con el MIDI sintético: entre capturas a 2,57 y 3,43 s el roll se corre 253 px hacia
  abajo (correlación 0,99); el cursor brilla justo sobre el teclado y las teclas se iluminan cuando llegan
  las notas.
- Chromium 780×360: diseño compacto sin cortes, roll horizontal con teclado a la izquierda, pestañas Synth
  y Song abiertas sobre el roll.

### No verificado

- Un celular real en ambas orientaciones.

## 98. Revisión 101 — teclado del celular acostado

### Pedido

Dekx, con la rev. 100 en el celular acostado: "El teclado en horizontal se ve apilado, como apeñuzcado".

### Causa

El diseño compacto dejaba el roll horizontal con el teclado vertical a la izquierda: 128 filas en
~150 px de alto (con la barra de Brave el roll queda en ~145 px), poco más de 1 px por tecla.

### Cambios

- `MainWindow.qml`: `falling` = `phone` o `compact` con menos de 540 px de alto. Con `falling` el roll usa
  el modo vertical de la rev. 100 (notas que caen, teclado abajo a lo ancho). En el celular acostado el
  teclado mide 36 px de alto y cada tecla ~5,7 px de ancho (128 en 740 px). Un compacto alto y angosto
  (menos de 860 px de ancho y 540 o más de alto) sigue horizontal.

### Verificación

- Compilación wasm local OK. Chromium 740×270 y 844×390 con el MIDI sintético: teclado abajo con las
  teclas legibles, notas cayendo e iluminando las teclas.

### No verificado

- Un celular real.

## 99. Revisión 102 — teclado horizontal en ventanas bajas (y se deshace la rev. 101)

### Pedido

Dekx: "No hiciste lo que te pedí, lo moviste a modo vertical enves de arreglar el teclado horizontal."
La rev. 101 cambió de modo en vez de arreglar el teclado horizontal; se deshace (`falling` vuelve a ser
solo `phone`) y el celular acostado vuelve al roll horizontal de la rev. 100.

### Causa

Con ~1,1 px por tecla (128 filas en ~145 px) cada fila seguía dibujando sus detalles de 1 px: el divisor
entre E-F y B-C, el divisor del medio del lado blanco de las teclas negras y el borde claro de las
barras negras. A ese tamaño esas líneas ocupan la fila entera y el teclado se ve como un código de barras
de rayas grises apiladas.

### Cambios

- `keyboard.cpp`, shader: con filas de menos de 4 px no se dibujan esos detalles de 1 px. Queda el lado
  blanco continuo y las barras negras sobre él, como un teclado. Con filas de 4 px o más no cambia nada.

### Verificación

- Compilación wasm local OK. Chromium 740×270 con el MIDI sintético, comparando capturas ampliadas: sin
  las rayas grises; las teclas que suenan se iluminan con el color de la nota.

### No verificado

- Un celular real.

## 100. Revisión 103 — panel del celular fuera del roll, teclas en píxeles enteros, ícono y reloj

### Pedido

Dekx, con capturas del celular (Brave, `devicePixelRatio` ~2,6): el panel de las pestañas tapaba la parte
de abajo del roll y las notas que llegan al teclado ("reducir el piano roll solo cambia qué tan lejos
puede ver"); el ícono de Colors estaba mal hecho; el tiempo mal centrado; los paneles mal adaptados en
horizontal; y el teclado horizontal se seguía viendo dañado.

### Causa

- El panel era un rectángulo encima del roll (rev. 99).
- El roll y el teclado se dibujan en un FBO del tamaño CSS y el navegador lo escala ×DPR. Con ~1,1 px
  CSS por tecla las filas caían en bordes fraccionarios: unas teclas de 1 px y otras de 2, sin patrón, y
  escaladas ×2,6 eso se ve como un código de barras irregular.
- El reloj del celular tenía posición fija (x 8, y 3) y el total se salía del recuadro.

### Cambios

- `MainWindow.qml`: el bloque roll + panel es un `GridLayout`. Celular vertical: el panel va debajo del
  teclado y el roll se achica (las notas siguen llegando al teclado). Celular acostado: el panel va a la
  derecha del roll (hasta 320 px o 45% del ancho), con desplazamiento si no cabe. Reloj centrado.
- Filas de teclas en píxeles enteros con todas las octavas iguales: octava = floor(L·12/128) px CSS,
  tecla j de la octava empieza en floor(j·octava/12), centrado en el largo L. Lo usan el shader de notas
  (`keyEdge`), el fondo (líneas de do) y el teclado (`rebuild`), así que las filas coinciden. Sobran
  como mucho ~11 px.
- `keyboard.cpp`: el FBO del teclado ahora es del tamaño en píxeles del dispositivo (es chico); las
  medidas del shader siguen en px CSS (`uScale`).
- `Glyphs.qml`: paleta de pintor rellena con la muesca a la izquierda y tres puntos.

### Verificación

- Compilación wasm local OK. Chromium con `device_scale_factor` 2,625: 390×844 (panel Song debajo del
  teclado, notas hasta el teclado, teclas regulares) y 760×300 (panel Synth a la derecha, teclado
  horizontal con el mismo patrón en cada octava, notas alineadas con las teclas iluminadas).

### No verificado

- Un celular real. Escritorio con DPR distinto de 1 (las filas ahora pueden no llenar todo el alto: hasta
  ~11 px de margen).

## 101. Revisión 104 — teclado sin franjas blancas, gráfico del ancho del note speed, registro de lag

### Pedido

Dekx: (1) no le gustan las partes sin color del teclado; (2) el gráfico abarca demasiados datos: debe
mostrar solo los segundos del note speed del roll, para que suba y baje tan rápido como el roll; (3) el
aviso de lag debe contar cuántos segundos dura y, con un botón de las opciones, guardar un registro
descargable mientras el MIDI esté abierto: tiempo total de lag por fuente, FPS mínimo/promedio/máximo,
nombre del MIDI y el segundo del MIDI donde empezó y terminó cada lag. Se borra al cerrar el programa o al
cambiar el MIDI o la SoundFont.

### Causa

- La fila de una tecla negra tenía una franja de cara blanca (`KindBlackRow`) que no se pintaba cuando
  sonaban las teclas blancas de al lado.
- `LiveGraph` usaba un ancho fijo de 10 s y su escala vertical tomaba el máximo de todas las muestras
  guardadas.

### Cambios

- `keyboard.cpp`: la franja blanca de una tecla negra se parte en dos mitades que toman el color de la
  tecla blanca de abajo y de la de arriba; el divisor de 1 px va entre ellas. Sin `KindBlackRow`.
- `Dock.qml`: el gráfico usa `span` = note speed (mínimo 0,1 s; `setSpan` acepta 0,1-120 s) y la etiqueta
  muestra los segundos. `live_graph.cpp`: el máximo de la escala es solo el de las muestras visibles.
- `Toasts.qml`: el aviso dice "Out of frames for N s" y, al recuperarse, "Caught up after N s". Con
  `lagLog` encendido, cada segundo de reproducción se anota: FPS del roll, el teclado y los gráficos, los
  segundos de lag por fuente y los episodios (segundos del MIDI de inicio y fin, duración y fuentes). Un
  salto o una pausa cierra el episodio. Se borra con `documentRevision` y con `soundfontName`.
- `AdvancedDialog.qml` (grupo View): botón "Lag log" ON/OFF (guardado en localStorage, `lagLog`),
  "Download" (texto `<midi>-lag-log.txt`) y el resumen "N s of lag in M episodes".
- `mainwindow.cpp`: propiedad `lagLog`, `downloadText(name, text)`; la descarga del navegador recibe el
  tipo MIME.

### Verificación

- Compilación wasm local OK. Chromium 1280×900 con `burst.mid`: gráfico de 1 s con note speed 1,00; con el
  registro encendido, "Download" baja el archivo con totales, FPS y un episodio (0:06.3-0:07.1, KEYBOARD).
- Teclado durante la ráfaga: las mitades blancas junto a las negras se pintan.

### No verificado

- En las PCs del usuario y en el celular.
