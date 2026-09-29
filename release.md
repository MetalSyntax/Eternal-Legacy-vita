# Eternal Legacy Vita — v1.0.0 (Initial Beta Release)

This is the first public release of the native PS Vita port of **Eternal Legacy** (Gameloft), running via dynamic shared library loading (*soloader*), *FalsoJNI*, and *vitaGL*.

---

## 🎮 Highlights & What's Working

- **Full Native ARM Execution:** Runs `libEternalLegacy.so` (armeabi) directly on the Vita's ARM Cortex-A9 CPU at a smooth 30 FPS.
- **Hardware 3D Graphics:** Accelerated OpenGL ES 2.0 rendering via vitaGL with GLSL shader translation and runtime caching.
- **Audio System:** Fully functional Vox audio mixer output routed through an Android AudioTrack JNI bridge to `sceAudioOut` (44100 Hz stereo).
- **Physical Controls:** Analog stick movement, camera control, and physical button bindings alongside original touch input.
- **Filesystem Optimization:** Custom data packer (`extras/pack_data.py`) groups the ~2600 game data files into `dat/NN/` subdirectories, eliminating the massive directory traversal slowdowns on the Vita's FAT filesystem.
- **Quest & Cutscene Fixes:** Includes reconstructed `functionmacro.lua` script helpers, allowing cutscenes, NPC dialogs, and chapter progression to work correctly without soft-locking.
- **Saving & Loading:** Autosave and manual save slots verified working.

---

## ⚠️ Known Issues & Visual Bugs

Please note that this is an initial beta release. The following known issues are currently being addressed:

1. **Stretched Polygon / Cyan Beam Artifact:**
   - As documented in the test screenshots, an elongated cyan/blue polygonal bar stretches horizontally across the screen at the character's waist level during exploration, cutscenes, and menus. This is an active rendering bug related to vertex skinning, bone transforms, or weapon/shadow volume attributes in the shader pipeline.
2. **Intro FMV Playback:**
   - The opening Gameloft logo and cinematic intro video (`LOGO_Gameloft_960x640.mp4`) do not play yet. The video is currently bypassed via `Get_logoVideoState` until an `SceAvPlayer` hardware video decoding pipeline is implemented.
3. **Lighting & Bloom Overexposure:**
   - Doorways and certain lit areas exhibit overexposure or missing depth fog/occlusion.

---

## 📦 Installation Instructions

### Requirements
- PS Vita / PS TV running **HENkaku** or **Enso** (Firmware 3.60 / 3.65).
- **kubridge.skprx** and **fd_fix.skprx** installed in your taiHEN `config.txt` under `*KERNEL`.
- **libshacccg.suprx** installed in `ur0:data/libshacccg.suprx` (extractable via *ShaRKBR33D*).

### Setup
1. Obtain the legal Android game files for **Eternal Legacy v1.0.8** (`Eternal_Legacy_1.0.8.apk`) and its cache archive.
2. Extract `libEternalLegacy.so` from `lib/armeabi/libEternalLegacy.so` in the APK and copy it to `ux0:data/eternallegacy/`.
3. Run the packing script on your PC to format the asset cache:
   ```bash
   python3 extras/pack_data.py "Eternal_Legacy_1.0.8 gameloft cache.zip" ux0_data/eternallegacy
   ```
4. Transfer the contents of `ux0_data/eternallegacy/` to `ux0:data/eternallegacy/` on your PS Vita:
   - `ux0:data/eternallegacy/libEternalLegacy.so`
   - `ux0:data/eternallegacy/GloftELHP/`
   - `ux0:data/eternallegacy/dat/`
5. Install `eternallegacy.vpk` using VitaShell.

---

## 🕹️ Controls

- **Left Analog / D-Pad:** Movement / Menu Navigation
- **Right Analog:** Camera rotation
- **Cross ($\times$):** Action / Interact / Advance Dialogue
- **Circle ($\bigcirc$):** Back / Cancel (in menus)
- **Start:** Pause Menu / System Menu
- **Touch Screen:** Virtual Controls & Touch UI

---

## 🐛 Troubleshooting & Bug Reports

If you experience crashes or issues:
1. Ensure `libshacccg.suprx`, `kubridge.skprx`, and `fd_fix.skprx` are properly installed.
2. Verify that `dat/00/file000000.dat` exists in `ux0:data/eternallegacy/`.
3. Check the log files generated at `ux0:data/eternallegacy/logs/eternallegacy_*.log` and provide them when reporting issues on GitHub.

---

## 👥 Credits

- **TheFloW** for `so_loader`, `kubridge`, and `fd_fix`.
- **Rinnegatamante** for `vitaGL` and shader tools.
- **v-atamanenko** for `soloader-boilerplate`.
- **Gameloft** for developing *Eternal Legacy*.
