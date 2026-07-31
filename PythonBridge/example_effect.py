"""
Example AviUtl ExEdit2 Python Bridge effect.

Point the plugin's "Script" field at this file, then adjust Param1..Param4
in AviUtl's filter panel.

Contract:
    process(frame, width, height, frame_no, params)

    frame    : a Python memoryview over the raw RGBA8 pixel buffer,
               tightly packed, row-major, size = width*height*4 bytes.
               Wrap it with numpy for convenience (see below). Writes to
               the numpy array (since it shares the same buffer) are
               written straight back into the frame.
    width,
    height   : current frame size in pixels.
    frame_no : current frame index (int).
    params   : list of 4 floats, taken from the Param1..Param4 sliders.

    Return value is ignored -- mutate `frame` in place.
"""

import numpy as np


def process(frame, width, height, frame_no, params):
    img = np.frombuffer(frame, dtype=np.uint8).reshape(height, width, 4)

    # Param1 = brightness multiplier on RGB, leaves alpha untouched.
    brightness = 1.0 + params[0] / 100.0
    rgb = img[:, :, :3].astype(np.float32)
    rgb *= brightness
    np.clip(rgb, 0, 255, out=rgb)
    img[:, :, :3] = rgb.astype(np.uint8)
