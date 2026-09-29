#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "input.h"
#include "reimpl/mem.h"
#include "reimpl/waitprof.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

#include <pthread.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

// 192 MB y no 256: vitaGL necesita RAM libre por encima de su ram_threshold o
// vglInitExtended aborta (GT-Racing-Motor-Academy-HD-vita, Fase 13).
int _newlib_heap_size_user = 192 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

#define SCREEN_W 960
#define SCREEN_H 544

#define JNI_PREFIX "Java_com_gameloft_android_TBFV_GloftELHP_ML_"

extern int g_main_loop_started; // java.c: Get_logoVideoState

static void *sym(const char *name) {
    void *p = (void *) so_symbol(&so_mod, name);
    if (!p) l_error("boot: falta export %s", name);
    return p;
}

/*
 * Todo el juego corre en este hilo y no en el main de la Vita: el hilo principal trae
 * 256 KB de stack y ALicenseCheck::LoadConfig() (0x25a978) reserva un buffer local de
 * 0x40000 bytes (`sub sp, sp, #0x40000`) -> data abort en el primer push del callee
 * (log 001 + dump 1790300693, PC = .so+0x25a8e4 CallJNIFuncChar).
 */
#define GAME_THREAD_STACK (8 * 1024 * 1024)

static void *game_main(void *arg) {
    (void) arg;
    soloader_init_all();

    // JNI_OnLoad: VoxSetJavaVM(vm) + NVThreadInit(vm) -- el JavaVM que usa el hilo de
    // audio de Vox (AttachCurrentThread/GetEnv) y NVThreadGetCurrentJNIEnv.
    int (* JNI_OnLoad)(void *jvm, void *reserved) = sym("JNI_OnLoad");
    if (JNI_OnLoad) {
        int ver = JNI_OnLoad(&jvm, NULL);
        l_info("boot: JNI_OnLoad -> 0x%x", ver);
    }

    gl_init();
    l_success("boot: vitaGL inicializado");
    heap_log_snapshot("post-vitaGL");

    void (* EternalLegacy_nativeSetPhone)(void *, void *, jint)   = sym(JNI_PREFIX "EternalLegacy_nativeSetPhone");
    void (* Device_nativeInit)(void *, void *)                    = sym(JNI_PREFIX "GLUtils_Device_nativeInit");
    void (* GameGLSurfaceView_nativeOnSize)(void *, void *, jint, jint) = sym(JNI_PREFIX "GameGLSurfaceView_nativeOnSize");
    void (* GameGLSurfaceView_nativeOnTouch)(void *, void *, jint, jint, jint, jint) = sym(JNI_PREFIX "GameGLSurfaceView_nativeOnTouch");
    void (* GameGLSurfaceView_nativeResume)(void *, void *)       = sym(JNI_PREFIX "GameGLSurfaceView_nativeResume");
    void (* GameRenderer_nativeGetJNIEnv)(void *, void *)         = sym(JNI_PREFIX "GameRenderer_nativeGetJNIEnv");
    void (* GLResLoader_nativeInit)(void *, void *, jint)         = sym(JNI_PREFIX "GLResLoader_nativeInit");
    void (* GLMediaPlayer_nativeInit)(void *, void *, jint)       = sym(JNI_PREFIX "GLMediaPlayer_nativeInit");
    void (* GameRenderer_nativeInit)(void *, void *, jint)        = sym(JNI_PREFIX "GameRenderer_nativeInit");
    void (* EternalLegacy_nativeInit)(void *, void *, jint)       = sym(JNI_PREFIX "EternalLegacy_nativeInit");
    void (* GameRenderer_nativeResize)(void *, void *, jint, jint) = sym(JNI_PREFIX "GameRenderer_nativeResize");
    void (* GameRenderer_nativeRender)(void *, void *)            = sym(JNI_PREFIX "GameRenderer_nativeRender");
    void (* EternalLegacy_nativeKeyDown)(void *, void *, jint)    = sym(JNI_PREFIX "EternalLegacy_nativeKeyDown");
    void (* EternalLegacy_nativeKeyUp)(void *, void *, jint)      = sym(JNI_PREFIX "EternalLegacy_nativeKeyUp");
    jint (* EternalLegacy_nativegetState)(void *, void *, jint)   = sym(JNI_PREFIX "EternalLegacy_nativegetState");

    // Un jclass/jobject no-NULL para el 2do argumento: los nativeInit hacen
    // NewGlobalRef(clazz) y FalsoJNI lo devuelve tal cual; GetStaticMethodID ignora la clase.
    void *clazz = (void *) 0x42424242;

    // --- EternalLegacy.onCreate() ---
    if (EternalLegacy_nativeSetPhone) EternalLegacy_nativeSetPhone(&jni, clazz, SCREEN_W); // no-op en el .so
    if (Device_nativeInit) {
        l_info("boot: Device.nativeInit (ADevice::Init)");
        Device_nativeInit(&jni, clazz);
    }

    // --- GameGLSurfaceView.onSizeChanged() ---
    if (GameGLSurfaceView_nativeOnSize) GameGLSurfaceView_nativeOnSize(&jni, clazz, SCREEN_W, SCREEN_H);

    // --- GameRenderer.onSurfaceCreated(), mismo orden que jadx ---
    // nativeGetJNIEnv guarda mEnv: todos los *_nativeInit siguientes lo usan.
    if (GameRenderer_nativeGetJNIEnv) GameRenderer_nativeGetJNIEnv(&jni, clazz);
    if (GLResLoader_nativeInit) {
        l_info("boot: GLResLoader.nativeInit(0)");
        GLResLoader_nativeInit(&jni, clazz, 0);
    }
    if (GLMediaPlayer_nativeInit) {
        l_info("boot: GLMediaPlayer.nativeInit(0)");
        GLMediaPlayer_nativeInit(&jni, clazz, 0);
    }
    log_flush();
    if (GameRenderer_nativeInit) {
        // g_appAlive == 0 -> appInit(): Get_PhoneManufacturer, InitGL, Application::Init
        l_info("boot: GameRenderer.nativeInit(1) -> appInit");
        log_flush();
        GameRenderer_nativeInit(&jni, clazz, 1);
        l_info("boot: appInit completado");
    }
    if (EternalLegacy_nativeInit) {
        l_info("boot: EternalLegacy.nativeInit(1) (ALicenseCheck_InitLicense)");
        EternalLegacy_nativeInit(&jni, clazz, 1);
    }

    // --- GameRenderer.onSurfaceChanged() ---
    if (GameRenderer_nativeResize) GameRenderer_nativeResize(&jni, clazz, SCREEN_W, SCREEN_H);

    // --- GameGLSurfaceView.onWindowFocusChanged(true) -> appResume ---
    if (GameGLSurfaceView_nativeResume) GameGLSurfaceView_nativeResume(&jni, clazz);

    input_set_game_state((fn_game_state_instance) so_symbol(&so_mod, "_ZN9GameState11GetInstanceEv"));
    input_init((fn_touch_evt) GameGLSurfaceView_nativeOnTouch,
               (fn_key_evt) EternalLegacy_nativeKeyDown,
               (fn_key_evt) EternalLegacy_nativeKeyUp,
               (fn_get_state) EternalLegacy_nativegetState);

    l_info("boot: entrando al main loop");
    log_flush();
    waitprof_start();

    // GameRenderer.onDrawFrame(): nativeRender() (= appUpdate) + sleep hasta 33 ms. El
    // motor no presenta por su cuenta (no importa EGL ni llama swapEGLBuffers): el swap
    // lo hace GLSurfaceView despues de onDrawFrame, aca gl_swap().
    unsigned int frame = 0;
    g_main_loop_started = 1;
    while (1) {
        unsigned int t0 = sceKernelGetProcessTimeLow();
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);

        input_poll(&jni, clazz);
        if (frame < 5) l_info("loop: frame %u -> nativeRender", frame);
        // Flush ANTES de renderizar en los primeros frames: si el motor crashea en
        // medio de un nativeRender (Bug #003/#004: frame 3), los fopen/debug previos
        // ya están en disco y el log muestra QUÉ archivo estaba cargando.
        if (frame < 10) log_flush();
        if (GameRenderer_nativeRender) GameRenderer_nativeRender(&jni, clazz);
        gl_swap();

        // Sin flush por frame (Bug #007): los errores flushean solos, waitprof vuelca
        // cada 2 s si hay frames lentos, y aca cada 300 frames.
        if ((++frame % 300) == 0) {
            l_info("loop: %u frames", frame);
            heap_log_snapshot("loop");
            log_flush();
        }

        unsigned int dt = sceKernelGetProcessTimeLow() - t0;
        waitprof_frame(dt / 1000);
        if (dt < 33000)
            sceKernelDelayThread(33000 - dt);
    }

    return NULL;
}

int main() {
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, GAME_THREAD_STACK);
    pthread_create(&t, &attr, game_main, NULL);
    pthread_join(t, NULL);
    return 0;
}
