#!/usr/bin/env bash
# Build like the CI and assemble site/ (needs `source tools/dev_env.sh`).
#   tools/local_build.sh            host tests + build + site/
#   tools/local_build.sh --no-tests build + site/
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "${1:-}" != "--no-tests" ]; then
  python3 tools/run_ci_step.py "Host tests (engine logic, no Emscripten)"
fi
python3 tools/run_ci_step.py "Configure"
cmake --build build --parallel "$(nproc)"
python3 tools/run_ci_step.py "Validate generated MIDI parser runtime"
rm -rf site
python3 tools/run_ci_step.py "Prepare Pages artifact"
echo "site/ ready: python3 tools/serve_site.py"
