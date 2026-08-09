# Auto-generated-once from the upstream Makefile's object lists (yquake2 8.70).
# See sources.mk for the annotated/human-readable version this was derived from.

CLIENT_SRCS=(
	"src/backends/generic/misc.c"
	"src/backends/unix/main.c"
	"src/backends/unix/network.c"
	"src/backends/unix/signalhandler.c"
	"src/backends/unix/system.c"
	"src/backends/unix/shared/hunk.c"
	"src/client/cl_cin.c"
	"src/client/cl_image.c"
	"src/client/cl_console.c"
	"src/client/cl_download.c"
	"src/client/cl_effects.c"
	"src/client/cl_entities.c"
	"src/client/cl_input.c"
	"src/client/cl_inventory.c"
	"src/client/cl_keyboard.c"
	"src/client/cl_lights.c"
	"src/client/cl_main.c"
	"src/client/cl_network.c"
	"src/client/cl_parse.c"
	"src/client/cl_particles.c"
	"src/client/cl_prediction.c"
	"src/client/cl_screen.c"
	"src/client/cl_tempentities.c"
	"src/client/cl_view.c"
	"src/client/curl/download.c"
	"src/client/curl/qcurl.c"
	"src/client/input/gyro.c"
	"src/client/input/sdl2.c"
	"src/client/menu/menu.c"
	"src/client/menu/qmenu.c"
	"src/client/menu/videomenu.c"
	"src/client/sound/ogg.c"
	"src/client/sound/openal.c"
	"src/client/sound/qal.c"
	"src/client/sound/sdl.c"
	"src/client/sound/sound.c"
	"src/client/sound/wave.c"
	"src/client/sound/webaudio.c"
	"src/client/sound/webaudio_music.c"
	"src/client/vid/vid.c"
	"src/client/vid/glimp_sdl2.c"
	"src/common/argproc.c"
	"src/common/clientserver.c"
	"src/common/collision.c"
	"src/common/crc.c"
	"src/common/cmdparser.c"
	"src/common/cvar.c"
	"src/common/filesystem.c"
	"src/common/glob.c"
	"src/common/md4.c"
	"src/common/movemsg.c"
	"src/common/frame.c"
	"src/common/netchan.c"
	"src/common/pmove.c"
	"src/common/szone.c"
	"src/common/zone.c"
	"src/common/shared/flash.c"
	"src/common/shared/rand.c"
	"src/common/shared/shared.c"
	"src/common/unzip/ioapi.c"
	"src/common/unzip/unzip.c"
	"src/common/unzip/miniz/miniz.c"
	"src/common/unzip/miniz/miniz_tdef.c"
	"src/common/unzip/miniz/miniz_tinfl.c"
	"src/server/sv_cmd.c"
	"src/server/sv_conless.c"
	"src/server/sv_entities.c"
	"src/server/sv_game.c"
	"src/server/sv_init.c"
	"src/server/sv_main.c"
	"src/server/sv_save.c"
	"src/server/sv_send.c"
	"src/server/sv_user.c"
	"src/server/sv_world.c"
)

# File-format parsing (.bsp surf tables, model loading, pcx/wal/stb image
# decoding, PVS) shared verbatim by every renderer backend -- not
# renderer-specific code, just historically compiled once per backend
# because each backend used to be built as its own fully standalone
# binary. Pulled out into its own array so the unified multi-renderer
# build (build.sh, RENDERER=unified) can compile it exactly once and
# link it into every renderer, instead of getting one colliding copy of
# every symbol in here per renderer statically linked into the same
# binary. REFSOFT_SRCS/REFGL1_SRCS/REFGL3_SRCS below still each include
# it too, for the older one-renderer-per-binary build paths that compile
# a renderer fully standalone.
REFFILES_SRCS=(
	"src/client/refresh/files/surf.c"
	"src/client/refresh/files/common.c"
	"src/client/refresh/files/models.c"
	"src/client/refresh/files/pcx.c"
	"src/client/refresh/files/stb.c"
	"src/client/refresh/files/wal.c"
	"src/client/refresh/files/pvs.c"
)

REFSOFT_ONLY_SRCS=(
	"src/client/refresh/soft/sw_aclip.c"
	"src/client/refresh/soft/sw_alias.c"
	"src/client/refresh/soft/sw_bsp.c"
	"src/client/refresh/soft/sw_draw.c"
	"src/client/refresh/soft/sw_edge.c"
	"src/client/refresh/soft/sw_image.c"
	"src/client/refresh/soft/sw_light.c"
	"src/client/refresh/soft/sw_main.c"
	"src/client/refresh/soft/sw_misc.c"
	"src/client/refresh/soft/sw_model.c"
	"src/client/refresh/soft/sw_part.c"
	"src/client/refresh/soft/sw_poly.c"
	"src/client/refresh/soft/sw_polyset.c"
	"src/client/refresh/soft/sw_rast.c"
	"src/client/refresh/soft/sw_scan.c"
	"src/client/refresh/soft/sw_sprite.c"
	"src/client/refresh/soft/sw_surf.c"
)

REFSOFT_SRCS=("${REFSOFT_ONLY_SRCS[@]}" "${REFFILES_SRCS[@]}")

# gl1 renderer built against glad-gles1 (OpenGL ES 1.1 fixed-function
# bindings), matching upstream's own ref_gles1 target (see
# engine/Makefile's REFGL1_OBJS_/REFGL1_OBJS_GLADEES_). ES1.1 keeps the
# old immediate-mode/matrix-stack API gl1's renderer code actually
# calls, unlike gl3 (GLES3/GLSL-ES-300, needs WebGL2 -- confirmed
# unavailable on this device's Gecko-48-class engine, WebGL1 only).
# Built with -s LEGACY_GL_EMULATION=1 so Emscripten backs those calls
# with WebGL1.
REFGL1_ONLY_SRCS=(
	"src/client/refresh/gl1/qgl.c"
	"src/client/refresh/gl1/gl1_draw.c"
	"src/client/refresh/gl1/gl1_image.c"
	"src/client/refresh/gl1/gl1_light.c"
	"src/client/refresh/gl1/gl1_lightmap.c"
	"src/client/refresh/gl1/gl1_main.c"
	"src/client/refresh/gl1/gl1_mesh.c"
	"src/client/refresh/gl1/gl1_misc.c"
	"src/client/refresh/gl1/gl1_model.c"
	"src/client/refresh/gl1/gl1_scrap.c"
	"src/client/refresh/gl1/gl1_surf.c"
	"src/client/refresh/gl1/gl1_warp.c"
	"src/client/refresh/gl1/gl1_sdl.c"
	"src/client/refresh/gl1/gl1_buffer.c"
	"src/client/refresh/gl1/glad-gles1/src/glad.c"
)

REFGL1_SRCS=("${REFGL1_ONLY_SRCS[@]}" "${REFFILES_SRCS[@]}")

# gl3 renderer, cut down to target GLES2/WebGL1 instead of its normal
# GLES3/GLSL-ES-300 baseline (see REFGL1_SRCS's comment for why gl1 was
# chosen first -- this is the follow-up attempt at a real shader/VBO
# renderer instead of gl1's emulated fixed-function path). No glad here:
# unlike gl1's GLES 1.1 fixed-function API (which Emscripten doesn't
# provide headers for at all, hence needing LEGACY_GL_EMULATION's JS
# shim plus glad-gles1's matching declarations), GLES2 is Emscripten's
# native/default GL binding -- GLES2/gl2.h ships in its sysroot and the
# symbols resolve directly, no loader or emulation flag needed.
REFGL3_ONLY_SRCS=(
	"src/client/refresh/gl3/gl3_draw.c"
	"src/client/refresh/gl3/gl3_image.c"
	"src/client/refresh/gl3/gl3_light.c"
	"src/client/refresh/gl3/gl3_lightmap.c"
	"src/client/refresh/gl3/gl3_main.c"
	"src/client/refresh/gl3/gl3_mesh.c"
	"src/client/refresh/gl3/gl3_misc.c"
	"src/client/refresh/gl3/gl3_model.c"
	"src/client/refresh/gl3/gl3_sdl.c"
	"src/client/refresh/gl3/gl3_shaders.c"
	"src/client/refresh/gl3/gl3_surf.c"
	"src/client/refresh/gl3/gl3_warp.c"
)

REFGL3_SRCS=("${REFGL3_ONLY_SRCS[@]}" "${REFFILES_SRCS[@]}")

GAME_SRCS=(
	"src/game/g_ai.c"
	"src/game/g_chase.c"
	"src/game/g_cmds.c"
	"src/game/g_combat.c"
	"src/game/g_func.c"
	"src/game/g_items.c"
	"src/game/g_main.c"
	"src/game/g_misc.c"
	"src/game/g_monster.c"
	"src/game/g_phys.c"
	"src/game/g_spawn.c"
	"src/game/g_svcmds.c"
	"src/game/g_target.c"
	"src/game/g_trigger.c"
	"src/game/g_turret.c"
	"src/game/g_utils.c"
	"src/game/g_weapon.c"
	"src/game/monster/berserker/berserker.c"
	"src/game/monster/boss2/boss2.c"
	"src/game/monster/boss3/boss3.c"
	"src/game/monster/boss3/boss31.c"
	"src/game/monster/boss3/boss32.c"
	"src/game/monster/brain/brain.c"
	"src/game/monster/chick/chick.c"
	"src/game/monster/flipper/flipper.c"
	"src/game/monster/float/float.c"
	"src/game/monster/flyer/flyer.c"
	"src/game/monster/gladiator/gladiator.c"
	"src/game/monster/gunner/gunner.c"
	"src/game/monster/hover/hover.c"
	"src/game/monster/infantry/infantry.c"
	"src/game/monster/insane/insane.c"
	"src/game/monster/medic/medic.c"
	"src/game/monster/misc/move.c"
	"src/game/monster/mutant/mutant.c"
	"src/game/monster/parasite/parasite.c"
	"src/game/monster/soldier/soldier.c"
	"src/game/monster/supertank/supertank.c"
	"src/game/monster/tank/tank.c"
	"src/game/player/client.c"
	"src/game/player/hud.c"
	"src/game/player/trail.c"
	"src/game/player/view.c"
	"src/game/player/weapon.c"
	"src/game/savegame/savegame.c"
)

