//----------------------------------------------------------------------------------
//  PythonFilter.cpp
//  A .auf2 filter-effect plugin for AviUtl ExEdit2 that embeds CPython and
//  forwards each frame to a user-supplied .py script's process() function.
//
//  Build target: 64-bit DLL, renamed to PythonFilter.auf2 after building.
//
//  IMPORTANT: this file talks to the *real* FILTER_PLUGIN_TABLE / FILTER_PROC_VIDEO
//  structs pulled from the official aviutl2_sdk_mirror (filter2.h). Do not guess
//  at struct layout if the SDK updates it later -- re-pull the header and diff it.
//----------------------------------------------------------------------------------
#include <windows.h>
#include <Python.h>

#include <string>
#include <vector>
#include <memory>

#include "filter2.h"
#include "logger2.h"

//---------------------------------------------------------------------
//  Forward decls
//---------------------------------------------------------------------
bool func_proc_video(FILTER_PROC_VIDEO* video);

//---------------------------------------------------------------------
//  Filter settings UI (these are the ONLY controls AviUtl shows for this
//  plugin, because a single DLL registers exactly one FILTER_PLUGIN_TABLE
//  with a FIXED item count -- that's baked in at compile time and cannot
//  be changed by a script at runtime, no matter what.
//
//  What CAN be script-driven: which of these slots are used, and what
//  they're LABELED (see PARAM_NAMES convention below and ApplyParamNames()).
//  What CANNOT: the total slot count (MAX_PARAMS, below -- bump it and
//  recompile if you want more) or each slot's min/max/step, because
//  FILTER_ITEM_TRACK declares those as `const double` in the SDK header --
//  structurally frozen at construction time, not just a convention.
//---------------------------------------------------------------------
constexpr int MAX_PARAMS = 16;

auto script_path = FILTER_ITEM_FILE(L"Script", L"", L"Python Script (*.py)\0*.py\0\0");
auto hot_reload = FILTER_ITEM_CHECK(L"Reload script every frame (dev mode)", false);

FILTER_ITEM_TRACK param_tracks[MAX_PARAMS] = {
    FILTER_ITEM_TRACK(L"(unused)",  0.0, -1000.0, 1000.0, 0.01),
    FILTER_ITEM_TRACK(L"(unused)",  0.0, -1000.0, 1000.0, 0.01),
    FILTER_ITEM_TRACK(L"(unused)",  0.0, -1000.0, 1000.0, 0.01),
    FILTER_ITEM_TRACK(L"(unused)",  0.0, -1000.0, 1000.0, 0.01),
    FILTER_ITEM_TRACK(L"(unused)",  0.0, -1000.0, 1000.0, 0.01),
    FILTER_ITEM_TRACK(L"(unused)",  0.0, -1000.0, 1000.0, 0.01),
    FILTER_ITEM_TRACK(L"(unused)",  0.0, -1000.0, 1000.0, 0.01),
    FILTER_ITEM_TRACK(L"(unused)",  0.0, -1000.0, 1000.0, 0.01),
    FILTER_ITEM_TRACK(L"(unused)",  0.0, -1000.0, 1000.0, 0.01),
    FILTER_ITEM_TRACK(L"(unused)", 0.0, -1000.0, 1000.0, 0.01),
    FILTER_ITEM_TRACK(L"(unused)", 0.0, -1000.0, 1000.0, 0.01),
    FILTER_ITEM_TRACK(L"(unused)", 0.0, -1000.0, 1000.0, 0.01),
    FILTER_ITEM_TRACK(L"(unused)", 0.0, -1000.0, 1000.0, 0.01),
    FILTER_ITEM_TRACK(L"(unused)", 0.0, -1000.0, 1000.0, 0.01),
    FILTER_ITEM_TRACK(L"(unused)", 0.0, -1000.0, 1000.0, 0.01),
    FILTER_ITEM_TRACK(L"(unused)", 0.0, -1000.0, 1000.0, 0.01),
};

// Owns the actual wide-char storage backing param_tracks[i].name after a
// script relabels it -- FILTER_ITEM_TRACK::name is just a raw LPCWSTR, so
// something has to keep the characters alive for as long as AviUtl might
// read them. Re-point .name at this array's c_str() every time a slot's
// label changes; never point it at a temporary.
std::wstring g_param_name_storage[MAX_PARAMS];

void* items[] = {
    &script_path, &hot_reload,
    &param_tracks[0],  &param_tracks[1],  &param_tracks[2],  &param_tracks[3],
    &param_tracks[4],  &param_tracks[5],  &param_tracks[6],  &param_tracks[7],
    &param_tracks[8],  &param_tracks[9],  &param_tracks[10], &param_tracks[11],
    &param_tracks[12], &param_tracks[13], &param_tracks[14], &param_tracks[15],
    nullptr
};

//---------------------------------------------------------------------
//  Plugin table
//---------------------------------------------------------------------
FILTER_PLUGIN_TABLE filter_plugin_table = {
    FILTER_PLUGIN_TABLE::FLAG_VIDEO,          // video only for now; add FLAG_AUDIO later
    L"Python Bridge",
    L"PyFX",
    L"Run a Python script as a filter effect. version 0.1",
    items,
    func_proc_video,
    nullptr,   // func_proc_audio
    nullptr,   // func_create
    nullptr    // func_destroy
};

//---------------------------------------------------------------------
//  Python runtime state
//---------------------------------------------------------------------
static PyThreadState* g_main_thread_state = nullptr;
static PyObject* g_loaded_module = nullptr;
static std::wstring g_loaded_path;
static HMODULE g_this_module = nullptr;
static LOG_HANDLE* g_logger = nullptr;   // set by InitializeLogger(), before InitializePlugin()

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), size, nullptr, nullptr);
    return out;
}

static std::wstring Utf8ToWide(const char* s) {
    if (!s || !*s) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(size - 1, 0);   // size includes the null terminator
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), size);
    return out;
}

//---------------------------------------------------------------------
//  Logging helpers -- go through logger2.h when available (visible in
//  AviUtl's own log window), otherwise fall back to OutputDebugString
//  (visible via DebugView) so nothing is silently lost before the
//  logger has been wired up.
//
//  Log messages are capped at 1024 chars by the host, so truncate.
//---------------------------------------------------------------------
static std::wstring TruncateForLog(std::wstring msg) {
    constexpr size_t kMax = 1000; // leave headroom under the 1024 limit
    if (msg.size() > kMax) msg.resize(kMax);
    return msg;
}

static void LogInfo(const std::wstring& msg) {
    auto m = TruncateForLog(L"[PythonFilter] " + msg);
    if (g_logger && g_logger->info) g_logger->info(g_logger, m.c_str());
    else OutputDebugStringW((m + L"\n").c_str());
}

static void LogWarn(const std::wstring& msg) {
    auto m = TruncateForLog(L"[PythonFilter] " + msg);
    if (g_logger && g_logger->warn) g_logger->warn(g_logger, m.c_str());
    else OutputDebugStringW((m + L"\n").c_str());
}

static void LogError(const std::wstring& msg) {
    auto m = TruncateForLog(L"[PythonFilter] " + msg);
    if (g_logger && g_logger->error) g_logger->error(g_logger, m.c_str());
    else OutputDebugStringW((m + L"\n").c_str());
}

static void LogVerbose(const std::wstring& msg) {
    auto m = TruncateForLog(L"[PythonFilter] " + msg);
    if (g_logger && g_logger->verbose) g_logger->verbose(g_logger, m.c_str());
    else OutputDebugStringW((m + L"\n").c_str());
}

static void LogPyError() {
    if (!PyErr_Occurred()) return;

    PyObject* type, * value, * traceback;
    PyErr_Fetch(&type, &value, &traceback);
    PyErr_NormalizeException(&type, &value, &traceback);

    std::wstring message = L"(unprintable Python error)";

    // Prefer a full formatted traceback (via the traceback module) so
    // the user gets a line number, not just the exception message.
    PyObject* tb_module = PyImport_ImportModule("traceback");
    if (tb_module && value) {
        PyObject* format_exc = PyObject_GetAttrString(tb_module, "format_exception");
        if (format_exc) {
            PyObject* args = PyTuple_Pack(3, type, value, traceback ? traceback : Py_None);
            PyObject* lines = PyObject_CallObject(format_exc, args);
            Py_DECREF(args);
            if (lines) {
                PyObject* sep = PyUnicode_FromString("");
                PyObject* joined = PyUnicode_Join(sep, lines);
                Py_DECREF(sep);
                if (joined) {
                    const char* msg = PyUnicode_AsUTF8(joined);
                    if (msg) message = Utf8ToWide(msg);
                    Py_DECREF(joined);
                }
                Py_DECREF(lines);
            }
            else {
                PyErr_Clear(); // formatting itself failed; fall through to the simple message below
            }
            Py_DECREF(format_exc);
        }
        Py_DECREF(tb_module);
    }

    if (message == L"(unprintable Python error)" && value) {
        PyObject* str = PyObject_Str(value);
        if (str) {
            const char* msg = PyUnicode_AsUTF8(str);
            if (msg) message = Utf8ToWide(msg);
            Py_DECREF(str);
        }
    }

    LogError(message);

    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(traceback);
}

// (Re)labels the param_tracks[] slots from the loaded script's PARAM_NAMES
// list (if any), falling back to generic "ParamN" for any slot the script
// doesn't name. Caller must already hold the GIL. module may be nullptr
// (used to reset everything to generic defaults, e.g. on load failure).
//
// Script side:
//   PARAM_NAMES = ["Radius", "Angle", "Strength"]
// gives you 3 meaningfully-labeled sliders; the remaining MAX_PARAMS-3
// stay as generic "ParamN" and are still passed to process() as extra
// entries in `params` if you want them, just unlabeled in the UI.
static void ApplyParamNames(PyObject* module) {
    PyObject* names_obj = nullptr;
    if (module) {
        names_obj = PyObject_GetAttrString(module, "PARAM_NAMES");
        if (!names_obj) PyErr_Clear(); // attribute simply absent; not an error
    }

    for (int i = 0; i < MAX_PARAMS; ++i) {
        std::wstring label;
        if (names_obj && PyList_Check(names_obj) && i < PyList_Size(names_obj)) {
            PyObject* item = PyList_GetItem(names_obj, i); // borrowed reference
            if (item && PyUnicode_Check(item)) {
                const char* s = PyUnicode_AsUTF8(item);
                if (s) label = Utf8ToWide(s);
            }
        }
        if (label.empty()) {
            label = L"(unused)" + std::to_wstring(i + 1);
        }
        g_param_name_storage[i] = label;
        param_tracks[i].name = g_param_name_storage[i].c_str();
    }

    Py_XDECREF(names_obj);
}


// Caller must already hold the GIL.
static PyObject* GetOrReloadScript(const std::wstring& path) {
    if (path.empty()) return nullptr;

    bool need_reload = hot_reload.value || (path != g_loaded_path) || (g_loaded_module == nullptr);
    if (!need_reload) return g_loaded_module;

    if (g_loaded_module) {
        Py_DECREF(g_loaded_module);
        g_loaded_module = nullptr;
    }

    PyObject* importlib_util = PyImport_ImportModule("importlib.util");
    if (!importlib_util) { LogPyError(); PyErr_Clear(); return nullptr; }

    std::string path_utf8 = WideToUtf8(path);
    PyObject* py_path = PyUnicode_FromString(path_utf8.c_str());
    PyObject* py_name = PyUnicode_FromString("pyfilter_user_script");

    PyObject* spec_func = PyObject_GetAttrString(importlib_util, "spec_from_file_location");
    PyObject* spec = nullptr;
    if (spec_func) {
        PyObject* args = PyTuple_Pack(2, py_name, py_path);
        spec = PyObject_CallObject(spec_func, args);
        Py_DECREF(args);
        Py_DECREF(spec_func);
    }
    Py_DECREF(importlib_util);
    Py_DECREF(py_path);
    Py_DECREF(py_name);

    if (!spec) { LogPyError(); PyErr_Clear(); return nullptr; }

    PyObject* module_from_spec = PyImport_ImportModule("importlib.util");
    PyObject* new_module = nullptr;
    if (module_from_spec) {
        PyObject* func = PyObject_GetAttrString(module_from_spec, "module_from_spec");
        if (func) {
            PyObject* args = PyTuple_Pack(1, spec);
            new_module = PyObject_CallObject(func, args);
            Py_DECREF(args);
            Py_DECREF(func);
        }
        Py_DECREF(module_from_spec);
    }

    if (!new_module) { Py_DECREF(spec); LogPyError(); PyErr_Clear(); return nullptr; }

    PyObject* loader = PyObject_GetAttrString(spec, "loader");
    PyObject* exec_result = nullptr;
    if (loader) {
        PyObject* exec_module = PyObject_GetAttrString(loader, "exec_module");
        if (exec_module) {
            PyObject* args = PyTuple_Pack(1, new_module);
            exec_result = PyObject_CallObject(exec_module, args);
            Py_DECREF(args);
            Py_DECREF(exec_module);
        }
        Py_DECREF(loader);
    }
    Py_DECREF(spec);

    if (!exec_result) {
        LogError(L"failed to load script: " + path);
        LogPyError();
        PyErr_Clear();
        Py_DECREF(new_module);
        ApplyParamNames(nullptr); // reset labels to generic Param1..N
        return nullptr;
    }
    Py_DECREF(exec_result);

    g_loaded_module = new_module;   // keep our reference
    g_loaded_path = path;
    ApplyParamNames(g_loaded_module);
    LogInfo(L"loaded script: " + path);
    return g_loaded_module;
}

//---------------------------------------------------------------------
//  Plugin lifecycle
//---------------------------------------------------------------------
EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* logger) {
    // Called by the host before InitializePlugin(), so this is the
    // earliest point logging becomes available.
    g_logger = logger;
}

EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD version) {
    (void)version;

    // Point Python at a self-contained runtime bundled next to this DLL:
    //   <plugin folder>/PythonBridge/python/   <- embeddable CPython distribution
    // This makes the plugin portable and not dependent on any system Python.
    wchar_t module_path[MAX_PATH];
    GetModuleFileNameW(g_this_module, module_path, MAX_PATH);
    std::wstring dll_path(module_path);
    std::wstring dll_dir = dll_path.substr(0, dll_path.find_last_of(L"\\/"));
    std::wstring python_home = dll_dir + L"\\PythonBridge\\python";

    LogInfo(L"initializing Python runtime at: " + python_home);

    // IMPORTANT: python3xx.dll is an implicit ("load-time") dependency of
    // this DLL via python3xx.lib. Without delay-loading it, Windows tries
    // to resolve python3xx.dll the instant AviUtl calls LoadLibrary on
    // this .auf2 -- before any of our code (including this function) runs
    // -- and it will only look in the standard search paths, NOT in our
    // bundled PythonBridge\python\ subfolder. That load fails silently
    // and AviUtl just won't show the plugin.
    //
    // Fix: mark python3xx.dll as /DELAYLOAD in the linker settings (see
    // README), which defers resolution until the first actual call into
    // it. That first call happens below, so telling the OS loader where
    // to look right here (via SetDllDirectoryW) is sufficient.
    if (!SetDllDirectoryW(python_home.c_str())) {
        LogWarn(L"SetDllDirectoryW failed for: " + python_home);
    }

    // PyConfig-based init replaces the deprecated Py_SetPythonHome() +
    // Py_Initialize() pair (Py_SetPythonHome is deprecated since 3.11).
    PyConfig config;
    PyConfig_InitPythonConfig(&config);

    PyStatus status = PyConfig_SetString(&config, &config.home, python_home.c_str());
    if (PyStatus_Exception(status)) {
        LogError(L"PyConfig_SetString(home) failed for: " + python_home);
        PyConfig_Clear(&config);
        return false;
    }

    status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config); // must be called whether init succeeded or not

    if (PyStatus_Exception(status)) {
        LogError(L"Py_InitializeFromConfig() failed -- check that PythonBridge\\python\\ "
            L"exists next to the DLL and matches the linked Python version.");
        return false;
    }

    // Ensure the folder containing user scripts (and this python_home) is importable.
    PyRun_SimpleString("import sys");

    LogInfo(L"Python runtime ready.");

    ApplyParamNames(nullptr); // start with generic Param1..N labels

    // Release the GIL on the main thread; func_proc_video will re-acquire it
    // via PyGILState_Ensure() since AviUtl may call us from a worker thread.
    g_main_thread_state = PyEval_SaveThread();

    return true;
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
    if (g_main_thread_state) {
        PyEval_RestoreThread(g_main_thread_state);
        Py_XDECREF(g_loaded_module);
        g_loaded_module = nullptr;
        Py_Finalize();
        g_main_thread_state = nullptr;
        LogInfo(L"Python runtime shut down.");
    }
}

EXTERN_C __declspec(dllexport) FILTER_PLUGIN_TABLE* GetFilterPluginTable(void) {
    return &filter_plugin_table;
}

//---------------------------------------------------------------------
//  Per-frame video processing
//---------------------------------------------------------------------
bool func_proc_video(FILTER_PROC_VIDEO* video) {
    auto w = video->object->width;
    auto h = video->object->height;
    std::vector<PIXEL_RGBA> buffer((size_t)w * h);
    video->get_image_data(buffer.data());

    PyGILState_STATE gstate = PyGILState_Ensure();

    PyObject* module = GetOrReloadScript(script_path.value ? script_path.value : L"");
    if (!module) {
        // No valid script selected/loaded -> pass the frame through unchanged.
        PyGILState_Release(gstate);
        video->set_image_data(buffer.data(), w, h);
        return true;
    }

    PyObject* func = PyObject_GetAttrString(module, "process");
    if (!func || !PyCallable_Check(func)) {
        Py_XDECREF(func);
        PyErr_Clear(); // GetAttrString raises AttributeError if missing; not a real error to log as one
        LogWarn(L"script has no process(frame, width, height, frame_no, params) function: " + g_loaded_path);
        PyGILState_Release(gstate);
        video->set_image_data(buffer.data(), w, h);
        return true;
    }

    // Zero-copy writable view over the pixel buffer: Python side does
    //   np.frombuffer(mv, dtype=np.uint8).reshape(h, w, 4)
    PyObject* mv = PyMemoryView_FromMemory(
        reinterpret_cast<char*>(buffer.data()),
        (Py_ssize_t)(buffer.size() * sizeof(PIXEL_RGBA)),
        PyBUF_WRITE);

    PyObject* params = PyList_New(MAX_PARAMS);
    for (int i = 0; i < MAX_PARAMS; ++i) {
        PyList_SetItem(params, i, PyFloat_FromDouble(param_tracks[i].value));
    }

    PyObject* py_w = PyLong_FromLong(w);
    PyObject* py_h = PyLong_FromLong(h);
    PyObject* py_frame = PyLong_FromLong(video->object->frame);

    PyObject* args = PyTuple_Pack(5, mv, py_w, py_h, py_frame, params);
    PyObject* result = PyObject_CallObject(func, args);

    if (!result) {
        LogError(L"process() raised in script: " + g_loaded_path);
        LogPyError();
        PyErr_Clear();
    }
    else {
        Py_DECREF(result);
    }

    Py_DECREF(args);
    Py_DECREF(py_w);
    Py_DECREF(py_h);
    Py_DECREF(py_frame);
    Py_DECREF(params);
    Py_DECREF(mv);
    Py_DECREF(func);

    PyGILState_Release(gstate);

    video->set_image_data(buffer.data(), w, h);
    return true;
}

//---------------------------------------------------------------------
//  DllMain: just stash our own module handle for GetModuleFileNameW above
//---------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_this_module = hModule;
    }
    return TRUE;
}