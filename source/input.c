#include "input.h"
#include "utils/logger.h"

#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include <stdint.h>
#include <string.h>

#define SCREEN_W 960
#define SCREEN_H 544

#define TOUCH_PANEL_W 1920
#define TOUCH_PANEL_H 1088

// Ids 0..2 para dedos reales; 3..5 para los toques sinteticos del pad (el motor recibe el
// id tal cual como pointerId: se mantienen chicos por si indexa una tabla).
#define MAX_TOUCH_SLOTS 3
#define FID_JOY   3
#define FID_CAM   4
#define FID_BTN   5

/*
 * Controles fisicos -> toques (Bug #009). El motor es solo tactil: appKeyPressed es un
 * no-op y appKeyReleased solo mira BACK/MENU. Posiciones medidas sobre capturas 960x544
 * del HUD de exploracion (joystick_960.swf):
 *   joystick virtual: centro (143,422), radio util ~60
 *   boton de accion (mano): (857,459)      pausa: (911,38)
 * Camara: el motor rota con un arrastre en la zona libre de la pantalla; se sostiene un
 * toque y se desplaza a velocidad proporcional al stick (mismo esquema que Shadow Guardian).
 */
#define JOY_CX     143
#define JOY_CY     422
#define JOY_R      60
#define JOY_DZ     28      // zona muerta en unidades de stick (0..127)
#define ACT_X      857
#define ACT_Y      459
#define TALK_X     480     // toque para avanzar dialogos de cutscene
#define TALK_Y     420
#define CAM_X      620
#define CAM_Y      250
#define CAM_DZ     0.18f
#define CAM_SPEED_X 11.0f  // px por frame a fondo
#define CAM_SPEED_Y 6.0f
#define CAM_WRAP   220     // re-centrar el arrastre al alejarse tanto del ancla

// GameState (data/script: GAME_FIELD_SCENE = 8, GAME_CINEMATIC = 9, carreras de Piyagi
// 13..15). GameState::Set_Child_State guarda el estado actual en el byte +9.
#define GS_FIELD          8
#define GS_CINEMATIC      9
#define GS_PIYAGI_PROG    14

// Keycodes Android
#define AKEY_BACK        4
#define AKEY_MENU        82

static fn_touch_evt s_on_touch;
static fn_key_evt s_key_down, s_key_up;
static fn_get_state s_get_state;

static uint32_t s_prev_buttons;
static fn_game_state_instance s_gs_instance;

static int s_joy_active;
static int s_joy_x, s_joy_y;
static int s_cam_active;
static float s_cam_fx, s_cam_fy;
static int s_cam_x, s_cam_y;
static int s_btn_active;
static int s_btn_x, s_btn_y;
static int s_last_state = -1;

// Nunca pasar el id crudo de SceTouchReport al motor (skill psvita-porting,
// input_handling.md): se traduce a un slot chico y estable, como un pointerId de Android.
typedef struct {
    int active;
    int report_id;
    int x, y;
} TouchSlot;

static TouchSlot s_slots[MAX_TOUCH_SLOTS];

void input_set_game_state(fn_game_state_instance get_instance) {
    s_gs_instance = get_instance;
}

static int game_state(void) {
    if (!s_gs_instance) return -1;
    const uint8_t * gs = (const uint8_t *) s_gs_instance();
    return gs ? gs[9] : -1;
}

void input_init(fn_touch_evt on_touch, fn_key_evt key_down, fn_key_evt key_up,
                fn_get_state get_state) {
    s_on_touch = on_touch;
    s_key_down = key_down;
    s_key_up = key_up;
    s_get_state = get_state;
    s_prev_buttons = 0;
    memset(s_slots, 0, sizeof(s_slots));

    // Viene apagado por default.
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
}

static void panel_to_screen(int px, int py, int *ox, int *oy) {
    int x = px * SCREEN_W / TOUCH_PANEL_W;
    int y = py * SCREEN_H / TOUCH_PANEL_H;
    *ox = x < 0 ? 0 : (x >= SCREEN_W ? SCREEN_W - 1 : x);
    *oy = y < 0 ? 0 : (y >= SCREEN_H ? SCREEN_H - 1 : y);
}

static void poll_touch(void *env, void *clazz) {
    SceTouchData touch;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) < 0) return;

    int seen[MAX_TOUCH_SLOTS] = {0};

    for (int r = 0; r < (int) touch.reportNum; r++) {
        int x, y;
        panel_to_screen(touch.report[r].x, touch.report[r].y, &x, &y);
        int rid = touch.report[r].id;

        int slot = -1;
        for (int i = 0; i < MAX_TOUCH_SLOTS; i++)
            if (s_slots[i].active && s_slots[i].report_id == rid) { slot = i; break; }

        if (slot < 0) {
            for (int i = 0; i < MAX_TOUCH_SLOTS; i++)
                if (!s_slots[i].active) { slot = i; break; }
            if (slot < 0) continue;
            s_slots[slot] = (TouchSlot) { 1, rid, x, y };
            s_on_touch(env, clazz, 1, x, y, slot);
        } else if (s_slots[slot].x != x || s_slots[slot].y != y) {
            s_slots[slot].x = x;
            s_slots[slot].y = y;
            s_on_touch(env, clazz, 2, x, y, slot);
        }
        seen[slot] = 1;
    }

    for (int i = 0; i < MAX_TOUCH_SLOTS; i++) {
        if (s_slots[i].active && !seen[i]) {
            s_slots[i].active = 0;
            s_on_touch(env, clazz, 0, s_slots[i].x, s_slots[i].y, i);
        }
    }
}

// EternalLegacy.onKeyDown: 82 (MENU) retorna antes de nativeKeyDown; el resto lo manda.
static void key_down(void *env, void *clazz, int code) {
    if (code == AKEY_MENU) return;
    if (s_key_down) s_key_down(env, clazz, code);
}

// EternalLegacy.onKeyUp: BACK y MENU pasan por nativegetState(); solo con 1 o 2 se
// entrega nativeKeyUp (2 ademas manda la app al fondo en Android -- aca se ignora).
static void key_up(void *env, void *clazz, int code) {
    if (code != AKEY_BACK && code != AKEY_MENU) return;
    int state = s_get_state ? s_get_state(env, clazz, code) : 1;
    l_debug("input: keyUp(%d) nativegetState -> %d", code, state);
    if ((state == 1 || state == 2) && s_key_up)
        s_key_up(env, clazz, code);
}

static void key_edge(void *env, void *clazz, uint32_t now, uint32_t before,
                     uint32_t mask, int code) {
    int was = (before & mask) != 0, is = (now & mask) != 0;
    if (is && !was) key_down(env, clazz, code);
    if (!is && was) key_up(env, clazz, code);
}

static void synth(void *env, void *clazz, int action, int x, int y, int id) {
    if (s_on_touch) s_on_touch(env, clazz, action, x, y, id);
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Stick izquierdo (o D-pad) -> dedo sostenido sobre el joystick virtual.
static void pad_joystick(void *env, void *clazz, int lx, int ly, int enabled) {
    int dx = 0, dy = 0;
    if (enabled) {
        int ax = lx < 0 ? -lx : lx, ay = ly < 0 ? -ly : ly;
        if (ax > JOY_DZ || ay > JOY_DZ) {
            dx = lx * JOY_R / 127;
            dy = ly * JOY_R / 127;
        }
    }

    if (dx == 0 && dy == 0) {
        if (s_joy_active) {
            synth(env, clazz, 0, s_joy_x, s_joy_y, FID_JOY);
            s_joy_active = 0;
        }
        return;
    }

    int x = JOY_CX + dx, y = JOY_CY + dy;
    if (!s_joy_active) {
        s_joy_active = 1;
        s_joy_x = JOY_CX;
        s_joy_y = JOY_CY;
        synth(env, clazz, 1, JOY_CX, JOY_CY, FID_JOY);
    }
    if (x != s_joy_x || y != s_joy_y) {
        s_joy_x = x;
        s_joy_y = y;
        synth(env, clazz, 2, x, y, FID_JOY);
    }
}

static float cam_curve(float v) {
    float a = v < 0 ? -v : v;
    if (a <= CAM_DZ) return 0.0f;
    float t = (a - CAM_DZ) / (1.0f - CAM_DZ);
    if (t > 1.0f) t = 1.0f;
    t = 0.35f * t + 0.65f * t * t;
    return v < 0 ? -t : t;
}

static void cam_release(void *env, void *clazz) {
    if (s_cam_active) {
        synth(env, clazz, 0, s_cam_x, s_cam_y, FID_CAM);
        s_cam_active = 0;
    }
}

// Stick derecho -> arrastre continuo de camara.
static void pad_camera(void *env, void *clazz, int rx, int ry, int enabled) {
    float vx = enabled ? cam_curve(rx / 127.0f) * CAM_SPEED_X : 0.0f;
    float vy = enabled ? cam_curve(ry / 127.0f) * CAM_SPEED_Y : 0.0f;

    if (vx == 0.0f && vy == 0.0f) {
        cam_release(env, clazz);
        return;
    }

    if (!s_cam_active) {
        s_cam_active = 1;
        s_cam_fx = CAM_X;
        s_cam_fy = CAM_Y;
        s_cam_x = CAM_X;
        s_cam_y = CAM_Y;
        synth(env, clazz, 1, CAM_X, CAM_Y, FID_CAM);
    }

    s_cam_fx += vx;
    s_cam_fy += vy;
    int x = (int) s_cam_fx, y = (int) s_cam_fy;

    // Al alejarse del ancla se suelta y se vuelve a apoyar: nunca sale de pantalla ni
    // entra en la zona del joystick/botones.
    if (x < CAM_X - CAM_WRAP || x > CAM_X + CAM_WRAP ||
        y < CAM_Y - CAM_WRAP / 2 || y > CAM_Y + CAM_WRAP / 2) {
        cam_release(env, clazz);
        return; // el proximo frame vuelve a apoyar en el ancla
    }

    if (x != s_cam_x || y != s_cam_y) {
        s_cam_x = x;
        s_cam_y = y;
        synth(env, clazz, 2, x, y, FID_CAM);
    }
}

// Boton fisico -> toque sostenido en (x, y) mientras este apretado.
static void pad_tap(void *env, void *clazz, int pressed, int released, int x, int y) {
    if (pressed && !s_btn_active) {
        s_btn_active = 1;
        s_btn_x = x;
        s_btn_y = y;
        synth(env, clazz, 1, x, y, FID_BTN);
    }
    if (released && s_btn_active) {
        s_btn_active = 0;
        synth(env, clazz, 0, s_btn_x, s_btn_y, FID_BTN);
    }
}

void input_poll(void *env, void *clazz) {
    if (s_on_touch) poll_touch(env, clazz);

    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0) return;
    uint32_t b = pad.buttons;
    uint32_t pressed = b & ~s_prev_buttons;
    uint32_t released = ~b & s_prev_buttons;

    int state = game_state();
    if (state != s_last_state) {
        l_debug("input: game state %d -> %d", s_last_state, state);
        s_last_state = state;
    }
    int field = (state == GS_FIELD || state == GS_PIYAGI_PROG);

    // Movimiento: stick izquierdo; el D-pad hace lo mismo a fondo.
    int lx = (int) pad.lx - 128, ly = (int) pad.ly - 128;
    if (b & SCE_CTRL_LEFT)  lx = -127;
    if (b & SCE_CTRL_RIGHT) lx = 127;
    if (b & SCE_CTRL_UP)    ly = -127;
    if (b & SCE_CTRL_DOWN)  ly = 127;
    pad_joystick(env, clazz, clampi(lx, -127, 127), clampi(ly, -127, 127), field);
    pad_camera(env, clazz, clampi((int) pad.rx - 128, -127, 127),
               clampi((int) pad.ry - 128, -127, 127), field);

    // CRUZ: accion (mano) en exploracion; avanzar dialogo en cutscenes.
    if (field)
        pad_tap(env, clazz, pressed & SCE_CTRL_CROSS, released & SCE_CTRL_CROSS, ACT_X, ACT_Y);
    else if (state == GS_CINEMATIC)
        pad_tap(env, clazz, pressed & SCE_CTRL_CROSS, released & SCE_CTRL_CROSS, TALK_X, TALK_Y);
    else if (released & SCE_CTRL_CROSS)
        pad_tap(env, clazz, 0, 1, 0, 0); // soltar si se cambio de estado con la cruz apretada

    // START: menu de pausa (MENU de Android, ya funcionaba).
    // CIRCULO: volver/cancelar (BACK), solo fuera de la exploracion: en campo el BACK de
    // Android abre el menu, y eso ya lo hace START.
    key_edge(env, clazz, b, s_prev_buttons, SCE_CTRL_START, AKEY_MENU);
    if (!field)
        key_edge(env, clazz, b, s_prev_buttons, SCE_CTRL_CIRCLE, AKEY_BACK);

    s_prev_buttons = b;
}
