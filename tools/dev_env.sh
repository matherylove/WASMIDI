# source tools/dev_env.sh — environment for local builds (see tools/dev_setup.sh).
TOOLS="${WASMIDI_TOOLS:-$HOME/wasmidi-tools}"
source "$TOOLS/emsdk/emsdk_env.sh" >/dev/null
export QT_ROOT_DIR="$TOOLS/Qt/6.8.3/wasm_singlethread"
export QT_HOST_PATH="$TOOLS/Qt/6.8.3/gcc_64"
[ -d /opt/pw-browsers ] && export PLAYWRIGHT_BROWSERS_PATH="${PLAYWRIGHT_BROWSERS_PATH:-/opt/pw-browsers}"
