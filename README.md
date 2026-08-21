# PythonBridge / PyFX

PythonBridge is an AviUtl2 video filter plugin that lets you write filter effects in Python. It forwards each video frame to a Python script's `process()` function.

## Installation

1. Build or download the 64-bit `PythonBridge.auf2` plugin.
2. Copy `PythonBridge.auf2` into your AviUtl2 `plugins` folder.
3. Download and install the Python 3 version you want to use normally from [python.org](https://www.python.org/downloads/).
4. Copy the complete installed Python folder into a `PythonBridge\python` folder next to the plugin. For example, if your Python installation is:

   ```text
   C:\Users\YourUsername\AppData\Local\Programs\Python\Python314
   ```

   copy that folder to:

   ```text
   C:\ProgramData\AviUtl2\plugins\PythonBridge\python
   ```

   Replace `YourUsername` and `Python314` with your Windows username and the Python 3 version you installed. The folder must contain the Python runtime files, including `python3xx.dll`.

After installation, restart AviUtl2. The **Python Bridge** filter should appear in the video filter list.

## Using a Python effect

Select a `.py` file in the plugin's **Python Script** setting. The script should define a `process()` function:

```python
def process(frame, width, height, frame_no, params):
	# `frame` is a writable memoryview containing RGBA pixel data.
	# Modify it in place to change the video frame.
	pass
```

Optional slider names can be supplied with `PARAM_NAMES`:

```python
PARAM_NAMES = ["Radius", "Strength"]
```

Because of a limitation in AviUtl2, parameter names are not detected automatically while the effect is already loaded. After adding or changing `PARAM_NAMES`, load the Python script, reload the **PythonBridge / PyFX** effect, and then reselect the script in the effect settings so the parameter names appear.

The plugin provides up to 16 numeric parameters to the script through `params`. Enable **Reload script every frame (dev mode)** while developing if you want changes to be picked up without restarting AviUtl2.

## Troubleshooting

- Make sure `PythonBridge.auf2` is in AviUtl2's `plugins` folder.
- Make sure the copied Python folder is exactly next to the plugin at `plugins\PythonBridge\python`.
- Use a 64-bit Python installation with a Python version compatible with the plugin build.
- Check that the Python folder contains the matching `python3xx.dll` runtime file.

If you run into an issue, please report it on this repository's **Issues** tab and include the AviUtl2 version, Python version, plugin version, and any error log output.

## Credits

Created by [redlean91](https://github.com/redlean91) and [murderer2026](https://github.com/murderer2026).
