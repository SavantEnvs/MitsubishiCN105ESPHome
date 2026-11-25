#!/usr/bin/env bash
# mayhem/build.sh — build the CN105 fuzz harnesses and the upstream GoogleTest suite.
#
# The component (components/cn105) is an ESPHome external component with no standalone build.
# 1) ESPHome (pinned, preinstalled offline in /opt/toolchains/esphome by mayhem/Dockerfile) generates
#    the host-platform source tree for mayhem/cn105host.yaml into mayhem/.esphome/ (gitignored).
# 2) The ESPHome core from that tree is compiled once per sanitizer config into mayhem/.esphome/obj;
#    components/cn105/*.cpp are compiled straight from the source tree with SanCov + sanitizers.
# 3) Each harness mode is linked as a libFuzzer binary and a -standalone reproducer.
# 4) The upstream unit tests (tests/unit) are built with normal flags against apt's googletest.
set -euo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
: "${SRC:=/mayhem}"
: "${ESPHOME_HOME:=/opt/toolchains/esphome}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"

# ---- 1) generate the ESPHome host tree (offline: only the Python package is needed) ----
GEN="$SRC/mayhem/.esphome/build/cn105host/src"
# A previous run replaced the generated cn105 copy with a symlink to the source tree; drop the link
# first so ESPHome never writes through it.
[ -L "$GEN/esphome/components/cn105" ] && rm -f "$GEN/esphome/components/cn105"
PYTHONPATH="$ESPHOME_HOME${PYTHONPATH:+:$PYTHONPATH}" PYTHONDONTWRITEBYTECODE=1 \
  python3 -m esphome compile --only-generate "$SRC/mayhem/cn105host.yaml" >/dev/null
[ -f "$GEN/esphome/core/defines.h" ] || { echo "esphome generation failed" >&2; exit 1; }
# Point the generated tree's cn105 copy at the real source tree, so every header and source is the
# one in components/cn105 (edits apply, backtraces name the real files, no duplicate definitions).
rm -rf "$GEN/esphome/components/cn105"
ln -s "$SRC/components/cn105" "$GEN/esphome/components/cn105"

CXXSTD="-std=gnu++20"
COMMON="$CXXSTD -DUSE_HOST -I$GEN -w"
FLAGS="$SANITIZER_FLAGS $DEBUG_FLAGS -O1"
OBJ="$SRC/mayhem/.esphome/obj"
mkdir -p "$OBJ/core" "$OBJ/cn105" "$OBJ/h"

compile() {  # src obj extra-flags...
  local src=$1 obj=$2; shift 2
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "$SRC/mayhem/build.sh" -nt "$obj" ] || \
     [ "$(cat "$obj.flags" 2>/dev/null)" != "$*" ]; then
    $CXX $COMMON "$@" -c "$src" -o "$obj"
    printf '%s' "$*" > "$obj.flags"
  fi
}
export -f compile; export CXX COMMON SRC

# ESPHome core (framework, no coverage). host/core.cpp holds main() and host/hal.cpp the clock.
jobs=()
while IFS= read -r f; do
  rel=${f#"$GEN/"}; o="$OBJ/core/$(echo "$rel" | tr '/' '_').o"
  case "$rel" in
    main.cpp|esphome/components/cn105/*) continue ;;
    esphome/components/host/core.cpp) extra="-Dmain=esphome_host_main" ;;
    esphome/components/host/hal.cpp) extra="-Dclock_gettime=cn105_fuzz_clock_gettime -Dnanosleep=cn105_fuzz_nanosleep" ;;
    *) extra="" ;;
  esac
  jobs+=("$f|$o|$extra")
done < <(find "$GEN" -name '*.cpp' | LC_ALL=C sort)
printf '%s\n' "${jobs[@]}" | FLAGS="$FLAGS" xargs -P "$MAYHEM_JOBS" -I{} bash -c \
  'IFS="|" read -r s o e <<< "{}"; compile "$s" "$o" $FLAGS $e'

# cn105 component, from the source tree (instrumented for coverage).
CN_FLAGS="$FLAGS -fsanitize=fuzzer-no-link"
ls "$SRC"/components/cn105/*.cpp | LC_ALL=C sort | FLAGS="$CN_FLAGS" xargs -P "$MAYHEM_JOBS" -I{} bash -c \
  'compile "{}" "$SRC/mayhem/.esphome/obj/cn105/$(basename "{}" .cpp).o" $FLAGS -I"$SRC/components/cn105"'

compile "$SRC/mayhem/harnesses/cn105_fuzz_clock.cpp" "$OBJ/h/clock.o" $FLAGS
compile "$SRC/mayhem/lsan_off.cc" "$OBJ/h/lsan_off.o" $FLAGS
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$OBJ/h/standalone_main.o"

LIBS=("$OBJ"/core/*.o "$OBJ"/cn105/*.o "$OBJ/h/clock.o" "$OBJ/h/lsan_off.o")
for spec in uart:0 control:1 lossnay:2; do
  name=fuzz_cn105_${spec%%:*}; mode=${spec##*:}
  compile "$SRC/mayhem/harnesses/cn105_fuzz.cpp" "$OBJ/h/$name.o" $CN_FLAGS -DCN105_FUZZ_MODE=$mode \
    -I"$SRC/components/cn105"
  $CXX $FLAGS $LIB_FUZZING_ENGINE "$OBJ/h/$name.o" "${LIBS[@]}" -lpthread -o "$SRC/$name"
  $CXX $FLAGS "$OBJ/h/standalone_main.o" "$OBJ/h/$name.o" "${LIBS[@]}" -lpthread -o "$SRC/$name-standalone"
done

# ---- 4) upstream unit tests, normal flags, googletest from apt (offline) ----
env -u CFLAGS -u CXXFLAGS -u LDFLAGS cmake -S tests/unit -B tests/unit/build -DCMAKE_BUILD_TYPE=Release \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/usr/src/googletest -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  -DCMAKE_CXX_FLAGS="$COVERAGE_FLAGS" -DCMAKE_EXE_LINKER_FLAGS="$COVERAGE_FLAGS" >/dev/null
cmake --build tests/unit/build -j"$MAYHEM_JOBS"
