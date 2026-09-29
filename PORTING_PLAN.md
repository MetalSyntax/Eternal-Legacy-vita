# Plan de Port — Eternal Legacy (PS Vita)

> Generado por psvita-port-toolkit el 2026-09-24. Punto de partida con lo detectado automáticamente --
confirmar todo con objdump/Ghidra/jadx a mano antes de asumirlo como cierto.

## 0. Contexto

- **Juego:** Eternal Legacy
- **Paquete Java:** com.gameloft.android.TBFV.GloftELHP.ML
- **APK original:** `Eternal_Legacy_1.0.8.apk`
- **TITLEID asignado:** `PSVELG001`

**¿Motor conocido?** Revisar si algún port hermano (bajo la misma BASE_DIR) comparte motor antes de
reusar su código -- confirmar con símbolos JNI reales, no por analogía superficial.

## 1. Detección automática

- **ABI(s):** armeabi
- **ABI elegida:** armeabi
- **Nota de arquitectura:** Solo armeabi (ARMv6, soft-float) -- Vita lo ejecuta igual (ARMv7 es superset), sin NEON de v7a.
- **Versión de GLES:** AndroidManifest.xml no declara glEsVersion -- usar heurística (GLES2 (glCreateShader/glCreateProgram/glUseProgram -- pipeline programable))

## 2. .so encontrados (ABI armeabi)

- `eternallegacy_extract/lib/armeabi/._libEternalLegacy.so` (4 KB)
- `eternallegacy_extract/lib/armeabi/libEternalLegacy.so` (10615 KB)


## 3. Exports JNI (convención `Java_*`)

**Corregido a mano (2026-09-24):** la detección automática dijo "0 exports", pero `nm -D` muestra
**29 `Java_*` + `JNI_OnLoad`**, sin `RegisterNatives`. Los que usa el loader:
`EternalLegacy_{nativeSetPhone,nativeInit,nativeKeyDown,nativeKeyUp,nativegetState}`,
`GLUtils_Device_nativeInit`, `GameGLSurfaceView_{nativeOnSize,nativeOnTouch,nativeResume}`,
`GameRenderer_{nativeGetJNIEnv,nativeInit,nativeResize,nativeRender}`,
`GLResLoader_nativeInit`, `GLMediaPlayer_nativeInit`.

## 3b. Motor confirmado

Gameloft **glitch** + **gameswf** + **Vox**, GLES2 con GLSL de `shaders.pak`. Familia Java TBFV
(igual que GT-Racing-Motor-Academy-HD-vita); motor/GL/audio igual que Asphalt-6-Vita. Detalle
completo (ciclo de vida, texturas PVRTC, AudioTrack, licencia, filesystem) en
`port_progress.md` Fase 3.

## 3c. Layout de datos en la Vita

```
ux0:data/eternallegacy/
  libEternalLegacy.so          (lib/armeabi/ del APK)
  GloftELHP/                   (gameloft/games/GloftELHP/ del cache: file*.dat, *.tga, *.ttf...)
  res/raw/igli.bin             (res/ del APK; solo igli.bin es leido hoy)
  assets/                      (assets/ del APK)
  saves/  logs/  shader_cache/ (los crea el loader)
```
Requisitos en consola: `kubridge.skprx` y `ur0:data/libshacccg.suprx`.


## 4. Checklist

- [x] Repo creado desde soloader-boilerplate, git init, .gitignore anti-DMCA.
- [x] APK decompilado (jadx) y .so decompilado(s) (Ghidra) -- ver sección 2/3.
- [x] Análisis del motor real (ciclo de vida nativo, reuso de otro port o boilerplate genérico).
- [x] Bootstrap del loader: so_file_load/so_relocate/so_resolve, primer build (BUILD OK 2026-09-24).
- [x] Tabla JNI (FalsoJNI): los ~60 métodos que el .so resuelve de verdad (`source/java.c`).
- [x] Imports: 324/324 resueltos (auditoría `nm -D -u` vs `dynlib.c`).
- [x] Gráficos: vitaGL vendorizado de Asphalt 6 (softfp, GLSL en caliente), 8 escenas/frame.
- [x] Audio: Vox → AudioTrack emulado sobre `sceAudioOut` (`source/reimpl/audiotrack.c`).
- [x] Assets: remapeo `/sdcard/.../GloftELHP` y `/data/data/<pkg>` + `chdir` emulado.
- [x] Input táctil + BACK/MENU; LiveArea validada.
- [ ] **Primer arranque en consola real** -- siguiente paso, requiere hardware.
- [ ] Controles físicos → touches sintéticos (joystick/botones del HUD): requiere ver el layout real.
- [ ] Video del logo (hoy se salta con `Get_logoVideoState=2`).
- [ ] Pruebas en hardware real.

## 5. Herramientas

Este port se gestiona con **psvita-port-toolkit** (standalone, fuera de este repo). Desde el
toolkit: `Continuar con un port existente` → elegí esta carpeta (ya tiene `.psvita-toolkit.json`).

## 6. Estándar de logs en consola

El log del juego vive en `<DATA_PATH>logs/<slug>_NNN.log` (`<slug>` = eternallegacy), incremental de
001 a 999 con `next.idx` (nunca `log_<timestamp>.txt`, nunca base 000, nunca ruta hardcodeada:
usar `DATA_PATH`). La subida FTP crea `logs/` y `saves/` en la consola aunque estén vacíos.
Comandos del toolkit (sin IA, deterministas): `psvita-toolkit log-standard --fix-dirs` (audita
y crea los dirs), `psvita-toolkit log-trace --ensure-flag` (inyecta trazas `[TRACE] >> f()` por
función bajo `#ifdef PORT_TRACE`; compilar con `-DPORT_TRACE=ON` para debug, `OFF` para
producción, o `psvita-toolkit log-trace --remove` para quitarlas del árbol).

## Auto-detected lifecycle methods (psvita-toolkit)

Native methods whose name matches a well-known Android/GL app lifecycle hook --
these are the ones `main.c`/the loader most likely needs to call directly to
drive the game (there's no real Android `Activity`/`GLSurfaceView` calling them
for you).

- `com.gameloft.android.TBFV.GloftELHP.ML.EternalLegacy.nativeInit(int)`
- `com.gameloft.android.TBFV.GloftELHP.ML.GLMediaPlayer.nativeInit(int)`
- `com.gameloft.android.TBFV.GloftELHP.ML.GLResLoader.nativeInit(int)`
- `com.gameloft.android.TBFV.GloftELHP.ML.GLUtils.Device.nativeInit(void)`
- `com.gameloft.android.TBFV.GloftELHP.ML.GameRenderer.nativeInit(int)`
- `com.gameloft.android.TBFV.GloftELHP.ML.GameRenderer.nativeRender(void)`
- `com.gameloft.android.TBFV.GloftELHP.ML.GameRenderer.nativeResize(int, int)`
