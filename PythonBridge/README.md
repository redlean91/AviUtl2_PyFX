# PythonBridge — write AviUtl ExEdit2 filter effects in Python

A `.auf2` filter plugin that embeds CPython and forwards each frame to a
`.py` script you pick in the filter's settings panel.

This README is a start-to-finish walkthrough: install tools → prove the
plain-C++ build works → swap in Python → get it actually loading →
package it so it works without a system Python install.

Do the stages **in order**. Each one isolates a different way things can
go wrong, so if something breaks you'll know which layer it's in.

---

## Stage 0 — Install the tools

1. **Visual Studio 2022 Community** (free): https://visualstudio.microsoft.com/vs/
   During install, check the **"Desktop development with C++"** workload.
   Nothing else is required for this project.

2. **AviUtl ExEdit2 + Plugin SDK**: https://spring-fragrance.mints.ne.jp/aviutl/
   This is the official author's site (ＫＥＮくん). Scroll to the
   "AviUtl ExEdit2" section and download both the application and the
   "Plugin SDK" zip. Install AviUtl ExEdit2 itself normally.

3. **A full desktop Python 3.12 install** (for headers/libs/pip during
   development — you'll switch to a redistributable "embeddable" copy
   only at the very end): https://www.python.org/downloads/
   Use the 64-bit installer, and it's fine to leave "Add to PATH"
   unchecked. Note the install path, e.g.
   `C:\Users\you\AppData\Local\Programs\Python\Python312`.
   **Version note:** whatever `3.1x` you pick here has to match the
   embeddable package you bundle later and the `.lib`/`.dll` names in
   the project — pick one and stay consistent throughout.

---

## Stage 1 — Prove the plain-C++ pipeline works (no Python yet)

Skip this at your peril: if you go straight to the Python version and
it doesn't show up in AviUtl, you won't know if the problem is your
VS project setup, the SDK, or Python embedding. Isolate that first.

1. Unzip the SDK somewhere, e.g. `C:\dev\aviutl2_sdk\`. Inside you'll
   have `include\aviutl2_sdk\*.h` and `examples\*.cpp`.
2. Visual Studio → **Create a new project** → **Dynamic-Link Library (DLL)**
   (C++) → name it e.g. `PythonFilter`, location wherever you like.
3. In Solution Explorer, delete the auto-generated `dllmain.cpp`,
   `pch.h`, `pch.cpp`, `framework.h` (Project → uses precompiled
   headers by default — turn that off: **Project Properties → C/C++ →
   Precompiled Headers → Not Using Precompiled Headers**).
4. Right-click **Source Files → Add → Existing Item** → select the
   SDK's `examples\MediaFilter.cpp`.
5. **Project Properties** (set for **All Configurations**, platform **x64**):
   - **C/C++ → General → Additional Include Directories**: add
     `C:\dev\aviutl2_sdk\include\aviutl2_sdk`
   - **General → Platform**: make sure the active platform is **x64**
     (AviUtl ExEdit2 is 64-bit only; use the Configuration Manager to
     add x64 if only Win32/x86 exist).
6. Build (Release, x64). You should get `PythonFilter.dll` with no errors.
7. Copy it to `C:\ProgramData\aviutl2\Plugin\` and **rename the
   extension to `.auf2`** (so `PythonFilter.auf2`).
8. Launch AviUtl ExEdit2 → it should prompt that a new plugin was
   found → trust it. Add an object to the timeline, add the "メディア
   フィルタ(sample)" / "MediaFilter" effect to it, and move the
   brightness slider — you should see the image change live.

If this doesn't work, fix it here before moving on — nothing below
will work either otherwise.

---

## Stage 2 — Swap in the Python bridge

1. Remove `MediaFilter.cpp` from the project (right-click → Exclude
   From Project, or Remove).
2. Add the three files from this delivery to the project folder and
   to **Source Files** / **Header Files** in Solution Explorer:
   - `PythonFilter.cpp`
   - `filter2.h` (already have it from the SDK — same file)
   - `logger2.h` (already have it from the SDK — same file)
3. **Project Properties** (All Configurations, x64):
   - **C/C++ → General → Additional Include Directories**: add, in
     addition to the SDK include dir from Stage 1:
     `C:\Users\you\AppData\Local\Programs\Python\Python312\include`
   - **Linker → General → Additional Library Directories**: add
     `C:\Users\you\AppData\Local\Programs\Python\Python312\libs`
   - **Linker → Input → Additional Dependencies**: add `python312.lib`
     (match your actual version, e.g. `python311.lib`)
   - **C/C++ → Code Generation → Runtime Library**: set to
     **Multi-threaded DLL (/MD)** for Release (or **/MDd** for Debug
     — but note python.org's official Windows build only ships the
     release-mode lib/dll, so building this project in Debug config
     will fail to link/run against it unless you also build a debug
     CPython yourself; **just use Release** throughout, simplest).
   - **C/C++ → Language → C++ Language Standard**: C++17 or newer.
   - **Advanced → Character Set**: Unicode.
4. **Delay-load python3xx.dll** (important — see the big comment in
   `InitializePlugin()` in the code for *why*):
   - **Linker → Input → Delay Loaded DLLs**: add `python312.dll`
     (exact filename, matching your version).
   - **Linker → Input → Additional Dependencies**: also add
     `delayimp.lib`.
5. Build (Release, x64).

### Fast inner loop while developing

For now, point straight at your full desktop Python install instead
of a bundled copy, so you're not repackaging on every test run. Temporarily
edit this line in `InitializePlugin()`:

```cpp
std::wstring python_home = dll_dir + L"\\PythonBridge\\python";
```

to a hardcoded path to your real install, e.g.:

```cpp
std::wstring python_home = L"C:\\Users\\you\\AppData\\Local\\Programs\\Python\\Python312";
```

Build, copy `PythonFilter.dll` → `Plugin\PythonFilter.auf2` (overwrite),
**restart AviUtl ExEdit2** (it won't pick up a rebuilt DLL without a
restart), add the "Python Bridge" effect, and point **Script** at
`python_scripts\example_effect.py`. Since your full install already
has `numpy`, this should just work.

If AviUtl doesn't even list the plugin, check **DebugView**
(Sysinternals) while launching AviUtl — our logger falls back to
`OutputDebugString` before `InitializeLogger()`/`InitializePlugin()`
run, and Windows itself will also report a DLL load failure there if
`python312.dll` can't be found at all.

Once you can move the Param1 slider and see brightness change live,
the whole pipeline — C++ shim, embedded Python, script loading,
pixel round-trip — is confirmed working. Revert the hardcoded path
before moving to packaging.

---

## Stage 3 — Package a self-contained runtime (for distributing to others)

Now make it work on a machine with no Python installed at all.

1. Download the **embeddable package** for your exact version from
   python.org, e.g. `python-3.12.x-embed-amd64.zip`.
2. Extract it into `Plugin\PythonBridge\python\` next to
   `PythonFilter.auf2`:

   ```
   C:\ProgramData\aviutl2\Plugin\
     PythonFilter.auf2
     PythonBridge\
       python\
         python312.dll
         python312.zip
         python3.dll
         python312._pth
         ... (rest of the embeddable zip contents)
   ```

3. **Enable site-packages** (needed for numpy). Open
   `PythonBridge\python\python312._pth` in a text editor. By default
   it looks like:

   ```
   python312.zip
   .
   #import site
   ```

   Change it to:

   ```
   python312.zip
   .
   Lib\site-packages
   import site
   ```

   (uncomment `import site` and add the `Lib\site-packages` line).
   Without this, `import numpy` will fail even if the files are there.

4. **Install numpy into that folder.** The embeddable distribution has
   no `pip`. Easiest: use your full desktop Python's pip with
   `--target` pointed at the embeddable copy:

   ```
   C:\Users\you\AppData\Local\Programs\Python\Python312\python.exe -m pip install numpy --target "C:\ProgramData\aviutl2\Plugin\PythonBridge\python\Lib\site-packages"
   ```

   (create the `Lib\site-packages` folder if it doesn't exist yet).

5. Revert the hardcoded path from Stage 2 back to the relative one:

   ```cpp
   std::wstring python_home = dll_dir + L"\\PythonBridge\\python";
   ```

   Rebuild, restart AviUtl, retest. This time it's running entirely
   off the bundled runtime — that's the version you'd zip up and hand
   to someone else (along with `PythonFilter.auf2` and the
   `PythonBridge\` folder).

---

## Troubleshooting quick-reference

| Symptom | Likely cause |
|---|---|
| Plugin doesn't appear in AviUtl's Plugin Info list at all | Windows couldn't load the DLL — usually a missing/mismatched `python3xx.dll`. Check with DebugView, or run `dumpbin /dependents PythonFilter.dll` from a VS Developer Command Prompt to see what it's expecting. |
| Plugin appears, but the effect does nothing and no log | `python312.lib` version doesn't match the runtime you're pointing `Py_SetPythonHome` at. |
| `LNK2038` about `_ITERATOR_DEBUG_LEVEL` or `RuntimeLibrary` mismatch | You're building Debug against a Release-only CPython. Build Release. |
| Script loads (info log) but nothing visually changes | Check your script's `process()` actually mutates the numpy view in place — assigning `img = new_array` inside the function does *not* propagate back, since that rebinds a local name instead of writing through the shared buffer. |
| `import numpy` fails only in the packaged (Stage 3) build, works in Stage 2 | The `_pth` file edit or the `--target` install path is wrong — reread step 3–4 above. |

---

## Where to take it from here

- **Audio**: set `FILTER_PLUGIN_TABLE::FLAG_AUDIO` and implement
  `func_proc_audio` the same way, passing `float*` PCM buffers instead
  of `PIXEL_RGBA*`.
- **More/typed params**: instead of 4 generic sliders, you could have
  your script declare its own parameter schema in a small metadata
  file/function and have the C++ side... actually the SDK's `items[]`
  array is fixed at compile time per DLL, so truly dynamic per-script
  UI would need either (a) a fixed larger set of generic sliders with
  labels your script renames via `FILTER_ITEM_TRACK::name` at load
  time (untested — verify the host re-reads `name` after load), or
  (b) accept the fixed generic-params UI as the tradeoff for "one
  compiled plugin, many scripts."
- **Error visibility**: done. The plugin exports `InitializeLogger()`
  from `logger2.h`, so once AviUtl calls it (before `InitializePlugin`)
  everything routes through the host's own log: `info` on successful
  script (re)loads and runtime startup/shutdown, `warn` when a script
  is missing a `process()` function, `error` with a full Python
  traceback when loading a script fails or `process()` raises. Until
  the host calls `InitializeLogger()` (or if it never does), messages
  fall back to `OutputDebugString` so nothing is silently lost —
  catch those with DebugView. Log lines are truncated to ~1000 chars
  since the host caps messages at 1024.
- **Performance**: memory is copied out of AviUtl's buffer, processed,
  copied back — fine for most effects. If you need GPU-resident
  processing, `FILTER_PROC_VIDEO::get_image_texture2d()` gives you the
  `ID3D11Texture2D*` directly; that's a much bigger step (DirectX
  interop from Python is impractical, so that path stays C++-only).

## Files here

- `PythonFilter.cpp` — the shim plugin.
- `logger2.h` — reference copy of the SDK header this code targets.
- `python_scripts/example_effect.py` — example script + the contract
  your scripts must follow.
