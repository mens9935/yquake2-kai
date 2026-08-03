/*
 * =======================================================================
 *
 * KaiOS input glue.
 *
 * KaiOS feature phones send DOM KeyboardEvent "key" values (SoftLeft,
 * SoftRight, Call, EndCall, ...) that Emscripten's SDL2 port has no
 * scancode mapping for, and there's no keyboard/mouse hardware to fall
 * back on. Rather than teaching SDL2 about phone keys, the KaiOS shell
 * (see platform/kaios/app.js) listens for keydown/keyup itself and
 * calls straight into the engine's key event pipeline through this
 * tiny exported bridge, bypassing SDL2 input entirely.
 *
 * =======================================================================
 */

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include "../client/header/keyboard.h"

EMSCRIPTEN_KEEPALIVE
void
KaiOS_KeyEvent(int key, int down, int special)
{
	Key_Event(key, down ? true : false, special ? true : false);
}

#endif /* __EMSCRIPTEN__ */
