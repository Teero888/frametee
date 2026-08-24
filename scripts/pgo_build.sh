#!/usr/bin/bash
# Builds frametee with profile-guided optimization, trained on a realistic workload.
#
#   ./scripts/pgo_build.sh                        # build into ./build-pgo
#   ./scripts/pgo_build.sh -s path/to/other.ftee  # different training workload
#   ./scripts/pgo_build.sh -b /tmp/pgo -v         # different build dir, verbose

set -euo pipefail

NAME=***********

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build-pgo"
TRAIN_SCRIPT="$ROOT/plugins/$NAME/scripts/A02-bench.ftee"
TMNF_DATA_DIR="$ROOT/data/games/tmnf"
TRAIN_CONFIG_TEMPLATE="$ROOT/scripts/pgo_training_config.toml"
VERBOSE=false

usage() {
  cat <<'EOF'
usage: pgo_build.sh [-b BUILD_DIR] [-s TRAINING_SCRIPT] [-v]

  -b  build directory (default: <repo>/build-pgo)
  -s  .ftee script used as the training workload
      (default: plugins/$NAME/scripts/A02-bench.ftee)
  -v  show build and run output
EOF
}

while getopts ":b:s:vh" opt; do
  case $opt in
    b) BUILD_DIR="$OPTARG" ;;
    s) TRAIN_SCRIPT="$OPTARG" ;;
    v) VERBOSE=true ;;
    h) usage; exit 0 ;;
    *) usage >&2; exit 1 ;;
  esac
done

run() {
  if [ "$VERBOSE" = true ]; then
    "$@"
  else
    "$@" > /dev/null 2>&1
  fi
}

PROFDATA_BIN=$(command -v llvm-profdata || true)
if [ -z "$PROFDATA_BIN" ]; then
  echo "ERROR: llvm-profdata not found in PATH." >&2
  exit 1
fi
if [ ! -f "$TRAIN_SCRIPT" ]; then
  echo "ERROR: training script not found: $TRAIN_SCRIPT" >&2
  exit 1
fi
if [ ! -f "$TRAIN_CONFIG_TEMPLATE" ]; then
  echo "ERROR: training config not found: $TRAIN_CONFIG_TEMPLATE" >&2
  exit 1
fi
if [ ! -f "$TMNF_DATA_DIR/Packs/packlist.dat" ]; then
  echo "ERROR: TrackMania packs not found: $TMNF_DATA_DIR/Packs" >&2
  exit 1
fi
if [ ! -d "$TMNF_DATA_DIR/GameData" ]; then
  echo "ERROR: TrackMania GameData not found: $TMNF_DATA_DIR/GameData" >&2
  exit 1
fi

PGO_DIR="$BUILD_DIR/pgo"
PGO_MERGED="$PGO_DIR/merged.profdata"
TRAIN_CONFIG_HOME="$PGO_DIR/config"

echo "==> build dir:       $BUILD_DIR"
echo "==> training script: $TRAIN_SCRIPT"
echo "==> TMNF data:       $TMNF_DATA_DIR"

# ---------------------------------------------------------------------------
# Stage 1: instrumented build
#
# Keep every optimization and ISA option matched to the profile-use build.
# Clang rejects counters when optimization changes a function's control-flow
# hash between the generate and use stages; a faster, mismatched instrumented
# build therefore leaves precisely the hot counters unavailable to PGO.
# ---------------------------------------------------------------------------
echo "==> [1/4] configuring + building instrumented binaries"
rm -rf "$PGO_DIR"
mkdir -p "$TRAIN_CONFIG_HOME/frametee"
cp "$TRAIN_CONFIG_TEMPLATE" "$TRAIN_CONFIG_HOME/frametee/config.toml"
run cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=On \
  -DENABLE_AGGRESSIVE_OPTIM=On \
  -DPGO_STAGE=GENERATE \
  -DPGO_PROFILE_DIR="$PGO_DIR"
run cmake --build "$BUILD_DIR" -j"$(nproc)"

# ---------------------------------------------------------------------------
# Stage 2: train
#
# Run from the build directory so the freshly built plugins/ are the ones that
# get loaded -- the plugin is about half the profile, so training without it
# loaded would leave that half cold.
# ---------------------------------------------------------------------------
echo "==> [2/4] running training workload"
(
  cd "$BUILD_DIR"
  run env XDG_CONFIG_HOME="$TRAIN_CONFIG_HOME" \
    ./frametee --game tmnf --auto "$TRAIN_SCRIPT"
)

shopt -s nullglob
PROFRAWS=("$PGO_DIR"/*.profraw)
shopt -u nullglob
if [ ${#PROFRAWS[@]} -eq 0 ]; then
  echo "ERROR: no .profraw files in $PGO_DIR -- the training run wrote no profile." >&2
  exit 1
fi
echo "    collected ${#PROFRAWS[@]} raw profile(s)"

# ---------------------------------------------------------------------------
# Stage 3: merge
# ---------------------------------------------------------------------------
echo "==> [3/4] merging profiles"
"$PROFDATA_BIN" merge -output="$PGO_MERGED" "${PROFRAWS[@]}"

# ---------------------------------------------------------------------------
# Stage 4: optimized rebuild against the profile
# ---------------------------------------------------------------------------
echo "==> [4/4] rebuilding with -fprofile-use + aggressive optimizations"
run cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=On \
  -DENABLE_AGGRESSIVE_OPTIM=On \
  -DPGO_STAGE=USE \
  -DPGO_PROFILE_DATA="$PGO_MERGED"
run cmake --build "$BUILD_DIR" -j"$(nproc)"

echo ""
echo "Done. Optimized binary: $BUILD_DIR/frametee"
echo ""
echo "Compare against your existing build, pinned, median of a few runs:"
echo "  taskset -c 0 $BUILD_DIR/frametee --game tmnf --auto $TRAIN_SCRIPT"
