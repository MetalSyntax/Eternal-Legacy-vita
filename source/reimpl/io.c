/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/io.h"

#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdarg.h>
#include <pthread.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <stdio.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include "utils/logger.h"
#include "utils/utils.h"
#include "reimpl/waitprof.h"

// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "reimpl/bits/_struct_converters.c"

/*
 * Estructura de datos en la Vita (ux0:data/eternallegacy/):
 *   GloftELHP/  <- /sdcard/gameloft/games/GloftELHP/<file>  (cache del juego: file*.dat,
 *                  splash*.tga, *.ttf, LOGO_*.mp4 -- confirmado: GetSaveFilePath_GLX,
 *                  CFileSystem y los sprintf de fuentes en out_ghidra.c usan ese prefijo)
 *   saves/      <- /data/data/com.gameloft.android.TBFV.GloftELHP.ML (RMS de ALicenseCheck)
 *   res/, assets/ <- servidos por los handlers GLResLoader de java.c
 *   logs/       <- logger del loader
 *
 * El motor arma las rutas con "%s/%s" sobre un prefijo que ya termina en '/', asi que
 * llegan dobles barras ("GloftELHP//data/...") -- se colapsan todas. Tambien hace
 * chdir() (CFileSystem::changeWorkingDirectoryTo) y despues abre rutas relativas: el
 * cwd se emula aca porque SceLibc (USE_SCELIBC_IO) no conoce el cwd de newlib.
 */
#define GAME_SDCARD_PREFIX  "/sdcard/gameloft/games/GloftELHP"
#define JAVA_SDCARD_PREFIX  "/sdcard/gameloft/games/EternalLegacy"
#define GAME_DATA_PREFIX    "/data/data/com.gameloft.android.TBFV.GloftELHP.ML"
#define GAME_DIR            DATA_PATH "GloftELHP"
#define SAVES_DIR           DATA_PATH "saves"

/*
 * Layout "dat/" (Bug #007): el driver de la Vita recorre linealmente la carpeta en cada
 * open -- con los ~2600 file*.dat en GloftELHP/, fopen tardaba 13 ms + 0.065 ms por
 * posicion en la carpeta (175 ms para file0025xx, log 011) y el menu abre ~1800 voces.
 * Si existe dat/00/file000000.dat, los fileNNNNNN.dat se leen de dat/<NNNNNN/64>/ (41
 * carpetas de 64 archivos). Si no, se sigue usando GloftELHP/ plano.
 */
#define DAT_DIR             DATA_PATH "dat"
#define DAT_BUCKET_FILES    64

static int s_dat_layout = 0;

static char s_cwd[256] = GAME_DIR;
// El motor hace chdir() y abre rutas relativas desde varios hilos (carga + audio):
// todo acceso a s_cwd y al buffer estatico de readdir va bajo este mutex.
static pthread_mutex_t s_io_mutex = PTHREAD_MUTEX_INITIALIZER;

#ifdef USE_SCELIBC_IO
#define RAW_IO(fn) sceLibcBridge_##fn
#else
#define RAW_IO(fn) fn
#endif

// Trazador del FILE* de un archivo concreto (Bug #005/#006 usaba "file002246.dat" =
// defaulteffects.bdae). Se arma en fopen y los wrappers de f* loguean+flushean cada
// operacion sobre ese fp. Vacio = apagado.
#define TRACE_NAME ""
static FILE *s_trace_fp = NULL;

/*
 * Rastreador de tamaño/posicion de los FILE* de solo lectura (Bug #006).
 *
 * glitch::io::CReadFile::openFile() mide el archivo con fseek(fp, 0, SEEK_END) + ftell(),
 * y CReadFile::read() desofusca los 4 primeros bytes de los file*.dat (-1,-2,-3,-4) solo
 * si ese tamaño es >= 4 y ftell() da una posicion < 4. En el hilo de carga (pthread) la
 * pila SceLibc/FIOS devolvio tamaño 0 (dump 1790309266: CReadFile+0x10 == 0 y el flag
 * +0x2c borrado, aunque CFile+0x24 == 1 desde el mapa de file000000.dat). El motor leyo
 * crudo el ZIP ofuscado de defaulteffects.bdae ("QM\x06\x08"), CZipReader::isValid fallo,
 * CResFileManager apago su deteccion de ZIP (+0x2a) para siempre y File::Init parseo el
 * ZIP como BRES -> new[] gigantes -> NULL -> data abort.
 *
 * Para no depender de eso: el tamaño sale de sceIoGetstat en el fopen, SEEK_END se
 * traduce a un SEEK_SET absoluto y la posicion se lleva aca (fread/fgetc/getc/ungetc/
 * fseek). ftell devuelve la posicion rastreada; si SceLibc discrepa se loguea una vez
 * por archivo (asi el log dice si el roto era fseek o ftell).
 */
#define MAX_TRACKED_FILES 64

typedef struct {
    FILE * fp;
    long size;
    long pos;
    int pos_valid; // 0 tras una operacion que no se puede seguir (fscanf)
    int warned;
} tracked_file_t;

static tracked_file_t s_tracked[MAX_TRACKED_FILES];
static pthread_mutex_t s_track_mutex = PTHREAD_MUTEX_INITIALIZER;

// Las funciones track_* asumen s_track_mutex tomado.
static tracked_file_t * track_find(FILE * fp) {
    for (int i = 0; i < MAX_TRACKED_FILES; i++) {
        if (s_tracked[i].fp == fp) return &s_tracked[i];
    }
    return NULL;
}

static int track_add(FILE * fp, long size) {
    tracked_file_t * t = track_find(NULL);
    if (!t) return 0;
    t->fp = fp;
    t->size = size;
    t->pos = 0;
    t->pos_valid = 1;
    t->warned = 0;
    return 1;
}

/*
 * Tamaños de GloftELHP/ (datos del juego, solo lectura). Un unico escaneo del directorio
 * reemplaza el sceIoGetstat por fopen: cada lookup recorre una carpeta de ~2600 entradas
 * y el motor abre ~1800 voces seguidas en el menu (Bug #007).
 */
typedef struct {
    char name[28];
    long size;
} dir_size_t;

static dir_size_t * s_sizes = NULL;
static int s_sizes_n = 0;
static int s_sizes_state = 0; // 0 = sin escanear, 1 = ok, -1 = fallo

static int dir_size_cmp(const void * a, const void * b) {
    // sin distinguir mayusculas, como el sistema de archivos de la Vita
    return strcasecmp(((const dir_size_t *) a)->name, ((const dir_size_t *) b)->name);
}

// Agrega los archivos (no carpetas) de `dir` al cache. Asume s_track_mutex tomado.
static int s_sizes_cap = 0;

static int s_game_dir_complete = 1; // 0 si GloftELHP/ tiene subcarpetas o nombres largos

static void size_cache_add_dir(const char * dir) {
    SceUID d = sceIoDopen(dir);
    if (d < 0) {
        s_game_dir_complete = 0;
        return;
    }

    SceIoDirent ent;
    while (sceIoDread(d, &ent) > 0) {
        if (SCE_S_ISDIR(ent.d_stat.st_mode) || strlen(ent.d_name) >= sizeof(s_sizes[0].name)) {
            if (strcmp(ent.d_name, ".") && strcmp(ent.d_name, "..")) s_game_dir_complete = 0;
            continue;
        }
        if (s_sizes_n == s_sizes_cap) {
            int cap = s_sizes_cap ? s_sizes_cap * 2 : 1024;
            dir_size_t * n = realloc(s_sizes, cap * sizeof(dir_size_t));
            if (!n) break;
            s_sizes = n;
            s_sizes_cap = cap;
        }
        strcpy(s_sizes[s_sizes_n].name, ent.d_name);
        s_sizes[s_sizes_n].size = (long) ent.d_stat.st_size;
        s_sizes_n++;
    }
    sceIoDclose(d);
}

// Asume s_track_mutex tomado.
static void game_dir_scan(void) {
    size_cache_add_dir(GAME_DIR);
    if (s_dat_layout) {
        SceUID d = sceIoDopen(DAT_DIR);
        if (d >= 0) {
            SceIoDirent ent;
            char sub[128];
            while (sceIoDread(d, &ent) > 0) {
                if (!SCE_S_ISDIR(ent.d_stat.st_mode)) continue;
                snprintf(sub, sizeof(sub), DAT_DIR "/%s", ent.d_name);
                size_cache_add_dir(sub);
            }
            sceIoDclose(d);
        }
    }
    qsort(s_sizes, s_sizes_n, sizeof(dir_size_t), dir_size_cmp);
    s_sizes_state = s_sizes_n ? 1 : -1;
}

/*
 * Busca un archivo de datos del juego (GloftELHP/<nombre> o dat/NN/<nombre>) en el cache.
 * Devuelve 1 (existe, *size), 0 (seguro que NO existe: la carpeta esta completa en el
 * cache) o -1 (no es ruta de datos o no se puede afirmar nada). Asume s_track_mutex.
 *
 * El 0 importa (Bug #007, log 012): el motor pide sin parar el recurso "npt" (nombre
 * placeholder de efectos en CBuff) y cada miss en una carpeta de exFAT recorre TODAS sus
 * entradas, incluidas las borradas (~170 ms en GloftELHP/). Con el cache no toca el disco.
 */
static int game_dir_lookup(const char * path, long * size) {
    const char * name;
    int in_game_dir;
    if (strncmp(path, GAME_DIR "/", sizeof(GAME_DIR)) == 0) {
        name = path + sizeof(GAME_DIR);
        in_game_dir = 1;
    } else if (strncmp(path, DAT_DIR "/", sizeof(DAT_DIR)) == 0) {
        name = strrchr(path, '/') + 1;
        if (name - path != (long) sizeof(DAT_DIR) + 3) return -1; // dat/NN/<nombre>
        in_game_dir = 0;
    } else {
        return -1;
    }

    if (s_sizes_state == 0) game_dir_scan();
    if (s_sizes_state != 1) return -1;

    if (strchr(name, '/'))  // GloftELHP/<sub>/...: solo es seguro si no hay subcarpetas
        return (in_game_dir && s_game_dir_complete) ? 0 : -1;
    if (strlen(name) >= sizeof(s_sizes[0].name))
        return -1;

    dir_size_t key;
    strcpy(key.name, name);
    dir_size_t * hit = bsearch(&key, s_sizes, s_sizes_n, sizeof(dir_size_t), dir_size_cmp);
    if (hit) {
        if (size) *size = hit->size;
        return 1;
    }
    return (in_game_dir && !s_game_dir_complete) ? -1 : 0;
}

static int game_dir_lookup_locked(const char * path, long * size) {
    pthread_mutex_lock(&s_track_mutex);
    int r = game_dir_lookup(path, size);
    pthread_mutex_unlock(&s_track_mutex);
    return r;
}

// Un warning por ruta faltante (el motor reintenta la misma ruta cientos de veces).
#define MISS_WARN_SLOTS 512
static uint32_t s_miss_warned[MISS_WARN_SLOTS];
static int s_miss_warned_n = 0;

static int miss_should_warn(const char * path) {
    uint32_t h = 2166136261u;
    for (const char * p = path; *p; p++) h = (h ^ (uint8_t) *p) * 16777619u;
    int warn = 0;
    pthread_mutex_lock(&s_track_mutex);
    int i;
    for (i = 0; i < s_miss_warned_n; i++) {
        if (s_miss_warned[i] == h) break;
    }
    if (i == s_miss_warned_n && s_miss_warned_n < MISS_WARN_SLOTS) {
        s_miss_warned[s_miss_warned_n++] = h;
        warn = 1;
    }
    pthread_mutex_unlock(&s_track_mutex);
    return warn;
}

void io_init_layout(void) {
    SceIoStat st;
    s_dat_layout = sceIoGetstat(DAT_DIR "/00/file000000.dat", &st) >= 0;
    if (s_dat_layout)
        l_info("io: datos en " DAT_DIR "/NN/ (%d por carpeta)", DAT_BUCKET_FILES);
    else
        l_warn("io: layout plano en " GAME_DIR "/ -- cada fopen recorre ~2600 entradas "
               "(lento); ver extras/pack_data.py");
}

int io_game_data_present(void) {
    SceIoStat st;
    return sceIoGetstat(DAT_DIR "/00/file000000.dat", &st) >= 0
        || sceIoGetstat(GAME_DIR "/file000000.dat", &st) >= 0;
}

// GloftELHP/fileNNNNNN.dat -> dat/<NNNNNN/64>/fileNNNNNN.dat (reescribe `out` in situ).
static void remap_to_dat_layout(char * out, size_t out_size) {
    const char * name = out + sizeof(GAME_DIR);
    if (!s_dat_layout || strncmp(out, GAME_DIR "/file", sizeof(GAME_DIR) + 4) != 0) return;
    for (int i = 4; i < 10; i++) {
        if (name[i] < '0' || name[i] > '9') return;
    }
    if (strcmp(name + 10, ".dat") != 0) return;

    unsigned n = (unsigned) atoi(name + 4);
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%s", name);
    snprintf(out, out_size, DAT_DIR "/%02u/%s", n / DAT_BUCKET_FILES, tmp);
}

static int is_read_only_mode(const char * mode) {
    return strchr(mode, 'r') && !strchr(mode, '+') && !strchr(mode, 'w') && !strchr(mode, 'a');
}

// Avanza (o retrocede) la posicion rastreada de fp, si la hay.
static void track_advance(FILE * fp, long delta) {
    pthread_mutex_lock(&s_track_mutex);
    tracked_file_t * t = track_find(fp);
    if (t && t->pos_valid) {
        t->pos += delta;
        if (t->pos < 0) t->pos = 0;
    }
    pthread_mutex_unlock(&s_track_mutex);
}

// Copia `rest` a `out` detras de `base` + '/', colapsando barras repetidas.
static void join_collapsed(char * out, size_t out_size, const char * base, const char * rest) {
    size_t o = snprintf(out, out_size, "%s", base);
    if (o >= out_size) return;
    while (*rest == '/') rest++;
    if (*rest && o + 1 < out_size) out[o++] = '/';
    char prev = '/';
    for (; *rest && o + 1 < out_size; rest++) {
        if (*rest == '/' && prev == '/') continue;
        // segmento "./" (o "." final): el motor arma "GloftELHP/./data/..."
        if (*rest == '.' && prev == '/' && (rest[1] == '/' || rest[1] == '\0')) continue;
        out[o++] = prev = *rest;
    }
    // sin barra final (sceIo acepta "dir/" en algunos calls y en otros no)
    while (o > 0 && out[o - 1] == '/') o--;
    out[o] = '\0';
}

static int match_prefix(const char * path, const char * prefix, const char ** rest) {
    size_t len = strlen(prefix);
    if (strncmp(path, prefix, len) != 0) return 0;
    if (path[len] != '/' && path[len] != '\0') return 0;
    *rest = path + len;
    return 1;
}

// Devuelve la ruta real en la Vita para una ruta del motor Android (siempre escribe `out`).
const char * remap_android_path(const char * path, char * out, size_t out_size) {
    const char * rest;

    if (!path) return path;

    if (strcmp(path, "/proc/cpuinfo") == 0) {
        snprintf(out, out_size, "app0:/cpuinfo");
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        snprintf(out, out_size, "app0:/meminfo");
    } else if (match_prefix(path, GAME_SDCARD_PREFIX, &rest)
            || match_prefix(path, JAVA_SDCARD_PREFIX, &rest)) {
        join_collapsed(out, out_size, GAME_DIR, rest);
    } else if (match_prefix(path, GAME_DATA_PREFIX, &rest)) {
        join_collapsed(out, out_size, SAVES_DIR, rest);
    } else if (strncmp(path, "/sdcard/", 8) == 0) {
        join_collapsed(out, out_size, DATA_PATH "sdcard", path + 8);
    } else if (path[0] != '/' && !strchr(path, ':')) {
        // relativa: al cwd emulado (strip de "./")
        char cwd[256];
        pthread_mutex_lock(&s_io_mutex);
        snprintf(cwd, sizeof(cwd), "%s", s_cwd);
        pthread_mutex_unlock(&s_io_mutex);
        while (path[0] == '.' && path[1] == '/') path += 2;
        join_collapsed(out, out_size, cwd, path);
    } else {
        snprintf(out, out_size, "%s", path);
    }
    remap_to_dat_layout(out, out_size);
    return out;
}

int chdir_soloader(const char * path) {
    char real[256];
    remap_android_path(path, real, sizeof(real));

    SceIoStat st;
    int ret = sceIoGetstat(real, &st);
    if (ret < 0 || !SCE_S_ISDIR(st.st_mode)) {
        l_warn("chdir(%s -> %s): no existe (0x%x)", path, real, ret);
        return -1;
    }
    pthread_mutex_lock(&s_io_mutex);
    snprintf(s_cwd, sizeof(s_cwd), "%s", real);
    pthread_mutex_unlock(&s_io_mutex);
    l_info("chdir(%s -> %s)", path, real);
    return 0;
}

int access_soloader(const char * path, int mode) {
    char real[256];
    remap_android_path(path, real, sizeof(real));
    int known = game_dir_lookup_locked(real, NULL);
    int ret;
    if (known >= 0) {
        ret = known ? 0 : -1;
    } else {
        SceIoStat st;
        ret = sceIoGetstat(real, &st) < 0 ? -1 : 0;
    }
    l_debug("access(%s -> %s, %d): %d", path, real, mode, ret);
    return ret;
}

FILE * fopen_soloader(const char * _filename, const char * mode) {
    char real[256];
    const char * filename = remap_android_path(_filename, real, sizeof(real));
    int read_only = mode && is_read_only_mode(mode);

    long size = -1;
    int known = read_only ? game_dir_lookup_locked(filename, &size) : -1;

    FILE* ret = NULL;
    if (known == 0) {
        errno = ENOENT; // no existe segun el cache: sin tocar el disco
    } else {
        WAITPROF(WP_FOPEN, ret = RAW_IO(fopen)(filename, mode));
    }

    if (ret) {
        l_debug("fopen(%s, %s): %p", filename, mode, ret);
    } else if (miss_should_warn(filename)) {
        l_warn("fopen(%s -> %s, %s): NULL%s", _filename, filename, mode,
               known == 0 ? " (cache, se avisa una sola vez)" : "");
    }

    if (ret && read_only) {
        if (known != 1) {
            SceIoStat st;
            int r;
            WAITPROF(WP_GETSTAT, r = sceIoGetstat(filename, &st));
            if (r >= 0 && !SCE_S_ISDIR(st.st_mode))
                size = (long) st.st_size;
        }
        if (size >= 0) {
            pthread_mutex_lock(&s_track_mutex);
            int ok = track_add(ret, size);
            pthread_mutex_unlock(&s_track_mutex);
            if (!ok)
                l_warn("fopen(%s): tabla de rastreo llena, ftell/SEEK_END van directo a libc", filename);
        }
    }

    if (ret && TRACE_NAME[0] && strstr(filename, TRACE_NAME)) {
        s_trace_fp = ret;
        l_error("trace: %s -> fp %p (se trazan sus fseek/ftell/fread)", filename, ret);
        log_flush();
    }

    // Sin log_flush por archivo (Bug #007: el menu abre ~1800 voces seguidas). El
    // logger vuelca solo con el buffer lleno, en errores y desde el main loop.
    return ret;
}

int open_soloader(const char * _path, int oflag, ...) {
    char real[256];
    const char * path = remap_android_path(_path, real, sizeof(real));

    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    oflag = oflags_bionic_to_newlib(oflag);
    int ret = open(path, oflag, mode);
    if (ret >= 0)
        l_debug("open(%s, %x): %i", path, oflag, ret);
    else
        l_warn("open(%s, %x): %i", path, oflag, ret);
    return ret;
}

int fstat_soloader(int fd, stat64_bionic * buf) {
    struct stat st;
    int res = fstat(fd, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("fstat(%i): %i", fd, res);
    return res;
}

int stat_soloader(const char * _path, stat64_bionic * buf) {
    char real[256];
    const char * path = remap_android_path(_path, real, sizeof(real));
    if (game_dir_lookup_locked(path, NULL) == 0) {
        errno = ENOENT;
        return -1;
    }
    struct stat st;
    int res = stat(path, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("stat(%s): %i", path, res);
    return res;
}

int fclose_soloader(FILE * f) {
    if (f == s_trace_fp) {
        l_error("trace: fclose(fp %p)", f);
        log_flush();
        s_trace_fp = NULL;
    }

    pthread_mutex_lock(&s_track_mutex);
    tracked_file_t * t = track_find(f);
    if (t) t->fp = NULL;
    pthread_mutex_unlock(&s_track_mutex);

    int ret;
    WAITPROF(WP_FCLOSE, ret = RAW_IO(fclose)(f));
    return ret;
}

size_t fread_soloader(void *ptr, size_t size, size_t count, FILE *stream) {
    size_t ret;
    WAITPROF(WP_FREAD, ret = RAW_IO(fread)(ptr, size, count, stream));

    pthread_mutex_lock(&s_track_mutex);
    tracked_file_t * t = track_find(stream);
    if (t && t->pos_valid) {
        // Un fread corto sobre un archivo regular es EOF: stdio ya consumio el resto
        // (con size > 1 puede haber quedado un elemento parcial que ret no cuenta).
        if (ret < count && size > 1 && t->pos < t->size)
            t->pos = t->size;
        else
            t->pos += (long) (ret * size);
    }
    pthread_mutex_unlock(&s_track_mutex);

    if (stream == s_trace_fp) {
        l_error("trace: fread(%p, %u, %u, fp %p) -> %u", ptr, (unsigned) size,
                (unsigned) count, stream, (unsigned) ret);
        log_flush();
    }
    return ret;
}

int fseek_soloader(FILE *stream, long offset, int origin) {
    int tracked = 0;
    long target = -1;

    pthread_mutex_lock(&s_track_mutex);
    tracked_file_t * t = track_find(stream);
    if (t) {
        if (origin == SEEK_SET) {
            target = offset;
            tracked = 1;
        } else if (origin == SEEK_CUR && t->pos_valid) {
            target = t->pos + offset;
            tracked = 1;
        } else if (origin == SEEK_END) {
            target = t->size + offset;
            tracked = 1;
        }
    }
    pthread_mutex_unlock(&s_track_mutex);

    int ret;
    if (tracked && target < 0) {
        errno = EINVAL;
        ret = -1;
    } else if (tracked) {
        // Siempre absoluto: SEEK_END/SEEK_CUR no pasan nunca por SceLibc.
        ret = RAW_IO(fseek)(stream, target, SEEK_SET);
    } else {
        ret = RAW_IO(fseek)(stream, offset, origin);
    }

    pthread_mutex_lock(&s_track_mutex);
    t = track_find(stream);
    if (t && ret == 0) {
        if (tracked) {
            t->pos = target;
            t->pos_valid = 1;
        } else {
            t->pos_valid = 0;
        }
    }
    pthread_mutex_unlock(&s_track_mutex);

    if (stream == s_trace_fp) {
        l_error("trace: fseek(fp %p, %ld, %d) -> %d (abs %ld)", stream, offset, origin, ret, target);
        log_flush();
    }
    return ret;
}

long ftell_soloader(FILE *stream) {
    long real = RAW_IO(ftell)(stream);
    long ret = real;
    int warn = 0;
    long size = 0;

    pthread_mutex_lock(&s_track_mutex);
    tracked_file_t * t = track_find(stream);
    if (t) {
        if (!t->pos_valid) {
            if (real >= 0) {
                t->pos = real;
                t->pos_valid = 1;
            }
        } else if (real != t->pos) {
            ret = t->pos;
            size = t->size;
            if (!t->warned) {
                t->warned = 1;
                warn = 1;
            }
        }
    }
    pthread_mutex_unlock(&s_track_mutex);

    if (warn) {
        l_warn("ftell(fp %p): libc=%ld, rastreado=%ld (size %ld) -- se usa el rastreado",
               stream, real, ret, size);
        log_flush();
    }
    if (stream == s_trace_fp) {
        l_error("trace: ftell(fp %p) -> %ld (libc %ld)", stream, ret, real);
        log_flush();
    }
    return ret;
}

int fgetc_soloader(FILE *stream) {
    int ret = RAW_IO(fgetc)(stream);
    if (ret != EOF) track_advance(stream, 1);
    return ret;
}

int getc_soloader(FILE *stream) {
    int ret = RAW_IO(getc)(stream);
    if (ret != EOF) track_advance(stream, 1);
    return ret;
}

int ungetc_soloader(int c, FILE *stream) {
    int ret = RAW_IO(ungetc)(c, stream);
    if (ret != EOF) track_advance(stream, -1);
    return ret;
}

int fscanf_soloader(FILE *stream, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int ret = RAW_IO(vfscanf)(stream, format, args);
    va_end(args);

    // Cuantos bytes consumio no se puede saber: el proximo ftell resincroniza.
    pthread_mutex_lock(&s_track_mutex);
    tracked_file_t * t = track_find(stream);
    if (t) t->pos_valid = 0;
    pthread_mutex_unlock(&s_track_mutex);
    return ret;
}

// Bionic: fpos_t == off_t (long de 32 bits). El fpos_t de SceLibc es una struct mas
// grande: pasarle el puntero del .so directo pisaria el stack del llamador.
int fgetpos_soloader(FILE *stream, long *pos) {
    long p = ftell_soloader(stream);
    if (p < 0) return -1;
    *pos = p;
    return 0;
}

int fsetpos_soloader(FILE *stream, const long *pos) {
    return fseek_soloader(stream, *pos, SEEK_SET);
}

int close_soloader(int fd) {
    int ret = close(fd);
    l_debug("close(%i): %i", fd, ret);
    return ret;
}

DIR* opendir_soloader(char* _pathname) {
    char real[256];
    const char * pathname = remap_android_path(_pathname, real, sizeof(real));
    DIR* ret = opendir(pathname);
    l_debug("opendir(\"%s\" -> \"%s\"): %p", _pathname, pathname, ret);
    return ret;
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    static struct dirent64_bionic dirent_tmp;

    pthread_mutex_lock(&s_io_mutex);
    struct dirent* ret = readdir(dir);
    l_debug("readdir(%p): %p", dir, ret);

    struct dirent64_bionic * out = NULL;
    if (ret) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(ret);
        memcpy(&dirent_tmp, entry_tmp, sizeof(dirent64_bionic));
        free(entry_tmp);
        out = &dirent_tmp;
    }
    pthread_mutex_unlock(&s_io_mutex);

    return out;
}

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result) {
    struct dirent dirent_tmp;
    struct dirent * pdirent_tmp;

    int ret = readdir_r(dirp, &dirent_tmp, &pdirent_tmp);

    if (ret == 0) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(&dirent_tmp);
        memcpy(entry, entry_tmp, sizeof(dirent64_bionic));
        *result = (pdirent_tmp != NULL) ? entry : NULL;
        free(entry_tmp);
    }

    l_debug("readdir_r(%p, %p, %p): %i", dirp, entry, result, ret);
    return ret;
}

int closedir_soloader(DIR * dir) {
    int ret = closedir(dir);
    l_debug("closedir(%p): %i", dir, ret);
    return ret;
}

int fcntl_soloader(int fd, int cmd, ...) {
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

int fsync_soloader(int fd) {
    int ret = fsync(fd);
    l_debug("fsync(%i): %i", fd, ret);
    return ret;
}
