#!/usr/bin/env bash
#
# Builds the KaiOS/Emscripten asm.js port of Yamagi Quake II.
#
# Requires an activated Emscripten SDK on PATH (`source
# /path/to/emsdk/emsdk_env.sh`). Tested against emsdk 2.0.34 -- see
# platform/kaios/README.md for why that specific version was chosen
# (newer emsdk releases emit ES2020+ syntax in the JS runtime glue that
# KaiOS 2.5's Gecko-48-class engine cannot parse).
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
ENGINE="$ROOT/engine"
OUT="$HERE/dist"
OBJDIR="$HERE/obj"

# RENDERER=gl1 builds the (normally shelved) GLES1-via-WebGL1 renderer
# instead of the shipped software one, for one-off real-device GL context
# testing -- see the comment by "gl1 shelved for now" below. Defaults to
# "soft", the one that actually ships. TOTAL_MEMORY_OVERRIDE and OUT_NAME
# exist for the same reason: testing a smaller heap reservation, or
# writing the build somewhere other than platform/kaios/ so a diagnostic
# build never overwrites the working shipped one.
RENDERER="${RENDERER:-soft}"
TOTAL_MEMORY_OVERRIDE="${TOTAL_MEMORY_OVERRIDE:-100663296}"
OUT_NAME="${OUT_NAME:-quake2-kaios}"
KAIOS_DIR="${KAIOS_DIR:-$ROOT/platform/kaios}"

if ! command -v emcc >/dev/null 2>&1; then
	echo "error: emcc not found on PATH. Run 'source <emsdk>/emsdk_env.sh' first." >&2
	exit 1
fi

# shellcheck source=/dev/null
source "$HERE/sources.mk.sh"

mkdir -p "$OUT" "$OBJDIR"

COMMON_FLAGS=(
	# -ffast-math tried alongside -O3 and reverted: measured worse on
	# a real device than -O3 alone (unconfirmed why -- possibly NaN/inf
	# handling somewhere in the renderer's math relies on strict IEEE
	# behavior that -ffast-math is allowed to break). -O3 by itself is
	# still valid for emcc's own -O parser (distinct from clang's,
	# which only understands -O0/-O1/-O2/-O3/-Os/-Oz -- "-Ofast" errors
	# out ("Invalid optimization level") there despite being valid for
	# clang itself).
	-O3
	-fno-strict-aliasing -fwrapv -fvisibility=hidden
	-Wno-missing-braces
	-DYQ2OSTYPE=\"KaiOS\"
	-DYQ2ARCH=\"asmjs\"
	-I"$ENGINE"
	-s USE_SDL=2
)

# gl1 shelved by default (see autoexec.cfg's comment: real-device WebGL
# context creation has failed every attempt so far, even bypassing SDL
# entirely and matching a known-working reference implementation's exact
# context attributes) -- RENDERER=gl1 brings it back for one-off testing
# of new theories without disturbing the shipped soft build.
if [ "$RENDERER" = "gl1" ]; then
	COMMON_FLAGS+=(-s LEGACY_GL_EMULATION=1)
	# gl1 only ever uses the fixed-function pipeline (no GLSL shaders
	# anywhere in this renderer) -- GL_FFP_ONLY tells the emulation
	# layer it never needs to support the programmable path, which per
	# Emscripten's own docs lets it skip some of LEGACY_GL_EMULATION's
	# overhead. Untested on real hardware yet; cheap to try given the
	# confirmed ~700-poly FPS cliff smells like per-GL-call overhead.
	COMMON_FLAGS+=(-s GL_FFP_ONLY=1)
fi

GL1_FLAGS=(
	-DYQ2_GL1_GLES
	-I"$ENGINE/src/client/refresh/gl1/glad-gles1/include"
	# Same story as GAME_ONLY_FLAGS above: in the normal dlopen() build
	# each of these is a per-shared-object cvar_t* handle (or, for
	# `modes`, a same-named-by-coincidence local array) that's harmless
	# duplicated across separate .so images, but collides once
	# everything is statically linked into one binary. Rename only the
	# C-level symbol on the gl1 side -- the cvar registration strings
	# passed to Cvar_Get() inside these files are untouched, so the
	# actual cvars stay named vid_fullscreen/vid_gamma/etc.
	-Dmodes=gl1_local_modes
	-Dvid_fullscreen=gl1_vid_fullscreen
	-Dvid_gamma=gl1_vid_gamma
	-Dgl1_stereo=gl1_local_stereo
	-Dgl1_stereo_separation=gl1_local_stereo_separation
	-Dgl1_stereo_convergence=gl1_local_stereo_convergence
)

# The "baseq2" game code is normally built as its own game.so, with its
# own private cvar_t* cache pointers that happen to share names with
# globals of the same purpose in the client/server (maxclients,
# dedicated). That's invisible across a dlopen() boundary but becomes a
# duplicate-symbol link error once everything is statically linked into
# one binary, so rename them at the preprocessor level for the game
# translation units only.
GAME_ONLY_FLAGS=(
	-Dmaxclients=kaios_game_maxclients
	-Ddedicated=kaios_game_dedicated
)

EMCC_LINK_FLAGS=(
	-s WASM=0
	-s LEGACY_VM_SUPPORT=1
	-s MIN_FIREFOX_VERSION=0
	-s MIN_CHROME_VERSION=0
	-s MIN_SAFARI_VERSION=0
	# Bumped from 64MB: a real device log showed Aborted(OOM) while
	# heap_used was reported at only 36.9% (~24.7MB) of the old 64MB
	# ceiling, right as CM_LoadMap tried to allocate a contiguous buffer
	# for a map file after 12 prior map loads/frees in the same session
	# (kaios_mapcycle soak test). That's heap fragmentation, not real
	# exhaustion -- dlmalloc has no compaction, and a long enough run of
	# alternating large (hunk/map file) and small (sound/image) alloc/
	# free cycles on a fixed-size linear memory can fail a large
	# allocation despite plenty of nominal free space elsewhere. 96MB
	# gives fragmentation more room to happen without hitting the wall;
	# it does not fix fragmentation itself. Must stay a multiple of
	# 16MB (asm.js linear memory requirement). Watch for this making the
	# KaiOS device's own memory pressure worse -- unlike the previous
	# leak/corruption bugs, this is a real tradeoff against whatever
	# total RAM budget the phone itself has for the browser tab.
	-s TOTAL_MEMORY=$TOTAL_MEMORY_OVERRIDE
	-s ALLOW_MEMORY_GROWTH=0
	-s FORCE_FILESYSTEM=1
	-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','FS','callMain']
	-s EXIT_RUNTIME=0
	-s ENVIRONMENT=web
	-lidbfs.js
)

compile_group() {
	local group_dir="$1"; shift
	local extra_flags_name="$1"; shift
	local -n files="$1"; shift
	local -n extra="$extra_flags_name"

	mkdir -p "$OBJDIR/$group_dir"

	for f in "${files[@]}"; do
		local obj="$OBJDIR/$group_dir/$(echo "$f" | tr '/' '_').o"
		OBJS+=("$obj")
		if [ -n "${QUICK:-}" ] && [ -f "$obj" ] && [ "$obj" -nt "$ENGINE/$f" ]; then
			continue
		fi
		echo "  CC  $f"
		emcc "${COMMON_FLAGS[@]}" "${extra[@]}" -c "$ENGINE/$f" -o "$obj"
	done
}

NO_EXTRA=()
OBJS=()

echo "==> Compiling client+server (${#CLIENT_SRCS[@]} files)"
compile_group client NO_EXTRA CLIENT_SRCS

if [ "$RENDERER" = "gl1" ]; then
	echo "==> Compiling gl1/GLES1 renderer (${#REFGL1_SRCS[@]} files)"
	compile_group refgl1 GL1_FLAGS REFGL1_SRCS
else
	echo "==> Compiling software renderer (${#REFSOFT_SRCS[@]} files)"
	compile_group refsoft NO_EXTRA REFSOFT_SRCS
fi

echo "==> Compiling baseq2 game (${#GAME_SRCS[@]} files)"
compile_group game GAME_ONLY_FLAGS GAME_SRCS

echo "==> Linking ${#OBJS[@]} object files ($(emcc --version | head -1))"
emcc "${COMMON_FLAGS[@]}" "${EMCC_LINK_FLAGS[@]}" \
	"${OBJS[@]}" \
	-o "$OUT/$OUT_NAME.js"

# autoexec.cfg is no longer baked into a generated .js at build time --
# platform/kaios/app.js now fetches the plain text file at runtime, once
# per app launch (see writeAutoexec() there), so it can be edited and
# redeployed on its own without a full engine rebuild. Nothing to
# generate here anymore; autoexec.cfg just needs to sit next to
# index.html, which it already does.

echo "==> Copying engine build into $KAIOS_DIR"
mkdir -p "$KAIOS_DIR"
cp "$OUT/$OUT_NAME.js" "$OUT/$OUT_NAME.js.mem" "$KAIOS_DIR/"

echo "==> Build complete: $KAIOS_DIR/ is ready to package/sideload"
