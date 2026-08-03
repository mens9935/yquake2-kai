# List of yquake2 8.70 source files needed for the KaiOS/Emscripten build.
#
# Unlike the upstream Makefile, this is *one* flat list: the client, the
# server (embedded in the client, as on every other platform), the "baseq2"
# game logic and the software renderer are all statically linked into a
# single asm.js module, because Emscripten's asm.js (WASM=0) output has no
# dynamic linking (no dlopen of "game.so"/"ref_soft.so"). See
# src/backends/unix/system.c (Sys_GetGameAPI) and src/client/vid/vid.c
# (VID_LoadRenderer) for the matching __EMSCRIPTEN__ static-link paths.
#
# Curl (HTTP downloads) and OpenAL are intentionally left out: KaiOS is
# offline/sideloaded and SDL2's own audio backend (through Emscripten's
# built-in SDL2 port) is used instead.

CLIENT_SRCS = \
	src/backends/generic/misc.c \
	src/backends/unix/main.c \
	src/backends/unix/network.c \
	src/backends/unix/signalhandler.c \
	src/backends/unix/system.c \
	src/backends/unix/shared/hunk.c \
	src/client/cl_cin.c \
	src/client/cl_image.c \
	src/client/cl_console.c \
	src/client/cl_download.c \
	src/client/cl_effects.c \
	src/client/cl_entities.c \
	src/client/cl_input.c \
	src/client/cl_inventory.c \
	src/client/cl_keyboard.c \
	src/client/cl_lights.c \
	src/client/cl_main.c \
	src/client/cl_network.c \
	src/client/cl_parse.c \
	src/client/cl_particles.c \
	src/client/cl_prediction.c \
	src/client/cl_screen.c \
	src/client/cl_tempentities.c \
	src/client/cl_view.c \
	src/client/curl/download.c \
	src/client/curl/qcurl.c \
	src/client/input/gyro.c \
	src/client/input/sdl2.c \
	src/client/menu/menu.c \
	src/client/menu/qmenu.c \
	src/client/menu/videomenu.c \
	src/client/sound/ogg.c \
	src/client/sound/openal.c \
	src/client/sound/qal.c \
	src/client/sound/sdl.c \
	src/client/sound/sound.c \
	src/client/sound/wave.c \
	src/client/vid/vid.c \
	src/client/vid/glimp_sdl2.c \
	src/common/argproc.c \
	src/common/clientserver.c \
	src/common/collision.c \
	src/common/crc.c \
	src/common/cmdparser.c \
	src/common/cvar.c \
	src/common/filesystem.c \
	src/common/glob.c \
	src/common/md4.c \
	src/common/movemsg.c \
	src/common/frame.c \
	src/common/netchan.c \
	src/common/pmove.c \
	src/common/szone.c \
	src/common/zone.c \
	src/common/shared/flash.c \
	src/common/shared/rand.c \
	src/common/shared/shared.c \
	src/common/unzip/ioapi.c \
	src/common/unzip/unzip.c \
	src/common/unzip/miniz/miniz.c \
	src/common/unzip/miniz/miniz_tdef.c \
	src/common/unzip/miniz/miniz_tinfl.c \
	src/server/sv_cmd.c \
	src/server/sv_conless.c \
	src/server/sv_entities.c \
	src/server/sv_game.c \
	src/server/sv_init.c \
	src/server/sv_main.c \
	src/server/sv_save.c \
	src/server/sv_send.c \
	src/server/sv_user.c \
	src/server/sv_world.c \
	src/kaios/kaios_input.c

REFSOFT_SRCS = \
	src/client/refresh/soft/sw_aclip.c \
	src/client/refresh/soft/sw_alias.c \
	src/client/refresh/soft/sw_bsp.c \
	src/client/refresh/soft/sw_draw.c \
	src/client/refresh/soft/sw_edge.c \
	src/client/refresh/soft/sw_image.c \
	src/client/refresh/soft/sw_light.c \
	src/client/refresh/soft/sw_main.c \
	src/client/refresh/soft/sw_misc.c \
	src/client/refresh/soft/sw_model.c \
	src/client/refresh/soft/sw_part.c \
	src/client/refresh/soft/sw_poly.c \
	src/client/refresh/soft/sw_polyset.c \
	src/client/refresh/soft/sw_rast.c \
	src/client/refresh/soft/sw_scan.c \
	src/client/refresh/soft/sw_sprite.c \
	src/client/refresh/soft/sw_surf.c \
	src/client/refresh/files/surf.c \
	src/client/refresh/files/common.c \
	src/client/refresh/files/models.c \
	src/client/refresh/files/pcx.c \
	src/client/refresh/files/stb.c \
	src/client/refresh/files/wal.c \
	src/client/refresh/files/pvs.c

GAME_SRCS = \
	src/game/g_ai.c \
	src/game/g_chase.c \
	src/game/g_cmds.c \
	src/game/g_combat.c \
	src/game/g_func.c \
	src/game/g_items.c \
	src/game/g_main.c \
	src/game/g_misc.c \
	src/game/g_monster.c \
	src/game/g_phys.c \
	src/game/g_spawn.c \
	src/game/g_svcmds.c \
	src/game/g_target.c \
	src/game/g_trigger.c \
	src/game/g_turret.c \
	src/game/g_utils.c \
	src/game/g_weapon.c \
	src/game/monster/berserker/berserker.c \
	src/game/monster/boss2/boss2.c \
	src/game/monster/boss3/boss3.c \
	src/game/monster/boss3/boss31.c \
	src/game/monster/boss3/boss32.c \
	src/game/monster/brain/brain.c \
	src/game/monster/chick/chick.c \
	src/game/monster/flipper/flipper.c \
	src/game/monster/float/float.c \
	src/game/monster/flyer/flyer.c \
	src/game/monster/gladiator/gladiator.c \
	src/game/monster/gunner/gunner.c \
	src/game/monster/hover/hover.c \
	src/game/monster/infantry/infantry.c \
	src/game/monster/insane/insane.c \
	src/game/monster/medic/medic.c \
	src/game/monster/misc/move.c \
	src/game/monster/mutant/mutant.c \
	src/game/monster/parasite/parasite.c \
	src/game/monster/soldier/soldier.c \
	src/game/monster/supertank/supertank.c \
	src/game/monster/tank/tank.c \
	src/game/player/client.c \
	src/game/player/hud.c \
	src/game/player/trail.c \
	src/game/player/view.c \
	src/game/player/weapon.c \
	src/game/savegame/savegame.c
