# AMD FSR 4 (Windows)

Video > Upscaler > **AMD FSR 4**: AMD's machine-learning upscaler (AMD FSR Upscaling 4.x), added by **MK-JH**
(Telegram: <https://t.me/MKJH_APPS>). It takes exactly what the FSR 3 upscaler takes (the scene, its depth, the game's own
motion vectors, the reactive mask, the jitter) and gives the scene at the window's size, so every upscaler mode works the
same; `runtime/nvn/vulkan/fsr4.cpp` is the whole of it.

## What you need

- Windows, and a Radeon RX 6000, 7000 or 9000 (AMD's INT8 model runs on RDNA 2 and 3; RDNA 4 has the full model).
- Two DLLs from **AMD's FSR SDK 2.4 or newer** (<https://gpuopen.com/amd-fsr-sdk/>, the SDK's `Kits/FidelityFX/signedbin`
  folder; the Vulkan ones):
  - `amd_fidelityfx_loader_vk.dll`
  - `amd_fidelityfx_upscaler_vk.dll`

## Where the files go

Put both files **in the same folder as `savagewind.exe`** (in the release zip: the `Savage Wind` folder). Start the game
again: Video > Upscaler lists **AMD FSR 4**. Without the DLLs the entry is greyed ("needs AMD's DLLs").

They are not in this repository on purpose: AMD's SDK licence allows its DLLs in binary form only, with its own notice,
and they must be AMD's signed ones. Use them unchanged.

## What happens

- With the DLLs next to the program the Vulkan device is made with what the ML upscaler may use (as AMD's own sample does):
  every core feature the driver has, cooperative matrices, FP8 (where the GPU has it) and integer dot products. The log
  has a `[fsr4] device:` line.
- At the first frame with FSR 4 picked, the loader is asked for the GPU's upscaler providers (`[fsr4] upscaler provider`
  lines); the newest 4.x one is used. A GPU or DLLs with no 4.x provider make FSR 4 give up for the run and **FSR 3 runs in
  its place** at the same render size (the menu says so).
- Video > Sharpening sets the upscaler's own sharpening, as with FSR 3.

## Testing switches (environment variables)

| Variable | Effect |
|---|---|
| `SAVAGEWIND_FSR4_HDR=0` | treat the scene as LDR (default: HDR input, the scene is linear light) |
| `SAVAGEWIND_FSR4_AUTOEXP=0` | no automatic exposure |
| `SAVAGEWIND_FSR4_DEBUG=1` | the upscaler's checks of its inputs, in the log |
| `SAVAGEWIND_FSR4_VIEW=1` | the upscaler's debug view of its inputs and state in the image |

## Not tested

This was written against AMD's SDK 2.4 headers (`third_party/FidelityFX/ffxapi`) without a Windows machine or an AMD GPU
at hand: build it, run it with the DLLs and look at the log first. Things to check on the first run: the `[fsr4]` lines in
the log, that the image is not black or smeared (the resource state, `FFX_API_RESOURCE_STATE_COMMON`, is meant to leave the
images in `VK_IMAGE_LAYOUT_GENERAL` as the FSR 3 path does), and the jitter direction (ghosting trails while moving).
