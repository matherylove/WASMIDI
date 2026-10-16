#!/usr/bin/env bash
# One-time toolchain for building WASMIDI outside GitHub Actions, with the same
# versions as the CI (.github/workflows/build-wasm.yml). Installs into ~/wasmidi-tools.
# Afterwards: `source tools/dev_env.sh` in every shell that builds.
set -euo pipefail
EMSCRIPTEN_VERSION=3.1.56
QT_VERSION=6.8.3
TOOLS="${WASMIDI_TOOLS:-$HOME/wasmidi-tools}"
mkdir -p "$TOOLS"
command -v ninja >/dev/null || { sudo apt-get update && sudo apt-get install -y ninja-build; }
command -v cmake >/dev/null || { sudo apt-get update && sudo apt-get install -y cmake; }
if [ ! -d "$TOOLS/emsdk" ]; then
  git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$TOOLS/emsdk"
fi
"$TOOLS/emsdk/emsdk" install "$EMSCRIPTEN_VERSION"
"$TOOLS/emsdk/emsdk" activate "$EMSCRIPTEN_VERSION"
python3 -m pip install --user --quiet 'aqtinstall==3.3.*' pyyaml playwright
if [ ! -d "$TOOLS/Qt/$QT_VERSION/wasm_singlethread" ]; then
  python3 -m aqt install-qt linux desktop "$QT_VERSION" linux_gcc_64 -O "$TOOLS/Qt"
  python3 -m aqt install-qt all_os wasm "$QT_VERSION" wasm_singlethread -O "$TOOLS/Qt"
fi
# Chromium for headless benchmarks (skipped if Playwright browsers already exist).
if [ -z "${PLAYWRIGHT_BROWSERS_PATH:-}" ] || [ ! -d "$PLAYWRIGHT_BROWSERS_PATH" ]; then
  python3 -m playwright install chromium || true
fi
echo "Done. Now: source tools/dev_env.sh"
