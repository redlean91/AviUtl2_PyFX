"""
Simple VHS script for AviUtl ExEdit2 (PythonBridge/PyFX)

murderer2026, redlean91

with the help of: claude, gemini, chatgpt, grok, siri ai, meta ai, snap ai, tiktok ai, deepseek, qwen, perplexity, bard, bing ai, llama2, falcon, mpt, kohya, stable diffusion, midjourney, dall-e, dreamstudio, runpod, replicate, huggingface, autocomplete, copilot, codeium, tabnine

"""

PARAM_NAMES = ["Aberration pixels", "Aberration", "Noise amount", "Jitter amount", "Scan Lines"]

import numpy as np

def process(frame, width, height, frame_no, params):
    img = np.frombuffer(frame, dtype=np.uint8).reshape(height, width, 4)

    aberration_pxls = params[0] / 100.0 * 24
    aberration = int(params[1] / 100.0 * aberration_pxls)
    if aberration > 0:
        img[:, :, 0] = np.roll(img[:, :, 0], aberration, axis=1)  
        img[:, :, 2] = np.roll(img[:, :, 2], -aberration, axis=1)  

    noise_amount = params[2] / 100.0 * 24
    if noise_amount > 0:
        noise = np.random.normal(0, noise_amount, (height, width, 3)).astype(np.uint8)
        rgb = img[:, :, :3].astype(np.int32) + noise
        np.clip(rgb, 0, 255, out=rgb)
        img[:, :, :3] = rgb.astype(np.uint8)

    jitter_amount = params[3] / 100.0 * 20
    if jitter_amount > 0:
        jitter = np.random.randint(-jitter_amount, jitter_amount + 1, size=height)
        for y in range(height):
            img[y, :, :3] = np.roll(img[y, :, :3], jitter[y], axis=0)

    scanline_strength = max(0.0, min(1.0, params[4] / 100.0)) * 0.15 
    if scanline_strength > 0:
        img[0::2, :, :3] = (
            img[0::2, :, :3].astype(np.float32) * (1.0 - scanline_strength)
        ).astype(np.uint8)