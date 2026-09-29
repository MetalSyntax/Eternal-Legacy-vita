# Registro de Progreso — Eternal Legacy (PS Vita)

## Fase 1: Configuración y Preparación (Completada — 2026-09-24)
- Repo creado desde soloader-boilerplate, `.gitignore` anti-DMCA.
- APK `Eternal_Legacy_1.0.8.apk` copiado y extraído.
- ABI detectada: armeabi (elegida: armeabi).
- GLES detectado: AndroidManifest.xml no declara glEsVersion -- usar heurística (GLES2 (glCreateShader/glCreateProgram/glUseProgram -- pipeline programable))

## Fase 2: Decompilación (Completada — 2026-09-24)
- jadx: corrido, resultados en decompiled/apk_jadx/.
- Ghidra (.so): corrido para cada .so.

## Fase 3: Análisis del Motor Real (Completada — 2026-09-24, sin consola)

**Motor:** Gameloft **glitch** (derivado de Irrlicht) + **gameswf** (UI Flash) + **Vox** (audio),
GLES2 real (NEEDED `libGLESv2.so` + `libGLESv1_CM.so`; los shaders salen de `shaders.pak`).
Mismo motor que Asphalt-6-Vita / Dungeon-Hunter-2-vita, y **misma familia Java TBFV** que
GT-Racing-Motor-Academy-HD-vita (`GLResLoader`, `GLMediaPlayer`, `GameRenderer`,
`GameGLSurfaceView` casi idénticos; el .so hasta exporta un `..._GTRacing_nativeOpenIGM`
sobrante de esa base de código).

- **JNI:** 29 exports `Java_*` + `JNI_OnLoad` (el toolkit reportó "0 exports", falso:
  `nm -D` los muestra). Sin `RegisterNatives`. `JNI_OnLoad` = `VoxSetJavaVM(vm)` +
  `NVThreadInit(vm)`, devuelve `0x10004`.
- **Ciclo de vida real** (jadx `EternalLegacy.onCreate` / `GameRenderer.onSurfaceCreated` +
  pseudo-C de cada export):
  - `nativeSetPhone` = no-op. `Device.nativeInit` → `ADevice::Init` (IDs `a b c d e f
    IsWifiEnable getHostName`).
  - `GameRenderer.nativeGetJNIEnv(env)` guarda `mEnv` — **todos** los `nativeInit` siguientes
    usan `mEnv`, así que va primero.
  - `GLResLoader.nativeInit` → `getResourceFull/Bytes/Length(String)`.
  - `GLMediaPlayer.nativeInit` → ~35 IDs (sonido Java, video, `Get_PhoneManufacturer`,
    `getWidth/getHeight`, `Get_logoVideoState`…).
  - `GameRenderer.nativeInit(1)` → `appInit()` (sólo la 1ª vez, `g_appAlive`).
  - `EternalLegacy.nativeInit(1)` → IDs `sendAppToBackground/Exit` +
    `ALicenseCheck_InitLicense` (IDs `d db dc da`).
  - `nativeResize(w,h)` = stores; `nativeRender()` = `appUpdate()`; `nativeOnTouch(a,x,y,id)`
    → `appOnTouch`; `nativeResume/Pause` → `appResume/appPause`.
- **Resolución:** `isingleWidthApk = getWidth()` elige perfiles de assets 960 / 854 / 800
  (`joystick_960.swf`, `LOGO_Gameloft_960x640.mp4`…). El alto se lee dinámico
  (`getHeight()`), no hay 640 hardcodeado → se reporta **960x544** (perfil 960).
- **Texturas:** el cache trae 311 PVR v2 formato 25 = **PVRTC 4bpp** (soportado por vitaGL).
  `Get_PhoneManufacturer`: 0 = LOW, 2 = LG/DXT, 5 = Cliq2, resto (1) = HIGH sin DXT → se
  devuelve **1**. Vita no soporta ATC, así que no hay que tocar los packs.
- **Audio:** todo por **Vox → `android/media/AudioTrack` por JNI** (`vox::DriverAndroid::_InitAT`
  / `UpdateThreadedAT` / `DoCallbackAT`): 44100 Hz estéreo PCM16, `NewByteArray`,
  `GetPrimitiveArrayCritical` + `write(byte[],0,frames*4)` en un hilo propio cuyo único
  ritmo es el bloqueo de `write`. Los 1832 `.dat` `SKIJ…WAVEfmt` (IMA ADPCM) son datos de
  Vox. El SoundPool de Java (`playSound`/`playSoundBig`) **no tiene callers nativos**.
- **Video del logo:** `Get_logoVideoState()` ≥ 2 = "terminó"; con 2 `Update_Gameloft_Logo`
  avanza sin llamar a `loadMovie` (y `ResumeGame` tampoco lo relanza).
- **DRM/licencia (`ALicenseCheck`)**: solo tracking; `ValidateLicense` nunca bloquea.
  Ojo: `CallJNIFuncChar` (ARM, 0x25a8e4) hace `GetArrayLength`+`GetByteArrayRegion` sobre
  el retorno **sin chequear NULL** → los stubs `a b c d f db dc getHostName` deben devolver
  byte arrays válidos (≥1 byte).
- **Input:** juego 100% táctil. `appKeyPressed` = no-op; `appKeyReleased` solo mira BACK(4) y
  MENU(82). jadx: MENU no manda `nativeKeyDown`; `onKeyUp` de 4/82 pasa por
  `nativegetState()` y solo con 1/2 entrega `nativeKeyUp`.
- **Filesystem:** `fopen` directo con prefijo `/sdcard/gameloft/games/GloftELHP/` (fuentes,
  splash, `file000000.dat` = ObfuscationMap, `shaders.pak`) + `chdir()` en
  `CFileSystem::changeWorkingDirectoryTo` y luego rutas relativas; `/data/data/<pkg>` para
  el RMS de licencia. `GetNativeResource*` (GLResLoader) sin callers directos visibles.
- **Resume/lost-context:** `UpdateResumeManager` solo corre si se re-llama `nativeInit(1)`
  con contexto perdido — no aplica (un solo init).

## Fase 4: Bootstrap del Loader (Completada en host — 2026-09-24)

Reuso deliberado de ports hermanos **con hardware confirmado**, no por analogía:
- `lib/falso_jni`: estaba vacío (submódulo sin clonar). Copiado de GT-Racing MA.
- `lib/vitaGL`: **fuente** de Asphalt-6-Vita (sin `.o`/`.a`), compilado desde CMake con
  `SOFTFP_ABI=1 NO_DEBUG=1 LOG_ERRORS=1 HAVE_SHADER_CACHE=1 NO_SPLASHSCREEN=1
  DRAW_SPEEDHACK=2` (el set con el que A6 compila GLSL de glitch en caliente). Incluye el
  parche de A6 que manda `vgl_log` a nuestro logger.
- Por qué no PVR_PSP2 (lo que usó DH2): el vitaGL del SDK de DH2 no traducía GLSL; el de
  A6 sí (66 shaders/33 programas en consola). Si acá fallan los shaders, PVR_PSP2 es el plan B.

Cambios:
- `source/main.c`: ciclo real en el orden de jadx (ver Fase 3), `clazz` dummy no-NULL,
  `nativeResume` como `onWindowFocusChanged(true)`, loop `input_poll → nativeRender →
  gl_swap` con throttle 33 ms (el `Thread.sleep(33-dt)` de Java). Heap 192 MB.
- `source/java.c`: reescrito con los ~60 métodos que el .so resuelve de verdad (no los 100+
  candidatos genéricos de `generated_jni_table.h`, que queda sin compilar como referencia).
  Colisión `d`: FalsoJNI resuelve solo por nombre → se registra `Device.d()[B` como Object;
  el `CallStaticVoidMethod(d)` de la licencia cae sin match y solo loguea (inocuo).
- `source/reimpl/audiotrack.c`: AudioTrack sobre `sceAudioOut` (puerto BGM 44100 estéreo,
  1024 frames), adaptado de A6 a `byte[]`/bytes (acá el grain es `size/4`, sin el Bug #034
  de A6).
- `source/reimpl/io.c`: `remap_android_path()` para `fopen/open/stat/access/chdir`:
  `/sdcard/gameloft/games/GloftELHP` → `ux0:data/eternallegacy/GloftELHP`,
  `/data/data/<pkg>` → `saves/`, relativas → cwd emulado; colapsa `//`.
- `source/input.c`: táctil frontal 1920x1088→960x544 con slots estables (nunca el id crudo),
  CIRCLE→BACK, START→MENU con la semántica de `getState`, CROSS/DPAD como keycodes (el
  motor los ignora hoy). Sin touches sintéticos: el layout del HUD se ve recién en consola.
- `source/dynlib.c`: 31 imports faltantes (22 `__aeabi_*` soft-float de libgcc,
  `_ZSt7nothrow`, `_ZnajRKSt9nothrow_t`, `__dso_handle`, `cosh`, `inet_addr`, 4
  `gl*3DOES` → `ret0`), `access`/`chdir`/`glLinkProgram` a wrappers.
- `source/utils/glutil.c`: `vglSetupRenderTargetScenesNum(8,8)` (gameswf hace RTT → sin
  esto sceGxmBeginScene se cuelga), sin MSAA, umbral 24 MB, cache de shaders en
  `DATA_PATH/shader_cache`, log de fallos de compile/link, `vgl_log()`.
- `source/utils/logger.c`: estándar `logs/eternallegacy_NNN.log` + `next.idx`, bufferizado
  en RAM con `log_flush()` (errores flushean solos).
- `source/utils/init.c`: crea `saves/` y `logs/`; error claro si falta `GloftELHP/`.
- `CMakeLists.txt`: `SO_PATH = libEternalLegacy.so`, vitaGL vendorizado,
  `DUMP_COMPILED_SHADERS=OFF` (buffer fijo de 32 KB que corrompía el heap, A6 #009/#010).
- `.gitignore`: `Makefile` → `/Makefile` (el toolkit copia respetando `.gitignore` y se
  comía `lib/vitaGL/Makefile`: primer build falló con "no makefile found").

## Fase 5: Primer Build (OK en host — 2026-09-24)
- `psvita-toolkit build --preset debug`: 3 intentos, **BUILD OK** → `build/eboot.bin`
  (~627 KB) + `build/eternallegacy.vpk` (~710 KB).
  1. `lib/vitaGL/Makefile` excluido por el `.gitignore` (ver Fase 4) → corregido.
  2. Link: `multiple definition of egl*` entre `source/reimpl/egl.c` del scaffold y el
     `egl.c` que trae el vitaGL de A6 → se sacó `reimpl/egl.c` del build (el .so no importa
     EGL: presenta nuestro `gl_swap`). `undefined utf16_to_utf8/utf8_to_utf16` →
     faltaba `lib/falso_jni/converter.c` en la lista.
- Verificado en el ELF: sin `Tag_ABI_VFP_args` (softfp en loader + vitaGL + libgcc, igual
  que el .so), rutas horneadas `ux0:data/eternallegacy/{libEternalLegacy.so,GloftELHP,
  logs/eternallegacy_%03d.log,shader_cache}`, símbolos `vgl_log`/`AudioTrack_write`/
  `remap_android_path`/`__aeabi_fadd` presentes.
- Imports: **324/324** resueltos (re-auditado tras los cambios). `livearea --validate` OK.

## Primera prueba en consola (pendiente — requiere hardware)
Instalación:
1. `kubridge.skprx` en config.txt de taiHEN y `ur0:data/libshacccg.suprx` (ShaRKBR33D).
2. Copiar `ux0_data/eternallegacy/` a `ux0:data/eternallegacy/` (ya tiene
   `libEternalLegacy.so`, `GloftELHP/` del cache, `res/`, `assets/`). Instalar el VPK.
3. Tras correr: bajar `ux0:data/eternallegacy/logs/eternallegacy_NNN.log` (+ `.psp2dmp` si
   crashea) y seguir con `so-crash-triage`, un bug a la vez.

Qué mirar primero en el log (orden del boot):
- `boot: JNI_OnLoad -> 0x10004`, `vitaGL inicializado`.
- `Device.nativeInit` → `GLResLoader/GLMediaPlayer.nativeInit` → `appInit completado`:
  si corta acá, sospechar `glitch::createDeviceEx` (GL/extensiones) o un `fopen` fallido
  justo antes (`fopen(... -> ...): NULL`).
- `AudioTrack: puerto 0x... abierto` (hilo de Vox).
- `glCompileShader(...) FALLO` / `[vitaGL]`: si los shaders de `shaders.pak` no traducen,
  plan B = PVR_PSP2 como Dungeon-Hunter-2-vita.
- `loop: N frames` cada 300 frames = main loop vivo.

Hipótesis abiertas que solo la consola confirma:
- 960x544 con el perfil de assets "960" (diseñado para 960x640): puede verse recortado/
  estirado; alternativa = reportar 854x480 (misma relación de aspecto que la Vita).
- `Get_logoVideoState=2` salta el video: si algún estado espera otro valor, se cuelga en el logo.

## Bug #001 (log 001 + dump 1790300693) — Stack overflow en `ALicenseCheck::LoadConfig` (resuelto en host, sin probar)

**Lo que muestra el log 001:** el boot entero anda — `.so` cargado/relocado, 324 imports,
`JNI_OnLoad -> 0x10004`, vitaGL, `Device`/`GLResLoader`/`GLMediaPlayer.nativeInit`,
`appInit` con `nativeGet_PhoneManufacturer -> 1` → **HIGH**, `Window Width = 960,
Height = 544` (los stubs de `getWidth/getHeight` llegan bien). Corta justo después de 4
errores de vitaGL (`glPixelStorei(GL_PACK_ALIGNMENT)`, `glDisable(GL_DITHER /
SAMPLE_ALPHA_TO_COVERAGE / SAMPLE_COVERAGE)` → `GL_INVALID_ENUM`, inocuos).

**Dump:** Data abort, `PC = 0x9825a8e4`. Ojo: el `psvita-toolkit analyze` lo etiquetó como
`.so + 0x5058e4 (std::codecvt...)` — **mal**, sumó la base dos veces. Con la base real
(`0x98000000`): `PC = .so+0x25a8e4` = **primera instrucción** (`push {r4-r8,lr}`) de
`ALicenseCheck::CallJNIFuncChar`; `LR = .so+0x25a9d0` = `ALicenseCheck::LoadConfig`, que
hace `sub sp, sp, #0x40000` (buffer local de 256 KB) y le pasa `R3 = 0x40000`. Un fault en
un `push` con un frame de 256 KB recién reservado = **stack overflow**: el hilo principal
de la Vita tiene 256 KB por defecto.

Barrido del `.so` por frames grandes (`objdump | sub sp, sp, #N`, N ≥ 16 KB): solo
`LoadRMS` (512 KB), `SaveRMS` (512 KB, llamado desde `validateResponse`) y `LoadConfig`
(256 KB).

**Fix:**
- `source/main.c`: todo el juego corre en `game_main()` sobre un pthread con **8 MB** de
  stack; `main()` solo lo crea y hace `join`.
- `source/reimpl/pthr.c`: hilos creados por el motor 512 KB → **1 MB** (`SaveRMS` no entra
  en 512 KB exactos).

**Además (pedido del usuario): flags GLSL del `README VITAGL.md`** agregados a
`VITAGL_MAKE_FLAGS`: `HAVE_GLSL_UBOS=1 HAVE_GLSL_TEXTURE_SIZE=1 HAVE_FIXED_ATTRIBUTES=1
SAFE_UNIFORMS=1` + `HAVE_SHARK_LOG=1` (el log del compilador llega a
`glGetShaderInfoLog`, que `glCompileShader_soloader` ya imprime). `HAVE_FFP_SHADER_SUPPORT`
no: es para built-ins de GLSL 1.20 de escritorio y el motor manda GLSL ES. Verificado en
el ELF (`shark_log_cb` presente → vitaGL recompilado con los flags).

Rebuild: **BUILD OK** (`eboot.bin` 630 KB, VPK 717 KB), sin warnings en `source/`.

Notas del log para después: `vglInitExtended` tardó ~8 s (1.8 s → 9.8 s); cada `vgl_log`
es `l_error` y flushea (~100 ms por línea en la SD) — si aparecen muchos errores de
vitaGL por frame, bajar a nivel sin flush.

## Bug #001 CONFIRMADO resuelto en consola (log 002)
El boot pasa `LoadConfig`/`ALicenseCheck_InitLicense` (NVThread `AttachCurrentThread: 0`),
`appInit` completo (lee `file000000.dat` = ObfuscationMap y `file000153.dat`), `APPRESUME`, y
entra al main loop.

## Bug #002 (log 002 + dump 1790301154) — `MenuMgr::m_Instance == NULL` en `CTutorialManager::Update` (resuelto en host, sin probar)

**Dump** (analizado con `--so-base 0x98000000`; sin eso el toolkit etiqueta mal): Data abort
en `.so+0x254c40` (Thumb) `strb r2, [r0, #0x38]` con `R0 = 0` = retorno de
`MenuMgr::getInstance()`. Stack: `nativeRender → appUpdate → Application::Update →
GameState::Update(+0x1d6) → CTutorialManager::Update`. El log se cortó en "entrando al main
loop": lo que el motor imprimió en esos frames quedó en el buffer RAM del logger.

**Cadena real (disasm, no Ghidra — ojo: las direcciones de Ghidra = real + 0x10000):**
- `GameState::Update` (0x24f47c) solo llega al tutorial con estado (`+9`) > 3.
- `MenuMgr` se construye (`_Znwj` + `MenuMgrC1Ev`, cuyo ctor hace `m_Instance = this`) en
  `Application::Init_Followup` (0x2580b0), que corre en los pasos 0..10 de
  `GameState::Update_Global_Loading` (estado 2). `MenuMgr::init()` de `onEvent` es
  `m_Instance = getInstance()`, no crea nada.
- Estados: 0 = `Update_Gameloft_Logo` (si `Get_logoVideoState()==2` → `Set_Child_State(2)`),
  2 = `Update_Global_Loading` (termina en `Set_Child_State(3)`), 3 = splash (touch-up en
  `Application::onEvent` → estado 4), 4 = menú principal.
- **Causa:** `Application::Init` hace `Set_Child_State(0)` **solo si
  `nativeGet_logoVideoState() < 2`**. El stub devolvía 2 desde el boot → el `GameState`
  nunca recibió estado inicial, arrancó en uno > 3 sin carga global → `MenuMgr` nunca creado.
  En Android `GLMediaPlayer.e` arranca en 0, `MyVideoView` lo pone en 1 y `a.java` en 2 al
  terminar el video.

**Fix:**
- `source/java.c`: `Get_logoVideoState()` = 0 hasta que arranca el main loop, 2 después
  (`g_main_loop_started`, seteado en `main.c`). `Init` fija el estado 0 y pide
  `loadMovie` (stub → 1); en el primer frame `Update_Gameloft_Logo` ve 2 y pasa a la carga.
- `source/main.c`: flush del log en cada uno de los primeros 1800 frames + traza de los 5
  primeros `nativeRender`.

Rebuild: **BUILD OK** (VPK 717 KB). Lo esperado en el log 003: `ALicenseCheck111...`,
`LoadSwf`/`fopen` de `.swf` y texturas, compilación de shaders, y quedarse en el splash
esperando un toque (estado 3 → 4 por touch).

## Bug #003 (log 004 + dump 1790303442) — data abort en un `pthread` durante el frame 3 (en fix, sin probar)

**Lo que muestra el log 004:** con el fix del Bug #002 el juego avanza — `appInit` completo,
`APPRESUME`, main loop con frames 0-3 (`nativeRender`), `ALicenseCheck`, y el hilo de Vox abre
el puerto de audio (`AudioTrack: puerto 0x100 abierto, 44100 Hz, 2 ch, 1024 frames/bloque`).
Corta ahí: no hay línea `loop: frame 4`, así que el crash es **durante el `nativeRender` del
frame 3** (el `log_flush()` post-frame nunca corrió). `file000153.dat` (643 KB) se abrió con
`fopen` en `appInit` y nunca se cerró antes del crash.

**Dump** (`vita-parse-core` directo, el `analyze` del toolkit no resolvió nada útil porque
`PC` cae en `SceLibKernel`):
- Hilo en crash: `pthread` 0x4001014f, Data abort. `PC = 0xe00000e8` (`SceLibKernel` seg1
  +0x38), `LR = 0x74666f6c` = ASCII `"loft"`, `R12 = 0x656d6167` = ASCII `"game"` (restos del
  path `/sdcard/gameloft/games/...` en registros), `R0 = 0`, `R1 = 0x82565438` (heap),
  `R2 = 0xfffffffc` (-4), `R3 = R4 = 0x81103224` = `g_OpStorage` de FalsoJNI,
  `R11 = 0x985acef8` (`.so` +0x5acef8, pegado a la vtable `stdio_streambuf_base` de STLport).
- Stack: en `SP+8` hay `0x98437429` = retorno Thumb a `.so` +0x437429 =
  `glitch::res::File::Init(FileReader*)` +0x99 (justo después de su `blx memcpy`), y más abajo
  direcciones de retorno a `SceLibFios2`/`SceLibc` con tamaños 0x10000/0x18000: el hilo estaba
  en una **lectura de archivo** (`fread` → Fios2 → `sceClibMemcpy`, que vive en `SceLibKernel`).
- Los otros hilos están sanos: un `pthread` bloqueado en `sceAudioOutOutput` (el hilo de Vox
  funciona), dos en `sceKernelDelayThread`, el principal en espera. Firma completa:
  `memcpy(dst = NULL, src = heap, len = 0xFFFFFFFC)`.
- Ojo con una pista falsa: `R11` cerca de una vtable y `LR/R12` con ASCII no son el bug, son
  restos del parseo del path en registros/stack.

**Causa raíz (hipótesis fuerte, no confirmada en consola):** `File::Init` calcula tamaños con
restas (`size - 60`, ver desensamblado en 0x4373f8-0x437442) y los pasa a
`FileReader::read` → `GetByteArrayRegion`/`memcpy`. Los macros `Get/SetPrimitiveArrayRegion`
de FalsoJNI solo chequeaban `length > len`, **nunca `length < 0`**: un largo negativo pasa el
chequeo y `memcpy(dst, src, -4)` copia 4 GB. Peor: `Get...` con `buffer == NULL` hacía
`malloc(length)` sin tamaño de elemento ni chequeo de fallo → `malloc(-4)` = NULL →
`memcpy(NULL, heap, 0xFFFFFFFC)` = la firma exacta del dump. El `Set...` con `buffer == NULL`
solo logueaba un warning y seguía al `memcpy` (crash garantizado).

**Fix (más endurecimiento pedido por el usuario):**
- `lib/falso_jni/FalsoJNI_ImplBridge.h`: ambos macros ahora rechazan `length < 0` y
  `start + length > len` (con suma en 64 bits), `length == 0` retorna sin tocar nada, `Get`
  usa `malloc(length * elemSize)` con chequeo de fallo, y ambos verifican `jda->array != NULL`
  y `buffer != NULL` antes del `memcpy`. (De paso corrige que el `malloc(length)` original ni
  multiplicaba por el tamaño del elemento.)
- `source/reimpl/audiotrack.c`: `write()` clampa `offset/size` contra `GetArrayLength`
  (negativos y overflows ya no leen fuera del array), ignora bytes no alineados a frame, y el
  resto (< grain) espera en un spill buffer para el próximo `write` en vez de tirarse
  (antes: clicks/underruns si el size no era múltiplo exacto del grain). Spill se resetea en
  ctor/release.
- `source/reimpl/io.c`: `opendir_soloader` ahora sí remapea paths Android (antes pasaba el
  path crudo → siempre fallaba para `/sdcard/...`); `s_cwd` y el buffer estático de
  `readdir_soloader` van bajo mutex (el motor carga desde varios hilos).
- `source/reimpl/pthr.c`: `sem_*_soloader` devuelven `EINVAL` con `uid == NULL` (antes
  desreferenciaban sin chequear → mismo data abort en kernel ante un semáforo sin init).

Rebuild: **BUILD OK** (`eboot.bin` 632 KB, VPK 719 KB), sin warnings en `source/`.
Lo esperado en el log 005: si el crash era el `length < 0`, el juego pasa el frame 3 y sigue
con la carga global (`Update_Global_Loading`, más `fopen`/`File::Init`); si crashea igual,
el dump nuevo dirá si la firma cambió (`R2` ya no 0xFFFFFFFC = era otro `memcpy`).

## Bug #003 — hipótesis del `length < 0` DESCARTADA (log 005 + dump 1790305840)

El log 005 es idéntico al 004 (corta tras `AudioTrack: puerto abierto`, sin `frame 4`) y el
dump 1790305840 es **byte a byte igual** al anterior: mismo hilo, mismo `SP`, mismos
registros (`R0=0, R1=0x82565438, R2=0xFFFFFFFC`, `R3=R4=g_OpStorage` — desplazado +0x10 por
los nuevos globales de audio, lo que confirma que son punteros reales a BSS de FalsoJNI y no
basura). El fix de los macros `Region` no cambió nada: el juego nunca llega a un
`Get/Set*Region` con largo negativo, o el `memcpy` fatal es otro.

## Bug #004 (log 005 + dump 1790305840) — `memcpy(dst=NULL, src=heap, len=-4)` en carga de archivo (en fix, sin probar)

**Hechos nuevos del análisis:**
- `nm -D -u` del `.so`: importa `memcpy`/`memmove`/`memset`/`fread`/`malloc` directos (el
  `File::Init` del desensamblado llama a `memcpy@plt`). Iban a `sceClibMemcpy` sin ningún
  chequeo: cualquier `dst == NULL` o largo con underflow = data abort en `SceLibKernel` sin
  rastro en el log.
- `lib/falso_jni/FalsoJNI.c` `GetStringRegion`/`GetStringUTFRegion` copiaban desde
  `string->utf16`/`string->utf8` (el **struct** `JavaDynArray`, en heap) en vez de
  `...->array`: corrupción de datos silenciosa (coherente con `R1` = heap en el dump si el
  `memcpy` fatal viniera de acá con `len` negativo — tampoco chequeaban `len < 0`).

**Fix (más endurecimiento general):**
- `source/reimpl/mem.c` (nuevo): `memcpy/memmove/memset_soloader` — caso sano idéntico (una
  comparación), caso roto (`dst/src == NULL` o `len > 128 MB`) = `l_error` + `log_flush` y se
  salta la copia en vez de crashear. `dynlib.c` remapea los 6 imports del `.so` a estos
  wrappers. Si el `memcpy` era el del crash, el log 006 mostrará la línea `RECHAZADO` con los
  argumentos exactos y el juego seguirá (la copia perdida puede causar otro bug después, pero
  visible en vez de un data abort mudo).
- `FalsoJNI.c`: `GetStringRegion`/`GetStringUTFRegion` copian de `...->array` y rechazan
  `start/len < 0`.
- `source/main.c`: `log_flush()` **antes** de cada `nativeRender` en los primeros 10 frames;
  `source/reimpl/io.c`: `fopen_soloader` flushea tras loguear. Hasta ahora un crash a mitad de
  frame borraba los `fopen` previos del buffer RAM; desde este build el log siempre muestra el
  último archivo abierto antes del corte.

Rebuild: **BUILD OK** (`eboot.bin` 632 KB, VPK 719 KB), sin warnings en `source/`.
Lo esperado en el log 006: o bien una línea `memcpy/memmove/memset(...): RECHAZADO` seguida de
más carga, o bien el último `fopen` antes del corte (dice qué archivo mata a `File::Init`).

## Bug #004 resuelto a medias (log 006) — el guard atrapó al culpable, nuevo crash sin dump

**Lo que muestra el log 006:** el guard funcionó dos veces —
1. `memcpy(0x8251c918, 0x0, 0x0)` en `appInit` (benigno, len 0, el juego sigue).
2. El juego **pasa el frame 3** (el muro de los logs 004/005), entra al frame 4, abre
   `file000122.dat` (4 KB, OK) y luego `file002246.dat` (334 KB) →
   `memcpy(0x0, 0x82565418, 0x3c): RECHAZADO`. El `src` coincide con el `R6` de los dumps
   viejos: era este `memcpy` el del data abort (el `R2=0xFFFFFFFC` de antes era el estado del
   loop interno, no el largo pedido).
3. El log corta justo ahí. **No hay dump nuevo**: tras saltarse la copia, `File::Init`
   sigue con el buffer en NULL y muere en silencio (lectura a `NULL+60`) o se cuelga.

**Causa raíz confirmada:** `GlitchAlloc(size)` del motor es un `malloc` directo (desensamblado
en 0x258120: solo `blx malloc@plt`) y **devuelve NULL** al cargar `file002246.dat`. Falta
saber si el tamaño pedido es basura (header mal parseado) o si el heap está agotado/corrupto.

**Comparación con hermanos (pedido del usuario):** Asphalt-6-Vita (mismo motor glitch) usa heap
de **256 MB**; acá se usan 192 MB por el umbral de vitaGL. Dungeon-Hunter-2 diagnostica el heap
con `mallinfo()`. No se sube el heap a ciegas: primero evidencia.

**Fix/diagnóstico:**
- `source/reimpl/mem.c`: `malloc/calloc/realloc_soloader` — ante un NULL loguean tamaño
  pedido + foto `mallinfo` (`arena/usados/libres`) y flushean. `dynlib.c` remapea los 3
  imports del `.so`. El log 007 dirá `malloc(0x...)` gigante (= parse mal) o sano con heap
  lleno (= agotado → subir a 256 MB como A6; o corrupción → otra cacería).
- `source/main.c`: foto de heap tras vitaGL y cada 300 frames (tendencia de consumo).

Rebuild: **BUILD OK** (VPK 720 KB), sin warnings en `source/`.
Lo esperado en el log 007: línea `malloc(0x...): NULL (heap: ...)` al abrir `file002246.dat`.

## Bug #005 (log 007 + dump 1790307819) — File::Init parsea basura: el header de 60 bytes nunca se llena (en diagnóstico, sin probar)

**Lo que muestra el log 007:**
- `heap[post-vitaGL]: arena=8433664 usados=8432792` y al fallar `arena=9641984
  usados=9310216`: heap casi vacío (**9 MB de 192 MB**). NO es agotamiento.
- `malloc(0xb5859cbc / 0xeaa0987c / 0xb57cd9a0 / 0x6af9b340): NULL`: tamaños basura de ~2-3
  GB al cargar `file002246.dat`. NINGUNO aparece en el archivo (búsqueda binaria en los 334
  KB) → no son bytes del archivo: el buffer del header de `File::Init` quedó **sin llenar**
  (memoria heap reciclada, determinista entre corridas — por eso `0xb5859cbc` era `R7` y
  `0xeaa0987c` estaba en el stack de los dumps viejos).
- `file002246.dat` SÍ es un archivo legítimo del juego (contiene recursos `.bdae` = meshes/
  animaciones; hay strings `data/3d/...bdae` en el `.so`), así que no se puede esconder sin más.

**Dump 1790307819:** por primera vez el `PC` cae en el `.so`:
`0x98437494` = `File::Init(FileReader*)` +0x104, instrucción `ldr r0, [r3, r1]` con
`R3 = 0` (`[this+28]`, la tabla de chunks del `new[]` fallido) en la 1ª iteración (`R7 = 1`).
El `count` (`[this+20] = header+48`) del archivo vale `0x7A69746E` (bytes `6e74 697a` del
header) — positivo como signed, así que entra al loop; `file000122.dat` se salvó de casualidad
(su count es negativo → `ble` lo salta). El `new[]` de 3.5 GB devuelve NULL y no hay chequeo.

**Qué falta:** saber QUÉ `new[]` pide cada tamaño (¿el de `File::Init`+0x7e con
`size = filesize - 4*H16 - H44 - H56`, u otro?) y por qué el `read` del header devuelve 0
bytes con el `fopen` exitoso (todos los `f*` que importa el `.so` van a SceLibc consistente;
no usa `fseeko`/`rewind`; no hay `gz*`). Hipótesis vivas: `getSize`/seek del `CReadFile`
intermedio, u otro `open` por una vía no wrapeada.

**Instrumentación (este build):** `malloc/calloc/realloc` y el guard de `memcpy` ahora loguean
`PC=<caller> (.so+0x...)` vía `__builtin_return_address`. El log 008 dirá el sitio exacto de
cada pedido basura y con el desensamblado se mapea al campo del header/cómputo que lo origina.

## ~~Bug #005 resuelto~~ (log 008 + dump 1790309266) — DIAGNÓSTICO INCORRECTO, ver Bug #006

> **Corrección (2026-09-28):** la ofuscación +1..+4 es real, pero el motor ya la revierte solo
> (`CReadFile::read`, con el flag que sale del mapa `file000000.dat`). `fix_assets.py`/`fix_data.py`
> reescribieron 2590 `.dat` locales **incluido el propio mapa** (su checksum `c|-|3k` deja de
> validar -> el motor no encuentra ningún recurso) y reemplazaron los ZIP multi-variante por un
> solo `.bdae`. Se restauraron los originales desde el zip de cache y se borraron ambos scripts.
> Nunca se probó en consola (el log 008 es anterior al script). Texto original:


**Causa raíz confirmada:**
- El crash en `glitch::res::File::Init` se debía a que los datos descargados a la tarjeta SD en la ruta `ux0_data/eternallegacy/GloftELHP/` se encontraban **ofuscados y sin extraer**.
- La ofuscación de Gameloft añade `1, 2, 3, 4` a los primeros cuatro bytes de cada archivo `.dat`. (Por ejemplo, un archivo ZIP que debería empezar con `PK\x03\x04` terminaba empezando por `QM\x06\x08`, y un archivo TGA que empieza con `0x00 0x00 0x02 0x00` terminaba como `0x01 0x02 0x05 0x04`).
- Puesto que el mapa de ofuscación de Gameloft (`pack.dat` u otro archivo `.nfo`) no estaba presente en los datos crudos proporcionados para el port, el motor en C++ de Android estaba forzado a leer bytes directamente (bypasseando cualquier desofuscación al vuelo provista por el Virtual File System).
- Peor aún, los modelos 3D (`.bdae`) en realidad estaban comprimidos en contenedores ZIP (ej. `file002246.dat`). En la versión original de Android, el código del instalador en Java descarga el ZIP, **extrae** un archivo `.bdae` específico a la arquitectura (ej. `little_endian_not_quantized.bdae`), y reemplaza el `.dat` en el disco. Al no ejecutarse el código Java, el motor C++ leyó un ZIP ofuscado en lugar de un archivo BRES extraído, interpretó `QM\x06\x08` como los tamaños internos del encabezado BRES, y solicitó múltiples `malloc` gigantes (ej. `0xb5859cbc`) provocando OOM (Out Of Memory) y luego el fatal `memcpy` a NULL abortado por el guard.

**El Fix:**
- Para que el motor C++ (sin su VFS completo activo) lea eficientemente los datos, he creado y ejecutado un script de Python `fix_assets.py` directamente en el entorno de desarrollo.
- Este script escaneó **los 2,594 archivos `.dat`** de la carpeta SD. Revirtió matemáticamente el cifrado de los primeros cuatro bytes en todos ellos. 
- En caso de encontrar un archivo ZIP (que en realidad era un contenedor de múltiples arquitecturas), el script extrajo el archivo interno para ARM (`little_endian_not_quantized.bdae`) y **sobrescribió permanentemente el archivo `.dat` en el disco**.
- Con los assets desofuscados y extraídos correctamente en el almacenamiento `ux0`, el juego ahora puede arrancar procesando archivos BRES y TGA nativos sin crashear en la inicialización de lectura.

## Bug #006 (log 008 + dump 1790309266) — `ftell` tras `fseek(SEEK_END)` da 0 en el hilo de carga -> no se desofusca (en fix, sin probar)

**Cadena real, confirmada con desensamblado + memoria del dump:**
1. `file000000.dat` es el mapa de ofuscación (`CFileSystem::addObfuscationFileMap`, clave `0x3a`,
   trailer `c|-|3k`). Decodificado en Python: `file002246.dat:defaulteffects.bdae`,
   `file000153.dat:shaders.pak`, `file000122.dat:data/2d/loading1.tga`, etc. Todos los `.dat` del
   mapa tienen los 4 primeros bytes +1,+2,+3,+4.
2. `CFileSystem::open` busca el nombre en el mapa y crea `CFile(fp, path, obfuscated=1)`
   (`CFile+0x24`). `CReadFile::openFile` copia ese flag a `CReadFile+0x2c` y mide el tamaño con
   `fseek(fp,0,SEEK_END)` + `ftell` (`CReadFile+0x10`). El ctor **borra el flag si size < 4**.
   `CReadFile::read` resta 1..4 a los bytes leídos en las posiciones 0..3 (según `ftell`).
3. `CResFile::CResFile` -> `CResFileManager::getReadFile`: si `+0x2a` y `CZipReader::isValid`
   (4 bytes == `PK\3\4`), abre `little_endian_not_quantized.bdae` de adentro del ZIP
   (defaulteffects.bdae es un ZIP con big/little endian x quantized/not + source.dae). Si no es ZIP,
   **apaga `+0x2a` para siempre** y le pasa el archivo crudo a `File::Init`.
4. Memoria del dump: `CReadFile@0x82567530` (vtable `0x588ff8`, nombre `./DefaultEffects.bdae`):
   `size(+0x10) = 0`, `flag(+0x2c) = 0`; su `CFile@0x82567568` tiene `+0x24 = 1`. O sea: el mapa
   funcionó, pero `fseek(END)`+`ftell` devolvió 0 en el hilo `pthread` de carga (en el hilo principal,
   `file000000.dat` sí midió bien). Sin tamaño no hay desofuscación -> `isValid` ve `QM\x06\x08` ->
   `File::Init` parsea el ZIP como BRES (count `0x7A69746E` = "ntiz" de
   `..._not_quantized` en el offset 0x30) -> `new[]` de GB -> NULL -> data abort.

**Fix (`source/reimpl/io.c`, `source/dynlib.c`, `lib/libc_bridge`):**
- Rastreador de tamaño/posición para todo `FILE*` abierto en solo lectura: tamaño por
  `sceIoGetstat` en el `fopen`; `SEEK_END`/`SEEK_CUR` se traducen a `SEEK_SET` absoluto;
  la posición se lleva en `fread/fgetc/getc/ungetc/fseek`; `ftell` devuelve la rastreada y, si
  SceLibc discrepa, loguea **una vez por archivo** `ftell(fp): libc=X, rastreado=Y` (así el log 009
  dice cuál de los dos estaba roto). `fscanf` (vía nuevo `sceLibcBridge_vfscanf`, NID 0xF137771A)
  invalida y el próximo `ftell` resincroniza.
- `fgetpos/fsetpos`: el `.so` usa el `fpos_t` de Bionic (= `long`); el de SceLibc es una struct más
  grande -> pasarlo directo pisaba el stack del llamador. Ahora van por `ftell/fseek_soloader`.
- `fseeko/ftello` apuntaban a las de **newlib** operando sobre `FILE*` de SceLibc: ahora van a
  `fseek/ftell_soloader` (off_t de armeabi = 32 bits, misma firma).

**Datos:** en la Vita tienen que estar los `.dat` ORIGINALES del zip de cache (sin tocar). Si se
subió la salida de `fix_assets.py`, hay que volver a subir `ux0_data/eternallegacy/GloftELHP/`.

Lo esperado en el log 009: posiblemente una línea `ftell(fp ...): libc=0, rastreado=...` y el
`trace:` de file002246.dat con `fseek(..., 0, 2) -> 0 (abs 333905)`; sin `malloc(0x...): NULL` ni
`memcpy ... RECHAZADO` tras abrir `file002246.dat`, y la carga siguiendo con más `fopen`.

## Bug #006 CONFIRMADO resuelto (log 010)

`file002246.dat` se mide bien (`fseek(...,0,2) -> abs 333905`, `ftell` libc == rastreado), el ZIP se
detecta y el juego pasa la carga inicial hasta el menú con música (`Bgm_Main`). No más `malloc` gigantes.
(En el log 010 SceLibc devolvió valores correctos: el tamaño 0 del dump 1790309266 no se reprodujo,
pero el rastreador queda como red de seguridad.)

## Bug #007 (log 010) — el menú carga ~1800 voces a ~85 ms c/u (>2 min) (en diagnóstico, sin probar)

**Hechos:**
- Desde t=31 s el motor abre `data/sound/voice/*.wav` una tras otra (1037 en 105 s, siguen al
  cortar el log a los 136 s). `fopen->fclose` = 11 ms; `fclose->siguiente fopen` = 74 ms de mediana,
  **sin correlación con el tamaño** del archivo (r = 0.02) → no es lectura ni decodificación.
- Nunca aparece `loop: 300 frames`: la carga corre DENTRO de `nativeRender` (hilo principal).
- Candidatos en el `.so`: `vox::AccessController::GetWriteAccess/GetReadAccess` (spin con
  `usleep(1000)`), `vox::VoxThread::_Update` (tick de hasta 33 ms), o latencia propia de
  `fopen` (SceLibc + FIOS + carpeta de 2600 entradas) + el `sceIoGetstat` del Bug #006.

**Cambios (optimización segura + medición):**
- `io.c`: los tamaños de `GloftELHP/` salen de UN escaneo del directorio (`sceIoDopen`, cache
  ordenada + `bsearch`) en vez de un `sceIoGetstat` por `fopen`. Sin `log_flush` por `fopen`,
  sin línea por `fclose`; trazador de file002246 apagado.
- `main.c`: sin `log_flush` por frame en los primeros 1800 frames (solo errores / cada 300).
- `waitprof.c` (nuevo): tiempo acumulado por (tipo, `.so+offset` de quien llama) en
  `usleep/nanosleep/sched_yield`, mutex con contención, `cond_wait/timedwait`, `fopen/fread/
  fclose/getstat`. Un hilo propio vuelca el top-12 cada 2 s **solo si** hubo frames >100 ms o
  menos de 40 frames (la carga ocurre dentro de un único `nativeRender`, así que no puede
  depender del main loop).
- `pthr.c`: `sem_timedwait` tenía la condición del deadline invertida (el `.so` no importa `sem_*`,
  pero queda corregido).

Lo esperado en el log 011: líneas `waitprof[2s]` durante la carga de voces que digan qué
(`usleep` desde `.so+0x2805c0..0x280656` = spin de `vox::AccessController`, `.so+0x2806a8` = `VoxThread::Sleep`, `fopen`, `mutex`, …) se come los ~75 ms/archivo.

### Bug #007 — causa confirmada (log 011, waitprof): `fopen` recorre linealmente la carpeta

- `waitprof` muestra el hilo principal con 0 frames por ventana y el tiempo en
  `fopen .so+0x38d0e1` (`CFileSystem::open`): 20-25 aperturas por 2 s a 80-95 ms c/u.
  (`usleep .so+0x2806b9` = `vox::VoxThread::Sleep`, el tick del hilo de audio: ruido, no bloquea.)
- Duración de `fopen` vs número de archivo (log 011, mediana por tramo de 250):
  file0000xx 13 ms, file0005xx 46 ms, file0010xx 82 ms, file0015xx 114 ms, file0020xx 146 ms,
  file0025xx 175 ms → **13 ms + 0.065 ms × posición en la carpeta** (orden de subida FTP). El
  driver de la Vita escanea la carpeta entera de ~2600 entradas en cada open.
- Carga de voces del menú: ~1800 × ~90 ms ≈ 3 min → "se queda en la pantalla de carga".

**Fix:** layout `ux0:data/eternallegacy/dat/NN/fileNNNNNN.dat` (NN = NNNNNN/64, 41 carpetas de 64).
`remap_android_path` reescribe `GloftELHP/fileNNNNNN.dat` → `dat/NN/...` si existe
`dat/00/file000000.dat` (si no, sigue el layout plano con un warning). El cache de tamaños escanea
`GloftELHP/` + `dat/*`. `extras/pack_data.py <zip de cache> <destino>` arma el layout desde el zip
(copia byte a byte, sin tocar la ofuscación). En GloftELHP/ quedan solo los ~45 archivos sueltos.
Esperado: ~15-20 ms por `fopen` → carga de voces en ~30 s en vez de ~3 min.

### Bug #007 — segunda parte (log 012): misses de `fopen` a ~170 ms ("npt")

- Con `dat/NN/` cada `fopen` bajó a **3-7 ms** (antes 13-175 ms) y la carga de voces terminó a los ~37 s.
- Después el motor queda pidiendo `GloftELHP/npt` cada ~172 ms (12 por ventana de 2 s, 0 frames).
  `"npt"` es el nombre placeholder de efecto/raíz en `CBuff::CBuff` (sin efecto): en Android el
  miss es instantáneo. Acá cada miss recorría TODA la carpeta, **incluidas las 2594 entradas
  borradas** (exFAT no compacta directorios) → ~170 ms = lo mismo que escanear 2600 posiciones.

**Fix:**
- `io.c`: `game_dir_lookup()` responde 1/0/-1 desde el listado cacheado de `GloftELHP/` + `dat/*`
  (sin distinguir mayúsculas). `fopen` de solo lectura, `access` y `stat` de una ruta de datos que
  el cache dice que no existe devuelven `ENOENT` sin tocar el disco. `GloftELHP/` no tiene
  subcarpetas, así que `GloftELHP/<sub>/...` (las voces `./data/Sound/voice/*.wav` que faltan)
  también es miss directo. Warning de `fopen` fallido: uno por ruta, sin `log_flush`.
- `join_collapsed` saltea segmentos `./` (el motor arma `GloftELHP/./data/...`).
- Vita: `GloftELHP/` recreada limpia (rename a `_old`, `MKD`, mover los 43 archivos, `RMD`), sin
  entradas borradas. Verificado: 43 archivos, tamaños y hashes de muestra iguales.

## Bug #008 (log 013) — no se puede entrar a la arena: `functionmacro.lua` vacío en el cache (en fix, sin probar)

**Síntoma:** gameplay OK, pero en el exterior de la arena (cap. 0) una "pared transparente" no deja pasar.

**Causa confirmada:**
- La entrada a la arena es la cadena de quests `Main_Arena_010` (ENTER_ZONE) → `Main_Arena_020`
  (TOUCH_TRIGGER "battle": pelea con el guardia, termina con `SetNextMap('arena_inside')` +
  `JumpNextMap()`) → `Main_Arena_030`. El log 013 muestra `main_arena_010.lua` (90 s) y
  `main_arena_020.lua` (120 s) abiertos, pero **ninguna** carga posterior (ni `cs_arena_out_dummy.bdae`,
  ni `cam_talk.bdae`, ni voces): los scripts mueren en la primera línea.
- Las quests llaman 12 helpers globales (`StartCutscene`, `EndCutscene`, `AddNPC`, `SetDialogAndWait`
  y variantes `_CamTalk/_CamSide/_CamSideR/_CamUp`, `SetSimpleDialog`, `SetFog`, `DirTo`,
  `GetNameStringIdx`) que no están definidos en ningún .lua: viven en `data/script/functionmacro.lua`
  = `file000923.dat`, que en el zip de cache pesa **0 bytes**. Otros 3 .dat también vacíos:
  `file000654` (data/2d/loading4.tga → el "FAILED to load TEXTURE"), `file000703`
  (levelscript/cave.lua), `file002173` (sfx_reinherz_attack.wav). `data.pack` (2816 archivos) pasa CRC.
- Los errores de Lua eran invisibles: `CScriptManager::OutputError` hace `fprintf(__sF+0x54, ...)`
  (FILE de Bionic = 0x54 bytes) y el loader exportaba un `FILE[3]` de newlib → basura a SceLibc.

**Fix:**
- `extras/data_fixes/functionmacro.lua`: reconstrucción de los 12 helpers a partir de
  `StartCinematic/EndCinematic/FieldNpcTalkScene` (scriptinit.lua), los ~1300 usos en quests, las
  funciones `EL.*` del .so (`SetNaviTargetName`, `AddNPC(n,m,x,y,z)`, `DoCameraAni(ani,obj,altura)`...)
  y las animaciones de `cam_talk.bdae` (`cam_talk`, `cam_side_shot`, `cam_side_shot_r`, `cam_up_shot`).
  Parsea como Lua (luaparser). No es el original: encuadres de cámara pueden diferir.
- `extras/pack_data.py`: `DATA_FIXES` repara los .dat vacíos (functionmacro reconstruido, loading4.tga
  = copia exacta de `GloftELHP/loading4.tga`, sfx_reinherz_attack = sfx_skill_reinherz_ghost_attack),
  ofuscados +1..+4 como el resto. `cave.lua` sigue vacío (sin fuente; afecta recién al cap. 3).
- `source/reimpl/stdio_std.c`: `__sF` con el layout de Bionic (3 × 0x54); `fprintf/vfprintf/fputs/
  fwrite/fputc/putc/fflush/printf/vprintf/puts/putchar` sobre stdout/stderr van al log
  (`[stdout]`/`[stderr]`, stderr con flush). Los errores de Lua ahora aparecen.

### Bug #008 — log 014: la partida guardada quedó "rota"

- Con `functionmacro.lua` reparado no hay errores de Lua (`[stderr]` solo muestra avisos de
  gameswf: `'setState' is not a function`, `can't find method gotoAndPlay`).
- Pero el log 014 **cargó una partida** (saveLoad → slot) guardada en el log 013, cuando las quests
  `Main_Arena_010/020` ya habían disparado su evento y el script había muerto en la línea 1. El
  save conserva esos eventos como consumidos: no se abre ningún script de quest, el trigger de la
  pelea imprime `ON_QUEST_TOUCH_TRIGGER = ` vacío y el portal `arena_outside_arena_inside` no lleva.
- En el log 013 (partida nueva) `main_arena_010` sí se disparó al entrar a la zona → hay que
  empezar **partida nueva** para que la cadena 010 → 020 → 030 corra con los helpers reparados.

## Bug #009 (log 014) — controles físicos (en fix, sin probar)

El motor es solo táctil (`appKeyPressed` no-op; `appKeyReleased` solo BACK/MENU), por eso solo
CÍRCULO (BACK) y START (MENU) hacían algo (los dos abrían el menú). `source/input.c`, esquema de
Shadow Guardian:
- Estado del juego: `GameState::GetInstance()` (símbolo del .so) byte +9 (`Set_Child_State`);
  constantes de `file000380.dat`: FIELD=8, CINEMATIC=9, PIYAGI_PROG=14.
- Stick izquierdo / D-pad → dedo sintético (id 3) sostenido sobre el joystick virtual (143,422),
  radio 60, zona muerta 28. Solo en FIELD/PIYAGI (no hay toques fantasma en menús).
- Stick derecho → arrastre continuo (id 4) desde (620,250), velocidad con curva, se re-apoya al
  alejarse 220 px del ancla.
- CRUZ → toque en el botón de acción (mano, 857,459) en exploración; en cutscene toca (480,420)
  para avanzar el diálogo. START → MENU (menú de pausa). CÍRCULO → BACK solo fuera de exploración
  (cancelar/volver en menús). Dedos reales → ids 0..2.
