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

if ! command -v emcc >/dev/null 2>&1; then
	echo "error: emcc not found on PATH. Run 'source <emsdk>/emsdk_env.sh' first." >&2
	exit 1
fi

# shellcheck source=/dev/null
source "$HERE/sources.mk.sh"

mkdir -p "$OUT" "$OBJDIR"

COMMON_FLAGS=(
	-O2
	-fno-strict-aliasing -fwrapv -fvisibility=hidden
	-Wno-missing-braces
	-DYQ2OSTYPE=\"KaiOS\"
	-DYQ2ARCH=\"asmjs\"
	-I"$ENGINE"
	-s USE_SDL=2
)

# gl1 shelved for now (see autoexec.cfg's comment) -- needs
# -s LEGACY_GL_EMULATION=1 added back to COMMON_FLAGS and the refgl1
# compile_group call restored in place of refsoft below, if revisited.

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
	-s TOTAL_MEMORY=67108864
	-s ALLOW_MEMORY_GROWTH=0
	-s FORCE_FILESYSTEM=1
	-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','FS','callMain']
	-s EXIT_RUNTIME=0
	-s ENVIRONMENT=web
	# Diagnostic-only, temporary: a real device log showed V_RenderView's
	# own last statement (its "returning" print) fire, but the very next
	# statement in its caller -- one macro invocation later, nothing else
	# in between -- never printed. That's not explainable by a normal
	# control-flow/logic bug; it's the signature of memory corruption
	# hitting the call stack itself (e.g. a stray out-of-bounds write
	# clobbering a return address) around the point where R_ScanEdges's
	# already-confirmed corrupted edge lists get processed. ASSERTIONS
	# and STACK_OVERFLOW_CHECK make Emscripten's generated code verify
	# the stack/heap as it runs and throw a real, localized JS error
	# instead of silently going off into undefined behavior, so the next
	# device log should point at the actual corrupting write instead of
	# yet another "nothing printed after X" bisection round. Remove once
	# root-caused -- both add real overhead on this already-slow device.
	-s ASSERTIONS=2
	-s STACK_OVERFLOW_CHECK=2
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

echo "==> Compiling software renderer (${#REFSOFT_SRCS[@]} files)"
compile_group refsoft NO_EXTRA REFSOFT_SRCS

echo "==> Compiling baseq2 game (${#GAME_SRCS[@]} files)"
compile_group game GAME_ONLY_FLAGS GAME_SRCS

echo "==> Linking ${#OBJS[@]} object files ($(emcc --version | head -1))"
emcc "${COMMON_FLAGS[@]}" "${EMCC_LINK_FLAGS[@]}" \
	"${OBJS[@]}" \
	-o "$OUT/quake2-kaios.js"

KAIOS_DIR="$ROOT/platform/kaios"

echo "==> Generating $KAIOS_DIR/autoexec.cfg.js from autoexec.cfg"
python3 - "$KAIOS_DIR/autoexec.cfg" "$KAIOS_DIR/autoexec.cfg.js" <<'PYEOF'
import json, sys
src, dst = sys.argv[1], sys.argv[2]
with open(src) as f:
	text = f.read()
with open(dst, 'w') as f:
	f.write('// Generated from autoexec.cfg by build.sh -- do not edit directly.\n')
	f.write('window.KAIOS_AUTOEXEC_CFG = ' + json.dumps(text) + ';\n')
PYEOF

echo "==> Copying engine build into $KAIOS_DIR"
cp "$OUT/quake2-kaios.js" "$OUT/quake2-kaios.js.mem" "$KAIOS_DIR/"

echo "==> Build complete: $KAIOS_DIR/ is ready to package/sideload"
