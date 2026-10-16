#!/usr/bin/env python3
"""Run one `run:` step of .github/workflows/build-wasm.yml locally, by name, so local
builds use exactly the CI commands (no copy to drift). Example:
    python3 tools/run_ci_step.py "Host tests (engine logic, no Emscripten)"
    python3 tools/run_ci_step.py "Prepare Pages artifact"
Steps that need emcc/Qt expect tools/dev_env.sh to be sourced first."""
import os
import subprocess
import sys

try:
    import yaml
except ImportError:
    sys.exit("PyYAML missing: pip install pyyaml")

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
workflow = yaml.safe_load(open(os.path.join(root, ".github/workflows/build-wasm.yml"), encoding="utf-8"))
steps = [s for job in workflow["jobs"].values() for s in job.get("steps", []) if "run" in s]
if len(sys.argv) < 2:
    print("Steps with run blocks:")
    for s in steps:
        print("  " + s.get("name", "?"))
    sys.exit(0)
match = [s for s in steps if s.get("name") == sys.argv[1]]
if not match:
    sys.exit("No step named %r (run without arguments to list them)" % sys.argv[1])
env = dict(os.environ)
env.setdefault("GITHUB_SHA", subprocess.run(["git", "rev-parse", "HEAD"], cwd=root, capture_output=True,
                                            text=True).stdout.strip() or "local")
sys.exit(subprocess.run(["bash", "-e", "-o", "pipefail", "-c", match[0]["run"]], cwd=root, env=env).returncode)
