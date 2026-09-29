#!/usr/bin/env python3
"""
Prepara los datos de Eternal Legacy para la Vita desde el cache de Android.

    python3 extras/pack_data.py "Eternal_Legacy_1.0.8 gameloft cache.zip" ux0_data/eternallegacy

Genera, dentro de <destino>:
    GloftELHP/   todo lo que NO es fileNNNNNN.dat (splash*.tga, *.ttf, LOGO_*.mp4, ...)
    dat/NN/      los fileNNNNNN.dat, 64 por carpeta (NN = NNNNNN // 64)

Los archivos se copian TAL CUAL (sin "desofuscar" ni extraer nada): el motor ya revierte
la ofuscacion de los 4 primeros bytes y abre los ZIP multi-variante por su cuenta.

Por que las carpetas: el driver de la Vita recorre la carpeta linealmente en cada open.
Con los ~2600 .dat juntos, cada fopen tardaba hasta 175 ms (Bug #007, port_progress.md).
El loader usa dat/ si existe dat/00/file000000.dat; si no, cae al GloftELHP/ plano.
"""
import os
import re
import sys
import zipfile

PREFIX = "gameloft/games/GloftELHP/"
BUCKET_FILES = 64
DAT_RE = re.compile(r"^file(\d{6})\.dat$")
HERE = os.path.dirname(os.path.abspath(__file__))

# .dat que en el cache disponible vienen con 0 bytes (descarga incompleta, Bug #008) y
# con que se reemplazan. Solo se aplican si el del zip esta vacio.
#   ("file", ruta)      archivo del repo (texto plano; se ofusca como el resto)
#   ("zip", entrada)    otro archivo del mismo zip, sin ofuscar (se ofusca)
#   ("dat", entrada)    otro .dat del zip, ya ofuscado (se copia tal cual)
DATA_FIXES = {
    # data/script/functionmacro.lua: helpers de cutscene (StartCutscene, AddNPC,
    # SetDialogAndWait...). Sin el ninguna quest con cutscene avanza (arena, cap. 0).
    "file000923.dat": ("file", os.path.join(HERE, "data_fixes", "functionmacro.lua")),
    # data/2d/loading4.tga: identico a GloftELHP/loading4.tga (verificado con 1,2,3,5).
    "file000654.dat": ("zip", PREFIX + "loading4.tga"),
    # data/sound/soundpack/sfx_reinherz_attack.wav: sustituto = sfx_skill_reinherz_ghost_attack.wav
    "file002173.dat": ("dat", PREFIX + "file001307.dat"),
    # file000703.dat (data/script/levelscript/cave.lua) sigue vacio: sin fuente.
}


def obfuscate(data):
    """Ofuscacion de Gameloft que el motor revierte en CReadFile::read: +1..+4 en los
    primeros 4 bytes (todas las entradas del mapa file000000.dat estan marcadas)."""
    data = bytearray(data)
    for i in range(min(4, len(data))):
        data[i] = (data[i] + i + 1) & 0xFF
    return bytes(data)


def fixed_content(z, name):
    kind, src = DATA_FIXES[name]
    if kind == "file":
        with open(src, "rb") as f:
            return obfuscate(f.read())
    if kind == "zip":
        return obfuscate(z.read(src))
    return z.read(src)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src_zip, dest = sys.argv[1], sys.argv[2]

    n_dat = n_other = n_fixed = 0
    with zipfile.ZipFile(src_zip) as z:
        for info in z.infolist():
            if info.is_dir() or not info.filename.startswith(PREFIX):
                continue
            name = info.filename[len(PREFIX):]
            if "/" in name or name in ("", ".nomedia", "Thumbs.db"):
                continue

            m = DAT_RE.match(name)
            if m:
                out = os.path.join(dest, "dat", "%02d" % (int(m.group(1)) // BUCKET_FILES), name)
                n_dat += 1
            else:
                out = os.path.join(dest, "GloftELHP", name)
                n_other += 1

            os.makedirs(os.path.dirname(out), exist_ok=True)
            if info.file_size == 0 and name in DATA_FIXES:
                data = fixed_content(z, name)
                with open(out, "wb") as fo:
                    fo.write(data)
                n_fixed += 1
                continue
            if os.path.exists(out) and os.path.getsize(out) == info.file_size:
                continue
            with z.open(info) as fi, open(out, "wb") as fo:
                while True:
                    buf = fi.read(1 << 20)
                    if not buf:
                        break
                    fo.write(buf)

    print("%d .dat en %s/dat/NN/, %d archivos en %s/GloftELHP/, %d reparados (DATA_FIXES)"
          % (n_dat, dest, n_other, dest, n_fixed))


if __name__ == "__main__":
    main()
