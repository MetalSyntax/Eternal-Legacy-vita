#include "reimpl/audiotrack.h"

#include <psp2/audioout.h>
#include <stdint.h>
#include <string.h>

#include "utils/logger.h"

/*
 * Lo que hace vox::DriverAndroid con estos metodos (out_ghidra.c, _InitAT/UpdateThreadedAT/
 * DoCallbackAT), con R = lo que devuelve getMinBufferSize:
 *
 *   getMinBufferSize(44100, CHANNEL_OUT_STEREO=0xc, ENCODING_PCM_16BIT=2)
 *   framesRaw = R >> 2 ; frames = min(framesRaw, 0x400)
 *   new AudioTrack(STREAM_MUSIC=3, 44100, 0xc, 2, framesRaw << 2, MODE_STREAM=1)
 *   buf = NewByteArray(framesRaw << 2)
 *   loop: GetPrimitiveArrayCritical(buf) -> _FillBuffer(ptr, frames) -> Release...
 *         write(buf, 0, frames << 2)          // BYTES: frames estereo de 16 bits
 *
 * A diferencia de Asphalt 6 (short[] y size en shorts, ver su Bug #034), aca `size` son
 * bytes y _FillBuffer llena exactamente frames*4 bytes: el grain del puerto es size/4.
 * write() tiene que BLOQUEAR como el AudioTrack real: es lo unico que marca el ritmo del
 * hilo de Vox (no duerme en ningun otro lado mientras no este en pausa).
 */
#define AT_CHANNEL_OUT_MONO    4
#define AT_CHANNEL_OUT_STEREO  12
#define AT_MIN_BUFFER_BYTES    4096   // -> 1024 frames/bloque (~23 ms), sin activar el clamp

typedef struct {
    int opened;
    int port;
    int channels;
    int grainFrames;
} AudioTrackState;

static AudioTrackState g_at = { .opened = 0, .port = -1, .channels = 2, .grainFrames = 0 };

// Resto (< grain) del ultimo write(), a completar con el siguiente. Se resetea al
// abrir/cerrar el puerto para no anteponer audio viejo tras un release/reopen.
static uint8_t s_spill[4096];
static int s_spillBytes = 0;

jint AudioTrack_getMinBufferSize(jmethodID id, va_list args) {
    jint sampleRate    = va_arg(args, jint);
    jint channelConfig = va_arg(args, jint);
    jint audioFormat   = va_arg(args, jint);

    l_info("AudioTrack.getMinBufferSize(%d, %d, %d) -> %d",
           sampleRate, channelConfig, audioFormat, AT_MIN_BUFFER_BYTES);
    return AT_MIN_BUFFER_BYTES;
}

jobject AudioTrack_ctor(jmethodID id, va_list args) {
    jint streamType        = va_arg(args, jint);
    jint sampleRate        = va_arg(args, jint);
    jint channelConfig     = va_arg(args, jint);
    jint audioFormat       = va_arg(args, jint);
    jint bufferSizeInBytes = va_arg(args, jint);
    jint mode              = va_arg(args, jint);
    (void) streamType;
    (void) audioFormat;
    (void) mode;

    if (g_at.opened) {
        sceAudioOutReleasePort(g_at.port);
        g_at.opened = 0;
    }

    int channels = (channelConfig == AT_CHANNEL_OUT_MONO) ? 1 : 2;
    int grainFrames = bufferSizeInBytes / (channels * 2);
    if (grainFrames > 1024) grainFrames = 1024;  // mismo clamp que el motor
    grainFrames -= grainFrames % 64;
    if (grainFrames < SCE_AUDIO_MIN_LEN) grainFrames = SCE_AUDIO_MIN_LEN;

    // BGM acepta 44100 Hz; MAIN exige 48000 (Asphalt-6-Vita Bug #036).
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, grainFrames, sampleRate,
                                   channels == 1 ? SCE_AUDIO_OUT_MODE_MONO
                                                 : SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        l_error("AudioTrack: sceAudioOutOpenPort(BGM, %d, %d) fallo (0x%08x)",
                grainFrames, sampleRate, (unsigned) port);
        return NULL;
    }

    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);

    g_at.opened      = 1;
    g_at.port        = port;
    g_at.channels    = channels;
    g_at.grainFrames = grainFrames;
    s_spillBytes     = 0;

    l_info("AudioTrack: puerto 0x%x abierto (%d Hz, %d ch, %d frames/bloque)",
           port, sampleRate, channels, grainFrames);

    // FalsoJNI nunca nos devuelve `this`: alcanza con un puntero estable no-NULL.
    return (jobject) &g_at;
}

// La pausa real la hace el motor dejando de llamar a write() (flag +0x60 de DriverAndroid).
void AudioTrack_play(jmethodID id, va_list args) { (void) id; (void) args; }
void AudioTrack_pause(jmethodID id, va_list args) { (void) id; (void) args; }
void AudioTrack_stop(jmethodID id, va_list args) { (void) id; (void) args; }

void AudioTrack_release(jmethodID id, va_list args) {
    (void) id; (void) args;
    if (!g_at.opened) return;
    sceAudioOutReleasePort(g_at.port);
    l_info("AudioTrack: puerto 0x%x cerrado", g_at.port);
    g_at.opened = 0;
    g_at.port = -1;
    s_spillBytes = 0;
}

jint AudioTrack_write(jmethodID id, va_list args) {
    jbyteArray arr    = va_arg(args, jbyteArray);
    jint       offset = va_arg(args, jint);
    jint       size   = va_arg(args, jint);

    if (!g_at.opened || !arr) return 0;
    if (offset < 0 || size <= 0) return 0;

    // El motor a veces pide mas bytes de los que trae el array (o con offset): en
    // Android eso tira excepcion; aca se clampean para no leer fuera del buffer.
    jsize arrLen = jni->GetArrayLength(&jni, arr);
    if (offset >= arrLen) return 0;
    if ((long long)offset + (long long)size > (long long)arrLen)
        size = arrLen - offset;

    jbyte *data = jni->GetByteArrayElements(&jni, arr, NULL);
    if (!data) return 0;

    int frameBytes = g_at.channels * 2;
    int frames = size / frameBytes;
    static int warned = 0;
    if (frames != g_at.grainFrames && !warned) {
        warned = 1;
        l_warn("AudioTrack.write(): %d frames != grain %d (se saca de a bloques de grain)",
               frames, g_at.grainFrames);
    }

    // sceAudioOutOutput lee siempre grainFrames frames: bloques completos, sin pasar del array.
    // El resto (< grain) espera en el spill buffer al proximo write (antes se tiraba,
    // lo que producia clicks/underruns cuando el grain no dividia exacto al size).
    uint8_t *p = (uint8_t *) data + offset;
    int grainBytes = g_at.grainFrames * frameBytes;
    int left = frames * frameBytes; // solo bytes alineados a frame
    while (left >= grainBytes - s_spillBytes) {
        int need = grainBytes - s_spillBytes;
        if (s_spillBytes > 0) {
            static uint8_t block[4096];
            memcpy(block, s_spill, s_spillBytes);
            memcpy(block + s_spillBytes, p, need);
            sceAudioOutOutput(g_at.port, block);
            s_spillBytes = 0;
        } else {
            sceAudioOutOutput(g_at.port, p);
        }
        p += need;
        left -= need;
    }
    if (left > 0 && s_spillBytes + left < (int) sizeof(s_spill)) {
        memcpy(s_spill + s_spillBytes, p, left);
        s_spillBytes += left;
    }

    jni->ReleaseByteArrayElements(&jni, arr, data, JNI_ABORT);
    return size;
}
