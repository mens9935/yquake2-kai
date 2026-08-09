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

# RENDERER=unified (the default, and what actually ships now) statically
# links every supported renderer into ONE binary and switches between
# them at runtime off the vid_renderer cvar (see vid.c's
# YQ2_KAIOS_UNIFIED_RENDERERS branch) -- the KaiOS launcher's own Video
# settings screen sets that cvar before Module.callMain() ever runs, so
# there's no more separate quake2-kaios-soft.js/quake2-kaios-gl3.js pair
# to ship or pick between; same single output filename as always.
#
# RENDERER=soft/gl1/gl3 still build a single renderer completely alone,
# same as before this existed -- useful for one-off single-variable perf
# testing (this session's whole demo1.dm2 A/B methodology depends on
# isolating one renderer at a time) without the unified binary's small
# extra size/complexity, and gl1 in particular is still just a shelved
# diagnostic target (see the "gl1 shelved for now" comment below) never
# linked into the unified build at all yet. TOTAL_MEMORY_OVERRIDE and
# OUT_NAME exist for the same one-off-testing reason: a smaller heap
# reservation, or writing the build somewhere other than platform/kaios/
# so a diagnostic build never overwrites the working shipped one.
RENDERER="${RENDERER:-unified}"
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

if [ "$RENDERER" = "unified" ]; then
	# Tells vid.c (the only file that reads it) to dispatch vid_renderer
	# by name across several statically-linked GetRefAPI-alikes instead
	# of assuming exactly one renderer's plain "GetRefAPI" was linked in
	# -- see vid.c's VID_LoadRenderer(). Global rather than scoped to
	# just the client compile group: harmless everywhere else, and
	# COMMON_FLAGS is what every compile_group call below actually uses.
	COMMON_FLAGS+=(-DYQ2_KAIOS_UNIFIED_RENDERERS)
fi

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

GL3_FLAGS=(
	# New platform macro (not one of upstream's own YQ2_GL3_GLES /
	# YQ2_GL3_GLES3): selects the ES2/WebGL1 header + code paths added
	# for this port, distinct from gl3's stock GLES3 and desktop-GL3.2
	# targets, neither of which this device's Gecko-48-class engine can
	# provide (confirmed WebGL1-only). No glad include path either --
	# see REFGL3_SRCS's comment in sources.mk.sh.
	-DYQ2_GL3_GLES2_WEB
	# Upstream's gl3_sdl.c already has a couple of "any GLES flavor"
	# checks (YQ2_GL3_GLES, distinct from the ES3-specific
	# YQ2_GL3_GLES3) -- e.g. skipping SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG,
	# which some GLES context creation paths reject. Define it here too.
	-DYQ2_GL3_GLES
	# Same duplicate-symbol story as GL1_FLAGS below: vid.c (always in
	# CLIENT_SRCS) already defines its own vid_fullscreen/vid_gamma
	# cvar_t* and modes[] array; gl3_main.c/gl3_image.c define their own
	# copies of the same names, which collides once both are statically
	# linked into one binary. Rename only the C-level symbol -- the
	# Cvar_Get() strings inside gl3 are untouched.
	-Dmodes=gl3_local_modes
	-Dvid_fullscreen=gl3_vid_fullscreen
	-Dvid_gamma=gl3_vid_gamma
)

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

# RENDERER=unified only: soft's own GetRefAPI (sw_main.c) needs the same
# per-renderer rename gl3/gl1 already get above, purely because vid.c
# now looks up each renderer's entry point by a distinct linked-in name
# (SoftGetRefAPI/GL3GetRefAPI) instead of the single generic "GetRefAPI"
# every standalone single-renderer build still exports. Nothing else
# about soft collides -- its own vid_fullscreen/vid_gamma/modes (see
# sw_main.c) are already `static`, unlike gl1's/gl3's, so no further
# renaming is needed there.
SOFT_UNIFIED_FLAGS=(
	-DGetRefAPI=SoftGetRefAPI
)

# RENDERER=unified only: gl3, on top of its already-existing GL3_FLAGS
# renames above (needed even for a single-renderer gl3 build once
# statically linked alongside vid.c), also needs GetRefAPI itself
# renamed for the same reason SOFT_UNIFIED_FLAGS does, plus a batch of
# further renames for every OTHER non-static global gl3_main.c/
# gl3_draw.c happen to share a name with in sw_main.c -- found by
# actually linking soft+gl3 together and diffing `llvm-nm -g
# --defined-only` between the two object sets for the real, complete
# list (wasm-ld's own duplicate-symbol errors truncate after a handful).
# Each of these was checked to make sure nothing outside gl3/soft's own
# files references the *specific* renderer's copy by this literal name
# (a `static` local, a same-named cvar *string* passed to Cvar_Get(),
# or a same-named struct field would all show up in a plain grep without
# actually being this collision) -- e.g. cl_main.c's "r_fullbright" is a
# config-defaults table string, not this cvar_t* pointer; sound.c's
# registration_sequence is a struct *field*, not this int. Three
# symbols that genuinely are reached directly from outside gl3/soft
# (`ri`, `r_polycount`, `kaios_cube_test_active`/`RI_KaiosCubeTestFrame`)
# are NOT in this list -- those are handled in gl3_main.c itself instead
# (see its YQ2_KAIOS_UNIFIED_RENDERERS block), by making gl3's copies
# extern to soft's single real definition, since unlike these genuinely
# gl3-private globals below, code outside either renderer needs to
# reach a single canonical instance of those three regardless of which
# renderer is actually active.
GL3_UNIFIED_FLAGS=(
	"${GL3_FLAGS[@]}"
	-DGetRefAPI=GL3GetRefAPI
	-Dd_8to24table=gl3_d_8to24table
	-Dfrustum=gl3_frustum
	-Dr_cull=gl3_r_cull
	-Dr_drawworld=gl3_r_drawworld
	-Dr_farsee=gl3_r_farsee
	-Dr_fixsurfsky=gl3_r_fixsurfsky
	-Dr_fullbright=gl3_r_fullbright
	-Dr_gunfov=gl3_r_gunfov
	-Dr_lerpmodels=gl3_r_lerpmodels
	-Dr_lightlevel=gl3_r_lightlevel
	-Dr_modulate=gl3_r_modulate
	-Dr_retexturing=gl3_r_retexturing
	-Dr_scale8bittextures=gl3_r_scale8bittextures
	-Dr_validation=gl3_r_validation
	-Dregistration_sequence=gl3_registration_sequence
	-Dvpn=gl3_vpn
	-Dvright=gl3_vright
	-Dvup=gl3_vup
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
elif [ "$RENDERER" = "gl3" ]; then
	echo "==> Compiling gl3/GLES2 renderer (${#REFGL3_SRCS[@]} files)"
	compile_group refgl3 GL3_FLAGS REFGL3_SRCS
elif [ "$RENDERER" = "unified" ]; then
	# Shared file-format code compiled exactly once (see REFFILES_SRCS's
	# comment in sources.mk.sh) and linked into both renderers below --
	# compiling it twice (once per renderer, like the standalone
	# single-renderer branches above do) would duplicate every symbol in
	# it once both renderers are statically linked into the same binary.
	echo "==> Compiling shared renderer file-format code (${#REFFILES_SRCS[@]} files)"
	compile_group reffiles NO_EXTRA REFFILES_SRCS
	echo "==> Compiling software renderer (${#REFSOFT_ONLY_SRCS[@]} files)"
	compile_group refsoft SOFT_UNIFIED_FLAGS REFSOFT_ONLY_SRCS
	echo "==> Compiling gl3/GLES2 renderer (${#REFGL3_ONLY_SRCS[@]} files)"
	compile_group refgl3 GL3_UNIFIED_FLAGS REFGL3_ONLY_SRCS
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
