// ?visualdebug=1: one [WASMIDI app] line per second in the console (HANDOFF sec. 79).
(function () {
  if (new URLSearchParams(location.search).get('visualdebug') !== '1') return;
  var st = { frames: 0, slow: 0, maxGap: 0, last: 0, longN: 0, longMs: 0,
             req: 0, rep: 0, lat: [], pending: new Map(), tick: 0, safe: 0, deltas: 0, under: -1, held: 0 };
  var W = window.Worker;
  window.Worker = function (url, opts) {
    var w = new W(url, opts), post = w.postMessage.bind(w);
    w.postMessage = function (m, t) {
      if (m && m.type === 'key-state') { st.req++; st.tick = m.tick; st.pending.set(m.requestId >>> 0, performance.now()); }
      return post(m, t);
    };
    w.addEventListener('message', function (e) {
      var m = e.data || {};
      if (m.type === 'live-state') {
        var t0 = st.pending.get(m.requestId >>> 0);
        if (t0 !== undefined) { st.rep++; st.lat.push(performance.now() - t0); st.pending.delete(m.requestId >>> 0); }
      } else if (m.type === 'sharp-render-delta') { st.deltas++; st.safe = Math.max(st.safe, m.safeThrough || 0); }
      else if (m.type === 'sharp-render-reset') st.safe = 0;
    });
    return w;
  };
  window.Worker.prototype = W.prototype;
  function frame(now) {
    if (st.last) { var gap = now - st.last; if (gap > st.maxGap) st.maxGap = gap; if (gap > 25) st.slow++; }
    st.last = now; st.frames++;
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
  try {
    new PerformanceObserver(function (list) {
      list.getEntries().forEach(function (e) { st.longN++; st.longMs += e.duration; });
    }).observe({ entryTypes: ['longtask'] });
  } catch (e) {}
  setInterval(function () {
    var b = globalThis.WasmidiSnappyBridge, s = b ? b.state : null;
    var lat = st.lat.splice(0), avg = 0, max = 0;
    for (var i = 0; i < lat.length; ++i) { avg += lat[i]; if (lat[i] > max) max = lat[i]; }
    if (lat.length) avg /= lat.length;
    var under = s ? s.underruns : -1;
    console.info('[WASMIDI app] frames ' + st.frames + ' (>25 ms: ' + st.slow + ', max gap ' + st.maxGap.toFixed(0) +
      ' ms), long tasks ' + st.longN + ' (' + st.longMs.toFixed(0) + ' ms), keyboard ' + st.rep + '/' + st.req +
      ' latency avg ' + avg.toFixed(1) + ' max ' + max.toFixed(1) + ' ms, roll lead ' + Math.round(st.safe - st.tick) +
      ' ticks (' + st.deltas + ' deltas), roll held ' + ((globalThis.__wasmidiRollHeld | 0) - st.held) +
      ' catch-ups ' + (globalThis.__wasmidiRollCatchUps | 0) + ', drawn max ' + (globalThis.__wasmidiRollDrawnMax || 0) +
      ' notes (raw ' + (globalThis.__wasmidiRollRawMax || 0) + ', resident ' + (globalThis.__wasmidiRollResident || 0) + '), tiles ' +
      (globalThis.__wasmidiRollTiles | 0) + ' (visible ' + (globalThis.__wasmidiRollTilesVisible | 0) + ', ' +
      (globalThis.__wasmidiRollTileMs || 0).toFixed(0) + ' ms, max ' + (globalThis.__wasmidiRollTileMax || 0).toFixed(0) + ' ms), synth voices ' + (s ? s.activeVoices : '-') + ' load ' +
      (s && s.renderLoadPercent != null ? Number(s.renderLoadPercent).toFixed(0) : '-') + '% ring ' +
      (s && s.ringFillPercent != null ? Number(s.ringFillPercent).toFixed(0) : '-') + '% underruns +' +
      (st.under >= 0 && under >= 0 ? under - st.under : 0) + ' caught up ' +
      (s && s.catchUpSeconds != null ? (s.catchUpSeconds * 1000).toFixed(0) : '-') + ' ms (late notes ' +
      (s ? s.lateDropped : '-') + ') workers ' + (s ? s.workerCount : '-') +
      ' gpu ' + (s ? (s.gpu ? (s.gpuStatus || 'on') : 'off') : '-') + ', app memory ' +
      (globalThis.__wasmidiAppMemory ? (globalThis.__wasmidiAppMemory / 1048576).toFixed(0) : '-') + ' MiB');
    st.under = under;
    st.held = globalThis.__wasmidiRollHeld | 0;
    globalThis.__wasmidiRollDrawnMax = globalThis.__wasmidiRollRawMax = 0;
    globalThis.__wasmidiRollTiles = globalThis.__wasmidiRollTilesVisible = 0;
    globalThis.__wasmidiRollTileMs = globalThis.__wasmidiRollTileMax = 0;
    st.frames = st.slow = st.maxGap = st.longN = st.longMs = st.req = st.rep = st.deltas = 0;
  }, 1000);
})();
