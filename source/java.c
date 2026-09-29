/*
 * java.c -- el "lado Java" de Eternal Legacy para FalsoJNI.
 *
 * Solo estan los metodos que libEternalLegacy.so resuelve de verdad con GetStaticMethodID/
 * GetMethodID (leidos en decompiled/.../out_ghidra.c, no los candidatos genericos de
 * generated_jni_table.h). Cada handler replica el Java de decompiled/apk_jadx:
 *
 *   GLResLoader_nativeInit   -> getResourceFull/Bytes/Length (String)
 *   EternalLegacy_nativeInit -> sendAppToBackground, Exit; ALicenseCheck_InitLicense ->
 *                               d, db, dc, da
 *   Device_nativeInit        -> ADevice::Init: a b c d e f IsWifiEnable getHostName
 *   GLMediaPlayer_nativeInit -> ~35 metodos de sonido/video/dispositivo
 *   vox::DriverAndroid       -> android/media/AudioTrack (reimpl/audiotrack.c)
 *
 * FalsoJNI resuelve IDs SOLO por nombre (no mira la firma ni la clase). Colision real:
 * "d" es EternalLegacy.d()V (tracking de ALicenseCheck) y Device.d()[B (version del juego).
 * Se registra una sola vez como Object (Device.d); el CallStaticVoidMethod de la licencia
 * cae en methodVoidCall sin match -> solo loguea, que es lo que haria el tracking offline.
 */

#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <psp2/kernel/processmgr.h>

#include "reimpl/audiotrack.h"
#include "reimpl/io.h"
#include "utils/logger.h"
#include "utils/utils.h"

// Resolucion que ve el motor (GLMediaPlayer.getWidth/getHeight -> isingleWidthApk).
// 960 elige el perfil de assets "_960" (joystick_960.swf, LOGO_960x640, splash_960x640).
#define VITA_SCREEN_W 960
#define VITA_SCREEN_H 544

/*
 * Helpers
 */

static jbyteArray make_byte_array(const void *data, size_t len) {
    // Nunca devolver NULL ni largo 0: ADevice/ALicenseCheck::CallJNIFuncChar hacen
    // GetArrayLength + GetByteArrayRegion sobre el resultado sin chequear NULL
    // (desensamblado en 0x25a8e4). Java devuelve `{0}` como minimo (Device.v).
    static const jbyte zero = 0;
    if (!data || len == 0) { data = &zero; len = 1; }
    jbyteArray arr = jni->NewByteArray(&jni, (jsize) len);
    if (arr) jni->SetByteArrayRegion(&jni, arr, 0, (jsize) len, (const jbyte *) data);
    return arr;
}

static jbyteArray make_string_bytes(const char *s) {
    return make_byte_array(s, s ? strlen(s) : 0);
}

// Equivalente al recorte de rutas de GLResLoader (".//", "./", trim).
static const char *resloader_strip(const char *s) {
    if (!s) return "";
    while (*s == ' ') s++;
    if (strncmp(s, ".//", 3) == 0) s += 3;
    else if (strncmp(s, "./", 2) == 0) s += 2;
    return s;
}

/*
 * GLResLoader: Java busca res/drawable "res_<nombre>", despues assets/, despues
 * /sdcard/gameloft/games/GloftELHP//<ruta>. Aca: assets/ y luego el cache del juego.
 */
static int resloader_find(const char *name, char *out, size_t out_size) {
    snprintf(out, out_size, DATA_PATH "assets/%s", name);
    if (file_exists(out)) return 1;
    char android_path[256];
    snprintf(android_path, sizeof(android_path), "/sdcard/gameloft/games/GloftELHP//%s", name);
    remap_android_path(android_path, out, out_size);
    return file_exists(out);
}

static long file_size_of(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz;
}

jobject GLResLoader_getResourceFull(jmethodID id, va_list args) {
    jstring jname = va_arg(args, jstring);
    const char *name = resloader_strip(jni->GetStringUTFChars(&jni, jname, NULL));
    char path[256];
    if (!resloader_find(name, path, sizeof(path))) {
        l_warn("GLResLoader.getResourceFull(%s): no existe", name);
        return NULL;
    }
    uint8_t *buf; size_t size;
    if (!file_load(path, &buf, &size)) return NULL;
    jbyteArray arr = jni->NewByteArray(&jni, (jsize) size);
    if (arr) jni->SetByteArrayRegion(&jni, arr, 0, (jsize) size, (jbyte *) buf);
    free(buf);
    l_debug("GLResLoader.getResourceFull(%s): %u bytes", name, (unsigned) size);
    return arr;
}

jobject GLResLoader_getResourceBytes(jmethodID id, va_list args) {
    jstring jname = va_arg(args, jstring);
    jint offset = va_arg(args, jint);
    jint len = va_arg(args, jint);
    const char *name = resloader_strip(jni->GetStringUTFChars(&jni, jname, NULL));
    char path[256];
    if (!resloader_find(name, path, sizeof(path)) || len <= 0) {
        l_warn("GLResLoader.getResourceBytes(%s, %d, %d): no existe", name, offset, len);
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    jbyte *buf = calloc(1, len);
    if (offset > 0) fseek(f, offset, SEEK_SET);
    fread(buf, 1, len, f);
    fclose(f);
    jbyteArray arr = jni->NewByteArray(&jni, len);
    if (arr) jni->SetByteArrayRegion(&jni, arr, 0, len, buf);
    free(buf);
    return arr;
}

jint GLResLoader_getResourceLength(jmethodID id, va_list args) {
    jstring jname = va_arg(args, jstring);
    const char *name = resloader_strip(jni->GetStringUTFChars(&jni, jname, NULL));
    char path[256];
    if (!resloader_find(name, path, sizeof(path))) return 0;
    long sz = file_size_of(path);
    return sz < 0 ? 0 : (jint) sz;
}

/*
 * EternalLegacy (Activity)
 */

void EternalLegacy_sendAppToBackground(jmethodID id, va_list args) {
    l_info("EternalLegacy.sendAppToBackground(): ignorado en Vita");
}

void EternalLegacy_Exit(jmethodID id, va_list args) {
    l_info("EternalLegacy.Exit(): saliendo");
    log_flush();
    sceKernelExitProcess(0);
}

// da(): la Activity. ALicenseCheck solo guarda el jmethodID; devolvemos un objeto no-NULL.
jobject EternalLegacy_da(jmethodID id, va_list args) {
    return (jobject) 0x42424242;
}

// db(): R.raw.igli (config cifrada de licencia) o, si no existe, Device.a() (IMEI).
jobject EternalLegacy_db(jmethodID id, va_list args) {
    uint8_t *buf; size_t size;
    if (file_load(DATA_PATH "res/raw/igli.bin", &buf, &size)) {
        jbyteArray arr = make_byte_array(buf, size);
        free(buf);
        return arr;
    }
    return make_string_bytes("0");
}

// dc(): clave de licencia; Java cae a Device.a() ante cualquier fallo.
jobject EternalLegacy_dc(jmethodID id, va_list args) {
    return make_string_bytes("000000000000000");
}

/*
 * GLUtils.Device (ADevice::Init). Datos de telefonia: valores neutros "sin SIM".
 */

jobject Device_a(jmethodID id, va_list args) { return make_string_bytes("000000000000000"); } // IMEI
jobject Device_b(jmethodID id, va_list args) { return make_string_bytes(""); }                // operador
jobject Device_c(jmethodID id, va_list args) { return make_string_bytes("00"); }              // linea
jobject Device_d(jmethodID id, va_list args) { return make_string_bytes("1.0.8"); }           // version
jobject Device_f(jmethodID id, va_list args) {                                                // user agent
    return make_string_bytes("Mozilla/5.0 (Linux; U; Android 2.3; en-us; PSVita) AppleWebKit/533.1");
}
jobject Device_getHostName(jmethodID id, va_list args) { return make_string_bytes("PSVita_PSVita"); }
jboolean Device_IsWifiEnable(jmethodID id, va_list args) { return JNI_FALSE; }
void Device_e(jmethodID id, va_list args) { (void) va_arg(args, jint); }                   // setUniqueCode

/*
 * GLMediaPlayer
 */

// Get_PhoneManufacturer: 0 = perfil LOW (HTC/Sony), 2 = LG (texturas DXT), 5 = Cliq2,
// 1 = generico -> VariableRes::s_DeviceCapability = 2 (HIGH) y g_isDxTCompression = 0.
// El cache trae 311 texturas PVR v2 formato 25 (PVRTC 4bpp), que vitaGL soporta.
jint GLMediaPlayer_Get_PhoneManufacturer(jmethodID id, va_list args) { return 1; }

// Get_logoVideoState: GLMediaPlayer.e -- 0 al arrancar, 1 mientras MyVideoView reproduce,
// 2 cuando termino (a.java). Tiene que arrancar en < 2: Application::Init solo hace
// GameState::Set_Child_State(0) (estado "logo" -> Update_Global_Loading, donde
// Init_Followup construye MenuMgr) si este valor es < 2. Devolver 2 desde el boot dejaba
// el GameState sin estado inicial y MenuMgr::m_Instance en NULL (Bug #002,
// CTutorialManager::Update). Sin reproductor: el video "termina" en el primer frame del
// main loop, y Update_Gameloft_Logo pasa al estado 2.
int g_main_loop_started = 0;
jint GLMediaPlayer_Get_logoVideoState(jmethodID id, va_list args) {
    return g_main_loop_started ? 2 : 0;
}

jint GLMediaPlayer_Get_DeviceLockState(jmethodID id, va_list args) { return 1; } // desbloqueado
jint GLMediaPlayer_Get_PhoneLanguage(jmethodID id, va_list args) { return 0; }   // "en"
jint GLMediaPlayer_detectPhoneLang(jmethodID id, va_list args) { return 0; }     // "eng"
jint GLMediaPlayer_getWidth(jmethodID id, va_list args) { return VITA_SCREEN_W; }
jint GLMediaPlayer_getHeight(jmethodID id, va_list args) { return VITA_SCREEN_H; }

jint GLMediaPlayer_loadMovie(jmethodID id, va_list args) {
    jstring jname = va_arg(args, jstring);
    l_info("GLMediaPlayer.loadMovie(%s): sin reproductor de video, se salta",
           jname ? jni->GetStringUTFChars(&jni, jname, NULL) : "(null)");
    return 1;
}

jint GLMediaPlayer_isMediaPlaying(jmethodID id, va_list args) { return 0; }

// El audio real sale por Vox/AudioTrack; el SoundPool/MediaPlayer de Java no tiene
// callers nativos que reproduzcan (playSoundID/playSoundBigID: 0 usos en out_ghidra.c).
// "Cargado" = 0 como en Java, asi ningun loop espera una carga que nunca llega.
jint GLMediaPlayer_isSoundLoaded(jmethodID id, va_list args) { return 0; }
jint GLMediaPlayer_isSoundLoadedBig(jmethodID id, va_list args) { return 0; }

void GLMediaPlayer_void_stub(jmethodID id, va_list args) { }

void GLMediaPlayer_openBrowser(jmethodID id, va_list args) {
    l_info("GLMediaPlayer.openBrowser(): ignorado en Vita");
}

/*
 * JNI Methods
 */

enum {
    // GLResLoader
    M_getResourceFull = 1,
    M_getResourceBytes,
    M_getResourceLength,
    // EternalLegacy
    M_sendAppToBackground,
    M_Exit,
    M_da,
    M_db,
    M_dc,
    // Device
    M_Device_a,
    M_Device_b,
    M_Device_c,
    M_Device_d,
    M_Device_e,
    M_Device_f,
    M_IsWifiEnable,
    M_getHostName,
    // GLMediaPlayer
    M_isSoundLoaded,
    M_isSoundLoadedBig,
    M_unloadSound,
    M_unloadSoundBig,
    M_loadSound,
    M_loadSoundBig,
    M_playSound,
    M_playSoundBig,
    M_pauseSound,
    M_pauseSoundBig,
    M_resumeSound,
    M_resumeSoundBig,
    M_stopSound,
    M_stopSoundBig,
    M_setVolume,
    M_setVolumeBig,
    M_resetSound,
    M_setPitch,
    M_stopAllSounds,
    M_stopAllPool,
    M_stopAllBig,
    M_destroySoundPool,
    M_initSoundPoolArray,
    M_loadMovie,
    M_getWidth,
    M_getHeight,
    M_OpenGLive,
    M_OpenIGP,
    M_openBrowser,
    M_Get_PhoneManufacturer,
    M_Get_logoVideoState,
    M_Get_DeviceLockState,
    M_Get_PhoneLanguage,
    M_isMediaPlaying,
    M_detectPhoneLang,
    // android/media/AudioTrack
    M_AudioTrack_init,
    M_AudioTrack_getMinBufferSize,
    M_AudioTrack_play,
    M_AudioTrack_pause,
    M_AudioTrack_stop,
    M_AudioTrack_release,
    M_AudioTrack_write,
};

NameToMethodID nameToMethodId[] = {
    { M_getResourceFull,        "getResourceFull",        METHOD_TYPE_OBJECT },
    { M_getResourceBytes,       "getResourceBytes",       METHOD_TYPE_OBJECT },
    { M_getResourceLength,      "getResourceLength",      METHOD_TYPE_INT },

    { M_sendAppToBackground,    "sendAppToBackground",    METHOD_TYPE_VOID },
    { M_Exit,                   "Exit",                   METHOD_TYPE_VOID },
    { M_da,                     "da",                     METHOD_TYPE_OBJECT },
    { M_db,                     "db",                     METHOD_TYPE_OBJECT },
    { M_dc,                     "dc",                     METHOD_TYPE_OBJECT },

    { M_Device_a,               "a",                      METHOD_TYPE_OBJECT },
    { M_Device_b,               "b",                      METHOD_TYPE_OBJECT },
    { M_Device_c,               "c",                      METHOD_TYPE_OBJECT },
    { M_Device_d,               "d",                      METHOD_TYPE_OBJECT }, // ver colision arriba
    { M_Device_e,               "e",                      METHOD_TYPE_VOID },
    { M_Device_f,               "f",                      METHOD_TYPE_OBJECT },
    { M_IsWifiEnable,           "IsWifiEnable",           METHOD_TYPE_BOOLEAN },
    { M_getHostName,            "getHostName",            METHOD_TYPE_OBJECT },

    { M_isSoundLoaded,          "isSoundLoaded",          METHOD_TYPE_INT },
    { M_isSoundLoadedBig,       "isSoundLoadedBig",       METHOD_TYPE_INT },
    { M_unloadSound,            "unloadSound",            METHOD_TYPE_VOID },
    { M_unloadSoundBig,         "unloadSoundBig",         METHOD_TYPE_VOID },
    { M_loadSound,              "loadSound",              METHOD_TYPE_VOID },
    { M_loadSoundBig,           "loadSoundBig",           METHOD_TYPE_VOID },
    { M_playSound,              "playSound",              METHOD_TYPE_VOID },
    { M_playSoundBig,           "playSoundBig",           METHOD_TYPE_VOID },
    { M_pauseSound,             "pauseSound",             METHOD_TYPE_VOID },
    { M_pauseSoundBig,          "pauseSoundBig",          METHOD_TYPE_VOID },
    { M_resumeSound,            "resumeSound",            METHOD_TYPE_VOID },
    { M_resumeSoundBig,         "resumeSoundBig",         METHOD_TYPE_VOID },
    { M_stopSound,              "stopSound",              METHOD_TYPE_VOID },
    { M_stopSoundBig,           "stopSoundBig",           METHOD_TYPE_VOID },
    { M_setVolume,              "setVolume",              METHOD_TYPE_VOID },
    { M_setVolumeBig,           "setVolumeBig",           METHOD_TYPE_VOID },
    { M_resetSound,             "resetSound",             METHOD_TYPE_VOID },
    { M_setPitch,               "setPitch",               METHOD_TYPE_VOID },
    { M_stopAllSounds,          "stopAllSounds",          METHOD_TYPE_VOID },
    { M_stopAllPool,            "stopAllPool",            METHOD_TYPE_VOID },
    { M_stopAllBig,             "stopAllBig",             METHOD_TYPE_VOID },
    { M_destroySoundPool,       "destroySoundPool",       METHOD_TYPE_VOID },
    { M_initSoundPoolArray,     "initSoundPoolArray",     METHOD_TYPE_VOID },
    { M_loadMovie,              "loadMovie",              METHOD_TYPE_INT },
    { M_getWidth,               "getWidth",               METHOD_TYPE_INT },
    { M_getHeight,              "getHeight",              METHOD_TYPE_INT },
    { M_OpenGLive,              "OpenGLive",              METHOD_TYPE_VOID },
    { M_OpenIGP,                "OpenIGP",                METHOD_TYPE_VOID },
    { M_openBrowser,            "openBrowser",            METHOD_TYPE_VOID },
    { M_Get_PhoneManufacturer,  "Get_PhoneManufacturer",  METHOD_TYPE_INT },
    { M_Get_logoVideoState,     "Get_logoVideoState",     METHOD_TYPE_INT },
    { M_Get_DeviceLockState,    "Get_DeviceLockState",    METHOD_TYPE_INT },
    { M_Get_PhoneLanguage,      "Get_PhoneLanguage",      METHOD_TYPE_INT },
    { M_isMediaPlaying,         "isMediaPlaying",         METHOD_TYPE_INT },
    { M_detectPhoneLang,        "detectPhoneLang",        METHOD_TYPE_INT },

    // GetMethodID("<init>") se resuelve como "<clase de FindClass>/<init>"
    { M_AudioTrack_init,        "android/media/AudioTrack/<init>", METHOD_TYPE_OBJECT },
    { M_AudioTrack_getMinBufferSize, "getMinBufferSize",  METHOD_TYPE_INT },
    { M_AudioTrack_play,        "play",                   METHOD_TYPE_VOID },
    { M_AudioTrack_pause,       "pause",                  METHOD_TYPE_VOID },
    { M_AudioTrack_stop,        "stop",                   METHOD_TYPE_VOID },
    { M_AudioTrack_release,     "release",                METHOD_TYPE_VOID },
    { M_AudioTrack_write,       "write",                  METHOD_TYPE_INT },
};

MethodsBoolean methodsBoolean[] = {
    { M_IsWifiEnable, Device_IsWifiEnable },
};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {};
MethodsInt methodsInt[] = {
    { M_getResourceLength,          GLResLoader_getResourceLength },
    { M_isSoundLoaded,              GLMediaPlayer_isSoundLoaded },
    { M_isSoundLoadedBig,           GLMediaPlayer_isSoundLoadedBig },
    { M_loadMovie,                  GLMediaPlayer_loadMovie },
    { M_getWidth,                   GLMediaPlayer_getWidth },
    { M_getHeight,                  GLMediaPlayer_getHeight },
    { M_Get_PhoneManufacturer,      GLMediaPlayer_Get_PhoneManufacturer },
    { M_Get_logoVideoState,         GLMediaPlayer_Get_logoVideoState },
    { M_Get_DeviceLockState,        GLMediaPlayer_Get_DeviceLockState },
    { M_Get_PhoneLanguage,          GLMediaPlayer_Get_PhoneLanguage },
    { M_isMediaPlaying,             GLMediaPlayer_isMediaPlaying },
    { M_detectPhoneLang,            GLMediaPlayer_detectPhoneLang },
    { M_AudioTrack_getMinBufferSize, AudioTrack_getMinBufferSize },
    { M_AudioTrack_write,           AudioTrack_write },
};
MethodsLong methodsLong[] = {};
MethodsObject methodsObject[] = {
    { M_getResourceFull,  GLResLoader_getResourceFull },
    { M_getResourceBytes, GLResLoader_getResourceBytes },
    { M_da,               EternalLegacy_da },
    { M_db,               EternalLegacy_db },
    { M_dc,               EternalLegacy_dc },
    { M_Device_a,         Device_a },
    { M_Device_b,         Device_b },
    { M_Device_c,         Device_c },
    { M_Device_d,         Device_d },
    { M_Device_f,         Device_f },
    { M_getHostName,      Device_getHostName },
    { M_AudioTrack_init,  AudioTrack_ctor },
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
    { M_sendAppToBackground, EternalLegacy_sendAppToBackground },
    { M_Exit,               EternalLegacy_Exit },
    { M_Device_e,           Device_e },
    { M_unloadSound,        GLMediaPlayer_void_stub },
    { M_unloadSoundBig,     GLMediaPlayer_void_stub },
    { M_loadSound,          GLMediaPlayer_void_stub },
    { M_loadSoundBig,       GLMediaPlayer_void_stub },
    { M_playSound,          GLMediaPlayer_void_stub },
    { M_playSoundBig,       GLMediaPlayer_void_stub },
    { M_pauseSound,         GLMediaPlayer_void_stub },
    { M_pauseSoundBig,      GLMediaPlayer_void_stub },
    { M_resumeSound,        GLMediaPlayer_void_stub },
    { M_resumeSoundBig,     GLMediaPlayer_void_stub },
    { M_stopSound,          GLMediaPlayer_void_stub },
    { M_stopSoundBig,       GLMediaPlayer_void_stub },
    { M_setVolume,          GLMediaPlayer_void_stub },
    { M_setVolumeBig,       GLMediaPlayer_void_stub },
    { M_resetSound,         GLMediaPlayer_void_stub },
    { M_setPitch,           GLMediaPlayer_void_stub },
    { M_stopAllSounds,      GLMediaPlayer_void_stub },
    { M_stopAllPool,        GLMediaPlayer_void_stub },
    { M_stopAllBig,         GLMediaPlayer_void_stub },
    { M_destroySoundPool,   GLMediaPlayer_void_stub },
    { M_initSoundPoolArray, GLMediaPlayer_void_stub },
    { M_OpenGLive,          GLMediaPlayer_void_stub },
    { M_OpenIGP,            GLMediaPlayer_void_stub },
    { M_openBrowser,        GLMediaPlayer_openBrowser },
    { M_AudioTrack_play,    AudioTrack_play },
    { M_AudioTrack_pause,   AudioTrack_pause },
    { M_AudioTrack_stop,    AudioTrack_stop },
    { M_AudioTrack_release, AudioTrack_release },
};

/*
 * JNI Fields
*/

// System-wide constant that applications sometimes request
// https://developer.android.com/reference/android/content/Context.html#WINDOW_SERVICE
char WINDOW_SERVICE[] = "window";

// System-wide constant that's often used to determine Android version
// https://developer.android.com/reference/android/os/Build.VERSION.html#SDK_INT
// Possible values: https://developer.android.com/reference/android/os/Build.VERSION_CODES
const int SDK_INT = 10; // Android 2.3.3 (el juego es de 2011)

NameToFieldID nameToFieldId[] = {
		{ 0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT },
		{ 1, "SDK_INT", FIELD_TYPE_INT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
		{ 1, SDK_INT },
};
FieldsObject fieldsObject[] = {
		{ 0, WINDOW_SERVICE },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
