cmake_minimum_required(VERSION 3.21)
project(wasmidi VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick Qml OpenGL)
qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(wasmidi
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.hpp
    src/pianoroll.cpp
    src/pianoroll.hpp
    src/keyboard.cpp
    src/keyboard.hpp
    src/midi/midi_parser.cpp
    src/midi/midi_parser.hpp
    src/midi/midi_document_codec.cpp
    src/midi/midi_document_codec.hpp
    src/midi/scheduler.cpp
    src/midi/scheduler.hpp
    src/renderer/gl_renderer.cpp
    src/renderer/gl_renderer.hpp
    src/renderer/note_raster_compositor.cpp
    src/renderer/note_raster_compositor.hpp
    src/neural_background.cpp
    src/neural_background.hpp
    src/live_graph.cpp
    src/live_graph.hpp
)

set_source_files_properties(src/qml/Dk.qml src/qml/Glyphs.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(wasmidi
    URI Wasmidi
    VERSION 1.0
    QML_FILES
        src/qml/MainWindow.qml
        src/qml/PianoRoll.qml
        src/qml/Keyboard.qml
        src/qml/Dock.qml
        src/qml/AdvancedDialog.qml
        src/qml/Dk.qml
        src/qml/Glyphs.qml
        src/qml/DkBevel.qml
        src/qml/DkButton.qml
        src/qml/DkCapButton.qml
        src/qml/DkCaption.qml
        src/qml/DkCell.qml
        src/qml/DkDialog.qml
        src/qml/DkField.qml
        src/qml/DkGroup.qml
        src/qml/DkReadout.qml
        src/qml/DkSlider.qml
        src/qml/PixelIcon.qml
        src/qml/DkCheckBox.qml
        src/qml/NoteSwatch.qml
        src/qml/ColorsDialog.qml
        src/qml/Toasts.qml
    RESOURCES
        assets/fonts/VT323-Regular.ttf
        assets/images/dither-face.png
        assets/images/dither-light.png
)

target_include_directories(wasmidi PRIVATE src)

target_link_libraries(wasmidi PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Qml
    Qt6::OpenGL
)

if(EMSCRIPTEN)
    # Emscripten 3.1.56 MEMORY64 has a growMemory() bug: its generated JS can
    # hand BigInt() a fractional page count. Patch the installed JS runtime
    # template before any project link step so the corrected helper is emitted
    # inside MODULARIZE's private factory scope. The patch is idempotent and
    # recognizes newer Emscripten versions that already contain the fix.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(
        COMMAND
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_emscripten_memory64_growth.py"
            "${CMAKE_CXX_COMPILER}"
        RESULT_VARIABLE WASMIDI_MEMORY64_PATCH_RESULT
        OUTPUT_VARIABLE WASMIDI_MEMORY64_PATCH_OUTPUT
        ERROR_VARIABLE WASMIDI_MEMORY64_PATCH_ERROR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT WASMIDI_MEMORY64_PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Could not apply the Emscripten Memory64 growth fix: "
            "${WASMIDI_MEMORY64_PATCH_ERROR}")
    endif()
    message(STATUS "${WASMIDI_MEMORY64_PATCH_OUTPUT}")
    # Parse large MIDI files in a dedicated Worker-owned WASM instance. The Qt
    # module receives only the compact finalized MidiDocument wire image, so the
    # browser UI/render thread never performs the two full MIDI parse passes.
    add_executable(midi_parser_core
        src/midi/midi_parser.cpp
        src/midi/midi_document_codec.cpp
        src/midi/midi_mapped_store.cpp
        src/midi/bpfa_midi_store.cpp
        src/midi/midi_worker_core.cpp
    )

    # zstd for the BPFA compressed MIDI source (HANDOFF sec. 46).
    include(FetchContent)
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Declare(wasmidi_zstd
        URL https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz)
    FetchContent_GetProperties(wasmidi_zstd)
    if(NOT wasmidi_zstd_POPULATED)
        FetchContent_Populate(wasmidi_zstd)
    endif()
    file(GLOB WASMIDI_ZSTD_SOURCES
        "${wasmidi_zstd_SOURCE_DIR}/lib/common/*.c"
        "${wasmidi_zstd_SOURCE_DIR}/lib/compress/*.c"
        "${wasmidi_zstd_SOURCE_DIR}/lib/decompress/*.c")
    add_library(wasmidi_zstd STATIC ${WASMIDI_ZSTD_SOURCES})
    target_include_directories(wasmidi_zstd PUBLIC "${wasmidi_zstd_SOURCE_DIR}/lib")
    target_compile_definitions(wasmidi_zstd PRIVATE ZSTD_DISABLE_ASM=1 XXH_NAMESPACE=ZSTD_)
    target_compile_options(wasmidi_zstd PRIVATE "-sMEMORY64=1" "-O3")
    target_link_libraries(midi_parser_core PRIVATE wasmidi_zstd)

    set_target_properties(midi_parser_core PROPERTIES
        OUTPUT_NAME "wasmidi-midi-parser"
        SUFFIX ".js"
    )

    target_include_directories(midi_parser_core PRIVATE src)
    target_compile_options(midi_parser_core PRIVATE
        -O3
        -flto
        -fexceptions
        # Chrome 133+ ships WebAssembly Memory64. The MIDI parser is isolated
        # from Qt, so it can use 64-bit pointers without changing the Qt wasm32
        # renderer/player ABI. This removes the artificial 4 GiB parser heap.
        "-sMEMORY64=1"
    )
    target_link_options(midi_parser_core PRIVATE
        -O3
        -flto
        -fexceptions
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=WasmidiMidiParserCore"
        # Keep a conventional Emscripten application lifecycle, but never tear
        # the runtime down. The Worker normally passes noInitialRun=true; if a
        # browser/generated-glue variant still executes main(), the inert main
        # simply returns and NO_EXIT_RUNTIME keeps the exported parser alive.
        "-sNO_EXIT_RUNTIME=1"
        # Package the parser's WASM payload into its JS glue. This removes the
        # possibility of a cached/new .js file being paired with a stale .wasm
        # file after a Pages deployment.
        "-sSINGLE_FILE=1"
        # Node support is build/CI-only. The same generated module runs as a
        # classic browser Worker in production and is executed in Node during
        # CI so a startup abort cannot reach Pages unnoticed again.
        "-sENVIRONMENT=worker,node"
        # Full wasm64 parser. Memory is committed only as allocations require it;
        # the browser/OS may refuse growth earlier under real memory pressure.
        # Current Chromium/V8 Memory64 has a 16 GiB per-memory hard ceiling, so
        # this is the highest useful declaration rather than an app-imposed RAM
        # budget.
        "-sMEMORY64=1"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=67108864"
        "-sMAXIMUM_MEMORY=17179869184"
        # +20% per growth, capped at 96 MiB of slack (HANDOFF sec. 57). The former
        # GEOMETRIC_STEP=0 grew by exactly each request: hundreds of growths on huge MIDIs.
        "-sMEMORY_GROWTH_GEOMETRIC_STEP=0.2"
        "-sMEMORY_GROWTH_GEOMETRIC_CAP=100663296"
        "-sSTACK_SIZE=4194304"
        "-sFILESYSTEM=0"
        # Parser-only assertions keep fatal startup/runtime diagnostics useful;
        # they do not affect the real-time renderer or SnappySynth hot paths.
        "-sASSERTIONS=1"
        "-sINCOMING_MODULE_JS_API=['noInitialRun','noExitRuntime','onAbort','print','printErr']"
        "-sEXPORTED_FUNCTIONS=['_wmp_parse_file_js','_wmp_pack','_wmp_result_ptr_js','_wmp_result_size_js','_wmp_release_result','_wmp_build_visual_page_js','_wmp_visual_page_ptr_js','_wmp_visual_page_count_js','_wmp_build_key_snapshot_js','_wmp_build_live_snapshot_js','_wmp_key_snapshot_ptr_js','_wmp_key_snapshot_word_count_js','_wmp_reset_event_cursor_js','_wmp_build_event_batch_js','_wmp_event_batch_ptr_js','_wmp_event_batch_count_js','_wmp_event_batch_time_ptr_js','_wmp_event_batch_complete_js','_wmp_event_batch_next_tick_js','_wmp_event_batch_has_next_tick_js','_wmp_build_historical_sysex_js','_wmp_build_historical_selector_state_js','_wmp_historical_selector_state_ptr_js','_wmp_historical_selector_state_count_js','_wmp_sysex_batch_event_ptr_js','_wmp_sysex_batch_event_count_js','_wmp_sysex_batch_data_ptr_js','_wmp_sysex_batch_data_size_js','_wmp_reset_render_cursor_js','_wmp_build_render_sweep_js','_wmp_render_append_ptr_js','_wmp_render_append_count_js','_wmp_render_append_base_js','_wmp_render_close_ptr_js','_wmp_render_close_count_js','_wmp_render_batch_complete_js','_wmp_render_batch_next_tick_js','_wmp_render_batch_has_next_tick_js','_wmp_tick_to_seconds_js','_wmp_error_ptr_js','_wmp_error_size_js','_wmp_pointer_bits','_wmp_memory_growth_probe_js','_wmp_heap_used_js','_wmp_set_raw_source_limit_js','_wmp_track_table_js','_wmp_track_table_ptr_js','_wmp_partial_index_js','_wmp_partial_blob_ptr_js','_wmp_partial_blob_size_js','_wmp_partial_release_js','_wmp_merge_begin_js','_wmp_merge_read_raw_js','_wmp_merge_buffer_js','_wmp_merge_part_js','_wmp_merge_finish_js','_wmp_seconds_to_tick_js','_wmp_document_info_js','_wmp_text_events_js','_wmp_text_events_size_js','_wmp_density_js','_wmp_song_peaks_js','_wmp_feed_config_js','_wmp_feed_reset_js','_wmp_feed_build_js','_wmp_feed_safe_seconds_js','_wmp_feed_complete_js','_wmp_feed_block_js','_wmp_feed_block_seconds_js','_wmp_feed_buffer_js','_wmp_feed_buffer_size_js','_wmp_feed_buffer_ptr_js','_wmp_feed_image_js','_wmp_source_slab_count_js','_wmp_source_slab_ptr_js','_wmp_source_slab_bytes_js','_wmp_feed_request_js','_wmp_feed_stitch_js','_wmp_feed_helper_init_js','_wmp_feed_decode_js','_wmp_joint_decode_js','_wmp_feed_block_tick_js','_wmp_feed_block_at_tick_js','_wmp_visual_config_js','_wmp_visual_plan_js','_wmp_visual_kept_js','_wmp_visual_keep_all_js','_wmp_roll_summary_js','_wmp_render_fold_js','_wmp_visual_held_bytes_js','_wmp_visual_ahead_words_js','_wmp_visual_prefill_local_js','_wmp_visual_head_js','_wmp_visual_done_js','_wmp_visual_span_end_js','_wmp_visual_request_js','_wmp_visual_decode_js','_wmp_visual_append_js','_wmp_live_buffer_js','_wmp_live_buffer_ptr_js','_wmp_live_buffer_size_js','_wmp_live_image_js','_wmp_live_stream_export_js','_wmp_live_rebuild_js','_wmp_live_external_js','_wmp_live_external_position_js','_wmp_live_init_js','_wmp_live_stream_import_js','_wmp_live_restart_js','_wmp_live_restart_at_js','_wmp_live_forward_begin_js','_wmp_live_forward_js','_wmp_live_forward_tick_js','_wmp_live_forward_base_js','_wmp_live_step_js','_wmp_live_resume_js','_wmp_live_position_js']"
        "-sEXPORTED_RUNTIME_METHODS=['UTF8ToString']"
    )

    # Qt stays single-threaded. SnappySynthV2 is a completely separate WASM
    # module hosted in its own browser Worker and may therefore use pthreads
    # without moving the QML/renderer runtime off Qt's wasm_singlethread build.
    add_executable(snappysynth_core
        third_party/snappysynthv2/snappy_wasm_core.c
        third_party/snappysynthv2/wasm_stubs.c
        third_party/snappysynthv2/Voice/voice.c
        third_party/snappysynthv2/Parser/sf2_parser.c
        third_party/snappysynthv2/Parser/sfz_parser.c
        third_party/snappysynthv2/Parser/wav_loader.c
    )

    set_target_properties(snappysynth_core PROPERTIES
        OUTPUT_NAME "snappysynth-core"
        SUFFIX ".js"
    )

    target_include_directories(snappysynth_core PRIVATE
        third_party/snappysynthv2
    )

    target_compile_definitions(snappysynth_core PRIVATE
        SNAPPYSYNTH_WASM=1
    )

    target_compile_options(snappysynth_core PRIVATE
        -O3
        -ffast-math
        -fomit-frame-pointer
        -funroll-loops
        -flto
        -pthread
        -msimd128
        -mrelaxed-simd
    )

    target_link_options(snappysynth_core PRIVATE
        -O3
        -ffast-math
        -fomit-frame-pointer
        -funroll-loops
        -flto
        -pthread
        -msimd128
        -mrelaxed-simd
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=SnappySynthCore"
        "-sENVIRONMENT=worker"
        # The render workers all come out of this pool, and it must not be sized
        # from navigator.hardwareConcurrency. Brave farbles that value down for
        # fingerprinting defence (a 24-thread machine reports 7), so sizing the
        # pool from it would cap the worker count the user explicitly asked for.
        # Use a floor that covers a high explicit request. Idle pooled workers
        # cost a Worker each but no CPU.
        "-sPTHREAD_POOL_SIZE=Math.max((navigator.hardwareConcurrency || 4) + 8, 34)"
        "-sPTHREAD_POOL_SIZE_STRICT=0"
        "-sALLOW_MEMORY_GROWTH=1"
        # This module runs with pthreads, so its heap is a SharedArrayBuffer and
        # memory.grow has to synchronise every worker thread. With 24 render
        # workers each growth freezes all of them at once: nothing runs and
        # nothing logs, which reads as a stall with no CPU load behind it.
        #
        # The allocation that provokes it scales with the worker count.
        # CH_EVENT_QUEUE_SIZE is 16384 events per (worker, channel), so 24
        # workers across 16 channels is over 150 MB of event queues alone,
        # against roughly 44 MB at 7 workers. Add the soundfont samples and the
        # voice pool and 256 MB is crossed during startup, then repeatedly as
        # the geometric step nibbles upward.
        #
        # So: start high enough that playback never grows, and make any growth
        # that does happen a single large step instead of many small ones.
        "-sINITIAL_MEMORY=1073741824"
        "-sMEMORY_GROWTH_GEOMETRIC_STEP=0"
        "-sMEMORY_GROWTH_LINEAR_STEP=268435456"
        "-sMAXIMUM_MEMORY=3221225472"
        "-sSTACK_SIZE=8388608"
        # Render workers use shallow stacks; the default would multiply across
        # the whole pool for no benefit.
        "-sDEFAULT_PTHREAD_STACK_SIZE=262144"
        "-sFILESYSTEM=1"
        "-sFORCE_FILESYSTEM=1"
        "-sEXPORTED_FUNCTIONS=['_ssw_init','_ssw_init_ex','_ssw_load_sf2','_ssw_clear_soundfonts','_ssw_set_volume','_ssw_set_vor_mode','_ssw_set_soft_clip','_ssw_reset','_ssw_set_song_time','_ssw_clear_events','_ssw_queue_events','_ssw_render','_ssw_render_into','_ssw_render_queued_into','_ssw_send_sysex','_ssw_active_voices','_ssw_free_voices','_ssw_steals','_ssw_rebalanced','_ssw_dropped_notes','_ssw_layer_count','_ssw_region_count','_ssw_channels','_ssw_sample_rate','_ssw_bits_per_sample','_ssw_num_buffers','_ssw_worker_count','_ssw_worker_thread_failures','_ssw_detected_cores','_ssw_render_load_x1000','_ssw_last_render_us','_ssw_last_dispatch_us','_ssw_path_fast_voices','_ssw_path_scalar_voices','_ssw_miss_interp','_ssw_workers_participating','_ssw_alloc_us','_ssw_notes_started','_ssw_presample_seen','_ssw_controllers_collapsed','_ssw_cc_hotspot','_ssw_set_debug_metrics','_ssw_cc_hotspot_count','_ssw_presample_skipped','_ssw_presample_resampled','_ssw_voices_recycled','_ssw_miss_loop','_ssw_miss_filter','_ssw_miss_other','_ssw_path_simd_voice_voices','_ssw_worker_busy_us','_ssw_render_budget','_ssw_set_render_limit_percent','_ssw_set_admission_percent','_ssw_admission_dropped','_ssw_skip_to','_ssw_late_dropped','_ssw_set_steal_cap_percent','_ssw_set_deterministic','_ssw_wgpu_set_enabled','_ssw_wgpu_begin_block','_ssw_wgpu_set_frame_cap','_ssw_wgpu_set_frame_base','_ssw_wgpu_requests_ptr','_ssw_wgpu_request_count','_ssw_wgpu_collect_samples','_ssw_global_mix_gain','_ssw_output_stage_apply','_ssw_warmup','_ssw_shutdown','_malloc','_free']"
        "-sEXPORTED_RUNTIME_METHODS=['FS','WORKERFS','lengthBytesUTF8','stringToUTF8']"
        "-lworkerfs.js"
    )

    # Archive reader (HANDOFF sec. 110): LZMA / LZMA2 from the public-domain LZMA SDK,
    # a small wasm32 module for web/archive-worker.js.
    add_executable(archive_core
        src/archive/archive_codec.c
        third_party/lzma/LzmaDec.c
        third_party/lzma/Lzma2Dec.c
    )
    set_target_properties(archive_core PROPERTIES
        OUTPUT_NAME "wasmidi-archive-core"
        SUFFIX ".js"
    )
    target_include_directories(archive_core PRIVATE third_party/lzma)
    target_compile_options(archive_core PRIVATE -O3)
    target_link_options(archive_core PRIVATE
        -O3
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=WasmidiArchiveCore"
        "-sENVIRONMENT=worker,node"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sMAXIMUM_MEMORY=4294967296"
        "-sFILESYSTEM=0"
        "-sEXPORTED_FUNCTIONS=['_wa_dec_new','_wa_dec_free','_wa_dec_input','_wa_dec_input_capacity','_wa_dec_run','_wa_dec_consumed','_wa_dec_out_ptr','_wa_dec_out_size','_malloc','_free']"
    )

    add_dependencies(wasmidi midi_parser_core snappysynth_core archive_core)

    # Qt for WebAssembly owns the generated HTML/JS/WASM packaging.
    target_link_options(wasmidi PRIVATE
        # Qt remains wasm32, but remove its historical 2 GiB default ceiling.
        # Parsed-document installation can therefore use the full 4 GiB wasm32
        # address space when the browser has enough memory.
        "-sALLOW_MEMORY_GROWTH=1"
        "-sMAXIMUM_MEMORY=4294967296"
        "-sMEMORY_GROWTH_GEOMETRIC_STEP=0"
        "-sFULL_ES3=1"
        "-sMIN_WEBGL_VERSION=2"
        "-sMAX_WEBGL_VERSION=2"
    )
    # Function names in wasmidi.wasm for DevTools profiles (local builds only).
    option(WASMIDI_PROFILE_FUNCS "Keep function names in wasmidi.wasm" OFF)
    if(WASMIDI_PROFILE_FUNCS)
        target_link_options(wasmidi PRIVATE "--profiling-funcs")
    endif()
endif()
