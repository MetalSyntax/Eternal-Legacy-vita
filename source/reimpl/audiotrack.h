/**
 * @file  audiotrack.h
 * @brief Emulacion minima de `android/media/AudioTrack` sobre `sceAudioOut`.
 *
 * `vox::DriverAndroid` (audio del motor glitch de Gameloft) no usa OpenSL/OpenAL: habla con
 * `android/media/AudioTrack` por JNI desde `_InitAT`/`UpdateThreadedAT` (out_ghidra.c
 * ~89700-90000). Patron tomado de Asphalt-6-Vita (source/reimpl/audiotrack.c), adaptado a
 * esta version de Vox, que usa `byte[]` (NewByteArray) y `write(byte[], 0, bytes)`.
 */

#ifndef SOLOADER_AUDIOTRACK_H
#define SOLOADER_AUDIOTRACK_H

#include <falso_jni/FalsoJNI.h>

#ifdef __cplusplus
extern "C" {
#endif

jint    AudioTrack_getMinBufferSize(jmethodID id, va_list args);
jobject AudioTrack_ctor(jmethodID id, va_list args);
void    AudioTrack_play(jmethodID id, va_list args);
void    AudioTrack_pause(jmethodID id, va_list args);
void    AudioTrack_stop(jmethodID id, va_list args);
void    AudioTrack_release(jmethodID id, va_list args);
jint    AudioTrack_write(jmethodID id, va_list args);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_AUDIOTRACK_H
