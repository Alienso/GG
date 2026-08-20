#!/usr/bin/env bash
# compile.sh — compile a .gg source file to a native Linux x86-64 executable.
# The Linux counterpart of compile.ps1 (same pipeline: GG → LLVM IR → clang → exe).
#
# Usage:
#   ./compile.sh samples/hello.gg
#   ./compile.sh samples/hello.gg --show-ir
#   ./compile.sh samples/hello.gg --run
#   ./compile.sh samples/hello.gg --debug            # emit DWARF (gdb/lldb) + clang -g
#   ./compile.sh samples/hello.gg -O2                # clang -O2 (default -O0)
#   ./compile.sh samples/hello.gg --overflow-checks  # trap on integer overflow / narrowing
#   ./compile.sh samples/hello.gg --target=<triple>  # override the target triple (default: host)
#   ./compile.sh samples/hello.gg -o bin/app          # gcc-style: choose the output executable path
set -u

# ---- Resolve paths relative to this script (GG writes build/<stem>.ll into the cwd, so cd here) ----
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR" || exit 1

GG="$SCRIPT_DIR/cmake-build-debug/GG"
BUILD="$SCRIPT_DIR/build"
CLANG="${CLANG:-clang}"   # honour $CLANG if set, else find clang on PATH

# ---- Parse arguments ----
# A plain `for arg in "$@"` loop can't handle a flag that takes a separate value
# token (`-o app`), so this uses an index-based loop with an explicit shift instead.
SOURCE=""
SHOW_IR=0
RUN=0
DEBUG=0
OPT="0"
OVERFLOW=0
TARGET=""
OUTPUT=""
while [ "$#" -gt 0 ]; do
    arg="$1"
    case "$arg" in
        --show-ir)         SHOW_IR=1 ;;
        --run)             RUN=1 ;;
        --debug|-g)        DEBUG=1 ;;
        --overflow-checks) OVERFLOW=1 ;;
        -O*)               OPT="${arg#-O}" ;;
        --target=*)        TARGET="${arg#--target=}" ;;
        -o)
            shift
            if [ "$#" -eq 0 ]; then echo "ERROR: -o requires an argument" >&2; exit 1; fi
            OUTPUT="$1"
            ;;
        --output=*)        OUTPUT="${arg#--output=}" ;;
        -*)                echo "ERROR: unknown flag: $arg" >&2; exit 1 ;;
        *)                 SOURCE="$arg" ;;
    esac
    shift
done

# ---- Validate ----
if [ -z "$SOURCE" ];      then echo "ERROR: no source file given" >&2; exit 1; fi
if [ ! -f "$SOURCE" ];    then echo "ERROR: source file not found: $SOURCE" >&2; exit 1; fi
if [ ! -x "$GG" ];        then
    echo "ERROR: GG not found at $GG" >&2
    echo "       Build it first:  cmake --build cmake-build-debug --target GG --parallel" >&2
    exit 1
fi
if ! command -v "$CLANG" >/dev/null 2>&1; then
    echo "ERROR: clang not found (set \$CLANG or add it to PATH)" >&2; exit 1
fi

stem="$(basename "${SOURCE%.*}")"
ll="$BUILD/$stem.ll"
if [ -n "$OUTPUT" ]; then
    exe="$OUTPUT"
    exeDir="$(dirname "$exe")"
    mkdir -p "$exeDir"
else
    exe="$BUILD/$stem"
fi

# ---- Step 1: GG → LLVM IR ----
echo
echo "==> [1/2]  GG  $SOURCE"
gg_args=("$SOURCE" "--unsafe-ptr")
[ "$DEBUG" -eq 1 ]    && gg_args+=("--debug")
[ "$OVERFLOW" -eq 1 ] && gg_args+=("--overflow-checks")
[ -n "$TARGET" ]      && gg_args+=("--target=$TARGET")
if ! "$GG" "${gg_args[@]}"; then
    echo "FAILED: GG exited with an error" >&2
    exit 1
fi
if [ ! -f "$ll" ]; then
    echo "FAILED: no IR file was written (semantic errors prevent codegen)" >&2
    exit 1
fi
echo "    wrote  $ll"

if [ "$SHOW_IR" -eq 1 ]; then
    echo
    echo "---- LLVM IR ($ll)"
    printf -- '-%.0s' {1..72}; echo
    sed 's/^/  /' "$ll"
    printf -- '-%.0s' {1..72}; echo
fi

# ---- Step 2: LLVM IR → native executable via clang ----
echo
echo "==> [2/2]  clang  $ll"
rm -f "$exe"
clang_args=("$ll" "-o" "$exe" "-O$OPT")
[ "$DEBUG" -eq 1 ] && clang_args+=("-g")
if ! "$CLANG" "${clang_args[@]}"; then
    echo "FAILED: clang exited with an error" >&2
    exit 1
fi
echo "    wrote  $exe"
echo
echo "OK  $exe"

# ---- Step 3 (optional): run ----
if [ "$RUN" -eq 1 ]; then
    echo
    echo "==> Running $exe ..."
    "$exe"
    code=$?
    echo "    exit code: $code"
    exit $code
fi
