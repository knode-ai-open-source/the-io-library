#!/usr/bin/env bash
set -Eeuo pipefail

# ----- knobs -----
PKG="${PKG:-$(awk 'BEGIN{IGNORECASE=1}
  $0 ~ /^[[:space:]]*project[[:space:]]*\(/ {
    gsub(/.*\(/,""); gsub(/[ ,)].*/,""); print; exit }' CMakeLists.txt)}"
PKG="${PKG:-${PWD##*/}}"; PKG="${PKG//-/_}"              # fallback + normalize
VARIANTS="${VARIANTS:-debug memory coverage static shared}"   # space-separated
PREFIX="/usr/local"
EXTRA_CMAKE_ARGS="${EXTRA_CMAKE_ARGS:-}"
SKIP="${SKIP:-}"              # e.g. SKIP="coverage,shared"
CLEAN="${CLEAN:-0}"           # CLEAN=1 wipes build root
AUTO_CLEAN="${AUTO_CLEAN:-1}" # auto-remove on generator mismatch
# ------------------

needs_sudo() {
  local p="$1"; local d="$p"
  [ -d "$d" ] || d="$(dirname "$d")"
  [ -w "$d" ] && return 1 || return 0
}
INSTALL_SUDO=""
case "$PREFIX" in
  /usr*) if [ "${EUID:-$(id -u)}" -ne 0 ] && needs_sudo "$PREFIX"; then INSTALL_SUDO="sudo"; fi ;;
esac

# pick generator (prefer Ninja)
if [ -n "${GENERATOR:-}" ]; then
  GEN="$GENERATOR"
else
  if command -v ninja >/dev/null 2>&1 || command -v ninja-build >/dev/null 2>&1; then
    GEN="Ninja"
  elif command -v make >/dev/null 2>&1; then
    GEN="Unix Makefiles"
  elif command -v xcodebuild >/dev/null 2>&1; then
    GEN="Xcode"
  elif command -v nmake >/dev/null 2>&1; then
    GEN="NMake Makefiles"
  else
    echo "No build tool found. Install Ninja (preferred) or Make/Xcode/VS tools." >&2
    exit 1
  fi
fi
echo "Using CMake generator: $GEN"
[ "$GEN" != "Ninja" ] && echo "Note: Ninja is preferred. Set GENERATOR=Ninja when installed."

# generator-specific build root
slugify() { echo "$1" | tr '[:upper:]' '[:lower:]' | tr ' ' '_' | tr -cd '[:alnum:]_-' ; }
GEN_SLUG="$(slugify "$GEN")"
BUILD_ROOT="${BUILD_ROOT:-build-$GEN_SLUG}"

# optional full clean
if [ "$CLEAN" != "0" ] && [ -d "$BUILD_ROOT" ]; then
  echo "Cleaning $BUILD_ROOT"
  cmake -E rm -rf "$BUILD_ROOT"
fi

# auto-remove on generator mismatch
maybe_clean_dir() {
  dir="$1"
  if [ -f "$dir/CMakeCache.txt" ]; then
    prev_gen="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$dir/CMakeCache.txt" || true)"
    if [ -n "$prev_gen" ] && [ "$prev_gen" != "$GEN" ]; then
      if [ "$AUTO_CLEAN" = "1" ]; then
        echo "⚠️  '$dir' was '$prev_gen'; removing to switch to '$GEN'…"
        cmake -E rm -rf "$dir"
      else
        echo "❌ Generator mismatch in '$dir' (was '$prev_gen', now '$GEN'). Set AUTO_CLEAN=1 or delete it." >&2
        exit 1
      fi
    fi
  fi
}

# helpers
is_skipped() { case ",$SKIP," in *,"$1",*) return 0;; *) return 1;; esac; }
build_type_for() {
  case "$1" in debug|memory|coverage) echo Debug;; static|shared) echo Release;; *) echo Release;; esac; }
flags_for() {
  v="$1"
  args="-DA_BUILD_VARIANT=${v}"
  case "$v" in
    debug)    args="$args -DA_BUILD_DEBUG_POSTFIX=_d" ;;
    memory)   args="$args -DA_BUILD_ENABLE_MEMORY_PROFILE=ON -DA_BUILD_MEMORY_DEFINE=_AML_DEBUG_ -DA_BUILD_DEBUG_POSTFIX=_mem_d" ;;
    coverage) args="$args -DA_BUILD_ENABLE_COVERAGE=ON -DA_BUILD_DEBUG_POSTFIX=_cov_d" ;;
    static)   args="$args -DBUILD_SHARED_LIBS=OFF" ;;
    shared)   args="$args -DBUILD_SHARED_LIBS=ON" ;;
  esac
  echo "$args"
}

conf() { cmake -S . -B "$1" -G "$GEN" -DCMAKE_BUILD_TYPE="$2" -DCMAKE_INSTALL_PREFIX="$PREFIX" $3 $EXTRA_CMAKE_ARGS; }
doit() { cmake --build "$1" --config "$2"; $INSTALL_SUDO cmake --install "$1" --config "$2"; }

# drive builds
for v in $VARIANTS; do
  is_skipped "$v" && { echo "⏭️  $v"; continue; }
  sub="$BUILD_ROOT/$v"
  maybe_clean_dir "$sub"
  bt="$(build_type_for "$v")"
  echo "▶️  $v (type=$bt)"
  conf "$sub" "$bt" "$(flags_for "$v")"
  doit "$sub" "$bt"
done

echo "✅ Installed variants to: $PREFIX"
echo "🗂️  Build artifacts under: $BUILD_ROOT"
