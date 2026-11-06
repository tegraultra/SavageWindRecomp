// AMD FSR 4 (AMD FSR Upscaling 4.x) through AMD's signed FidelityFX SDK 2.x DLLs on Vulkan (fsr4.h). Added by MK-JH.
//
// The SDK's ffx-api, loaded at run time: amd_fidelityfx_loader_vk.dll next to savagewind.exe finds
// amd_fidelityfx_upscaler_vk.dll beside it and hands out a provider per GPU (FSR 4's ML model where the GPU can run it,
// FSR 3.1 otherwise). Only the provider whose version is 4.x is used here: without one FSR 4 is off for the run and
// the caller falls back to FSR 3 (exec.c fsr3_scene). The headers are the SDK's (third_party/FidelityFX/ffxapi).
//
// The inputs are FSR 3's (fsr3.cpp): LDR-range linear scene, standard depth, low-resolution jitter-free motion vectors,
// reactive mask; the provider passes the dispatch's jitter and camera on to its upscaler unchanged, so the jitter and
// motion vector conventions of fsr3.h hold here too.
#include "fsr4.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define FFX_API_ENTRY  // (imported through function pointers: not the SDK's dllexport)
#include <windows.h>

#include <api/include/ffx_api.h>
#include <api/include/ffx_api_types.h>
#include <api/include/vk/ffx_api_vk.h>
#include <upscalers/include/ffx_upscale.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <vector>

#include "../../rt.h"

static const wchar_t *const k_loader = L"amd_fidelityfx_loader_vk.dll";
static const wchar_t *const k_upscaler = L"amd_fidelityfx_upscaler_vk.dll";

static HMODULE mod;
static PfnFfxCreateContext p_create;
static PfnFfxDestroyContext p_destroy;
static PfnFfxQuery p_query;
static PfnFfxDispatch p_dispatch;
static bool failed;  // (given up for this run)
static ffxContext ctx;
static bool have_ctx;
static uint32_t ctx_rw, ctx_rh, ctx_ow, ctx_oh;
static VkDevice ctx_dev;
static VkDeviceContext dev_ctx;  // (the backend keeps a pointer to it)
static char version_name[64];

// The folder savagewind.exe is in (with a trailing backslash)
static bool exe_dir(wchar_t *out, size_t cap) {
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)cap);
    if (!n || n >= cap) return false;
    while (n && out[n - 1] != L'\\' && out[n - 1] != L'/') n--;
    out[n] = 0;
    return n > 0;
}

static bool file_here(const wchar_t *dir, const wchar_t *name) {
    wchar_t p[MAX_PATH + 64];
    swprintf(p, sizeof p / sizeof *p, L"%ls%ls", dir, name);
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

extern "C" bool fsr4_dlls_present(void) {
    static int present = -1;
    if (present < 0) {
        wchar_t dir[MAX_PATH];
        present = exe_dir(dir, MAX_PATH) && file_here(dir, k_loader) && file_here(dir, k_upscaler);
        LOG("[fsr4] AMD's FSR DLLs %s\n", present ? "found next to the program" : "not found next to the program (docs/FSR4.md)");
    }
    return present;
}

extern "C" bool fsr4_available(void) { return !failed && fsr4_dlls_present(); }

extern "C" bool fsr4_gave_up(void) { return failed; }

extern "C" const char *fsr4_version_name(void) { return have_ctx && version_name[0] ? version_name : NULL; }

static void message(uint32_t type, const wchar_t *msg) {
    char buf[512];
    snprintf(buf, sizeof buf, "%ls", msg);
    LOG("[fsr4] %s: %s\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", buf);
}

static bool load_loader(void) {
    if (mod) return true;
    wchar_t dir[MAX_PATH], p[MAX_PATH + 64];
    if (exe_dir(dir, MAX_PATH)) {
        swprintf(p, sizeof p / sizeof *p, L"%ls%ls", dir, k_loader);
        mod = LoadLibraryExW(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);  // (its neighbours are looked for beside it)
    }
    if (!mod) mod = LoadLibraryW(k_loader);
    if (!mod) {
        LOG("[fsr4] could not load %ls (error %lu)\n", k_loader, (unsigned long)GetLastError());
        return false;
    }
    p_create = (PfnFfxCreateContext)GetProcAddress(mod, "ffxCreateContext");
    p_destroy = (PfnFfxDestroyContext)GetProcAddress(mod, "ffxDestroyContext");
    p_query = (PfnFfxQuery)GetProcAddress(mod, "ffxQuery");
    p_dispatch = (PfnFfxDispatch)GetProcAddress(mod, "ffxDispatch");
    if (!p_create || !p_destroy || !p_query || !p_dispatch) {
        LOG("[fsr4] %ls lacks ffx-api functions\n", k_loader);
        FreeLibrary(mod);
        mod = NULL;
        return false;
    }
    return true;
}

extern "C" void fsr4_destroy(void) {
    if (!have_ctx) return;
    vkDeviceWaitIdle(ctx_dev);
    p_destroy(&ctx, nullptr);
    ctx = nullptr;
    have_ctx = false;
    version_name[0] = 0;
}

// The backend's device functions: an extension's KHR name this device was not made with is the core function it was
// promoted to, and debug labels do nothing without VK_EXT_debug_utils (as fsr3.cpp's; the labels become
// SAVAGEWIND_GPUPROF_AT marks)
extern "C" void vk_gpuprof_mark(VkCommandBuffer cb, const char *what);
static VKAPI_ATTR void VKAPI_CALL no_label(VkCommandBuffer cb, const VkDebugUtilsLabelEXT *l) {
    char buf[96];
    snprintf(buf, sizeof buf, "FSR 4: %s", l && l->pLabelName ? l->pLabelName : "?");
    vk_gpuprof_mark(cb, buf);
}
static VKAPI_ATTR void VKAPI_CALL no_label_end(VkCommandBuffer) {}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice dev, const char *name) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(dev, name);
    if (!f && !strcmp(name, "vkCmdBeginDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)no_label;
    if (!f && !strcmp(name, "vkCmdEndDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)no_label_end;
    size_t n = strlen(name);
    if (!f && n > 3 && n < 128 && !strcmp(name + n - 3, "KHR")) {
        char core[128];
        memcpy(core, name, n - 3);
        core[n - 3] = 0;
        f = vkGetDeviceProcAddr(dev, core);
    }
    return f;
}

// The first number in a provider's name: "4.1.2" -> 4, "FSR 3.1.6" -> 3 (0: none)
static int major_of(const char *name) {
    for (; name && *name; name++)
        if (*name >= '0' && *name <= '9') return atoi(name);
    return 0;
}

// The id of the newest 4.x provider this GPU has (0: none)
static uint64_t find_fsr4(const Fsr3Dispatch *d) {
    dev_ctx = {d->dev, d->phys, device_proc};
    ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = &dev_ctx;
    uint64_t n = 0;
    q.outputCount = &n;
    ffxReturnCode_t rc = p_query(nullptr, &q.header);
    if (rc != FFX_API_RETURN_OK || !n) {
        LOG("[fsr4] version query: error %u, %llu versions\n", (unsigned)rc, (unsigned long long)n);
        return 0;
    }
    std::vector<uint64_t> ids(n);
    std::vector<const char *> names(n);
    q.versionIds = ids.data();
    q.versionNames = names.data();
    rc = p_query(nullptr, &q.header);
    if (rc != FFX_API_RETURN_OK) {
        LOG("[fsr4] version query: error %u\n", (unsigned)rc);
        return 0;
    }
    uint64_t best = 0;
    int best_major = 0;
    for (uint64_t i = 0; i < n; i++) {
        int m = major_of(names[i]);
        LOG("[fsr4] upscaler provider %llu: %s\n", (unsigned long long)i, names[i] ? names[i] : "?");
        if (m >= 4 && (!best || m >= best_major)) best = ids[i], best_major = m;
    }
    return best;
}

static bool make_context(const Fsr3Dispatch *d) {
    fsr4_destroy();
    if (!load_loader()) return false;
    uint64_t id = find_fsr4(d);
    if (!id) {
        LOG("[fsr4] no FSR 4 provider for this GPU: staying on FSR 3\n");
        return false;
    }
    // (the scene is linear light: HDR input and auto exposure as AMD's sample sets them; LDR-range values pass either
    // way. SAVAGEWIND_FSR4_HDR=0 / SAVAGEWIND_FSR4_AUTOEXP=0 turn them off; SAVAGEWIND_FSR4_DEBUG=1: its checks of the
    // inputs, in the log)
    static int hdr = -1, autoexp = -1, debug = -1;
    if (hdr < 0) hdr = !getenv("SAVAGEWIND_FSR4_HDR") || atoi(getenv("SAVAGEWIND_FSR4_HDR")) != 0;
    if (autoexp < 0) autoexp = !getenv("SAVAGEWIND_FSR4_AUTOEXP") || atoi(getenv("SAVAGEWIND_FSR4_AUTOEXP")) != 0;
    if (debug < 0) debug = getenv("SAVAGEWIND_FSR4_DEBUG") != NULL;
    static ffxCreateContextDescUpscale cu;  // (the descriptions stay alive for the context: ffx-api)
    static ffxCreateBackendVKDesc bd;
    static ffxCreateContextDescUpscaleVersion vd;
    static ffxOverrideVersion ov;
    cu = {};
    bd = {};
    vd = {};
    ov = {};
    cu.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    cu.flags = (hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : 0) | (autoexp ? FFX_UPSCALE_ENABLE_AUTO_EXPOSURE : 0) |
               (debug ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0);
    // (low-resolution, jitter-free motion vectors and standard depth: no flag for them)
    cu.maxRenderSize = {d->color.width, d->color.height};
    cu.maxUpscaleSize = {d->output.width, d->output.height};
    cu.fpMessage = message;
    bd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    bd.deviceContext = dev_ctx;
    vd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    vd.version = FFX_UPSCALER_VERSION;
    ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    ov.versionId = id;
    cu.header.pNext = &bd.header;
    bd.header.pNext = &vd.header;
    vd.header.pNext = &ov.header;
    ffxReturnCode_t rc = p_create(&ctx, &cu.header, nullptr);
    if (rc != FFX_API_RETURN_OK) {
        LOG("[fsr4] context: error %u\n", (unsigned)rc);
        ctx = nullptr;
        return false;
    }
    have_ctx = true;
    ctx_dev = d->dev;
    ctx_rw = d->color.width, ctx_rh = d->color.height, ctx_ow = d->output.width, ctx_oh = d->output.height;
    ffxQueryGetProviderVersion pv = {};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (p_query(&ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName)
        snprintf(version_name, sizeof version_name, "%s", pv.versionName);
    LOG("[fsr4] context %ux%u -> %ux%u, provider %s\n", ctx_rw, ctx_rh, ctx_ow, ctx_oh, version_name[0] ? version_name : "?");
    return true;
}

static FfxApiResource resource(const Fsr3Image *m, bool uav) {
    FfxApiResourceDescription rd = {};
    rd.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    rd.format = ffxApiGetSurfaceFormatVK(m->format);
    rd.width = m->width, rd.height = m->height, rd.depth = 1, rd.mipCount = 1;
    rd.flags = FFX_API_RESOURCE_FLAGS_NONE;
    rd.usage = uav ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    return ffxApiGetResourceVK((void *)m->image, rd, FFX_API_RESOURCE_STATE_COMMON);  // (COMMON: GENERAL, kept)
}

static FfxApiResource none(void) {
    FfxApiResource r = {};
    r.state = FFX_API_RESOURCE_STATE_COMMON;
    return r;
}

extern "C" bool fsr4_dispatch(const Fsr3Dispatch *d) {
    if (failed || !fsr4_dlls_present()) return false;
    if ((!have_ctx || d->color.width != ctx_rw || d->color.height != ctx_rh || d->output.width != ctx_ow ||
         d->output.height != ctx_oh) && !make_context(d)) {
        failed = true;  // (no FSR 4 this run)
        return false;
    }
    ffxDispatchDescUpscale dd = {};
    dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dd.commandList = (void *)d->cb;
    dd.color = resource(&d->color, false);
    dd.depth = resource(&d->depth, false);
    dd.motionVectors = resource(&d->motion, false);
    dd.exposure = none();
    dd.reactive = resource(&d->reactive, false);
    dd.transparencyAndComposition = none();
    dd.output = resource(&d->output, true);
    dd.jitterOffset = {d->jitter[0], d->jitter[1]};
    dd.motionVectorScale = {d->motion_scale[0], d->motion_scale[1]};
    dd.renderSize = {d->color.width, d->color.height};
    dd.upscaleSize = {d->output.width, d->output.height};
    dd.enableSharpening = d->sharpness > 0;
    dd.sharpness = d->sharpness;
    dd.frameTimeDelta = d->dt_ms;
    dd.preExposure = 1.0f;
    dd.reset = d->reset;
    dd.cameraNear = d->near_;
    dd.cameraFar = d->far_;
    dd.cameraFovAngleVertical = d->fov_y;
    dd.viewSpaceToMetersFactor = 1.0f;  // (BOTW's units are meters)
    static int view = -1;  // SAVAGEWIND_FSR4_VIEW=1: the upscaler's own debug view of its inputs and state in the output
    if (view < 0) view = getenv("SAVAGEWIND_FSR4_VIEW") != NULL;
    if (view) dd.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
    ffxReturnCode_t rc = p_dispatch(&ctx, &dd.header);
    if (rc != FFX_API_RETURN_OK) {
        static int logged;
        if (logged++ < 8) LOG("[fsr4] dispatch: error %u\n", (unsigned)rc);
        return false;
    }
    return true;
}

#else  // not Windows: AMD ships the Vulkan DLLs for Windows only
extern "C" bool fsr4_dlls_present(void) { return false; }
extern "C" bool fsr4_available(void) { return false; }
extern "C" bool fsr4_gave_up(void) { return false; }
extern "C" bool fsr4_dispatch(const Fsr3Dispatch *) { return false; }
extern "C" void fsr4_destroy(void) {}
extern "C" const char *fsr4_version_name(void) { return NULL; }
#endif
