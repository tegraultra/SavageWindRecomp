// AMD FSR 4 (AMD FSR Upscaling 4.x, the machine-learning upscaler) through AMD's own signed FidelityFX SDK 2.x DLLs
// (fsr4.cpp: amd_fidelityfx_loader_vk.dll + amd_fidelityfx_upscaler_vk.dll, the ffx-api on Vulkan), for the 3D scene at
// its composite (exec.c scene_post, Video > Upscaler FSR 4). It takes exactly what the FSR 3 upscaler takes (fsr3.h's
// Fsr3Dispatch: scene, depth, motion vectors and reactive mask at the internal resolution, the jitter, the camera) and
// gives the scene at the window's size. Windows only; elsewhere fsr4_available is false. The DLLs are not part of this
// repository (AMD's SDK licence allows them in binary form only, with its notice): docs/FSR4.md says where they go.
// Added by MK-JH.
#pragma once
#include <stdbool.h>

#include "fsr3.h"

#ifdef __cplusplus
extern "C" {
#endif

// The two DLLs are next to savagewind.exe (a cheap file check, cached): Video > Upscaler lists FSR 4 only then, and
// the Vulkan device (device.c) is made with what the ML upscaler needs only then
bool fsr4_dlls_present(void);
// Present, and not given up on this run (no FSR 4 provider for this GPU, a failed context)
bool fsr4_available(void);
// Given up on this run: FSR 3 runs in its place (exec.c fsr3_scene)
bool fsr4_gave_up(void);
// Records FSR 4 into d->cb (the context is made, and made again, for the sizes given); false when it failed: the caller
// goes on with FSR 3 for the frame
bool fsr4_dispatch(const Fsr3Dispatch *d);
void fsr4_destroy(void);
// The provider's name once a context exists ("4.1.2"), else NULL
const char *fsr4_version_name(void);

#ifdef __cplusplus
}
#endif
