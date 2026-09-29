/*
 * Input del port: panel tactil frontal + botones de la Vita hacia los entry points
 * nativos del motor (jadx: GameGLSurfaceView.onTouchEvent, EternalLegacy.onKeyDown/Up):
 *
 *   GameGLSurfaceView.nativeOnTouch(env, clazz, accion, x, y, id)
 *     accion: 1 = down, 2 = move, 0 = up; x,y en pixeles de 960x544; id = pointerId.
 *   EternalLegacy.nativeKeyDown/Up(env, clazz, keycode), nativegetState(keycode)
 *     El motor es tactil: appKeyPressed() es un no-op y appKeyReleased() solo mira
 *     BACK (4) y MENU (82) (out_ghidra.c).
 */
#ifndef ETERNALLEGACY_INPUT_H
#define ETERNALLEGACY_INPUT_H

#include <falso_jni/FalsoJNI.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (* fn_touch_evt)(void *env, void *clazz, jint action, jint x, jint y, jint id);
typedef void (* fn_key_evt)(void *env, void *clazz, jint keycode);
typedef jint (* fn_get_state)(void *env, void *clazz, jint keycode);

typedef void * (* fn_game_state_instance)(void);

/** GameState::GetInstance() del .so: habilita los controles fisicos solo en exploracion. */
void input_set_game_state(fn_game_state_instance get_instance);

/** Guarda los entry points nativos resueltos. Cualquiera puede ser NULL. */
void input_init(fn_touch_evt on_touch, fn_key_evt key_down, fn_key_evt key_up,
                fn_get_state get_state);

/** Lee touch + pad y dispara los callbacks. Una vez por frame, en el hilo de render. */
void input_poll(void *env, void *clazz);

#ifdef __cplusplus
};
#endif

#endif // ETERNALLEGACY_INPUT_H
