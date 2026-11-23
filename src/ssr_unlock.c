// Hello Neighbor Tweaks - graphics mod for Hello Neighbor (UE 4.20, Win64).
// Copyright (c) 2026 forgotten893. Released under the MIT License (see LICENSE).
// Not affiliated with or endorsed by tinyBuild or Dynamic Pixels.
//
// Started as a reflection quality unlock + live settings check; the sections below describe the core hooks.
//
// Loaded as a stand-in XINPUT1_3.dll next to HelloNeighbor-Win64-Shipping.exe.
// All XInput exports are forwarded (same ordinals - the game imports 2 and 3 by
// ordinal) to the real C:\Windows\System32\XInput1_3.dll.
//
// 1) SSR patch. The SSR pass computes
//      Quality = min(QualityFromPostProcessVolume, clamp(r.SSR.Quality, 0, 5))
//    and the maps set ScreenSpaceReflectionQuality = 10 (=> level 2), so the
//    cvar can only lower it. We NOP the min ("cmp ecx,eax / cmovle eax,ecx").
//    Found by a unique signature; patched only if found exactly once.
//
// 2) Settings check. Every key in Engine.ini [SystemSettings] is looked up in
//    the engine's memory (read-only - no engine functions are called):
//      TAutoConsoleVariable registration:
//        lea rdx,[name] ; (mov [rsp+20h],flags) ; call [rax+08h|10h] (float|int)
//        ... call [rdx+40h|38h] (AsVariableFloat|Int) ; (lea rcx,..) ;
//        mov [rip+G],rax      -> *G = TConsoleVariableData { T game, render }
//      FAutoConsoleVariableRef<float> registration:
//        lea r8,[var] ; ... ; lea rdx,[name] ; call [rax+20h]   -> float at var
//
// 3) Frame counter. IDXGISwapChain::Present is hooked through the swap chain
//    vtable (found via a throw-away 64x64 swap chain on a hidden window) to
//    count frames and record the game's SyncInterval and back-buffer size.
//
// Everything is written to HelloNeighborTweaks.log next to the DLL, rewritten
// every 5 seconds.

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wchar.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#include "version.h"

// v34: release builds have one hotkey, F1 (the menu). The testing tools - F5 compare with the original game,
// F6 volumetric fog, F7 contact shadows, F8 lighting mode, F9 AO, F10 overlay pin, F11 display mode cycle, and the
// menu's live (unsaved) "Lighting mode" / "Ambient occlusion" switches - are only built with
// /DHNT_DEBUG_HOTKEYS=1, so players cannot toggle them by accident.
#ifndef HNT_DEBUG_HOTKEYS
#define HNT_DEBUG_HOTKEYS 0
#endif
#define XI "C:\\Windows\\System32\\XInput1_3."
#pragma comment(linker, "/export:XInputGetState=" XI "XInputGetState,@2")
#pragma comment(linker, "/export:XInputSetState=" XI "XInputSetState,@3")
#pragma comment(linker, "/export:XInputGetCapabilities=" XI "XInputGetCapabilities,@4")
#pragma comment(linker, "/export:XInputEnable=" XI "XInputEnable,@5")
#pragma comment(linker, "/export:XInputGetDSoundAudioDeviceGuids=" XI "XInputGetDSoundAudioDeviceGuids,@6")
#pragma comment(linker, "/export:XInputGetBatteryInformation=" XI "XInputGetBatteryInformation,@7")
#pragma comment(linker, "/export:XInputGetKeystroke=" XI "XInputGetKeystroke,@8")
#pragma comment(linker, "/export:XInputOrd100=" XI "#100,@100,NONAME")
#pragma comment(linker, "/export:XInputOrd101=" XI "#101,@101,NONAME")
#pragma comment(linker, "/export:XInputOrd102=" XI "#102,@102,NONAME")
#pragma comment(linker, "/export:XInputOrd103=" XI "#103,@103,NONAME")

// ---------------------------------------------------------------- image info
static HMODULE g_self;
static unsigned char *g_base;
static unsigned char *g_text; static size_t g_textLen;
static struct { unsigned char *p; size_t n; } g_data[16]; static int g_dataCount;

static void InitImage(unsigned char *base)
{
    g_base = base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (!memcmp(s->Name, ".text", 6)) { g_text = g_base + s->VirtualAddress; g_textLen = s->Misc.VirtualSize; }
        else if ((s->Characteristics & IMAGE_SCN_MEM_READ) && !(s->Characteristics & IMAGE_SCN_MEM_EXECUTE) && g_dataCount < 16) {
            g_data[g_dataCount].p = g_base + s->VirtualAddress; g_data[g_dataCount].n = s->Misc.VirtualSize; g_dataCount++;
        }
    }
}

static unsigned char *Rip(unsigned char *insEnd, unsigned char *dispAt) { return insEnd + *(int *)dispAt; }

// ---------------------------------------------------------------- SSR patch
static const unsigned char kSig[] = {
    0x8B,0x50,0x04,0x85,0xD2,0x79,0x05,0x41,0x8B,0xC0,0xEB,0x07,0x3B,0xD7,0x8B,0xC7,
    0x0F,0x4C,0xC2,0x3B,0xC8,0x0F,0x4E,0xC1,0x83,0xF8,0x01,0x7D,0x08,0x41,0xBC,0x01,
    0x00,0x00,0x00 };
#define PATCH_OFS 19
#define PATCH_LEN 5
static const unsigned char kNop5[PATCH_LEN] = { 0x0F,0x1F,0x44,0x00,0x00 };
static unsigned char *g_patchSite;
static char g_patchMsg[256];

static void PatchSSR(void)
{
    if (!g_text) { strcpy_s(g_patchMsg, sizeof g_patchMsg, "NOT patched (no .text section)"); return; }
    unsigned char alt[sizeof kSig]; memcpy(alt, kSig, sizeof kSig); memcpy(alt + PATCH_OFS, kNop5, PATCH_LEN);
    unsigned char *hit = NULL; int count = 0;
    for (size_t i = 0; i + sizeof kSig <= g_textLen; i++) {
        if (g_text[i] != 0x8B) continue;
        if (!memcmp(g_text + i, kSig, sizeof kSig) || !memcmp(g_text + i, alt, sizeof kSig)) { if (!hit) hit = g_text + i; count++; }
    }
    if (count != 1) {
        sprintf_s(g_patchMsg, sizeof g_patchMsg, "NOT patched - signature found %d times (need 1). Game updated?", count);
        return;
    }
    unsigned char *site = hit + PATCH_OFS; DWORD old;
    if (memcmp(site, kNop5, PATCH_LEN)) {
        if (!VirtualProtect(site, PATCH_LEN, PAGE_EXECUTE_READWRITE, &old)) { strcpy_s(g_patchMsg, sizeof g_patchMsg, "NOT patched (VirtualProtect failed)"); return; }
        memcpy(site, kNop5, PATCH_LEN);
        VirtualProtect(site, PATCH_LEN, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, PATCH_LEN);
    }
    g_patchSite = site;
    sprintf_s(g_patchMsg, sizeof g_patchMsg, "patched at exe+0x%llX", (unsigned long long)(site - g_base));
}

// ---------------------------------------------------------------- AO override
// Hook point: the engine's "finalise post-process settings" code (FSceneView,
// rbx = view), right where it applies r.Tonemapper.Quality:
//   mov rax,[rip+X] (X = r.Tonemapper.Quality data) ; mov ecx,[rax] ; cmp ecx,1 ;
//   jge +6 ; mov [rbx+0F4Ch],esi ; cmp ecx,2 ; jge +6 ; mov [rbx+1150h],esi ...
// AO fields in FFinalPostProcessSettings (verified in the AO pass-count and
// AO shader-parameter code): Intensity 115Ch, StaticFraction 1160h,
// Radius 1164h, Power 1178h.
static const unsigned char kAoSig[] = {
    0x48,0x8B,0x05,0,0,0,0, 0x8B,0x08, 0x83,0xF9,0x01, 0x7D,0x06, 0x89,0xB3,0x4C,0x0F,0x00,0x00,
    0x83,0xF9,0x02, 0x7D,0x06, 0x89,0xB3,0x50,0x11,0x00,0x00 };
// Overridden FFinalPostProcessSettings fields. AO offsets verified in the AO
// pass-count + AO shader-parameter code; SSR Intensity/MaxRoughness verified in
// the SSR shader-parameter code (reads 124Ch then 1244h).
// FinalPostProcessSettings sits at view+0xD60; the UE reflection data in the exe gives the
// FPostProcessSettings offsets (Vignette 3F0h, AO Intensity 3FCh, SSR Intensity 4E4h,
// AO FadeDistance 40Ch, FadeRadius 410h, IndirectLightingIntensity 440h), which match.
// v23: eye adaptation (auto-exposure). FPostProcessSettings offsets from the exe's reflection data (property params:
// name ptr, then the offset at +0x28): Bias 31Ch, Method 320h, MinBrightness 32Ch, MaxBrightness 330h, SpeedUp 334h,
// SpeedDown 338h (cross-checked: the same lookup gives AO Intensity 3FCh and SSR Intensity 4E4h). +0xD60 for the view.
// UE: Min == Max = exposure locked; lowering Min lets dark rooms brighten over time. SpeedDown = bright -> dark.
#define AO_FIELDS 13
#define F_AO_INT 0
#define F_LM_INT 8
#define F_EX_MIN 9
#define F_EX_MAX 10
static const unsigned int  kAoOff[AO_FIELDS]  = { 0x115C, 0x1164, 0x1178, 0x1160, 0x116C, 0x1170, 0x1244, 0x124C, 0x11A0,
                                                  0x108C, 0x1090, 0x1094, 0x1098 };
static const wchar_t      *kAoSec[AO_FIELDS]  = { L"AmbientOcclusion", L"AmbientOcclusion", L"AmbientOcclusion", L"AmbientOcclusion", L"AmbientOcclusion", L"AmbientOcclusion", L"Reflections", L"Reflections", L"Lighting",
                                                  L"Exposure", L"Exposure", L"Exposure", L"Exposure" };
static const wchar_t      *kAoKey[AO_FIELDS]  = { L"Intensity", L"Radius", L"Power", L"StaticFraction", L"FadeDistance", L"FadeRadius", L"Intensity", L"MaxRoughness", L"IndirectLightingIntensity",
                                                  L"MinBrightness", L"MaxBrightness", L"SpeedUp", L"SpeedDown" };
static const wchar_t      *kAoDef[AO_FIELDS]  = { L"0.5", L"200", L"2", L"-1", L"8000", L"5000", L"100", L"0.6", L"-1",
                                                  L"0.17", L"-1", L"-1", L"3" };
static const char         *kAoLbl[AO_FIELDS]  = { "AO Intensity", "AO Radius", "AO Power", "AO StaticFraction", "AO FadeDistance", "AO FadeRadius", "SSR Intensity", "SSR MaxRoughness", "Lightmap/GI scale",
                                                  "Exposure min", "Exposure max", "Exposure speed up", "Exposure speed down" };

// Hotkey toggles (game window focused only)
static volatile LONG g_lmOff, g_aoOff;
// v24: F5 = "compare to the original game". While on, every live mod effect is off (post-process overrides incl.
// exposure, baked fill, real-time static lights -> BAKED, sun cascades, SSR unlock, volumetric fog, contact shadows,
// lamp shadow cvars). Creation-time changes (Stationary lights' real-time shadows, ForceRealtimeShadows, sun
// distribution/resolution/sharpen) stay until a level loads with the mod off. FOV and Engine.ini are not touched.
static volatile LONG g_vanilla;
static UINT g_vkCompare = VK_F5;
// Set once the engine renders views (AO hook hits or Present frames): scalability and the game's own settings are
// applied by then, so the "game default" captured for a cvar really is the game's value.
static volatile LONG g_engineUp;
#define LM_OFF_EFF (g_lmOff && !g_vanilla)
static UINT g_vkLm = VK_F8, g_vkAo = VK_F9, g_vkOverlay = VK_F10;
static UINT g_vkFog = VK_F6, g_vkContact = VK_F7;          // live A/B switches (not saved to the ini)
static volatile LONG g_fogHotOff, g_contactHotOff;           // 1 = forced off by the hotkey this session
static volatile LONG g_showOverlay = 0; static int g_downOv;   // 1 = pinned on (F10); else auto-hide
#define OVERLAY_SHOW_MS 5000
static volatile LONG g_ovFlash = 1;   // 1 = show for OVERLAY_SHOW_MS from the next time the game is in front
static UINT g_vkMenu = VK_F1;
static volatile LONG g_menuOpen, g_menuSel;
static void ReadAoConfig(void);
static void ApplyLampShadows(void);
static void ApplyVSync(void);
static void ApplyGraphics(void);
static void SyncRestartSettings(void);
static unsigned char *g_setResHolder;           // display mode (defined with the F1 menu code)
static volatile LONG g_dispMode;
static char g_dispMsg[160];
static void FindSetRes(void);
static int SavedDisplayMode(void);
static int StartDisplayMode(void);
static void SetOrRestore(const wchar_t *name, double v);
static volatile LONG g_cvarsReady;

// Real-time static lights. FLightSceneProxy constructor (rdi = ULightComponent):
//   movups xmm0,[rdi+238h] (LightGuid) ... cmp byte [rdi+1BEh],2 (Mobility==Movable) ; sete cl ...
// then bStaticLighting = Owner && Mobility==Static, bStaticShadowing = Owner && Mobility!=Movable.
// With [Lighting] RealtimeStaticLights=1 the stub sets a Static light's Mobility to Movable right
// before those reads, so the engine builds it as a normal dynamic light (live light + shadows).
static const unsigned char kRtlSig[] = {
    0x0F,0x10,0x87,0x38,0x02,0x00,0x00, 0x8B,0x83,0x38,0x01,0x00,0x00, 0x48,0x89,0xB3,0x30,0x01,0x00,0x00,
    0x0F,0x11,0x83,0xF0,0x00,0x00,0x00, 0x80,0xBF,0xBE,0x01,0x00,0x00,0x02, 0x0F,0x94,0xC1, 0x83,0xE0,0xFE,
    0x0B,0xC8, 0x89,0x8B,0x38,0x01,0x00,0x00 };
#define RTL_SITE 0x1B
typedef struct { volatile LONG enabled; volatile LONG converted; volatile LONG seen; } RtlData;
static RtlData *g_rtl;
static char g_rtlMsg[160] = "not installed";

// Field of view. FMinimalViewInfo::CalculateProjectionMatrixGivenView (player view projection):
//   movss xmm1,[rcx+18h] (ViewInfo.FOV, horizontal) ; maxss xmm1,[0.001] ; mov eax,[r9+8] ... mulss xmm1,[pi/360]
// The stub multiplies the FOV by [Camera] FOV / BaseFOV (so the game's own FOV changes stay proportional).
static const unsigned char kFovSig1[] = { 0xF3,0x0F,0x10,0x49,0x18, 0xF3,0x0F,0x5F,0x0D };
static const unsigned char kFovSig2[] = { 0x41,0x8B,0x41,0x08, 0x41,0x8B,0x49,0x0C, 0x41,0x2B,0x01, 0x41,0x2B,0x49,0x04, 0xF3,0x0F,0x59,0x0D };
typedef struct { volatile float scale; volatile float lastOrig; volatile LONG hits; } FovData;
static FovData *g_fov;
static char g_fovMsg[160] = "not installed";
static volatile float g_fovTarget = -1.0f, g_fovBase = 90.0f;
static volatile LONG g_fovVertical;              // [Camera] FOVMode: 0 = horizontal (UE's native), 1 = vertical
static volatile LONG g_bbW, g_bbH;               // back-buffer size (filled by the Present hook; defined again below)
static float FovAspect(void) { return (g_bbW > 0 && g_bbH > 0) ? (float)g_bbW / (float)g_bbH : 16.0f / 9.0f; }
static float FovHfromV(float v, float a) { return 2.0f * atanf(tanf(v * 3.14159265f / 360.0f) * a) * 180.0f / 3.14159265f; }
static float FovVfromH(float h, float a) { return 2.0f * atanf(tanf(h * 3.14159265f / 360.0f) / a) * 180.0f / 3.14159265f; }

// AO pass-count diagnostic: exit of ComputeAmbientOcclusionPassCount (eax = passes).
static const unsigned char kAoPassSig[] = {
    0x03,0xD7,0x85,0xC9,0x0F,0x49,0xD1,0x3B,0xD0,0x0F,0x46,0xC2,0xEB,0x02,0x8B,0xC7,
    0x48,0x8B,0x5C,0x24,0x40,0x48,0x8B,0x74,0x24,0x48,0x0F,0x28,0x74,0x24,0x20,0x48,0x83,0xC4,0x30,0x5F,0xC3 };
#define AOPASS_SITE 0x10
typedef struct { volatile LONG passes; volatile LONG calls; } AoPassData;
static AoPassData *g_aoPass;
static char g_aoPassMsg[160] = "not installed";

// Sun shadow cascades. FDirectionalLightSceneProxy "GetNumShadowMappedCascades":
//   mov edi,[rcx+194h] (light's DynamicShadowCascades) ... return min(that, view max)
// The maps' sun lights use 1 cascade. We replace the read with the ini value
// (only for lights that have >0 cascades; the view max / r.Shadow.CSM.MaxCascades still caps it).
static const unsigned char kCsmSig[] = {
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7C,0x24,0x20,
    0x41,0x56,0x48,0x83,0xEC,0x20,0x8B,0xB9,0x94,0x01,0x00,0x00,0x33,0xDB,0x45,0x0F,0xB6,0xF0,0x8B,0xEA,
    0x48,0x8B,0xF1,0x45,0x84,0xC0,0x75 };
#define CSM_SITE 0x1A
typedef struct { volatile LONG cfg; volatile LONG map; volatile LONG hits; } CsmData;
static CsmData *g_csm;
static char g_csmMsg[200] = "not installed";
typedef struct {
    volatile float cfg[AO_FIELDS];      // < 0 = keep the map's value
    volatile float map[AO_FIELDS];      // value the map/volumes asked for (last view)
    volatile LONG  hits;                // times the hook ran (~ views rendered)
    LONG pad;
    unsigned char *holder;              // original rip target X
} AoData;
static AoData *g_ao;
static unsigned char *g_aoSite, *g_aoHolder;
static char g_aoMsg[200] = "not installed";
static wchar_t g_cfgPath[MAX_PATH];

static unsigned char *AllocNear(unsigned char *target, size_t size)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    ULONG_PTR gran = si.dwAllocationGranularity;
    ULONG_PTR lo = (ULONG_PTR)target > 0x70000000 ? (ULONG_PTR)target - 0x70000000 : gran;
    ULONG_PTR hi = (ULONG_PTR)target + 0x70000000;
    for (ULONG_PTR a = ((ULONG_PTR)target & ~(gran - 1)); a > lo; a -= gran) {
        void *p = VirtualAlloc((void *)a, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    for (ULONG_PTR a = ((ULONG_PTR)target & ~(gran - 1)) + gran; a < hi; a += gran) {
        void *p = VirtualAlloc((void *)a, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    return NULL;
}

static void Rel32(unsigned char **pp, unsigned char *target, int extra)
{   // writes rel32 for an instruction whose remaining length after this field is 'extra'
    *(int *)*pp = (int)(target - (*pp + 4 + extra)); *pp += 4;
}

static void InstallAoHook(void)
{
    if (!g_text) return;
    unsigned char *hit = NULL; int count = 0;
    for (size_t i = 0; i + sizeof kAoSig <= g_textLen; i++) {
        if (g_text[i] != 0x48 || g_text[i + 1] != 0x8B || g_text[i + 2] != 0x05) continue;
        if (!memcmp(g_text + i + 7, kAoSig + 7, sizeof kAoSig - 7)) { if (!hit) hit = g_text + i; count++; }
    }
    if (count != 1) { sprintf_s(g_aoMsg, sizeof g_aoMsg, "NOT installed - signature found %d times (need 1)", count); return; }
    unsigned char *stub = AllocNear(hit, 0x1000);
    if (!stub) { strcpy_s(g_aoMsg, sizeof g_aoMsg, "NOT installed - no memory near the exe"); return; }
    AoData *ao = (AoData *)(stub + 0x800);
    for (int f = 0; f < AO_FIELDS; f++) { ao->cfg[f] = -1.0f; ao->map[f] = -1.0f; }
    ao->holder = Rip(hit + 7, hit + 3);

    unsigned char *p = stub;
    for (int f = 0; f < AO_FIELDS; f++) {
        *p++ = 0x8B; *p++ = 0x83; *(unsigned int *)p = kAoOff[f]; p += 4;           // mov eax,[rbx+off]
        *p++ = 0x89; *p++ = 0x05; Rel32(&p, (unsigned char *)&ao->map[f], 0);       // mov [rip+map],eax
        *p++ = 0x8B; *p++ = 0x05; Rel32(&p, (unsigned char *)&ao->cfg[f], 0);       // mov eax,[rip+cfg]
        *p++ = 0x85; *p++ = 0xC0;                                                    // test eax,eax
        *p++ = 0x78; *p++ = 0x06;                                                    // js +6
        *p++ = 0x89; *p++ = 0x83; *(unsigned int *)p = kAoOff[f]; p += 4;           // mov [rbx+off],eax
    }
    *p++ = 0xFF; *p++ = 0x05; Rel32(&p, (unsigned char *)&ao->hits, 0);             // inc dword [rip+hits]
    *p++ = 0x48; *p++ = 0x8B; *p++ = 0x05; Rel32(&p, (unsigned char *)&ao->holder, 0); // mov rax,[rip+holder]
    *p++ = 0x48; *p++ = 0x8B; *p++ = 0x00;                                           // mov rax,[rax]
    *p++ = 0xE9; Rel32(&p, hit + 7, 0);                                              // jmp back
    FlushInstructionCache(GetCurrentProcess(), stub, (SIZE_T)(p - stub));

    unsigned char jmp[7] = { 0xE9, 0, 0, 0, 0, 0x66, 0x90 };
    *(int *)(jmp + 1) = (int)(stub - (hit + 5));
    DWORD old;
    if (!VirtualProtect(hit, 7, PAGE_EXECUTE_READWRITE, &old)) { strcpy_s(g_aoMsg, sizeof g_aoMsg, "NOT installed (VirtualProtect)"); return; }
    memcpy(hit, jmp, 7);
    VirtualProtect(hit, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), hit, 7);
    g_ao = ao; g_aoSite = hit; g_aoHolder = ao->holder;
    sprintf_s(g_aoMsg, sizeof g_aoMsg, "installed at exe+0x%llX", (unsigned long long)(hit - g_base));
}

static void InstallCsmHook(void)
{
    if (!g_text) return;
    unsigned char *hit = NULL; int count = 0;
    for (size_t i = 0; i + sizeof kCsmSig <= g_textLen; i++) {
        if (g_text[i] == 0x48 && !memcmp(g_text + i, kCsmSig, sizeof kCsmSig)) { if (!hit) hit = g_text + i; count++; }
    }
    if (count != 1) { sprintf_s(g_csmMsg, sizeof g_csmMsg, "NOT installed - signature found %d times (need 1)", count); return; }
    unsigned char *site = hit + CSM_SITE;
    unsigned char *stub = AllocNear(site, 0x1000);
    if (!stub) { strcpy_s(g_csmMsg, sizeof g_csmMsg, "NOT installed - no memory near the exe"); return; }
    CsmData *cd = (CsmData *)(stub + 0x800);
    cd->cfg = -1; cd->map = -1; cd->hits = 0;
    unsigned char *p = stub, *j1, *j2;
    *p++ = 0x8B; *p++ = 0xB9; *(unsigned int *)p = 0x194; p += 4;                 // mov edi,[rcx+194h]
    *p++ = 0x89; *p++ = 0x3D; Rel32(&p, (unsigned char *)&cd->map, 0);           // mov [rip+map],edi
    *p++ = 0xFF; *p++ = 0x05; Rel32(&p, (unsigned char *)&cd->hits, 0);          // inc dword [rip+hits]
    *p++ = 0x85; *p++ = 0xFF;                                                     // test edi,edi
    *p++ = 0x7E; j1 = p++;                                                        // jle back
    *p++ = 0x83; *p++ = 0x3D; Rel32(&p, (unsigned char *)&cd->cfg, 1); *p++ = 0;  // cmp dword [rip+cfg],0
    *p++ = 0x7E; j2 = p++;                                                        // jle back
    *p++ = 0x8B; *p++ = 0x3D; Rel32(&p, (unsigned char *)&cd->cfg, 0);           // mov edi,[rip+cfg]
    *j1 = (unsigned char)(p - (j1 + 1)); *j2 = (unsigned char)(p - (j2 + 1));
    *p++ = 0xE9; Rel32(&p, site + 6, 0);                                          // back: jmp site+6
    FlushInstructionCache(GetCurrentProcess(), stub, (SIZE_T)(p - stub));

    unsigned char jmp[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
    *(int *)(jmp + 1) = (int)(stub - (site + 5));
    DWORD old;
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) { strcpy_s(g_csmMsg, sizeof g_csmMsg, "NOT installed (VirtualProtect)"); return; }
    memcpy(site, jmp, 6);
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    g_csm = cd;
    sprintf_s(g_csmMsg, sizeof g_csmMsg, "installed at exe+0x%llX", (unsigned long long)(site - g_base));
}

static void InstallFovHook(void)
{
    if (!g_text) return;
    unsigned char *hit = NULL; int count = 0;
    for (size_t i = 0; i + 40 <= g_textLen; i++) {
        unsigned char *q = g_text + i;
        if (q[0] == 0xF3 && !memcmp(q, kFovSig1, sizeof kFovSig1) && !memcmp(q + 13, kFovSig2, sizeof kFovSig2)) { if (!hit) hit = q; count++; }
    }
    if (count != 1) { sprintf_s(g_fovMsg, sizeof g_fovMsg, "NOT installed - signature found %d times", count); return; }
    unsigned char *stub = AllocNear(hit, 0x1000);
    if (!stub) { strcpy_s(g_fovMsg, sizeof g_fovMsg, "NOT installed - no memory near the exe"); return; }
    FovData *fd = (FovData *)(stub + 0x800); fd->scale = 0.0f; fd->lastOrig = 0.0f; fd->hits = 0;
    unsigned char *p = stub;
    memcpy(p, hit, 5); p += 5;                                                        // movss xmm1,[rcx+18h] (original)
    *p++ = 0xF3; *p++ = 0x0F; *p++ = 0x11; *p++ = 0x0D; Rel32(&p, (unsigned char *)&fd->lastOrig, 0); // movss [rip+lastOrig],xmm1
    *p++ = 0xFF; *p++ = 0x05; Rel32(&p, (unsigned char *)&fd->hits, 0);              // inc dword [rip+hits]
    *p++ = 0x8B; *p++ = 0x05; Rel32(&p, (unsigned char *)&fd->scale, 0);             // mov eax,[rip+scale]
    *p++ = 0x85; *p++ = 0xC0;                                                         // test eax,eax
    *p++ = 0x7E; *p++ = 0x08;                                                         // jle skip (scale <= 0: off)
    *p++ = 0xF3; *p++ = 0x0F; *p++ = 0x59; *p++ = 0x0D; Rel32(&p, (unsigned char *)&fd->scale, 0);    // mulss xmm1,[rip+scale]
    *p++ = 0xE9; Rel32(&p, hit + 5, 0);                                               // skip: jmp back
    FlushInstructionCache(GetCurrentProcess(), stub, (SIZE_T)(p - stub));
    unsigned char jmp[5] = { 0xE9 }; *(int *)(jmp + 1) = (int)(stub - (hit + 5));
    DWORD old;
    if (!VirtualProtect(hit, 5, PAGE_EXECUTE_READWRITE, &old)) { strcpy_s(g_fovMsg, sizeof g_fovMsg, "NOT installed (VirtualProtect)"); return; }
    memcpy(hit, jmp, 5);
    VirtualProtect(hit, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), hit, 5);
    g_fov = fd;
    sprintf_s(g_fovMsg, sizeof g_fovMsg, "installed at exe+0x%llX", (unsigned long long)(hit - g_base));
}
static void ApplyFov(void)
{
    if (!g_fov) return;
    float t = g_fovTarget, b = g_fovBase > 1.0f ? g_fovBase : 90.0f;
    if (t > 1.0f && g_fovVertical) t = FovHfromV(t, FovAspect());   // the projection uses horizontal FOV
    g_fov->scale = (t > 1.0f) ? t / b : 0.0f;
}

static void InstallAoPassHook(void)
{
    if (!g_text) return;
    unsigned char *hit = NULL; int count = 0;
    for (size_t i = 0; i + sizeof kAoPassSig <= g_textLen; i++) {
        if (g_text[i] == 0x03 && !memcmp(g_text + i, kAoPassSig, sizeof kAoPassSig)) { if (!hit) hit = g_text + i; count++; }
    }
    if (count != 1) { sprintf_s(g_aoPassMsg, sizeof g_aoPassMsg, "NOT installed - signature found %d times", count); return; }
    unsigned char *site = hit + AOPASS_SITE;
    unsigned char *stub = AllocNear(site, 0x1000);
    if (!stub) { strcpy_s(g_aoPassMsg, sizeof g_aoPassMsg, "NOT installed - no memory near the exe"); return; }
    AoPassData *pd = (AoPassData *)(stub + 0x800); pd->passes = -1; pd->calls = 0;
    unsigned char *p = stub;
    *p++ = 0x89; *p++ = 0x05; Rel32(&p, (unsigned char *)&pd->passes, 0);   // mov [rip+passes],eax
    *p++ = 0xFF; *p++ = 0x05; Rel32(&p, (unsigned char *)&pd->calls, 0);    // inc dword [rip+calls]
    *p++ = 0x48; *p++ = 0x8B; *p++ = 0x5C; *p++ = 0x24; *p++ = 0x40;         // mov rbx,[rsp+40h] (original)
    *p++ = 0xE9; Rel32(&p, site + 5, 0);                                     // jmp back
    FlushInstructionCache(GetCurrentProcess(), stub, (SIZE_T)(p - stub));
    unsigned char jmp[5] = { 0xE9 }; *(int *)(jmp + 1) = (int)(stub - (site + 5));
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) { strcpy_s(g_aoPassMsg, sizeof g_aoPassMsg, "NOT installed (VirtualProtect)"); return; }
    memcpy(site, jmp, 5);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    g_aoPass = pd;
    sprintf_s(g_aoPassMsg, sizeof g_aoPassMsg, "installed at exe+0x%llX", (unsigned long long)(site - g_base));
}

// ---- converted-light tracking (proxy colour at +0xC0; black = engine skips light + shadows)
#define PROXY_COLOR 0xC0
#define MAX_CONV 4096
typedef struct {
    unsigned char *proxy; void *vt; float color[4]; int ready;
    int local; float createdI, createdR;      // scales already baked in when the light was converted
    float radius; int haveRadius;             // proxy radius (+0x178) after the derived ctor ran
} Conv;
#define PROXY_RADIUS 0x178
#define PROXY_INVRADIUS 0x17C
static Conv g_conv[MAX_CONV]; static int g_convN;
static SRWLOCK g_convLock = SRWLOCK_INIT;
static volatile LONG g_convDestroyed, g_dtorHooks, g_ctorEndHooked;
static char g_swapMsg[200] = "not installed";
static unsigned char *g_swapStub, *g_swapEnd, *g_swapDtor0;

static void *g_baseVt;                         // FLightSceneProxy base vtable
static unsigned char *g_localRet[8]; static int g_nLocalRet;   // return addresses of point/spot/rect proxy ctors
static volatile float g_scaleI = 1.0f, g_scaleR = 1.0f;     // [Lighting] ConvertedBrightnessScale / ConvertedRadiusScale
static volatile LONG g_scaledR;
#define COMP_INTENSITY 0x24C   // ULightComponentBase::Intensity
#define COMP_ATTRADIUS 0x370   // ULocalLightComponent::AttenuationRadius

static void WriteColor(Conv *c, unsigned char *proxy)
{
    float *col = (float *)(proxy + PROXY_COLOR);
    if (LM_OFF_EFF) {
        float k = (c->createdI > 0.0f && g_scaleI > 0.0f) ? g_scaleI / c->createdI : 1.0f;
        col[0] = c->color[0] * k; col[1] = c->color[1] * k; col[2] = c->color[2] * k; col[3] = c->color[3];
    } else { col[0] = col[1] = col[2] = 0.0f; }
}
static void ApplyOne(Conv *c)
{
    if (!c->ready) return;
    void *v = *(void **)c->proxy;
    // The base ctor ends with the base vtable; the derived ctor then installs the real one.
    // Adopt that once, then require it to match (never touch a dead/reused object).
    if (!c->vt) {
        if (v == g_baseVt) return;
        c->vt = v;
        if (c->local) { c->radius = *(float *)(c->proxy + PROXY_RADIUS); c->haveRadius = 1; }
    }
    if (v != c->vt) return;
    WriteColor(c, c->proxy);
    if (c->haveRadius && c->createdR > 0.0f && g_scaleR > 0.0f) {
        float r = c->radius * (g_scaleR / c->createdR);
        *(float *)(c->proxy + PROXY_RADIUS) = r;
        *(float *)(c->proxy + PROXY_INVRADIUS) = 1.0f / (r > 0.0001f ? r : 0.0001f);
    }
}
// Converted components, so a later rebuild of the same light (new proxy from the same component) gets the
// *current* scale applied to the light's ORIGINAL values instead of the already-scaled ones.
// A component is identified by address + UObject InternalIndex (+0xC) + ClassPrivate (+0x10) (UE 4.20 UObjectBase).
typedef struct { unsigned char *comp; int index; void *cls; float origI, origR, lastI, lastR; int local; } CompRec;
#define MAX_COMP 4096
static CompRec g_comp[MAX_COMP]; static int g_compN;
static volatile LONG g_rebuilt, g_statConverted;
static volatile LONG g_stationaryRT;            // [Lighting] RealtimeStationaryShadows
static volatile LONG g_forceShadows;            // [Lighting] ForceRealtimeShadows: 0 off, 1 converted lights, 2 all lights
static volatile LONG g_forced;
#define PROXY_COMP 0x10
#define COMP_SHADOWFLAGS 0x254                  // bit 0x2 CastShadows, 0x4 CastStaticShadow, 0x8 CastDynamicShadow
#define PROXY_BITS 0x138                        // bit 0x8 = bCastDynamicShadow
#define OBJ_INDEX 0x0C
#define OBJ_CLASS 0x10
// shadow statistics per light category (this session)
enum { CAT_STATIC_RT, CAT_STATIC_KEPT, CAT_STATIONARY, CAT_STATIONARY_RT, CAT_MOVABLE, CAT_N };
static volatile LONG g_catN[CAT_N], g_catDyn[CAT_N];
static __declspec(thread) unsigned char *t_proxy; static __declspec(thread) int t_cat, t_dir;
// Sun (directional light) shadow settings, applied as the light is built (next level load)
static unsigned char *g_dirRet;                 // return address of the directional proxy ctor's base-ctor call
static volatile float g_sunRes = -1.0f, g_sunDist = -1.0f, g_sunSharp = -1.0f;   // -1 = map value
static volatile LONG g_sunApplied;
#define COMP_CASCADE_EXP 0x390    // UDirectionalLightComponent::CascadeDistributionExponent (read by the derived ctor)
#define PROXY_SHADOWRES  0xD8     // FLightSceneProxy copy of ShadowResolutionScale (component +0x280)
#define PROXY_SHARPEN    0xE0     // FLightSceneProxy copy of ShadowSharpen (component +0x288)
#define PROXY_CONTACT    0xE4     // FLightSceneProxy copy of ContactShadowLength (component +0x28C)
static volatile float g_contactLen = -1.0f;     // [Shadows] ContactShadowLength: the sun (directional) only, -1 = as authored
static volatile float g_lampContactLen = -1.0f; // [Shadows] LampContactShadowLength: every other light, -1 = as authored
// v20: contact shadows on lamps made lamp light seen through windows fade in and out (screen-space ray length)
static volatile LONG g_contactApplied;

static CompRec *FindComp(unsigned char *comp)
{
    for (int i = 0; i < g_compN; i++)
        if (g_comp[i].comp == comp && g_comp[i].index == *(int *)(comp + OBJ_INDEX) && g_comp[i].cls == *(void **)(comp + OBJ_CLASS))
            return &g_comp[i];
    return NULL;
}
static void TrackProxy(unsigned char *proxy, int local, float si, float sr)
{
    int i; for (i = 0; i < g_convN && g_conv[i].proxy != proxy; i++) { }
    if (i == g_convN && g_convN < MAX_CONV) g_convN++;
    if (i < MAX_CONV) {
        Conv *c = &g_conv[i]; memset(c, 0, sizeof *c);
        c->proxy = proxy; c->local = local; c->createdI = si; c->createdR = local ? sr : 1.0f;
    }
}
// Called for EVERY light proxy, inside the base proxy ctor, before it reads Mobility/Intensity and before the
// derived ctor reads AttenuationRadius.
static void __cdecl CbProxyStart(unsigned char *proxy, unsigned char *comp, unsigned char *ret)
{
    unsigned char mob = comp[0x1BE];
    int local = 0; for (int k = 0; k < g_nLocalRet; k++) if (ret == g_localRet[k]) local = 1;
    float si = g_scaleI > 0.0f ? g_scaleI : 1.0f, sr = g_scaleR > 0.0f ? g_scaleR : 1.0f;
    float *pI = (float *)(comp + COMP_INTENSITY), *pR = (float *)(comp + COMP_ATTRADIUS);
    t_proxy = proxy;
    t_dir = (g_dirRet && ret == g_dirRet);
    if (t_dir && g_sunDist > 0.0f) *(float *)(comp + COMP_CASCADE_EXP) = g_sunDist;   // absolute value: idempotent on rebuilds
    AcquireSRWLockExclusive(&g_convLock);
    if (mob == 0 && g_rtl && g_rtl->enabled) {                         // Static -> real-time
        comp[0x1BE] = 2; InterlockedIncrement(&g_rtl->converted);
        CompRec *r = FindComp(comp);
        if (!r && g_compN < MAX_COMP) r = &g_comp[g_compN++];
        if (r) {
            r->comp = comp; r->index = *(int *)(comp + OBJ_INDEX); r->cls = *(void **)(comp + OBJ_CLASS);
            r->origI = *pI; r->origR = local ? *pR : 0.0f; r->local = local;
            *pI = r->origI * si; r->lastI = *pI;
            if (local) { *pR = r->origR * sr; r->lastR = *pR; InterlockedIncrement(&g_scaledR); }
        }
        TrackProxy(proxy, local, si, sr);
        t_cat = CAT_STATIC_RT;
    } else if (mob == 2 && FindComp(comp)) {                           // rebuild of a converted light
        CompRec *r = FindComp(comp);
        if (*pI != r->lastI) r->origI = *pI;                          // a script set a new (unscaled) value
        if (r->local && *pR != r->lastR) r->origR = *pR;
        *pI = r->origI * si; r->lastI = *pI;
        if (r->local) { *pR = r->origR * sr; r->lastR = *pR; }
        TrackProxy(proxy, r->local, si, sr);
        InterlockedIncrement(&g_rebuilt);
        t_cat = CAT_STATIC_RT;
    } else if (mob == 1 && g_stationaryRT) {                           // Stationary -> fully real-time shadows
        comp[0x1BE] = 2; InterlockedIncrement(&g_statConverted);
        t_cat = CAT_STATIONARY_RT;
    } else {
        t_cat = mob == 0 ? CAT_STATIC_KEPT : mob == 1 ? CAT_STATIONARY : CAT_MOVABLE;
    }
    ReleaseSRWLockExclusive(&g_convLock);
}
static void __cdecl CbCtorDone(unsigned char *proxy)
{
    // shadow statistics: proxy+0x138 bit3 = bCastDynamicShadow (CastShadows && CastDynamicShadow)
    if (t_proxy == proxy) {
        int cat = t_cat; t_proxy = NULL;
        unsigned int *bits = (unsigned int *)(proxy + PROXY_BITS);
        unsigned char *comp = *(unsigned char **)(proxy + PROXY_COMP);
        int force = 0;
        if (g_forceShadows >= 2) force = 1;
        else if (g_forceShadows == 1 && (cat == CAT_STATIC_RT || cat == CAT_STATIONARY_RT) && comp &&
                 (*(unsigned int *)(comp + COMP_SHADOWFLAGS) & 6) == 6) force = 1;   // relied on baked shadows
        if (force && !(*bits & 8)) { *bits |= 8; InterlockedIncrement(&g_forced); }
        {
            float len = t_dir ? g_contactLen : g_lampContactLen;
            if (len >= 0.0f) { *(float *)(proxy + PROXY_CONTACT) = len; InterlockedIncrement(&g_contactApplied); }
        }
        if (t_dir) {
            if (g_sunRes > 0.0f) *(float *)(proxy + PROXY_SHADOWRES) = g_sunRes;
            if (g_sunSharp >= 0.0f) *(float *)(proxy + PROXY_SHARPEN) = g_sunSharp;
            InterlockedIncrement(&g_sunApplied);
        }
        InterlockedIncrement(&g_catN[cat]);
        if (*bits & 8) InterlockedIncrement(&g_catDyn[cat]);
    }
    AcquireSRWLockExclusive(&g_convLock);
    for (int i = 0; i < g_convN; i++) if (g_conv[i].proxy == proxy) {
        Conv *c = &g_conv[i];
        memcpy(c->color, proxy + PROXY_COLOR, sizeof c->color);   // already includes createdI
        c->vt = NULL; c->ready = 1;
        WriteColor(c, proxy);          // still inside the constructor: object is alive
        break;
    }
    ReleaseSRWLockExclusive(&g_convLock);
}
static void __cdecl CbDestroyed(unsigned char *proxy)
{
    AcquireSRWLockExclusive(&g_convLock);
    for (int i = 0; i < g_convN; i++) if (g_conv[i].proxy == proxy) {
        g_conv[i] = g_conv[--g_convN]; InterlockedIncrement(&g_convDestroyed); break;
    }
    ReleaseSRWLockExclusive(&g_convLock);
}
static void ApplyAllConverted(void)
{
    AcquireSRWLockExclusive(&g_convLock);
    for (int i = 0; i < g_convN; i++) ApplyOne(&g_conv[i]);
    ReleaseSRWLockExclusive(&g_convLock);
}
static int ConvCount(int *ready)
{
    AcquireSRWLockShared(&g_convLock);
    int n = g_convN, r = 0; for (int i = 0; i < n; i++) r += g_conv[i].ready;
    ReleaseSRWLockShared(&g_convLock);
    *ready = r; return n;
}

// Emits: save all volatile GPRs/XMM0-5, set args, call fn, restore.
// args: bit0 = rcx := rbx; bit1 = rdx := rdi and r8 := [original rsp + 48h] (return address of the base proxy ctor)
static unsigned char *EmitCallC(unsigned char *p, int argIsRbx, void *fn)
{
    static const unsigned char pre[] = {
        0x55, 0x48,0x89,0xE5, 0x48,0x83,0xE4,0xF0, 0x48,0x81,0xEC,0xC0,0x00,0x00,0x00,
        0x48,0x89,0x44,0x24,0x20, 0x48,0x89,0x4C,0x24,0x28, 0x48,0x89,0x54,0x24,0x30,
        0x4C,0x89,0x44,0x24,0x38, 0x4C,0x89,0x4C,0x24,0x40, 0x4C,0x89,0x54,0x24,0x48, 0x4C,0x89,0x5C,0x24,0x50,
        0xF3,0x0F,0x7F,0x44,0x24,0x60, 0xF3,0x0F,0x7F,0x4C,0x24,0x70,
        0xF3,0x0F,0x7F,0x94,0x24,0x80,0x00,0x00,0x00, 0xF3,0x0F,0x7F,0x9C,0x24,0x90,0x00,0x00,0x00,
        0xF3,0x0F,0x7F,0xA4,0x24,0xA0,0x00,0x00,0x00, 0xF3,0x0F,0x7F,0xAC,0x24,0xB0,0x00,0x00,0x00 };
    static const unsigned char post[] = {
        0xF3,0x0F,0x6F,0x44,0x24,0x60, 0xF3,0x0F,0x6F,0x4C,0x24,0x70,
        0xF3,0x0F,0x6F,0x94,0x24,0x80,0x00,0x00,0x00, 0xF3,0x0F,0x6F,0x9C,0x24,0x90,0x00,0x00,0x00,
        0xF3,0x0F,0x6F,0xA4,0x24,0xA0,0x00,0x00,0x00, 0xF3,0x0F,0x6F,0xAC,0x24,0xB0,0x00,0x00,0x00,
        0x48,0x8B,0x44,0x24,0x20, 0x48,0x8B,0x4C,0x24,0x28, 0x48,0x8B,0x54,0x24,0x30,
        0x4C,0x8B,0x44,0x24,0x38, 0x4C,0x8B,0x4C,0x24,0x40, 0x4C,0x8B,0x54,0x24,0x48, 0x4C,0x8B,0x5C,0x24,0x50,
        0x48,0x89,0xEC, 0x5D };
    memcpy(p, pre, sizeof pre); p += sizeof pre;
    if (argIsRbx & 4) { *p++ = 0x48; *p++ = 0x89; *p++ = 0xD1; }            // mov rcx,rdx
    if (argIsRbx & 1) { *p++ = 0x48; *p++ = 0x89; *p++ = 0xD9; }            // mov rcx,rbx
    if (argIsRbx & 2) {
        *p++ = 0x48; *p++ = 0x89; *p++ = 0xFA;                              // mov rdx,rdi
        *p++ = 0x4C; *p++ = 0x8B; *p++ = 0x45; *p++ = 0x50;                 // mov r8,[rbp+50h] (= [orig rsp+48h])
    }
    *p++ = 0x48; *p++ = 0xB8; *(void **)p = fn; p += 8;                    // mov rax,imm64
    *p++ = 0xFF; *p++ = 0xD0;                                               // call rax
    memcpy(p, post, sizeof post); p += sizeof post;
    return p;
}

static int PatchJmp(unsigned char *site, int len, unsigned char *to)
{
    unsigned char j[16]; j[0] = 0xE9; *(int *)(j + 1) = (int)(to - (site + 5));
    for (int i = 5; i < len; i++) j[i] = 0x90;
    DWORD old;
    if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(site, j, len);
    VirtualProtect(site, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    return 1;
}

static void InstallSwapHooks(unsigned char *rtlSite)
{
    // constructor end: "mov rax,rbx ; mov rbx,[rsp+50h] ; add rsp,40h ; pop rdi ; ret" at rtlSite+0x3F1
    static const unsigned char kEnd[] = { 0x48,0x8B,0xC3, 0x48,0x8B,0x5C,0x24,0x50, 0x48,0x83,0xC4,0x40, 0x5F, 0xC3 };
    unsigned char *end = rtlSite + 0x3F1;
    if (memcmp(end, kEnd, sizeof kEnd)) { strcpy_s(g_swapMsg, sizeof g_swapMsg, "NOT installed - constructor end bytes differ"); return; }
    // base vtable from the constructor's "lea rax,[rip+X]" at rtlSite-0xCF
    unsigned char *lea = rtlSite - 0xCF;
    if (lea[0] != 0x48 || lea[1] != 0x8D || lea[2] != 0x05) { strcpy_s(g_swapMsg, sizeof g_swapMsg, "NOT installed - base vtable lea not found"); return; }
    unsigned char *baseVt = Rip(lea + 7, lea + 3);
    g_baseVt = baseVt;
    {   // callers of the base ctor whose next ~0x60 bytes read AttenuationRadius: movss xmm1,[rdi+370h]
        unsigned char *ctorStart = rtlSite - 0xDE;
        static const unsigned char kRad[] = { 0xF3, 0x0F, 0x10, 0x8F, 0x70, 0x03, 0x00, 0x00 };
        // the directional ctor: the caller that next reads its own +0x36C..+0x38C fields (mov eax,[rbx+370h])
        static const unsigned char kDir[] = { 0x8B, 0x83, 0x70, 0x03, 0x00, 0x00 };
        for (size_t i = 0; i + 0x70 <= g_textLen && g_nLocalRet < 8; i++) {
            unsigned char *q = g_text + i;
            if (q[0] != 0xE8 || Rip(q + 5, q + 1) != ctorStart) continue;
            int isLocal = 0;
            for (int k = 5; k < 0x60; k++) if (!memcmp(q + k, kRad, sizeof kRad)) { g_localRet[g_nLocalRet++] = q + 5; isLocal = 1; break; }
            if (!isLocal) for (int k = 5; k < 0x60; k++) if (!memcmp(q + k, kDir, sizeof kDir)) { g_dirRet = q + 5; break; }
        }
    }
    // deleting destructors: push rbx ; sub rsp,20h ; lea rax,[baseVt] ; mov rbx,rcx ; mov [rcx],rax ; test dl,1
    unsigned char *dtors[8]; int nd = 0;
    for (size_t i = 0; i + 24 <= g_textLen && nd < 8; i++) {
        unsigned char *q = g_text + i;
        if (q[0] == 0x40 && q[1] == 0x53 && q[2] == 0x48 && q[3] == 0x83 && q[4] == 0xEC && q[5] == 0x20 &&
            q[6] == 0x48 && q[7] == 0x8D && q[8] == 0x05 && Rip(q + 13, q + 9) == baseVt &&
            q[13] == 0x48 && q[14] == 0x8B && q[15] == 0xD9 && q[16] == 0x48 && q[17] == 0x89 && q[18] == 0x01 && q[19] == 0xF6 && q[20] == 0xC2 && q[21] == 0x01)
            dtors[nd++] = q;
    }
    if (nd == 0) { strcpy_s(g_swapMsg, sizeof g_swapMsg, "NOT installed - no light proxy destructors found"); return; }
    unsigned char *stub = AllocNear(end, 0x2000);
    if (!stub) { strcpy_s(g_swapMsg, sizeof g_swapMsg, "NOT installed - no memory near the exe"); return; }
    unsigned char *p = stub;
    // ctor-end stub
    unsigned char *s1 = p;
    p = EmitCallC(p, 1, (void *)CbCtorDone);
    memcpy(p, end, 8); p += 8;                       // original mov rax,rbx ; mov rbx,[rsp+50h]
    *p++ = 0xE9; Rel32(&p, end + 8, 0);
    // destructor stubs
    unsigned char *ds[8];
    for (int k = 0; k < nd; k++) {
        ds[k] = p;
        p = EmitCallC(p, 0, (void *)CbDestroyed);
        memcpy(p, dtors[k], 6); p += 6;               // original push rbx ; sub rsp,20h
        *p++ = 0xE9; Rel32(&p, dtors[k] + 6, 0);
    }
    FlushInstructionCache(GetCurrentProcess(), stub, (SIZE_T)(p - stub));
    // destructors first (so nothing can be tracked without a destroy hook), then the ctor end
    for (int k = 0; k < nd; k++) if (PatchJmp(dtors[k], 6, ds[k])) g_dtorHooks++;
    if (g_dtorHooks == nd && PatchJmp(end, 8, s1)) g_ctorEndHooked = 1;
    g_swapStub = stub; g_swapEnd = end; g_swapDtor0 = dtors[0];
    sprintf_s(g_swapMsg, sizeof g_swapMsg, "%s (destructor hooks %ld/%d, constructor-end hook %s)",
              g_ctorEndHooked ? "installed" : "NOT installed", g_dtorHooks, nd, g_ctorEndHooked ? "ok" : "failed");
}

// ---- volumetric fog. FExponentialHeightFogSceneInfo ctor (rcx = this, rdx = UExponentialHeightFogComponent):
//   mov [rsp+8],rbx ; mov [rsp+10h],rbp ; ... reads FogDensity +238h, FogHeightFalloff +28Ch,
//   movzx eax,byte [rdx+29Ch] (bEnableVolumetricFog:1), VolumetricFogScatteringDistribution +2A0h ... Distance +2BCh
static const unsigned char kFogSig[] = { 0x48,0x89,0x5C,0x24,0x08, 0x57, 0x48,0x83,0xEC,0x30, 0xF3,0x0F,0x10,0x0D };
#define FOG_DENSITY  0x238
#define FOG_VOLBITS  0x29C
#define FOG_VOLDIST  0x2BC
#define FOG_EXTSCALE 0x2B8   // VolumetricFogExtinctionScale: volumetric-only density multiplier (ctor reads [rdi+2B8h])
// v21: DensityScale goes to ExtinctionScale, not FogDensity. Scaling FogDensity also thickened the normal height fog,
// so with volumetric fog off (F6) the extra fog stayed, only flat-coloured. Now OFF = the map's own fog.
typedef struct { unsigned char *comp; int index; void *cls; float origExt; float origDist; } FogRec;
static FogRec g_fogRec[256]; static int g_fogN;
static volatile LONG g_fogSeen, g_fogVolOn;
static volatile LONG g_volFog = -1;             // [Fog] VolumetricFog: 1 on, 0 off, -1 as authored
static volatile float g_fogDensityScale = 1.0f, g_fogVolDist = -1.0f;
static char g_fogMsg[160] = "not installed";

static void __cdecl CbFog(unsigned char *comp)
{
    InterlockedIncrement(&g_fogSeen);
    AcquireSRWLockExclusive(&g_convLock);
    FogRec *r = NULL;
    for (int i = 0; i < g_fogN; i++)
        if (g_fogRec[i].comp == comp && g_fogRec[i].index == *(int *)(comp + OBJ_INDEX) && g_fogRec[i].cls == *(void **)(comp + OBJ_CLASS)) r = &g_fogRec[i];
    if (!r && g_fogN < 256) {
        r = &g_fogRec[g_fogN++];
        r->comp = comp; r->index = *(int *)(comp + OBJ_INDEX); r->cls = *(void **)(comp + OBJ_CLASS);
        r->origExt = *(float *)(comp + FOG_EXTSCALE); r->origDist = *(float *)(comp + FOG_VOLDIST);
    }
    if (r) {
        float k = g_fogDensityScale > 0.0f ? g_fogDensityScale : 1.0f;
        *(float *)(comp + FOG_EXTSCALE) = r->origExt * k;                    // always from the original value; FogDensity untouched
        *(float *)(comp + FOG_VOLDIST) = g_fogVolDist > 0.0f ? g_fogVolDist : r->origDist;
    }
    if (g_volFog == 1) comp[FOG_VOLBITS] |= 1; else if (g_volFog == 0) comp[FOG_VOLBITS] &= ~1;
    if (comp[FOG_VOLBITS] & 1) InterlockedIncrement(&g_fogVolOn);
    ReleaseSRWLockExclusive(&g_convLock);
}

static void InstallFogHook(void)
{
    if (!g_text) return;
    unsigned char *hit = NULL; int count = 0;
    for (size_t i = 0; i + 32 <= g_textLen; i++) {
        unsigned char *q = g_text + i;
        if (q[0] != 0x48 || memcmp(q, kFogSig, sizeof kFogSig)) continue;
        // confirm: within the next 0xF0 bytes "movzx eax,byte ptr [rdx+29Ch]" (0F B6 82 9C 02 00 00)
        int ok = 0; for (int k = 0; k < 0xF0; k++) if (q[k] == 0x0F && q[k+1] == 0xB6 && q[k+2] == 0x82 && q[k+3] == 0x9C && q[k+4] == 0x02 && q[k+5] == 0 && q[k+6] == 0) ok = 1;
        if (ok) { if (!hit) hit = q; count++; }
    }
    if (count != 1) { sprintf_s(g_fogMsg, sizeof g_fogMsg, "NOT installed - signature found %d times", count); return; }
    unsigned char *stub = AllocNear(hit, 0x1000);
    if (!stub) { strcpy_s(g_fogMsg, sizeof g_fogMsg, "NOT installed - no memory near the exe"); return; }
    unsigned char *p = stub;
    p = EmitCallC(p, 4, (void *)CbFog);              // CbFog(component = rdx)
    memcpy(p, hit, 5); p += 5;                        // original mov [rsp+8],rbx
    *p++ = 0xE9; Rel32(&p, hit + 5, 0);
    FlushInstructionCache(GetCurrentProcess(), stub, (SIZE_T)(p - stub));
    if (!PatchJmp(hit, 5, stub)) { strcpy_s(g_fogMsg, sizeof g_fogMsg, "NOT installed (VirtualProtect)"); return; }
    sprintf_s(g_fogMsg, sizeof g_fogMsg, "installed at exe+0x%llX", (unsigned long long)(hit - g_base));
}

static void InstallRtlHook(void)
{
    if (!g_text) return;
    unsigned char *hit = NULL; int count = 0;
    for (size_t i = 0; i + sizeof kRtlSig <= g_textLen; i++) {
        if (g_text[i] == 0x0F && !memcmp(g_text + i, kRtlSig, sizeof kRtlSig)) { if (!hit) hit = g_text + i; count++; }
    }
    if (count != 1) { sprintf_s(g_rtlMsg, sizeof g_rtlMsg, "NOT installed - signature found %d times", count); return; }
    unsigned char *site = hit + RTL_SITE;
    unsigned char *stub = AllocNear(site, 0x1000);
    if (!stub) { strcpy_s(g_rtlMsg, sizeof g_rtlMsg, "NOT installed - no memory near the exe"); return; }
    RtlData *rd = (RtlData *)(stub + 0x800); rd->enabled = 0; rd->converted = 0; rd->seen = 0;
    unsigned char *p = stub;
    *p++ = 0xFF; *p++ = 0x05; Rel32(&p, (unsigned char *)&rd->seen, 0);                 // inc dword [rip+seen]
    g_rtl = rd;                                                                           // CbProxyStart reads g_rtl->enabled
    p = EmitCallC(p, 3, (void *)CbProxyStart);                                            // (proxy=rbx, comp=rdi, caller) - decides everything
    memcpy(p, site, 7); p += 7;                                                           // original cmp byte [rdi+1BEh],2
    *p++ = 0xE9; Rel32(&p, site + 7, 0);                                                  // jmp back
    FlushInstructionCache(GetCurrentProcess(), stub, (SIZE_T)(p - stub));
    unsigned char jmp[7] = { 0xE9, 0, 0, 0, 0, 0x66, 0x90 }; *(int *)(jmp + 1) = (int)(stub - (site + 5));
    DWORD old;
    if (!VirtualProtect(site, 7, PAGE_EXECUTE_READWRITE, &old)) { strcpy_s(g_rtlMsg, sizeof g_rtlMsg, "NOT installed (VirtualProtect)"); return; }
    memcpy(site, jmp, 7);
    VirtualProtect(site, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 7);
    g_rtl = rd;
    sprintf_s(g_rtlMsg, sizeof g_rtlMsg, "installed at exe+0x%llX", (unsigned long long)(site - g_base));
    InstallSwapHooks(site);
}

static UINT ParseKey(const wchar_t *s, UINT def)
{
    if ((s[0] == L'F' || s[0] == L'f') && s[1] >= L'1' && s[1] <= L'9') {
        int n = _wtoi(s + 1); if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (s[0] == L'0' && (s[1] == L'x' || s[1] == L'X')) return (UINT)wcstoul(s, NULL, 16);
    if (s[0] && !s[1]) return (UINT)towupper(s[0]);
    return def;
}

// v22: REAL-TIME mode keeps a fraction of the baked lightmap/GI ([Lighting] RealtimeBakedFill, default 0.15) so
// unlit areas (doors in the basements, before the flashlight) are not pure black. 0 = old behaviour (lightmaps off).
// The base is the ini IndirectLightingIntensity, or 1.0 (UE default) when that is -1 = map value. The captured map
// value is deliberately not used as the base, so a written value can never feed back into itself.
static volatile float g_rtFill = 0.6f;
static float RtLmValue(float iniVal) { return (iniVal >= 0.0f ? iniVal : 1.0f) * g_rtFill; }
static float IniLmValue(void)
{
    wchar_t v[64]; GetPrivateProfileStringW(L"Lighting", L"IndirectLightingIntensity", L"-1", v, 64, g_cfgPath);
    return (float)wcstod(v, NULL);
}
static void ApplyToggles(void)
{
    if (!g_ao) return;
    if (g_vanilla) {   // compare mode: every post-process field = what the map asks for
        for (int f = 0; f < AO_FIELDS; f++) g_ao->cfg[f] = -1.0f;
        if (g_csm) g_csm->cfg = -1;
        return;
    }
    if (g_lmOff) g_ao->cfg[F_LM_INT] = RtLmValue(IniLmValue());
    if (g_aoOff) g_ao->cfg[F_AO_INT] = 0.0f;
}

static void ToggleLightingMode(void)
{
    g_lmOff = !g_lmOff;
    if (!g_lmOff && g_ao) { wchar_t v[64]; GetPrivateProfileStringW(L"Lighting", L"IndirectLightingIntensity", L"-1", v, 64, g_cfgPath); g_ao->cfg[F_LM_INT] = (float)wcstod(v, NULL); }
    ApplyToggles();
    ApplyAllConverted();   // baked: converted static lights black (skipped); real-time: restored
}
static void ToggleAO(void)
{
    g_aoOff = !g_aoOff;
    if (!g_aoOff && g_ao) { wchar_t v[64]; GetPrivateProfileStringW(L"AmbientOcclusion", L"Intensity", L"-1", v, 64, g_cfgPath); g_ao->cfg[F_AO_INT] = (float)wcstod(v, NULL); }
    ApplyToggles();
}

// r.VolumetricFog / r.ContactShadows: ini setting, overridden to 0 while the F6/F7 hotkey has it off.
static SRWLOCK g_liveLock = SRWLOCK_INIT;
static void ApplyLiveSwitches(void)
{
    if (!g_cvarsReady) return;
    AcquireSRWLockExclusive(&g_liveLock);
    SetOrRestore(L"r.VolumetricFog", (g_fogHotOff || g_vanilla) ? 0.0 : g_volFog == 1 ? 1.0 : g_volFog == 0 ? 0.0 : -1.0);
    SetOrRestore(L"r.ContactShadows", (g_contactHotOff || g_vanilla) ? 0.0 : (g_contactLen > 0.0f || g_lampContactLen > 0.0f) ? 1.0 :
                                      (g_contactLen == 0.0f && g_lampContactLen == 0.0f) ? 0.0 : -1.0);
    ReleaseSRWLockExclusive(&g_liveLock);
}

// SSR unlock on/off while the game runs. Hot-patch protocol: a 2-byte "jmp +3" first (behaves like the NOP), then
// the 3 tail bytes, then the real first 2 bytes, so another thread never runs a half-written instruction.
static void SetSsrUnlock(int on)
{
    unsigned char *s = g_patchSite;
    if (!s) return;
    const unsigned char *want = on ? kNop5 : kSig + PATCH_OFS;
    if (!memcmp(s, want, PATCH_LEN)) return;
    if (((ULONG_PTR)s & 63) == 63) return;    // the 2-byte store would straddle a cache line: not atomic, leave it
    DWORD old;
    if (!VirtualProtect(s, PATCH_LEN, PAGE_EXECUTE_READWRITE, &old)) return;
    *(volatile unsigned short *)s = 0x03EB;                                   // EB 03 = jmp over the tail
    FlushInstructionCache(GetCurrentProcess(), s, PATCH_LEN);
    memcpy(s + 2, want + 2, PATCH_LEN - 2);
    FlushInstructionCache(GetCurrentProcess(), s, PATCH_LEN);
    *(volatile unsigned short *)s = *(const unsigned short *)want;
    FlushInstructionCache(GetCurrentProcess(), s, PATCH_LEN);
    VirtualProtect(s, PATCH_LEN, old, &old);
}

static void ToggleCompare(void)
{
    g_vanilla = !g_vanilla;
    SetSsrUnlock(!g_vanilla);
    ReadAoConfig();          // post-process fields, cascades, sun distance, lamp cvars, fog/contact switches
    ApplyToggles();
    ApplyAllConverted();     // converted static lights: hidden while comparing (BAKED look)
}

#if HNT_DEBUG_HOTKEYS
static DWORD WINAPI HotkeyThread(LPVOID arg)
{
    (void)arg;
    int downLm = 0, downAo = 0, downFog = 0, downCt = 0, downCmp = 0;
    for (;;) {
        Sleep(20);
        HWND fg = GetForegroundWindow(); DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (pid != GetCurrentProcessId()) { downLm = downAo = downFog = downCt = downCmp = 0; continue; }
        {
            int k = (GetAsyncKeyState((int)g_vkCompare) & 0x8000) != 0;
            if (k && !downCmp) { ToggleCompare(); g_ovFlash = 1; }
            downCmp = k;
        }
        {
            int fk = (GetAsyncKeyState((int)g_vkFog) & 0x8000) != 0;
            int ck = (GetAsyncKeyState((int)g_vkContact) & 0x8000) != 0;
            if (fk && !downFog) { g_fogHotOff = !g_fogHotOff; ApplyLiveSwitches(); g_ovFlash = 1; }
            if (ck && !downCt) { g_contactHotOff = !g_contactHotOff; ApplyLiveSwitches(); g_ovFlash = 1; }
            downFog = fk; downCt = ck;
        }
        int lm = (GetAsyncKeyState((int)g_vkLm) & 0x8000) != 0;
        int ao = (GetAsyncKeyState((int)g_vkAo) & 0x8000) != 0;
        if (lm && !downLm) ToggleLightingMode();
        {
            int ov = (GetAsyncKeyState((int)g_vkOverlay) & 0x8000) != 0;
            if (ov && !g_downOv) { g_showOverlay = !g_showOverlay; g_ovFlash = 0; }
            g_downOv = ov;
        }
        if ((lm && !downLm) || (ao && !downAo)) g_ovFlash = 1;   // show the new state for 5 s
        if (ao && !downAo) ToggleAO();
        downLm = lm; downAo = ao;
        ApplyToggles();
    }
}
#endif

static void EnsureKey(const wchar_t *sec, const wchar_t *key, const wchar_t *def)
{
    wchar_t v[64];
    GetPrivateProfileStringW(sec, key, L"\x1", v, 64, g_cfgPath);
    if (v[0] == 1 && v[1] == 0) WritePrivateProfileStringW(sec, key, def, g_cfgPath);
}

// v33 (release 1.0.0): the DLL writes a complete, commented settings file on first run, so no ini has to ship with
// it. Defaults = the High preset + the tuned look; personal settings (display mode, V-Sync, fps limit, FOV, mouse
// smoothing) are left to the game. Called from DllMain before anything reads the file, so the creation-time
// settings (lighting conversion) are right on the very first level load too. Keys added in later versions are
// appended by EnsureKey with the same defaults.
static const char kDefaultIni[] =
    "; Hello Neighbor Tweaks " HNT_VERSION " - settings\r\n"
    "; Written by XINPUT1_3.dll on first run. Change them in game with F1, or edit this file:\r\n"
    "; it is re-read every 5 seconds while the game runs. -1 = keep the game's / map's own value.\r\n"
    "; Delete this file to get the defaults back.\r\n"
    "\r\n"
    "[Display]\r\n"
    "; Game = leave the game's own setting. Windowed / Borderless / Exclusive = set it at start (also in F1 > DISPLAY).\r\n"
    "; Exclusive fullscreen hides the F1 menu: to leave it, press Alt+Enter or set Mode=Borderless here.\r\n"
    "Mode=Game\r\n"
    "; window size for Windowed mode, e.g. 1920x1080 (Auto = 75% of the monitor)\r\n"
    "WindowedResolution=Auto\r\n"
    "; 0 = off, 1 = on, -1 = game setting\r\n"
    "VSync=-1\r\n"
    "; 30-360 fps, 0 = unlimited, -1 = game setting\r\n"
    "FrameRateLimit=-1\r\n"
    "\r\n"
    "[Input]\r\n"
    "; 0 = off, 1 = on, -1 = game setting (needs a game restart)\r\n"
    "MouseSmoothing=-1\r\n"
    "\r\n"
    "[Lighting]\r\n"
    "; real-time lighting: static/stationary lamps become real-time lights with real-time shadows (next level load)\r\n"
    "RealtimeStaticLights=1\r\n"
    "RealtimeStationaryShadows=1\r\n"
    "ForceRealtimeShadows=2\r\n"
    "ConvertedBrightnessScale=1\r\n"
    "ConvertedRadiusScale=1\r\n"
    "; share of the baked lightmap kept as fill light in REAL-TIME mode (0-1), so dark rooms stay readable\r\n"
    "RealtimeBakedFill=0.6\r\n"
    "IndirectLightingIntensity=-1\r\n"
    "\r\n"
    "[AmbientOcclusion]\r\n"
    "; maps ask for 0.1 / 10 / 1 (barely visible); Unreal's defaults are 0.5 / 200 / 2\r\n"
    "Intensity=0.5\r\n"
    "Radius=200\r\n"
    "Power=2\r\n"
    "StaticFraction=-1\r\n"
    "FadeDistance=8000\r\n"
    "FadeRadius=5000\r\n"
    "\r\n"
    "[Reflections]\r\n"
    "; maps turn screen-space reflections off (Intensity 0); 100 = Unreal's default\r\n"
    "Intensity=100\r\n"
    "MaxRoughness=0.6\r\n"
    "\r\n"
    "[Exposure]\r\n"
    "; eye adaptation limits and speeds (-1 = map value)\r\n"
    "MinBrightness=0.17\r\n"
    "MaxBrightness=-1\r\n"
    "SpeedUp=-1\r\n"
    "SpeedDown=3\r\n"
    "\r\n"
    "[Shadows]\r\n"
    "; sun: maps use 1 cascade; more than 4 makes the sun shadow disappear\r\n"
    "SunCascades=4\r\n"
    "SunResolutionScale=-1\r\n"
    "SunShadowDistance=1.75\r\n"
    "SunCascadeDistribution=2\r\n"
    "SunShadowSharpen=0.3\r\n"
    "ContactShadowLength=0.05\r\n"
    "LampContactShadowLength=-1\r\n"
    "LampMinResolution=512\r\n"
    "LampFadeResolution=64\r\n"
    "LampTexelDensity=1.5\r\n"
    "\r\n"
    "[Fog]\r\n"
    "VolumetricFog=1\r\n"
    "DensityScale=2\r\n"
    "VolumetricDistance=-1\r\n"
    "\r\n"
    "[Camera]\r\n"
    "; field of view in degrees (-1 = game setting); FOVMode Horizontal or Vertical\r\n"
    "FOV=-1\r\n"
    "FOVMode=Horizontal\r\n"
    "BaseFOV=90\r\n"
    "\r\n"
    "[Graphics]\r\n"
    "; quality settings (F1 > QUALITY > Preset sets them all); -1 = game setting\r\n"
    "ScreenPercentage=100\r\n"
    "AAQuality=5\r\n"
    "Sharpen=0.7\r\n"
    "; 0 = off, 1 = on\r\n"
    "Vignette=0\r\n"
    "MotionBlur=0\r\n"
    "DepthOfField=0\r\n"
    "ViewDistance=3\r\n"
    "MeshDetailDistance=0.5\r\n"
    "FoliageDetailDistance=3\r\n"
    "LightDrawDistance=3\r\n"
    "ShadowMapResolution=4096\r\n"
    "ShadowRadiusThreshold=0.005\r\n"
    "ObjectShadowDensity=1.5\r\n"
    "AOLevels=3\r\n"
    "SSRQuality=4\r\n"
    "TexturePoolMB=6144\r\n"
    "FullyLoadTextures=1\r\n"
    "; needs a game restart (written to the game's Engine.ini)\r\n"
    "Anisotropy=16\r\n"
    "\r\n"
    "[Hotkeys]\r\n"
    "; key that opens the settings menu\r\n"
    "ToggleMenu=F1\r\n"
#if HNT_DEBUG_HOTKEYS
    "CompareOriginal=F5\r\n"
    "ToggleVolumetricFog=F6\r\n"
    "ToggleContactShadows=F7\r\n"
    "ToggleLightmaps=F8\r\n"
    "ToggleAO=F9\r\n"
    "ToggleOverlay=F10\r\n"
#endif
    "\r\n"
    "[Overlay]\r\n"
    "; 0 = show the status for 5 s at start, 1 = always on\r\n"
    "Show=0\r\n";
static void WriteDefaultIni(void)
{
    if (GetFileAttributesW(g_cfgPath) != INVALID_FILE_ATTRIBUTES) return;
    HANDLE h = CreateFileW(g_cfgPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, kDefaultIni, (DWORD)(sizeof kDefaultIni - 1), &w, NULL); CloseHandle(h); }
}

static void ReadAoConfig(void)
{
    WriteDefaultIni();   // recreated if deleted while the game runs
    for (int f = 0; f < AO_FIELDS; f++) EnsureKey(kAoSec[f], kAoKey[f], kAoDef[f]);
    EnsureKey(L"Shadows", L"SunCascades", L"4");
    EnsureKey(L"Lighting", L"RealtimeStaticLights", L"1");
    if (g_rtl) g_rtl->enabled = GetPrivateProfileIntW(L"Lighting", L"RealtimeStaticLights", 1, g_cfgPath) > 0;
    EnsureKey(L"Lighting", L"RealtimeStationaryShadows", L"1");
    g_stationaryRT = GetPrivateProfileIntW(L"Lighting", L"RealtimeStationaryShadows", 1, g_cfgPath) > 0;
    EnsureKey(L"Lighting", L"ForceRealtimeShadows", L"2");
    g_forceShadows = GetPrivateProfileIntW(L"Lighting", L"ForceRealtimeShadows", 2, g_cfgPath);
    EnsureKey(L"Display", L"Mode", L"Game");
    EnsureKey(L"Display", L"WindowedResolution", L"Auto");
    EnsureKey(L"Shadows", L"SunResolutionScale", L"-1");
    EnsureKey(L"Shadows", L"SunCascadeDistribution", L"2");
    EnsureKey(L"Shadows", L"SunShadowSharpen", L"0.3");
    EnsureKey(L"Shadows", L"SunShadowDistance", L"1.75");
    {
        wchar_t v[64];
        GetPrivateProfileStringW(L"Shadows", L"SunResolutionScale", L"-1", v, 64, g_cfgPath);     g_sunRes = (float)wcstod(v, NULL);
        GetPrivateProfileStringW(L"Shadows", L"SunCascadeDistribution", L"-1", v, 64, g_cfgPath); g_sunDist = (float)wcstod(v, NULL);
        GetPrivateProfileStringW(L"Shadows", L"SunShadowSharpen", L"-1", v, 64, g_cfgPath);       g_sunSharp = (float)wcstod(v, NULL);
        if (g_cvarsReady && g_engineUp) { GetPrivateProfileStringW(L"Shadows", L"SunShadowDistance", L"-1", v, 64, g_cfgPath); SetOrRestore(L"r.Shadow.DistanceScale", g_vanilla ? -1.0 : wcstod(v, NULL)); }
    }
    EnsureKey(L"Display", L"VSync", L"-1");
    EnsureKey(L"Display", L"FrameRateLimit", L"-1");
    ApplyVSync();
    ApplyGraphics();          // also creates the [Graphics] / [Input] keys
    SyncRestartSettings();
    EnsureKey(L"Fog", L"VolumetricFog", L"1");
    EnsureKey(L"Fog", L"DensityScale", L"2");
    EnsureKey(L"Fog", L"VolumetricDistance", L"-1");
    {
        wchar_t v[64];
        g_volFog = GetPrivateProfileIntW(L"Fog", L"VolumetricFog", -1, g_cfgPath);
        GetPrivateProfileStringW(L"Fog", L"DensityScale", L"2", v, 64, g_cfgPath);        g_fogDensityScale = (float)wcstod(v, NULL);
        GetPrivateProfileStringW(L"Fog", L"VolumetricDistance", L"-1", v, 64, g_cfgPath); g_fogVolDist = (float)wcstod(v, NULL);
        // r.VolumetricFog is read every frame: OFF takes effect at once; ON needs fog built with the bit (next load)
    }
    EnsureKey(L"Shadows", L"ContactShadowLength", L"0.05");
    EnsureKey(L"Shadows", L"LampContactShadowLength", L"-1");
    {
        wchar_t v[64];
        GetPrivateProfileStringW(L"Shadows", L"ContactShadowLength", L"-1", v, 64, g_cfgPath); g_contactLen = (float)wcstod(v, NULL);
        GetPrivateProfileStringW(L"Shadows", L"LampContactShadowLength", L"-1", v, 64, g_cfgPath); g_lampContactLen = (float)wcstod(v, NULL);
        ApplyLiveSwitches();   // r.VolumetricFog + r.ContactShadows (ini value, or 0 while F6/F7 has it off)
    }
    // PCSS (r.Shadow.FilterMethod 1) was tried in v15 and removed in v16: UE 4.20's experimental PCSS looks blocky.
    EnsureKey(L"Shadows", L"LampMinResolution", L"512");
    EnsureKey(L"Shadows", L"LampFadeResolution", L"64");
    EnsureKey(L"Shadows", L"LampTexelDensity", L"1.5");
    ApplyLampShadows();
    EnsureKey(L"Camera", L"FOV", L"-1");
    EnsureKey(L"Camera", L"FOVMode", L"Horizontal");
    EnsureKey(L"Camera", L"BaseFOV", L"90");
    {
        wchar_t v[64];
        GetPrivateProfileStringW(L"Camera", L"FOVMode", L"Horizontal", v, 64, g_cfgPath); g_fovVertical = (v[0] == L'V' || v[0] == L'v');
        GetPrivateProfileStringW(L"Camera", L"FOV", L"-1", v, 64, g_cfgPath); g_fovTarget = (float)wcstod(v, NULL);
        GetPrivateProfileStringW(L"Camera", L"BaseFOV", L"90", v, 64, g_cfgPath); g_fovBase = (float)wcstod(v, NULL);
        ApplyFov();
    }
#if HNT_DEBUG_HOTKEYS
    EnsureKey(L"Hotkeys", L"ToggleLightmaps", L"F8");
    EnsureKey(L"Hotkeys", L"ToggleAO", L"F9");
    EnsureKey(L"Hotkeys", L"ToggleOverlay", L"F10");
    EnsureKey(L"Hotkeys", L"ToggleVolumetricFog", L"F6");
    EnsureKey(L"Hotkeys", L"ToggleContactShadows", L"F7");
    EnsureKey(L"Hotkeys", L"CompareOriginal", L"F5");
    EnsureKey(L"Overlay", L"Show", L"0");   // 0 = show 5 s at start and after F8/F9, then hide; 1 = always on
    {
        wchar_t v[32];
        GetPrivateProfileStringW(L"Hotkeys", L"ToggleLightmaps", L"F8", v, 32, g_cfgPath); g_vkLm = ParseKey(v, VK_F8);
        GetPrivateProfileStringW(L"Hotkeys", L"ToggleAO", L"F9", v, 32, g_cfgPath); g_vkAo = ParseKey(v, VK_F9);
        GetPrivateProfileStringW(L"Hotkeys", L"ToggleOverlay", L"F10", v, 32, g_cfgPath); g_vkOverlay = ParseKey(v, VK_F10);
        GetPrivateProfileStringW(L"Hotkeys", L"ToggleVolumetricFog", L"F6", v, 32, g_cfgPath); g_vkFog = ParseKey(v, VK_F6);
        GetPrivateProfileStringW(L"Hotkeys", L"ToggleContactShadows", L"F7", v, 32, g_cfgPath); g_vkContact = ParseKey(v, VK_F7);
        GetPrivateProfileStringW(L"Hotkeys", L"CompareOriginal", L"F5", v, 32, g_cfgPath); g_vkCompare = ParseKey(v, VK_F5);
    }
#else
    EnsureKey(L"Overlay", L"Show", L"0");   // 0 = show 5 s at start, then hide; 1 = always on
#endif
    EnsureKey(L"Lighting", L"RealtimeBakedFill", L"0.6");
    {
        wchar_t v[64]; GetPrivateProfileStringW(L"Lighting", L"RealtimeBakedFill", L"0.6", v, 64, g_cfgPath);
        float fill = (float)wcstod(v, NULL);
        g_rtFill = fill < 0.0f ? 0.0f : fill > 1.0f ? 1.0f : fill;
    }
    if (g_ao) for (int f = 0; f < AO_FIELDS; f++) {
        wchar_t v[64];
        GetPrivateProfileStringW(kAoSec[f], kAoKey[f], L"-1", v, 64, g_cfgPath);
        float x = (float)wcstod(v, NULL);
        if (g_vanilla) x = -1.0f;
        else if (f == F_LM_INT && g_lmOff) x = RtLmValue(x);
        else if (f == F_AO_INT && g_aoOff) x = 0.0f;
        g_ao->cfg[f] = x;
    }
    if (g_ao) {   // exposure: keep Min <= Max (against the map's Max when ours is "map value")
        float mx = g_ao->cfg[F_EX_MAX] >= 0.0f ? g_ao->cfg[F_EX_MAX] : g_ao->map[F_EX_MAX];
        if (g_ao->cfg[F_EX_MIN] >= 0.0f && mx >= 0.0f && g_ao->cfg[F_EX_MIN] > mx) g_ao->cfg[F_EX_MIN] = mx;
    }
    if (g_csm) {
        // More than 4 cascades at 4096 px no longer fit the CSM shadow-depth texture: the engine then drops the
        // sun shadow entirely. Clamp to 4 (user report 2026-11-21: >4 makes shadows disappear).
        LONG c = (LONG)GetPrivateProfileIntW(L"Shadows", L"SunCascades", -1, g_cfgPath);
        if (c > 4) c = 4;
        g_csm->cfg = g_vanilla ? -1 : c;
    }
    EnsureKey(L"Lighting", L"ConvertedBrightnessScale", L"1");
    EnsureKey(L"Lighting", L"ConvertedRadiusScale", L"1");
    EnsureKey(L"Hotkeys", L"ToggleMenu", L"F1");
    {
        wchar_t v[64]; float si, sr;
        GetPrivateProfileStringW(L"Lighting", L"ConvertedBrightnessScale", L"1", v, 64, g_cfgPath); si = (float)wcstod(v, NULL);
        GetPrivateProfileStringW(L"Lighting", L"ConvertedRadiusScale", L"1", v, 64, g_cfgPath); sr = (float)wcstod(v, NULL);
        if (si > 0.0f && sr > 0.0f && (si != g_scaleI || sr != g_scaleR)) { g_scaleI = si; g_scaleR = sr; ApplyAllConverted(); }
        GetPrivateProfileStringW(L"Hotkeys", L"ToggleMenu", L"F1", v, 64, g_cfgPath); g_vkMenu = ParseKey(v, VK_F1);
    }
}

// ---------------------------------------------------------------- cvar lookup
enum { CV_NONE, CV_INT, CV_FLOAT, CV_FLOATREF, CV_INTREF };
typedef struct {
    wchar_t name[96];
    wchar_t want[64];
    int kind;                 // CV_*
    unsigned char *holder;    // CV_INT/FLOAT: address of the TConsoleVariableData* global
    unsigned char *var;       // CV_FLOATREF: address of the float
    double last; int haveLast;
    int extra;                // added by the DLL (menu-controlled), not from Engine.ini
    double orig; int haveOrig;// engine value before the DLL first changed it
} CVar;
#define MAX_CVARS 96
static CVar g_cv[MAX_CVARS]; static int g_cvCount;
static wchar_t g_iniPath[MAX_PATH];

static void ReadIni(void)
{
    wchar_t la[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) return;
    swprintf_s(g_iniPath, MAX_PATH, L"%s\\HelloNeighbor\\Saved\\Config\\WindowsNoEditor\\Engine.ini", la);
    static wchar_t buf[32768];
    DWORD n = GetPrivateProfileSectionW(L"SystemSettings", buf, 32768, g_iniPath);
    for (wchar_t *p = buf; n && *p && g_cvCount < MAX_CVARS; p += wcslen(p) + 1) {
        while (*p == L' ' || *p == L'\t') p++;
        if (*p == L';' || *p == L'#') continue;
        wchar_t *eq = wcschr(p, L'=');
        if (!eq || eq == p) continue;
        CVar *c = &g_cv[g_cvCount];
        size_t kl = (size_t)(eq - p); while (kl && (p[kl - 1] == L' ' || p[kl - 1] == L'\t')) kl--;
        if (kl >= 96) continue;
        wcsncpy_s(c->name, 96, p, kl);
        const wchar_t *v = eq + 1; while (*v == L' ') v++;
        wcsncpy_s(c->want, 64, v, _TRUNCATE);
        // a later duplicate key wins, like UE
        int dup = -1;
        for (int i = 0; i < g_cvCount; i++) if (!_wcsicmp(g_cv[i].name, c->name)) dup = i;
        if (dup >= 0) { wcscpy_s(g_cv[dup].want, 64, c->want); continue; }
        g_cvCount++;
    }
}

static int Classify(unsigned char *lea, CVar *c)
{
    // lea is at "48 8D 15 disp32" (lea rdx,[name])
    unsigned char *p = lea + 7;
    if (p[0] == 0xC7 && p[1] == 0x44 && p[2] == 0x24 && p[3] == 0x20) p += 8;  // mov dword [rsp+20h], imm32
    if (p[0] != 0xFF || p[1] != 0x50) return 0;                               // call [rax+imm8]
    unsigned char slot = p[2]; p += 3;
    if (slot == 0x08 || slot == 0x10) {
        unsigned char want = slot == 0x08 ? 0x40 : 0x38;
        for (int k = 0; k < 48; k++, p++) {
            if (p[0] == 0xFF && p[1] == 0x52 && p[2] == want) {
                unsigned char *q = p + 3;
                if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x0D) q += 7;              // lea rcx,[...]
                if (q[0] == 0x48 && q[1] == 0x89 && q[2] == 0x05) {                    // mov [rip+G],rax
                    c->holder = Rip(q + 7, q + 3);
                    c->kind = slot == 0x08 ? CV_FLOAT : CV_INT;
                    return 1;
                }
                return 0;
            }
        }
        return 0;
    }
    if (slot == 0x20 || slot == 0x28) {                                           // RegisterConsoleVariableRef(float& / int32&)
        for (unsigned char *b = lea - 7; b >= lea - 32; b--) {
            if (b[0] == 0x4C && b[1] == 0x8D && b[2] == 0x05) {                        // lea r8,[var]
                c->var = Rip(b + 7, b + 3); c->kind = slot == 0x20 ? CV_FLOATREF : CV_INTREF; return 1;
            }
        }
    }
    return 0;
}

static void ResolveCvars(void)
{
    // collect every occurrence of each name (UTF-16 + NUL) in the data sections
    enum { MAXS = 512 };
    static unsigned char *str[MAXS]; static short owner[MAXS]; int ns = 0;
    for (int ci = 0; ci < g_cvCount; ci++) {
        size_t bl = (wcslen(g_cv[ci].name) + 1) * 2;
        wchar_t c0 = g_cv[ci].name[0], c1 = g_cv[ci].name[1];
        for (int s = 0; s < g_dataCount; s++) {
            unsigned char *d = g_data[s].p; size_t n = g_data[s].n;
            for (size_t i = 0; i + bl <= n; i += 2) {
                const wchar_t *w = (const wchar_t *)(d + i);
                if (w[0] != c0 || w[1] != c1) continue;
                if (!memcmp(d + i, g_cv[ci].name, bl) && ns < MAXS) { str[ns] = d + i; owner[ns] = (short)ci; ns++; }
            }
        }
    }
    if (!ns) return;
    for (size_t i = 0; i + 16 < g_textLen; i++) {
        if (g_text[i] != 0x48 || g_text[i + 1] != 0x8D || g_text[i + 2] != 0x15) continue;
        unsigned char *t = Rip(g_text + i + 7, g_text + i + 3);
        for (int k = 0; k < ns; k++) {
            if (t == str[k] && g_cv[owner[k]].kind == CV_NONE && Classify(g_text + i, &g_cv[owner[k]])) break;
        }
    }
}

// returns 0 unreadable, 1 ok (game/render filled)
static int ReadCvar(CVar *c, double *game, double *render)
{
    __try {
        if (c->kind == CV_FLOATREF) { *game = *render = *(float *)c->var; return 1; }
        if (c->kind == CV_INTREF) { *game = *render = *(int *)c->var; return 1; }
        if (c->kind == CV_INT || c->kind == CV_FLOAT) {
            unsigned char *d = *(unsigned char **)c->holder;
            if (!d) return 0;
            if (c->kind == CV_INT) { *game = ((int *)d)[0]; *render = ((int *)d)[1]; }
            else { *game = ((float *)d)[0]; *render = ((float *)d)[1]; }
            return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return 0;
}

static CVar *FindCv(const wchar_t *n) { for (int i = 0; i < g_cvCount; i++) if (!_wcsicmp(g_cv[i].name, n)) return &g_cv[i]; return NULL; }

// Menu-controlled engine settings that are not in Engine.ini: added to the resolver list as "extra".
static void AddExtraCvar(const wchar_t *n)
{
    if (FindCv(n) || g_cvCount >= MAX_CVARS) return;
    CVar *c = &g_cv[g_cvCount++]; memset(c, 0, sizeof *c);
    wcscpy_s(c->name, 96, n); c->extra = 1;
}
static int WriteCvar(CVar *c, double v)
{
    __try {
        if (c->kind == CV_FLOATREF) { *(float *)c->var = (float)v; return 1; }
        if (c->kind == CV_INTREF) { *(int *)c->var = (int)v; return 1; }
        if (c->kind == CV_INT || c->kind == CV_FLOAT) {
            unsigned char *d = *(unsigned char **)c->holder;
            if (!d) return 0;
            if (c->kind == CV_INT) { ((int *)d)[0] = (int)v; ((int *)d)[1] = (int)v; }
            else { ((float *)d)[0] = (float)v; ((float *)d)[1] = (float)v; }
            return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return 0;
}
// The value "restore" goes back to. For a cvar listed in Engine.ini [SystemSettings] that is the ini value:
// the DLL's first read can happen before the engine has applied the ini (the cvar still holds its constructor
// default), and remembering that default made "restore" undo Engine.ini (v17: r.Shadow.DistanceScale 2 -> 1).
static int CvarIniValue(const CVar *c, double *out)
{
    if (c->extra || !c->want[0]) return 0;
    wchar_t *end = NULL; double d = wcstod(c->want, &end);
    if (end == c->want) return 0;
    *out = d; return 1;
}
// value < 0 = restore the engine's own value (Engine.ini value for ini cvars)
static void SetOrRestore(const wchar_t *name, double v)
{
    CVar *c = FindCv(name); double g, r;
    if (!c || c->kind == CV_NONE || !ReadCvar(c, &g, &r)) return;
    if (!c->haveOrig) { if (!CvarIniValue(c, &c->orig)) c->orig = r; c->haveOrig = 1; }
    double want = v < 0.0 ? c->orig : v;
    if (r != want) WriteCvar(c, want);
}
static double CvarOrig(const wchar_t *name)
{
    CVar *c = FindCv(name); double g, r, ini;
    if (!c || c->kind == CV_NONE) return -1.0;
    if (c->haveOrig) return c->orig;
    if (CvarIniValue(c, &ini)) return ini;
    return ReadCvar(c, &g, &r) ? r : -1.0;
}
// v25: [Display] VSync -1 = leave r.VSync to the game (Engine.ini / its own options), 0 = off, 1 = on. Live: UE reads
// r.VSync when it presents each frame. -1 only restores the original value if the DLL changed it before, so the game's
// own V-Sync option is never overridden. Compare mode (F5) leaves it alone.
// v29: graphics options that used to be hand-edited Engine.ini lines. [Graphics] in the mod ini, -1 = game default.
// A switch entry: -1 or 1 = ON (the game's value), 0 = OFF (offVal written to the cvar). All live, applied once the
// engine is up. -1 only restores the game's value if the DLL changed it, so the game's own settings menu is not fought.
typedef struct { const wchar_t *key, *cvar, *cvar2; float offVal; int isSwitch; } GfxCvar;
static const GfxCvar kGfx[] = {
    { L"ScreenPercentage",      L"r.ScreenPercentage" },
    { L"AAQuality",             L"r.PostProcessAAQuality" },
    { L"Sharpen",               L"r.Tonemapper.Sharpen" },
    { L"Vignette",              L"r.Tonemapper.Quality",          NULL, 1.0f, 1 },   // OFF = tonemapper quality 1
    { L"MotionBlur",            L"r.MotionBlurQuality",           NULL, 0.0f, 1 },
    { L"DepthOfField",          L"r.DepthOfFieldQuality",         NULL, 0.0f, 1 },
    { L"ViewDistance",          L"r.ViewDistanceScale" },
    { L"MeshDetailDistance",    L"r.StaticMeshLODDistanceScale" },
    { L"FoliageDetailDistance", L"foliage.LODDistanceScale" },
    { L"LightDrawDistance",     L"r.LightMaxDrawDistanceScale" },
    { L"ShadowMapResolution",   L"r.Shadow.MaxResolution",        L"r.Shadow.MaxCSMResolution" },
    { L"ShadowRadiusThreshold", L"r.Shadow.RadiusThreshold" },
    { L"ObjectShadowDensity",   L"r.Shadow.TexelsPerPixel" },
    { L"AOLevels",              L"r.AmbientOcclusionLevels" },
    { L"SSRQuality",            L"r.SSR.Quality" },
    { L"TexturePoolMB",         L"r.Streaming.PoolSize" },
    { L"FullyLoadTextures",     L"r.Streaming.FullyLoadUsedTextures" },
};
#define N_GFX ((int)(sizeof kGfx / sizeof kGfx[0]))
static const GfxCvar *FindGfx(const wchar_t *key) { for (int i = 0; i < N_GFX; i++) if (!wcscmp(kGfx[i].key, key)) return &kGfx[i]; return NULL; }
static void AddGraphicsCvars(void)
{
    for (int i = 0; i < N_GFX; i++) { AddExtraCvar(kGfx[i].cvar); if (kGfx[i].cvar2) AddExtraCvar(kGfx[i].cvar2); }
    AddExtraCvar(L"r.Shadow.CSM.MaxCascades");
    AddExtraCvar(L"r.Shadow.DistanceScale");
    AddExtraCvar(L"r.VSync");
    AddExtraCvar(L"t.MaxFPS");
}
static void ApplyGraphics(void)
{
    static int touched[N_GFX], csmTouched;
    for (int i = 0; i < N_GFX; i++) EnsureKey(L"Graphics", kGfx[i].key, L"-1");
    EnsureKey(L"Graphics", L"Anisotropy", L"-1");
    EnsureKey(L"Input", L"MouseSmoothing", L"-1");
    if (!g_cvarsReady || !g_engineUp) return;
    for (int i = 0; i < N_GFX; i++) {
        const GfxCvar *e = &kGfx[i];
        wchar_t v[64]; GetPrivateProfileStringW(L"Graphics", e->key, L"-1", v, 64, g_cfgPath);
        double x = g_vanilla ? -1.0 : wcstod(v, NULL);
        double want = x < 0.0 ? -1.0 : e->isSwitch ? (x > 0.5 ? -1.0 : (double)e->offVal) : x;
        if (want >= 0.0) { SetOrRestore(e->cvar, want); if (e->cvar2) SetOrRestore(e->cvar2, want); touched[i] = 1; }
        else if (touched[i]) { SetOrRestore(e->cvar, -1.0); if (e->cvar2) SetOrRestore(e->cvar2, -1.0); touched[i] = 0; }
    }
    // the sun's cascade count is capped by r.Shadow.CSM.MaxCascades: raise it only if the game's value is lower
    LONG sc = (g_csm && !g_vanilla) ? g_csm->cfg : -1;
    double o = CvarOrig(L"r.Shadow.CSM.MaxCascades");
    if (sc > 0 && o >= 0.0 && o < (double)sc) { SetOrRestore(L"r.Shadow.CSM.MaxCascades", (double)sc); csmTouched = 1; }
    else if (csmTouched) { SetOrRestore(L"r.Shadow.CSM.MaxCascades", -1.0); csmTouched = 0; }
}
// Restart-only settings live in the game's own config files; the mod writes them there when they differ.
//   [Graphics] Anisotropy -> Engine.ini [SystemSettings] r.MaxAnisotropy
//   [Input] MouseSmoothing -> Input.ini [/Script/Engine.InputSettings] bEnableMouseSmoothing
// -1 = game setting: the line is removed again, but only if the mod wrote it (v33: recorded in the mod ini's
// [State] section, so a player's own Engine.ini / Input.ini tweak is never deleted).
// [State] <mark> = what the line held before the mod first changed it ("-" = there was no line).
static void SyncOne(const wchar_t *sec, const wchar_t *key, const wchar_t *file, const wchar_t *want, const wchar_t *mark)
{
    wchar_t cur[64], was[64];
    GetPrivateProfileStringW(sec, key, L"", cur, 64, file);
    GetPrivateProfileStringW(L"State", mark, L"", was, 64, g_cfgPath);
    if (!want) {   // back to the game setting: undo the mod's change
        if (was[0]) {
            WritePrivateProfileStringW(sec, key, wcscmp(was, L"-") ? was : NULL, file);
            WritePrivateProfileStringW(L"State", mark, NULL, g_cfgPath);
        }
        return;
    }
    if (_wcsicmp(cur, want)) {
        if (!was[0]) WritePrivateProfileStringW(L"State", mark, cur[0] ? cur : L"-", g_cfgPath);
        WritePrivateProfileStringW(sec, key, want, file);
    }
}
static void SyncRestartSettings(void)
{
    if (!g_iniPath[0]) return;
    wchar_t inputIni[MAX_PATH]; wcscpy_s(inputIni, MAX_PATH, g_iniPath);
    wchar_t *slash = wcsrchr(inputIni, L'\\'); if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - inputIni), L"Input.ini");
    wchar_t v[64];
    int an = GetPrivateProfileIntW(L"Graphics", L"Anisotropy", -1, g_cfgPath);
    if (an >= 0) swprintf_s(v, 64, L"%d", an);
    SyncOne(L"SystemSettings", L"r.MaxAnisotropy", g_iniPath, an >= 0 ? v : NULL, L"WroteAnisotropy");
    int ms = GetPrivateProfileIntW(L"Input", L"MouseSmoothing", -1, g_cfgPath);
    SyncOne(L"/Script/Engine.InputSettings", L"bEnableMouseSmoothing", inputIni, ms < 0 ? NULL : ms ? L"True" : L"False", L"WroteMouseSmoothing");
}

static void ApplyVSync(void)
{
    static int touched;
    if (!g_cvarsReady || !g_engineUp) return;
    int v = GetPrivateProfileIntW(L"Display", L"VSync", -1, g_cfgPath);
    if (v >= 0) { SetOrRestore(L"r.VSync", v ? 1.0 : 0.0); touched = 1; }
    else if (touched) { SetOrRestore(L"r.VSync", -1.0); touched = 0; }
    // [Display] FrameRateLimit: -1 = game setting, 0 = unlimited, N = cap via t.MaxFPS (UE's own limiter, live).
    static int fpsTouched;
    int fl = GetPrivateProfileIntW(L"Display", L"FrameRateLimit", -1, g_cfgPath);
    if (fl >= 0) { SetOrRestore(L"t.MaxFPS", (double)fl); fpsTouched = 1; }
    else if (fpsTouched) { SetOrRestore(L"t.MaxFPS", -1.0); fpsTouched = 0; }
}
static void ApplyLampShadows(void)
{
    if (!g_cvarsReady) return;
    wchar_t v[64];
    GetPrivateProfileStringW(L"Shadows", L"LampMinResolution", L"-1", v, 64, g_cfgPath);  SetOrRestore(L"r.Shadow.MinResolution", g_vanilla ? -1.0 : wcstod(v, NULL));
    GetPrivateProfileStringW(L"Shadows", L"LampFadeResolution", L"-1", v, 64, g_cfgPath); SetOrRestore(L"r.Shadow.FadeResolution", g_vanilla ? -1.0 : wcstod(v, NULL));
    GetPrivateProfileStringW(L"Shadows", L"LampTexelDensity", L"-1", v, 64, g_cfgPath);
    double k = g_vanilla ? -1.0 : wcstod(v, NULL);
    const wchar_t *tpp[] = { L"r.Shadow.TexelsPerPixelPointlight", L"r.Shadow.TexelsPerPixelSpotlight", L"r.Shadow.TexelsPerPixelRectlight" };
    for (int i = 0; i < 3; i++) { double o = CvarOrig(tpp[i]); if (o > 0.0) SetOrRestore(tpp[i], k > 0.0 ? o * k : -1.0); }
}

// ---------------------------------------------------------------- Present hook
typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain *, UINT, UINT);
static PresentFn g_origPresent;
static volatile LONG64 g_frames;
static volatile LONG g_sync = -1, g_bbW, g_bbH, g_windowed = -1;
static volatile LONG g_wantDesc = 1;
static char g_hookMsg[160] = "not installed yet";

static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain *sc, UINT sync, UINT flags)
{
    InterlockedIncrement64(&g_frames);
    g_sync = (LONG)sync;
    if (InterlockedExchange(&g_wantDesc, 0)) {
        DXGI_SWAP_CHAIN_DESC d;
        if (SUCCEEDED(IDXGISwapChain_GetDesc(sc, &d))) { g_bbW = d.BufferDesc.Width; g_bbH = d.BufferDesc.Height; g_windowed = d.Windowed; }
    }
    return g_origPresent(sc, sync, flags);
}

static void InstallPresentHook(void)
{
    HMODULE d3d = LoadLibraryW(L"d3d11.dll");
    PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN create = d3d ? (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain") : NULL;
    if (!create) { strcpy_s(g_hookMsg, sizeof g_hookMsg, "failed (d3d11 not available)"); return; }
    WNDCLASSW wc = {0}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = g_self; wc.lpszClassName = L"HNTweaksDummy";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 64, 64, NULL, NULL, g_self, NULL);
    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferCount = 1; sd.BufferDesc.Width = 64; sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain *sc = NULL; ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, NULL, &ctx);
    if (SUCCEEDED(hr) && sc) {
        void **vt = *(void ***)sc; DWORD old;
        if (VirtualProtect(&vt[8], sizeof(void *), PAGE_READWRITE, &old)) {
            g_origPresent = (PresentFn)vt[8];
            InterlockedExchangePointer(&vt[8], (void *)HookPresent);
            VirtualProtect(&vt[8], sizeof(void *), old, &old);
            strcpy_s(g_hookMsg, sizeof g_hookMsg, "installed");
        } else strcpy_s(g_hookMsg, sizeof g_hookMsg, "failed (VirtualProtect)");
    } else sprintf_s(g_hookMsg, sizeof g_hookMsg, "failed (dummy swap chain hr=0x%08lX)", (unsigned long)hr);
    if (ctx) ID3D11DeviceContext_Release(ctx);
    if (dev) ID3D11Device_Release(dev);
    if (sc) IDXGISwapChain_Release(sc);
    if (hwnd) DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, g_self);
}

// ---------------------------------------------------------------- log
static char g_hist[24][200]; static int g_histN;
static void Hist(const char *s)
{
    if (g_histN == 24) { memmove(g_hist[0], g_hist[1], sizeof g_hist[0] * 23); g_histN = 23; }
    strcpy_s(g_hist[g_histN++], 200, s);
}

static void Fmt(char *o, size_t n, int kind, double v)
{
    if (kind == CV_INT) sprintf_s(o, n, "%d", (int)v); else sprintf_s(o, n, "%g", v);
}

static void WriteLog(const char *body)
{
    wchar_t path[MAX_PATH]; DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    while (n && path[n - 1] != L'\\') n--;
    if (!n || n + 32 >= MAX_PATH) return;
    wcscpy_s(path + n, MAX_PATH - n, L"HelloNeighborTweaks.log");
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(h, body, (DWORD)strlen(body), &w, NULL); CloseHandle(h);
}

static DWORD WINAPI Worker(LPVOID arg)
{
    (void)arg;
    static char out[65536];
    SYSTEMTIME st0; GetLocalTime(&st0);
    ReadIni();
    AddExtraCvar(L"r.Shadow.MinResolution");
    AddExtraCvar(L"r.Shadow.FadeResolution");
    AddExtraCvar(L"r.Shadow.TexelsPerPixelPointlight");
    AddExtraCvar(L"r.Shadow.TexelsPerPixelSpotlight");
    AddExtraCvar(L"r.Shadow.TexelsPerPixelRectlight");
    AddExtraCvar(L"r.ContactShadows");
    AddExtraCvar(L"r.VolumetricFog");
    AddGraphicsCvars();
    ResolveCvars();
    g_cvarsReady = 1;
    FindSetRes();
    g_dispMode = StartDisplayMode();
    LARGE_INTEGER qf, q0, qPrev; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0); qPrev = q0;
    LONG64 fPrev = 0; double fpsMin = 1e9, fpsMax = 0, fpsLast = 0; LONG64 fWarm = -1; LARGE_INTEGER qWarm = {0};
    int hookTried = 0;
    ReadAoConfig();
    LONG aoPrevHits = 0;
    for (int tick = 0;; tick++) {
        Sleep(5000);
        ReadAoConfig();
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        double t = (double)(q.QuadPart - q0.QuadPart) / qf.QuadPart;
        if (!hookTried && t >= 10) { hookTried = 1; InstallPresentHook(); }
        if (!g_engineUp && ((g_ao && g_ao->hits > 0) || g_frames > 0)) { g_engineUp = 1; ReadAoConfig(); }

        LONG64 f = g_frames;
        double dt = (double)(q.QuadPart - qPrev.QuadPart) / qf.QuadPart;
        fpsLast = dt > 0 ? (f - fPrev) / dt : 0; fPrev = f; qPrev = q;
        if (f > 0 && fWarm < 0 && t >= 30) { fWarm = f; qWarm = q; }
        if (fWarm >= 0 && fpsLast > 0) { if (fpsLast < fpsMin) fpsMin = fpsLast; if (fpsLast > fpsMax) fpsMax = fpsLast; }
        double fpsAvg = (fWarm >= 0 && q.QuadPart > qWarm.QuadPart) ? (f - fWarm) / ((double)(q.QuadPart - qWarm.QuadPart) / qf.QuadPart) : 0;
        g_wantDesc = 1;

        int len = 0, ok = 0, bad = 0, unk = 0;
        #define P(...) len += sprintf_s(out + len, sizeof out - len, __VA_ARGS__)
        SYSTEMTIME st; GetLocalTime(&st);
        P("Hello Neighbor Tweaks " HNT_VERSION " - live check (rewritten every 5 s)\r\n");
        if (g_vanilla) P("*** COMPARE MODE (F5): live mod effects are OFF - values below show the original game ***\r\n");
        P("Game started %02d:%02d:%02d, now %02d:%02d:%02d (%.0f s)\r\n\r\n", st0.wHour, st0.wMinute, st0.wSecond, st.wHour, st.wMinute, st.wSecond, t);

        // SSR
        int still = g_patchSite && !memcmp(g_patchSite, kNop5, PATCH_LEN);
        P("[Reflections]\r\n  Patch: %s%s\r\n", g_patchMsg, g_patchSite ? (still ? " (still in place)" : g_vanilla ? " (reverted while comparing, F5)" : " (BYTES CHANGED!)") : "");
        CVar *ssr = FindCv(L"r.SSR.Quality"); double g, r;
        if (ssr && ReadCvar(ssr, &g, &r)) {
            int q2 = (int)r; if (q2 > 5) q2 = 5; if (q2 < 0) q2 = 0;
            int eff = still ? q2 : (q2 < 2 ? q2 : 2); if (eff < 1) eff = 1; if (eff > 4) eff = 4;
            P("  r.SSR.Quality = %d -> effective SSR level %d of 4 (maps alone would give 2)\r\n", (int)r, eff);
        }
        // vignette / grain
        CVar *tq = FindCv(L"r.Tonemapper.Quality");
        if (tq && ReadCvar(tq, &g, &r))
            P("[Tonemapper]\r\n  r.Tonemapper.Quality = %d -> vignette %s, film grain %s (engine zeroes vignette below 2, grain below 4)\r\n",
              (int)r, r < 2 ? "OFF" : "on", r < 4 ? "OFF" : "on");

        // ambient occlusion
        P("[Post-process overrides: ambient occlusion + reflections]  hook: %s\r\n", g_aoMsg);
        if (g_ao) {
            CVar *tqc = FindCv(L"r.Tonemapper.Quality");
            if (tqc && tqc->kind == CV_INT)
                P("  hook target check: %s\r\n", tqc->holder == g_aoHolder ? "OK (reads r.Tonemapper.Quality, as expected)" : "UNEXPECTED target");
            LONG h = g_ao->hits; LONG dh = h - aoPrevHits; aoPrevHits = h;
            P("  ran %ld times in the last 5 s (once per rendered view)\r\n", dh);
            for (int fi = 0; fi < AO_FIELDS; fi++) {
                char m[32], c[48];
                if (g_ao->map[fi] < 0 && h == 0) strcpy_s(m, sizeof m, "?"); else sprintf_s(m, sizeof m, "%g", g_ao->map[fi]);
                if (g_ao->cfg[fi] < 0) strcpy_s(c, sizeof c, "keep map value"); else sprintf_s(c, sizeof c, "%g", g_ao->cfg[fi]);
                P("  %-18s map asks %-8s -> applied %s\r\n", kAoLbl[fi], m, c);
            }
        }
        if (g_aoPass) {
            static LONG apPrev; LONG c = g_aoPass->calls, dc = c - apPrev; apPrev = c;
            P("  AO pass count (engine decision): %ld%s  [checked %ld times in 5 s]\r\n", g_aoPass->passes,
              g_aoPass->passes == 0 ? " = AO NOT RENDERED" : (g_aoPass->passes > 0 ? " = AO rendered" : " (not computed yet)"), dc);
        } else P("  AO pass-count probe: %s\r\n", g_aoPassMsg);
#if HNT_DEBUG_HOTKEYS
        {
            char k1[16], k2[16];
            sprintf_s(k1, sizeof k1, g_vkLm >= VK_F1 && g_vkLm <= VK_F24 ? "F%u" : "0x%02X", g_vkLm >= VK_F1 && g_vkLm <= VK_F24 ? g_vkLm - VK_F1 + 1 : g_vkLm);
            sprintf_s(k2, sizeof k2, g_vkAo >= VK_F1 && g_vkAo <= VK_F24 ? "F%u" : "0x%02X", g_vkAo >= VK_F1 && g_vkAo <= VK_F24 ? g_vkAo - VK_F1 + 1 : g_vkAo);
            P("  Hotkeys: %s = lightmaps/GI %s,  %s = AO %s\r\n", k1, g_lmOff ? "OFF (toggled)" : "on", k2, g_aoOff ? "OFF (toggled)" : "on");
        }
#endif
        {
            P("[Lamp shadows (menu)]\r\n");
            const wchar_t *ln[] = { L"r.Shadow.MinResolution", L"r.Shadow.FadeResolution", L"r.Shadow.TexelsPerPixelPointlight", L"r.Shadow.TexelsPerPixelSpotlight", L"r.Shadow.TexelsPerPixelRectlight",
                                    L"r.ContactShadows" };
            P("  contact shadow length: sun %g, lamps %g (-1 = as authored; lights set this session: %ld); hotkey: %s\r\n", g_contactLen, g_lampContactLen, g_contactApplied, g_contactHotOff ? "OFF (forced)" : "not forcing");
            {
                CVar *vf = FindCv(L"r.VolumetricFog"); double fg, fr;
                if (!vf || vf->kind == CV_NONE) P("  r.VolumetricFog                      not found in exe\r\n");
                else if (ReadCvar(vf, &fg, &fr)) P("  r.VolumetricFog                      %g  (live switch for volumetric fog; hotkey: %s)\r\n", fr, g_fogHotOff ? "OFF (forced)" : "not forcing");
            }
            for (int k = 0; k < 6; k++) {
                CVar *c = FindCv(ln[k]); double lg, lr;
                if (!c || c->kind == CV_NONE) { P("  %-36ls not found in exe\r\n", ln[k]); continue; }
                if (!ReadCvar(c, &lg, &lr)) { P("  %-36ls not readable yet\r\n", ln[k]); continue; }
                P("  %-36ls %g  (engine default %g)\r\n", ln[k], lr, c->haveOrig ? c->orig : lr);
            }
        }
        P("[Fog]  hook: %s; height-fog components built this session: %ld, with volumetric fog ON: %ld\r\n", g_fogMsg, g_fogSeen, g_fogVolOn);
        P("  setting: volumetric %ld (1 on / 0 off / -1 as authored), density x%g, volumetric distance %g (-1 = default)\r\n", g_volFog, g_fogDensityScale, g_fogVolDist);
        P("[Display]  r.SetRes %s; mode: %s\r\n", g_setResHolder ? "found" : "NOT FOUND",
          g_dispMsg[0] ? g_dispMsg : SavedDisplayMode() < 0 ? "game setting (not changed by the mod)" : "not applied yet");
        P("[Graphics]  (mod ini [Graphics]; -1 = game default; applied once the engine renders: %s)\r\n", g_engineUp ? "yes" : "not yet");
        for (int gi = 0; gi < N_GFX; gi++) {
            const GfxCvar *e = &kGfx[gi]; wchar_t v[64]; char nm[96], cv[96];
            GetPrivateProfileStringW(L"Graphics", e->key, L"-1", v, 64, g_cfgPath);
            WideCharToMultiByte(CP_UTF8, 0, e->key, -1, nm, sizeof nm, NULL, NULL);
            WideCharToMultiByte(CP_UTF8, 0, e->cvar, -1, cv, sizeof cv, NULL, NULL);
            CVar *c = FindCv(e->cvar); double gg, rr;
            if (c && c->kind != CV_NONE && ReadCvar(c, &gg, &rr))
                P("  %-22s ini %-7g %-32s live %-8g game default %g\r\n", nm, wcstod(v, NULL), cv, rr, CvarOrig(e->cvar));
            else P("  %-22s ini %-7g %-32s NOT RESOLVED\r\n", nm, wcstod(v, NULL), cv);
        }
        P("  Anisotropy (Engine.ini r.MaxAnisotropy, restart) ini %d; MouseSmoothing (Input.ini, restart) ini %d\r\n",
          GetPrivateProfileIntW(L"Graphics", L"Anisotropy", -1, g_cfgPath), GetPrivateProfileIntW(L"Input", L"MouseSmoothing", -1, g_cfgPath));
        P("[Field of view]  hook: %s\r\n", g_fovMsg);
        if (g_fov) {
            static LONG fovPrev; LONG fh = g_fov->hits, fd = fh - fovPrev; fovPrev = fh;
            float a = FovAspect(), gf = g_fov->lastOrig, hh = g_fov->scale > 0.0f ? gf * g_fov->scale : gf;
            P("  game camera FOV: H %.1f (V %.1f); rendered: H %.1f (V %.1f); setting: %s %s; aspect %.3f; projections in 5 s: %ld\r\n",
              gf, FovVfromH(gf, a), hh, FovVfromH(hh, a), g_fovTarget > 1.0f ? "" : "game default", g_fovTarget > 1.0f ? (g_fovVertical ? "vertical" : "horizontal") : "", a, fd);
        }
        P("[Real-time static lights]  hook: %s\r\n", g_rtlMsg);
        if (g_rtl) {
            int ready = 0, tracked = ConvCount(&ready);
            P("  RealtimeStaticLights=%ld (applies as levels load); lights created this session: %ld, static ones converted: %ld\r\n",
              g_rtl->enabled, g_rtl->seen, g_rtl->converted);
            P("  F8 swap: %s; currently tracked converted lights: %d (%d ready), destroyed/unloaded: %ld\r\n",
              g_swapMsg, tracked, ready, g_convDestroyed);
            P("  Mode: %s\r\n", g_lmOff ? "REAL-TIME (converted static lights live)" : "BAKED (lightmaps on, converted static lights hidden)");
            P("  RealtimeBakedFill=%g (REAL-TIME keeps this fraction of the lightmap/GI; 0 = lightmaps off)\r\n", g_rtFill);
            P("  Converted-light scales: brightness x%g, radius x%g (point/spot/rect ctors found: %d; radius-scaled lights: %ld; rebuilt converted lights re-scaled: %ld)\r\n",
              g_scaleI, g_scaleR, g_nLocalRet, g_scaledR, g_rebuilt);
            P("  RealtimeStationaryShadows=%ld (applies as levels load); stationary lights switched to real-time shadows: %ld\r\n",
              g_stationaryRT, g_statConverted);
            P("  ForceRealtimeShadows=%ld (0 off, 1 converted lights, 2 all; applies as levels load); lights whose real-time shadows were forced on: %ld\r\n",
              g_forceShadows, g_forced);
            {
                static const char *nm[CAT_N] = { "Static -> real-time (converted)", "Static (kept baked)", "Stationary (static geometry: BAKED shadow map)",
                                                 "Stationary -> fully real-time shadows", "Movable (map-authored)" };
                P("[Light shadows - lights created this session]\r\n  %-48s %6s %s\r\n", "kind", "lights", "real-time shadows on/off");
                for (int k = 0; k < CAT_N; k++)
                    if (g_catN[k]) P("  %-48s %6ld %ld / %ld\r\n", nm[k], g_catN[k], g_catDyn[k], g_catN[k] - g_catDyn[k]);
            }
        }
        P("[Sun shadows]  directional ctor found: %s; sun lights built with these settings this session: %ld\r\n", g_dirRet ? "yes" : "NO", g_sunApplied);
        P("  resolution scale %s, cascade distribution %s, sharpen %s (all applied as the sun light is built = next level load)\r\n",
          g_sunRes > 0 ? "set" : "map 0.7", g_sunDist > 0 ? "set" : "map 3", g_sunSharp >= 0 ? "set" : "map 0");
        P("  values: res %g, distribution %g, sharpen %g\r\n", g_sunRes, g_sunDist, g_sunSharp);
        P("[Sun shadow cascades]  hook: %s\r\n", g_csmMsg);
        if (g_csm) {
            static LONG csmPrev; LONG h2 = g_csm->hits; LONG d2 = h2 - csmPrev; csmPrev = h2;
            CVar *mc = FindCv(L"r.Shadow.CSM.MaxCascades"); double mg = 0, mr = 0; int haveMc = mc && ReadCvar(mc, &mg, &mr);
            if (d2 == 0) P("  no directional light shadows queried in the last 5 s (indoors / no sun light loaded)\r\n");
            else {
                LONG want = g_csm->cfg, mapv = g_csm->map;
                LONG eff = (mapv > 0 && want > 0) ? want : mapv;
                if (haveMc && eff > (LONG)mr) eff = (LONG)mr;
                P("  map light asks %ld cascade(s) -> ini SunCascades=%ld -> effective %ld (capped by r.Shadow.CSM.MaxCascades=%d); queried %ld times in 5 s\r\n",
                  mapv, want, eff, haveMc ? (int)mr : -1, d2);
            }
        }
        P("  (edit %ls - picked up within 5 s)\r\n", g_cfgPath);

        // frames
        P("\r\n[Frames]  Present hook: %s\r\n", g_hookMsg);
        if (g_frames > 0) {
            P("  Last 5 s: %.1f fps", fpsLast);
            if (fWarm >= 0) P("   since 30 s: avg %.1f, min %.1f, max %.1f (5 s windows)", fpsAvg, fpsMin, fpsMax);
            P("\r\n  Game's Present SyncInterval = %ld (%s)\r\n", g_sync, g_sync == 0 ? "in-game VSync OFF" : "in-game VSync ON");
            if (g_bbW) P("  Back buffer %ldx%ld, %s\r\n", g_bbW, g_bbH, g_windowed ? "windowed / borderless" : "exclusive fullscreen");
        } else if (!strcmp(g_hookMsg, "installed")) P("  no frames counted yet\r\n");

        // settings table
        char tmp[300];
        P("\r\n[Engine.ini SystemSettings vs live engine values]\r\n");
        P("  %-36s %-10s %-10s %s\r\n", "setting", "ini", "engine", "result");
        for (int i = 0; i < g_cvCount; i++) {
            if (g_cv[i].extra) continue;   // menu-controlled settings are listed separately
            CVar *c = &g_cv[i]; char name[100], want[64], have[64] = "?";
            WideCharToMultiByte(CP_UTF8, 0, c->name, -1, name, sizeof name, NULL, NULL);
            WideCharToMultiByte(CP_UTF8, 0, c->want, -1, want, sizeof want, NULL, NULL);
            const char *res;
            if (c->kind == CV_NONE) { res = "not found in exe (can't check)"; unk++; }
            else if (!ReadCvar(c, &g, &r)) { res = "not readable yet"; unk++; }
            else {
                double w = wcstod(c->want, NULL);
                int match = c->kind == CV_INT ? ((int)r == (int)w) : (fabs(r - w) <= 1e-4 * (fabs(w) > 1 ? fabs(w) : 1));
                Fmt(have, sizeof have, c->kind, r);
                if (g != r) { char gs[32]; Fmt(gs, sizeof gs, c->kind, g); strcat_s(have, sizeof have, " (gt "); strcat_s(have, sizeof have, gs); strcat_s(have, sizeof have, ")"); }
                if (!match && c->haveOrig && fabs(r - c->orig) > 1e-4) { res = "set by F1 menu"; ok++; }
                else { res = match ? "OK" : "MISMATCH"; match ? ok++ : bad++; }
                if (c->haveLast && c->last != r) {
                    char a[32], b[32]; Fmt(a, sizeof a, c->kind, c->last); Fmt(b, sizeof b, c->kind, r);
                    sprintf_s(tmp, sizeof tmp, "%02d:%02d:%02d  %s changed %s -> %s", st.wHour, st.wMinute, st.wSecond, name, a, b); Hist(tmp);
                }
                c->last = r; c->haveLast = 1;
            }
            P("  %-36s %-10s %-10s %s\r\n", name, want, have, res);
        }
        P("  => %d OK, %d mismatch, %d unchecked%s\r\n", ok, bad, unk, g_cvCount ? "" : "  (could not read Engine.ini)");
        if (g_histN) { P("\r\n[Changes seen while running]\r\n"); for (int i = 0; i < g_histN; i++) P("  %s\r\n", g_hist[i]); }
        P("\r\nini: %ls\r\n", g_iniPath);
        #undef P
        WriteLog(out);
    }
}

// ---------------------------------------------------------------- F1 settings menu
// Values live in HelloNeighborTweaks.ini; a change is written there and re-read at once (ReadAoConfig).
typedef struct { const wchar_t *label; int kind; const wchar_t *sec, *key; float mn, mx, step; int allowMap; const wchar_t *def; } MItem;
enum { MK_MODE, MK_AO, MK_FLOAT, MK_INT, MK_BOOL, MK_HEADER, MK_DISPLAY, MK_PRESET };

// v27: quality presets. Each writes only the performance-relevant keys below; the look settings (AO, exposure, fog
// density, baked fill, light brightness/radius, sun distribution/sharpen) and personal ones (display, V-Sync, fps
// limit, FOV, hotkeys) are never touched. Ultra = the user's own tuned settings of 2026-11-22.
// Lighting conversion and sun shadow settings apply on the next level load; the rest is live.
#define N_PRESETS 4
static const wchar_t *kPresetName[N_PRESETS] = { L"Low", L"Medium", L"High", L"Ultra" };
typedef struct { const wchar_t *sec, *key; const wchar_t *v[N_PRESETS]; } PresetKey;   // Low, Medium, High, Ultra
static const PresetKey kPreset[] = {
    // real-time lighting and shadows are ON in every preset (user decision 2026-11-22); presets scale distance and
    // quality instead.
    { L"Lighting",    L"RealtimeStaticLights",      { L"1",    L"1",    L"1",    L"1"    } },
    { L"Lighting",    L"RealtimeStationaryShadows", { L"1",    L"1",    L"1",    L"1"    } },
    { L"Lighting",    L"ForceRealtimeShadows",      { L"2",    L"2",    L"2",    L"2"    } },
    // sun: cascades, resolution (-1 = map 0.7), distance (-1 = Engine.ini r.Shadow.DistanceScale 2)
    { L"Shadows",     L"SunCascades",               { L"2",    L"3",    L"4",    L"4"    } },
    { L"Shadows",     L"SunResolutionScale",        { L"0.5",  L"0.6",  L"-1",   L"-1"   } },
    { L"Shadows",     L"SunShadowDistance",         { L"1.25", L"1.5",  L"1.75", L"2"    } },   // v29: explicit (Engine.ini line removed)
    // lamps: min resolution + texel density = sharpness; fade resolution = how far lamp shadows reach
    // (a shadow fades out once its resolution drops below it, so higher = they fade out closer)
    { L"Shadows",     L"LampMinResolution",         { L"128",  L"256",  L"512",  L"1024" } },
    { L"Shadows",     L"LampFadeResolution",        { L"128",  L"96",   L"64",   L"64"   } },
    { L"Shadows",     L"LampTexelDensity",          { L"1",    L"1.25", L"1.5",  L"2"    } },
    { L"Shadows",     L"ContactShadowLength",       { L"0.05", L"0.05", L"0.05", L"0.05" } },
    // reflections: rougher surfaces traced = more cost (-1 = map 0.3)
    { L"Reflections", L"MaxRoughness",              { L"-1",   L"0.4",  L"0.6",  L"0.6"  } },
    // volumetric fog (-1 distance = engine 6000)
    { L"Fog",         L"VolumetricFog",             { L"0",    L"1",    L"1",    L"1"    } },
    { L"Fog",         L"VolumetricDistance",        { L"-1",   L"4000", L"-1",   L"-1"   } },
    // v29/v30 graphics options (formerly Engine.ini). Every quality setting is in the presets; only cosmetic ones
    // stay out: sharpen, vignette, motion blur, depth of field, mouse smoothing (user decision 2026-11-22).
    { L"Graphics",    L"ScreenPercentage",          { L"75",   L"90",   L"100",  L"111.2"} },
    { L"Graphics",    L"AAQuality",                 { L"3",    L"4",    L"5",    L"6"    } },   // 3-6 = TAA
    { L"Graphics",    L"ObjectShadowDensity",       { L"1",    L"1.25", L"1.5",  L"2"    } },
    { L"Graphics",    L"TexturePoolMB",             { L"2048", L"4096", L"6144", L"8192" } },
    { L"Graphics",    L"FullyLoadTextures",         { L"0",    L"0",    L"1",    L"1"    } },
    { L"Graphics",    L"Anisotropy",                { L"4",    L"8",    L"16",   L"16"   } },   // restart to apply
    { L"Graphics",    L"ShadowMapResolution",       { L"1024", L"2048", L"4096", L"4096" } },
    { L"Graphics",    L"ShadowRadiusThreshold",     { L"0.03", L"0.01", L"0.005",L"0.005"} },
    { L"Graphics",    L"AOLevels",                  { L"1",    L"2",    L"3",    L"3"    } },
    { L"Graphics",    L"SSRQuality",                { L"2",    L"3",    L"4",    L"4"    } },
    { L"Graphics",    L"ViewDistance",              { L"1",    L"2",    L"3",    L"3"    } },
    { L"Graphics",    L"MeshDetailDistance",        { L"1",    L"0.75", L"0.5",  L"0.5"  } },
    { L"Graphics",    L"FoliageDetailDistance",     { L"1",    L"2",    L"3",    L"3"    } },
    { L"Graphics",    L"LightDrawDistance",         { L"1",    L"2",    L"3",    L"3"    } },
};
#define N_PRESET_KEYS ((int)(sizeof kPreset / sizeof kPreset[0]))
static int CurrentPreset(void)   // index, or -1 = Custom
{
    for (int p = N_PRESETS - 1; p >= 0; p--) {
        int all = 1;
        for (int k = 0; k < N_PRESET_KEYS && all; k++) {
            wchar_t v[64]; GetPrivateProfileStringW(kPreset[k].sec, kPreset[k].key, L"-1", v, 64, g_cfgPath);
            if (fabs(wcstod(v, NULL) - wcstod(kPreset[k].v[p], NULL)) > 1e-4) all = 0;
        }
        if (all) return p;
    }
    return -1;
}
static void ApplyPreset(int p)
{
    for (int k = 0; k < N_PRESET_KEYS; k++) WritePrivateProfileStringW(kPreset[k].sec, kPreset[k].key, kPreset[k].v[p], g_cfgPath);
    ReadAoConfig();
    // Without real-time static lights, REAL-TIME mode would only dim the lightmaps: go to BAKED.
    if (g_lmOff && !GetPrivateProfileIntW(L"Lighting", L"RealtimeStaticLights", 1, g_cfgPath)) ToggleLightingMode();
}

static const MItem kMenu[] = {
    { L"DISPLAY",                    MK_HEADER },
#if HNT_DEBUG_HOTKEYS
    { L"Display mode  (F11 cycles)", MK_DISPLAY, L"Display", L"Mode" },
#else
    { L"Display mode",               MK_DISPLAY, L"Display", L"Mode" },
#endif
    { L"V-Sync",                     MK_INT,   L"Display",  L"VSync",                  0.0f,    1.0f,  1.0f,  1, L"-1" },
    { L"Frame rate limit",           MK_INT,   L"Display",  L"FrameRateLimit",         30.0f, 360.0f,  1.0f,  1, L"-1" },
    { L"INPUT",                      MK_HEADER },
    { L"Mouse smoothing",            MK_INT,   L"Input",    L"MouseSmoothing",         0.0f,    0.0f,  1.0f,  1, L"-1" },
    { L"QUALITY",                    MK_HEADER },
    { L"Preset",                     MK_PRESET },
    { L"IMAGE",                      MK_HEADER },
    { L"Render resolution",          MK_FLOAT, L"Graphics", L"ScreenPercentage",       50.0f, 200.0f,  1.0f,  1, L"-1" },
    { L"Anti-aliasing quality",      MK_INT,   L"Graphics", L"AAQuality",              0.0f,    6.0f,  1.0f,  1, L"-1" },
    { L"Sharpen",                    MK_FLOAT, L"Graphics", L"Sharpen",                0.0f,    2.0f,  0.05f, 1, L"-1" },
    { L"Vignette",                   MK_INT,   L"Graphics", L"Vignette",               0.0f,    0.0f,  1.0f,  1, L"-1" },
    { L"Motion blur",                MK_INT,   L"Graphics", L"MotionBlur",             0.0f,    0.0f,  1.0f,  1, L"-1" },
    { L"Depth of field",             MK_INT,   L"Graphics", L"DepthOfField",           0.0f,    0.0f,  1.0f,  1, L"-1" },
    { L"VIEW DISTANCE",              MK_HEADER },
    { L"View distance",              MK_FLOAT, L"Graphics", L"ViewDistance",           0.5f,    5.0f,  0.25f, 1, L"-1" },
    { L"Mesh detail distance",       MK_FLOAT, L"Graphics", L"MeshDetailDistance",     0.1f,    2.0f,  0.05f, 1, L"-1" },
    { L"Foliage detail distance",    MK_FLOAT, L"Graphics", L"FoliageDetailDistance",  0.5f,    5.0f,  0.25f, 1, L"-1" },
    { L"Light draw distance",        MK_FLOAT, L"Graphics", L"LightDrawDistance",      0.5f,    5.0f,  0.25f, 1, L"-1" },
    { L"LIGHTING",                   MK_HEADER },
#if HNT_DEBUG_HOTKEYS
    { L"Lighting mode",              MK_MODE },
#endif
    { L"Lightmap / GI scale",        MK_FLOAT, L"Lighting", L"IndirectLightingIntensity", 0.0f, 4.0f, 0.1f,  1, L"-1" },
    { L"Real-time baked fill",       MK_FLOAT, L"Lighting", L"RealtimeBakedFill",      0.0f,    1.0f,  0.05f, 0, L"0.15" },
    { L"Converted light brightness", MK_FLOAT, L"Lighting", L"ConvertedBrightnessScale", 0.25f, 8.0f, 0.25f, 0, L"2" },
    { L"Converted light radius",     MK_FLOAT, L"Lighting", L"ConvertedRadiusScale",   0.25f,   8.0f,  0.25f, 0, L"2" },
    { L"Stationary: real-time shdw", MK_BOOL,  L"Lighting", L"RealtimeStationaryShadows", 0.0f, 1.0f, 1.0f, 0, L"0" },
    { L"Force real-time shadows",    MK_INT,   L"Lighting", L"ForceRealtimeShadows",   0.0f,    2.0f,  1.0f,  0, L"0" },
    { L"AMBIENT OCCLUSION",          MK_HEADER },
#if HNT_DEBUG_HOTKEYS
    { L"Ambient occlusion",          MK_AO },
#endif
    { L"AO quality levels",          MK_INT,   L"Graphics", L"AOLevels",               0.0f,    3.0f,  1.0f,  1, L"-1" },
    { L"AO intensity",               MK_FLOAT, L"AmbientOcclusion", L"Intensity",      0.0f,    2.0f,  0.05f, 1, L"0.5" },
    { L"AO radius",                  MK_FLOAT, L"AmbientOcclusion", L"Radius",         0.0f, 1000.0f, 25.0f,  1, L"200" },
    { L"AO power",                   MK_FLOAT, L"AmbientOcclusion", L"Power",          0.25f,   8.0f,  0.25f, 1, L"-1" },
    { L"AO fade distance",           MK_FLOAT, L"AmbientOcclusion", L"FadeDistance",   500.0f, 20000.0f, 500.0f, 1, L"8000" },
    { L"REFLECTIONS",                MK_HEADER },
    { L"Reflection quality",         MK_INT,   L"Graphics", L"SSRQuality",             0.0f,    4.0f,  1.0f,  1, L"-1" },
    { L"Reflections intensity",      MK_FLOAT, L"Reflections", L"Intensity",           0.0f,  100.0f, 10.0f,  1, L"100" },
    { L"Reflections max roughness",  MK_FLOAT, L"Reflections", L"MaxRoughness",        0.05f,   1.0f,  0.05f, 1, L"-1" },
    { L"EXPOSURE (eye adaptation)",  MK_HEADER },
    { L"Dark adaptation limit",      MK_FLOAT, L"Exposure", L"MinBrightness",          0.01f,   2.0f,  0.01f, 1, L"-1" },
    { L"Bright adaptation limit",    MK_FLOAT, L"Exposure", L"MaxBrightness",          0.1f,   16.0f,  0.1f,  1, L"-1" },
    { L"Adapt speed: into dark",     MK_FLOAT, L"Exposure", L"SpeedDown",              0.1f,   10.0f,  0.1f,  1, L"-1" },
    { L"Adapt speed: into light",    MK_FLOAT, L"Exposure", L"SpeedUp",                0.1f,   10.0f,  0.1f,  1, L"-1" },
    { L"SUN SHADOWS",                MK_HEADER },
    { L"Sun shadow cascades",        MK_INT,   L"Shadows",  L"SunCascades",            1.0f,    4.0f,  1.0f,  1, L"4" },
    { L"Sun shadow resolution",      MK_FLOAT, L"Shadows",  L"SunResolutionScale",     0.25f,   1.0f,  0.05f, 1, L"-1" },
    { L"Sun cascade distribution",   MK_FLOAT, L"Shadows",  L"SunCascadeDistribution", 1.0f,    4.0f,  0.1f,  1, L"2" },
    { L"Sun shadow sharpen",         MK_FLOAT, L"Shadows",  L"SunShadowSharpen",       0.0f,    1.0f,  0.05f, 1, L"0.3" },
    { L"Sun shadow distance",        MK_FLOAT, L"Shadows",  L"SunShadowDistance",      0.25f,   2.0f,  0.05f, 1, L"-1" },
    { L"LAMP SHADOWS",               MK_HEADER },
    { L"Lamp shadow min resolution", MK_INT,   L"Shadows",  L"LampMinResolution",      32.0f, 2048.0f, 32.0f, 1, L"256" },
    { L"Lamp shadow fade resolution",MK_INT,   L"Shadows",  L"LampFadeResolution",     8.0f,   256.0f, 8.0f,  1, L"32" },
    { L"Lamp shadow density",        MK_FLOAT, L"Shadows",  L"LampTexelDensity",       0.5f,   6.0f,  0.25f, 1, L"2" },
    { L"SHADOW EXTRAS",              MK_HEADER },
    { L"Shadow map resolution",      MK_INT,   L"Graphics", L"ShadowMapResolution",    512.0f, 8192.0f, 512.0f, 1, L"-1" },
    { L"Small-object shadows",       MK_FLOAT, L"Graphics", L"ShadowRadiusThreshold",  0.001f,  0.1f, 0.001f, 1, L"-1" },
    { L"Object shadow density",      MK_FLOAT, L"Graphics", L"ObjectShadowDensity",    0.5f,    4.0f,  0.25f, 1, L"-1" },
    { L"Sun contact shadows",        MK_FLOAT, L"Shadows",  L"ContactShadowLength",    0.0f,    0.5f,  0.01f, 1, L"0.05" },
    { L"Lamp contact shadows",       MK_FLOAT, L"Shadows",  L"LampContactShadowLength",0.0f,    0.5f,  0.01f, 1, L"-1" },
    { L"FOG",                        MK_HEADER },
    { L"Volumetric fog",             MK_BOOL,  L"Fog",      L"VolumetricFog",          0.0f,    1.0f,  1.0f,  0, L"1" },
    { L"Volumetric fog density",     MK_FLOAT, L"Fog",      L"DensityScale",           0.25f,  10.0f,  0.25f, 0, L"1" },
    { L"Volumetric fog distance",    MK_FLOAT, L"Fog",      L"VolumetricDistance",     1000.0f, 30000.0f, 1000.0f, 1, L"-1" },
    { L"CAMERA",                     MK_HEADER },
    { L"Field of view",              MK_FLOAT, L"Camera",   L"FOV",                    30.0f,  150.0f, 1.0f,  1, L"-1" },
    { L"FOV mode",                   MK_BOOL,  L"Camera",   L"FOVMode",                0.0f,    1.0f,  1.0f,  0, L"Horizontal" },
    { L"TEXTURES",                   MK_HEADER },
    { L"Texture pool",               MK_INT,   L"Graphics", L"TexturePoolMB",          512.0f, 16384.0f, 512.0f, 1, L"-1" },
    { L"Fully load used textures",   MK_INT,   L"Graphics", L"FullyLoadTextures",      0.0f,    1.0f,  1.0f,  1, L"-1" },
    { L"Anisotropic filtering",      MK_INT,   L"Graphics", L"Anisotropy",             1.0f,   16.0f,  1.0f,  1, L"-1" },
};
#define MENU_N ((int)(sizeof kMenu / sizeof kMenu[0]))

// v31: collapsible categories. Headers are selectable: "> NAME" = collapsed, "v NAME" = expanded. Right/Enter opens,
// Left/Enter closes, Right on an open header steps into it. All start collapsed; several may be open at once, and the
// menu scrolls to keep the selection on screen.
static volatile LONG g_menuExpanded;                 // bit n = n-th header is open (session only)
static volatile LONG g_menuMaxLines = 40;            // rows that fit on the game window (set by the overlay thread)
static int MenuHeaderOrd(int i) { int h = -1; for (int k = 0; k <= i && k < MENU_N; k++) if (kMenu[k].kind == MK_HEADER) h++; return h; }
static int MenuParent(int i) { for (int k = i; k >= 0; k--) if (kMenu[k].kind == MK_HEADER) return k; return -1; }
static int MenuIsOpen(int header) { return (g_menuExpanded >> MenuHeaderOrd(header)) & 1; }
static int MenuVisible(int i)
{
    if (kMenu[i].kind == MK_HEADER) return 1;
    int p = MenuParent(i); return p < 0 || MenuIsOpen(p);
}
static void MenuMove(int dir)
{
    int s = g_menuSel;
    for (int k = 0; k < MENU_N; k++) { s = (s + (dir > 0 ? 1 : MENU_N - 1)) % MENU_N; if (MenuVisible(s)) break; }
    g_menuSel = s;
}
static void MenuSetOpen(int header, int on)
{
    LONG bit = 1L << MenuHeaderOrd(header);
    if (on) InterlockedOr(&g_menuExpanded, bit); else InterlockedAnd(&g_menuExpanded, ~bit);
}

// ---- display mode via r.SetRes (same call the engine's FSystemResolution::RequestResolutionChange makes):
//   flags = cvar->GetFlags() & 0xFF000000 (vtable +0x18) ; cvar->Set(L"WxH[f|wf|w]", flags) (vtable +0x60)
// The engine's r.SetRes sink then resizes the game window. Must run on the game thread (window proc).
static HWND g_gameWnd;                          // (also declared in the overlay section)
// g_setResHolder: global holding r.SetRes's IConsoleVariable*; g_dispMode: 0 windowed, 1 borderless, 2 exclusive
// (declared near the top)
#define WM_HNT_APPLYDISPLAY (WM_APP + 0x51)
static const wchar_t *kDispName[3] = { L"Windowed", L"Borderless fullscreen", L"Exclusive fullscreen" };

static void FindSetRes(void)
{
    const unsigned char *needle = (const unsigned char *)L"r.SetRes"; size_t nl = sizeof L"r.SetRes";
    for (int s = 0; s < g_dataCount && !g_setResHolder; s++) {
        unsigned char *d = g_data[s].p; size_t n = g_data[s].n;
        for (size_t i = 0; i + nl <= n && !g_setResHolder; i += 2) {
            if (d[i] != 'r' || memcmp(d + i, needle, nl)) continue;
            unsigned char *str = d + i;
            for (size_t k = 0; k + 32 <= g_textLen; k++) {
                unsigned char *q = g_text + k;
                if (q[0] != 0x48 || q[1] != 0x8D || q[2] != 0x15 || Rip(q + 7, q + 3) != str) continue;
                if (q[7] != 0xFF || q[8] != 0x10) continue;                     // call qword ptr [rax] (string cvar)
                for (int j = 9; j < 24; j++)
                    if (q[j] == 0x48 && q[j + 1] == 0x89 && q[j + 2] == 0x05) { g_setResHolder = Rip(q + j + 7, q + j + 3); break; }
                break;
            }
        }
    }
}
static int ParseWxH(const wchar_t *s, int *w, int *h) { return swscanf_s(s, L"%dx%d", w, h) == 2 && *w > 0 && *h > 0; }
static void ApplyDisplayMode(int mode)   // game thread only
{
    if (!g_setResHolder || !*(void **)g_setResHolder) { strcpy_s(g_dispMsg, sizeof g_dispMsg, "r.SetRes not found"); return; }
    int w = 1920, h = 1080, mw = 0, mh = 0, ww = 0, wh = 0;
    if (g_gameWnd) {
        MONITORINFO mi = { sizeof mi };
        if (GetMonitorInfoW(MonitorFromWindow(g_gameWnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            mw = mi.rcMonitor.right - mi.rcMonitor.left; mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
            ww = mi.rcWork.right - mi.rcWork.left;       wh = mi.rcWork.bottom - mi.rcWork.top;
        }
    }
    if (mode == 0) {
        // v33: "Auto" (or anything unreadable) = 75% of the monitor; a size larger than the desktop work area is
        // scaled down to fit it (keeping its aspect ratio)
        wchar_t v[32]; GetPrivateProfileStringW(L"Display", L"WindowedResolution", L"Auto", v, 32, g_cfgPath);
        if (!ParseWxH(v, &w, &h)) { if (mw > 0) { w = mw * 3 / 4; h = mh * 3 / 4; } else { w = 1600; h = 900; } }
        if (ww > 0 && wh > 0 && (w > ww || h > wh)) {
            double s = (double)ww / w < (double)wh / h ? (double)ww / w : (double)wh / h;
            w = (int)(w * s); h = (int)(h * s);
        }
    } else if (mw > 0) { w = mw; h = mh; }
    wchar_t res[48]; swprintf_s(res, 48, L"%dx%d%s", w, h, mode == 2 ? L"f" : mode == 1 ? L"wf" : L"w");
    __try {
        void *cv = *(void **)g_setResHolder; void **vt = *(void ***)cv;
        unsigned int flags = ((unsigned int (__fastcall *)(void *))vt[3])(cv) & 0xFF000000u;
        ((void (__fastcall *)(void *, const wchar_t *, unsigned int))vt[12])(cv, res, flags);
        g_dispMode = mode;
        sprintf_s(g_dispMsg, sizeof g_dispMsg, "%ls (r.SetRes %ls)", kDispName[mode], res);
    } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(g_dispMsg, sizeof g_dispMsg, "r.SetRes call failed"); }
}
// [Display] Mode: Windowed 0, Borderless 1, Exclusive 2; Game (or missing) = -1 = leave the game's own setting.
static int SavedDisplayMode(void)
{
    wchar_t v[32]; GetPrivateProfileStringW(L"Display", L"Mode", L"Game", v, 32, g_cfgPath);
    return (v[0] == L'W' || v[0] == L'w') ? 0 : (v[0] == L'E' || v[0] == L'e') ? 2 : (v[0] == L'B' || v[0] == L'b') ? 1 : -1;
}
// The mode the game itself starts in (GameUserSettings.ini FullscreenMode: 0 fullscreen, 1 windowed fullscreen,
// 2 windowed), so the menu shows it and the overlay stays off in exclusive fullscreen.
static int GameDisplayMode(void)
{
    wchar_t gus[MAX_PATH], la[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) return 1;
    swprintf_s(gus, MAX_PATH, L"%s\\HelloNeighbor\\Saved\\Config\\WindowsNoEditor\\GameUserSettings.ini", la);
    int fm = GetPrivateProfileIntW(L"/Script/Engine.GameUserSettings", L"FullscreenMode", 1, gus);
    return fm == 0 ? 2 : fm == 2 ? 0 : 1;
}
static int StartDisplayMode(void) { int s = SavedDisplayMode(); return s >= 0 ? s : GameDisplayMode(); }
static void SetDisplayMode(int mode)     // game thread only
{
    WritePrivateProfileStringW(L"Display", L"Mode", mode == 0 ? L"Windowed" : mode == 2 ? L"Exclusive" : L"Borderless", g_cfgPath);
    ApplyDisplayMode(mode);
}
// Exclusive fullscreen hides the overlay/menu, so it is only applied after an explicit confirmation.
static volatile LONG g_confirmExclusive;
static void RequestDisplayMode(int mode)  // game thread only
{
    if (mode == 2 && g_dispMode != 2) { g_confirmExclusive = 1; return; }   // ask first
    g_confirmExclusive = 0;
    SetDisplayMode(mode);
}

// Frame rate over ~1 s windows from the Present hook's counter (overlay thread only; 0 = not known yet)
static double MenuFps(void)
{
    static LONG64 f0 = -1; static LARGE_INTEGER q0; static double v;
    LARGE_INTEGER q, qf; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&qf);
    LONG64 f = g_frames;
    if (f0 < 0) { f0 = f; q0 = q; return v; }
    double dt = (double)(q.QuadPart - q0.QuadPart) / (double)qf.QuadPart;
    if (dt >= 1.0) { v = f > f0 ? (double)(f - f0) / dt : 0.0; f0 = f; q0 = q; }
    return v;
}
static float MenuGet(const MItem *it)
{
    wchar_t v[64]; GetPrivateProfileStringW(it->sec, it->key, it->def, v, 64, g_cfgPath);
    return (float)wcstod(v, NULL);
}
static void MenuValueText(const MItem *it, wchar_t *o, size_t n)
{
    if (it->kind == MK_HEADER) { o[0] = 0; return; }
    if (it->kind == MK_DISPLAY) { swprintf_s(o, n, L"%s%s", kDispName[g_dispMode], SavedDisplayMode() < 0 ? L" (game setting)" : L""); return; }
    if (it->kind == MK_PRESET) {
        int p = CurrentPreset();
        if (p < 0) wcscpy_s(o, n, L"Custom (Left/Right picks a preset)");
        else swprintf_s(o, n, L"%s  (lighting/sun: next load; aniso: restart)", kPresetName[p]);
        return;
    }
    if (it->kind == MK_MODE) { wcscpy_s(o, n, g_lmOff ? L"REAL-TIME" : L"BAKED"); return; }
    if (it->kind == MK_AO) { wcscpy_s(o, n, g_aoOff ? L"OFF" : L"ON"); return; }
    if (!wcscmp(it->key, L"FOVMode")) { wcscpy_s(o, n, g_fovVertical ? L"Vertical" : L"Horizontal"); return; }
    float x = MenuGet(it);
    if (!wcscmp(it->sec, L"Fog")) {
        if (!wcscmp(it->key, L"VolumetricFog")) wcscpy_s(o, n, x > 0.5f ? L"ON (live if the level loaded with it ON)" : L"OFF (live)");
        else if (!wcscmp(it->key, L"DensityScale")) swprintf_s(o, n, L"x%.2f (volumetric only; next load)", x);
        else if (x < 0.0f) wcscpy_s(o, n, L"engine default (6000 = 60 m)");
        else swprintf_s(o, n, L"%.0f = %.0f m (next load)", x, x / 100.0f);
        return;
    }
    if (!wcscmp(it->sec, L"Input")) {   // mouse smoothing (Input.ini)
        wcscpy_s(o, n, x == 0.0f ? L"OFF (restart to apply)" : L"ON - game default (restart to apply)");
        return;
    }
    if (!wcscmp(it->sec, L"Graphics")) {
        if (!wcscmp(it->key, L"Anisotropy")) {   // Engine.ini r.MaxAnisotropy
            if (x < 0.0f) wcscpy_s(o, n, L"game default (restart to apply)");
            else swprintf_s(o, n, L"%dx (restart to apply)", (int)(x + 0.5f));
            return;
        }
        const GfxCvar *e = FindGfx(it->key);
        if (e && e->isSwitch) { wcscpy_s(o, n, x == 0.0f ? L"OFF - live" : L"ON - game default"); return; }
        if (x < 0.0f) {
            double g0 = e ? CvarOrig(e->cvar) : -1.0;
            if (!g_engineUp || g0 < 0.0) wcscpy_s(o, n, L"game default");
            else swprintf_s(o, n, L"game default (%g)", g0);
            return;
        }
        if (!wcscmp(it->key, L"ScreenPercentage")) swprintf_s(o, n, L"%.1f%% - live", x);
        else if (!wcscmp(it->key, L"ShadowMapResolution")) swprintf_s(o, n, L"%d px - live", (int)(x + 0.5f));
        else if (!wcscmp(it->key, L"TexturePoolMB")) swprintf_s(o, n, L"%d MB - live", (int)(x + 0.5f));
        else if (!wcscmp(it->key, L"FullyLoadTextures")) wcscpy_s(o, n, x > 0.5f ? L"ON - live" : L"OFF - live");
        else if (!wcscmp(it->key, L"MeshDetailDistance")) swprintf_s(o, n, L"%.2f (lower = more detail) - live", x);
        else if (!wcscmp(it->key, L"ShadowRadiusThreshold")) swprintf_s(o, n, L"%.3f (lower = more shadows) - live", x);
        else if (it->kind == MK_INT) swprintf_s(o, n, L"%d - live", (int)(x + 0.5f));
        else swprintf_s(o, n, L"%.2f - live", x);
        return;
    }
    if (!wcscmp(it->key, L"FrameRateLimit")) {
        double fps = MenuFps();
        wchar_t now[32]; if (fps > 0.0) swprintf_s(now, 32, L"%.0f fps now", fps); else wcscpy_s(now, 32, L"fps ?");
        if (x < 0.0f) { double o0 = CvarOrig(L"t.MaxFPS"); swprintf_s(o, n, L"game setting (%s; %s)", o0 > 0.0 ? L"capped" : L"unlimited", now); }
        else if (x < 1.0f) swprintf_s(o, n, L"UNLIMITED - live (%s)", now);
        else swprintf_s(o, n, L"%d fps - live (%s)", (int)(x + 0.5f), now);
        return;
    }
    if (!wcscmp(it->key, L"VSync")) {
        const wchar_t *now = g_sync < 0 ? L"?" : g_sync ? L"ON" : L"OFF";   // what the game passes to Present
        if (x < 0.0f) swprintf_s(o, n, L"game setting (now %s)", now);
        else swprintf_s(o, n, L"%s - live (now %s)", x > 0.5f ? L"ON" : L"OFF", now);
        return;
    }
    if (!wcscmp(it->sec, L"Exposure")) {
        int fi = !wcscmp(it->key, L"MinBrightness") ? F_EX_MIN : !wcscmp(it->key, L"MaxBrightness") ? F_EX_MAX :
                 !wcscmp(it->key, L"SpeedUp") ? F_EX_MAX + 1 : F_EX_MAX + 2;
        float m = g_ao ? g_ao->map[fi] : -1.0f;
        wchar_t mv[24]; if (m < 0.0f) wcscpy_s(mv, 24, L"?"); else swprintf_s(mv, 24, L"%.3g", m);
        const wchar_t *lock = (g_ao && g_ao->map[F_EX_MIN] >= 0.0f && g_ao->map[F_EX_MIN] == g_ao->map[F_EX_MAX]) ? L", map has it locked" : L"";
        if (x < 0.0f) swprintf_s(o, n, L"map value (%s%s)", mv, fi <= F_EX_MAX ? lock : L"");
        else swprintf_s(o, n, L"%.2f  (map %s) - live", x, mv);
        return;
    }
    if (!wcscmp(it->key, L"RealtimeBakedFill")) {
        if (x <= 0.0f) wcscpy_s(o, n, L"OFF - pitch black (REAL-TIME only, live)");
        else swprintf_s(o, n, L"%.0f%% of baked light (REAL-TIME only, live)", x * 100.0f);
        return;
    }
    if (!wcscmp(it->key, L"ContactShadowLength") || !wcscmp(it->key, L"LampContactShadowLength")) {
        if (x < 0.0f) wcscpy_s(o, n, L"as authored (mostly off)");
        else if (x == 0.0f) wcscpy_s(o, n, L"OFF (next load)");
        else swprintf_s(o, n, L"%.2f of screen (next load)", x);
        return;
    }
    if (it->kind == MK_BOOL) { wcscpy_s(o, n, x > 0.5f ? L"ON (next level load)" : L"OFF (baked, next load)"); return; }
    if (!wcscmp(it->key, L"ForceRealtimeShadows")) {
        int m = (int)(x + 0.5f);
        wcscpy_s(o, n, m >= 2 ? L"ALL lights (next load)" : m == 1 ? L"converted lights (next load)" : L"OFF - as authored (next load)");
        return;
    }
    if (!wcscmp(it->key, L"FOV")) {
        float a = FovAspect(), g = g_fov ? g_fov->lastOrig : 0.0f, s = g_fov ? g_fov->scale : 0.0f;
        float h = (s > 0.0f) ? g * s : g;                  // horizontal FOV actually rendered
        if (x < 0.0f) swprintf_s(o, n, L"game default (H %.0f / V %.0f)", g, FovVfromH(g, a));
        else if (g_fovVertical) swprintf_s(o, n, L"%.0f vertical  (= H %.0f; game H %.0f)", x, h, g);
        else swprintf_s(o, n, L"%.0f horizontal  (= V %.0f; game H %.0f)", x, FovVfromH(h, a), g);
        return;
    }
    if (!wcscmp(it->sec, L"Shadows") && !wcsncmp(it->key, L"Sun", 3) && wcscmp(it->key, L"SunCascades")) {
        if (!wcscmp(it->key, L"SunShadowDistance")) {
            double o0 = CvarOrig(L"r.Shadow.DistanceScale");
            if (x < 0.0f) swprintf_s(o, n, L"game default (%g)", o0); else swprintf_s(o, n, L"%.2f  (game %g) - live", x, o0);
        } else {
            const wchar_t *mapv = !wcscmp(it->key, L"SunResolutionScale") ? L"0.7" : !wcscmp(it->key, L"SunCascadeDistribution") ? L"3" : L"0";
            if (x < 0.0f) swprintf_s(o, n, L"map value (%s)", mapv); else swprintf_s(o, n, L"%.2f  (map %s) - next load", x, mapv);
        }
        return;
    }
    if (!wcscmp(it->sec, L"Shadows") && wcsstr(it->key, L"Lamp")) {
        const wchar_t *cv = !wcscmp(it->key, L"LampMinResolution") ? L"r.Shadow.MinResolution" :
                            !wcscmp(it->key, L"LampFadeResolution") ? L"r.Shadow.FadeResolution" : L"r.Shadow.TexelsPerPixelPointlight";
        double o0 = CvarOrig(cv);
        if (x < 0.0f) swprintf_s(o, n, L"engine default (%g)", o0);
        else if (!wcscmp(it->key, L"LampTexelDensity")) swprintf_s(o, n, L"x%.2f  (point %.2f, default %.2f)", x, o0 * x, o0);
        else swprintf_s(o, n, L"%.0f  (default %g)", x, o0);
        return;
    }
    if (it->allowMap && x < 0.0f) { wcscpy_s(o, n, L"map value"); return; }
    if (it->kind == MK_INT) swprintf_s(o, n, L"%d", (int)x);
    else if (!wcscmp(it->sec, L"Lighting") && wcsstr(it->key, L"Converted")) swprintf_s(o, n, L"x%.2f", x);
    else swprintf_s(o, n, L"%.2f", x);
}
static void MenuAdjust(int dir, int big)
{
    const MItem *it = &kMenu[g_menuSel];
    if (it->kind == MK_HEADER) return;
    if (it->kind == MK_DISPLAY) { RequestDisplayMode((g_dispMode + (dir > 0 ? 1 : 2)) % 3); return; }
    if (it->kind == MK_PRESET) {   // Right = higher quality; from Custom, Right = Ultra and Left = Low
        int p = CurrentPreset();
        if (p < 0) p = dir > 0 ? N_PRESETS - 1 : 0;
        else { p += dir > 0 ? 1 : -1; if (p < 0) p = 0; if (p >= N_PRESETS) p = N_PRESETS - 1; }
        ApplyPreset(p); return;
    }
    if (it->kind == MK_MODE) { ToggleLightingMode(); return; }
    if (it->kind == MK_AO) { ToggleAO(); return; }
    if (!wcscmp(it->key, L"FOVMode")) {
        // switch H <-> V and convert the stored FOV so the view does not change (H 90 <-> V 59 at 16:9)
        wchar_t v[32]; float f; GetPrivateProfileStringW(L"Camera", L"FOV", L"-1", v, 32, g_cfgPath); f = (float)wcstod(v, NULL);
        float a = FovAspect();
        if (f > 1.0f) {
            f = g_fovVertical ? FovHfromV(f, a) : FovVfromH(f, a);
            swprintf_s(v, 32, L"%.0f", f); WritePrivateProfileStringW(L"Camera", L"FOV", v, g_cfgPath);
        }
        WritePrivateProfileStringW(L"Camera", L"FOVMode", g_fovVertical ? L"Horizontal" : L"Vertical", g_cfgPath);
        ReadAoConfig(); return;
    }
    if (it->kind == MK_BOOL) {
        WritePrivateProfileStringW(it->sec, it->key, MenuGet(it) > 0.5f ? L"0" : L"1", g_cfgPath);
        ReadAoConfig(); return;
    }
    float x = MenuGet(it), st = it->step * (big ? 5.0f : 1.0f);
    if (!wcscmp(it->key, L"ShadowMapResolution") || !wcscmp(it->key, L"Anisotropy")) {   // powers of two
        if (x < 0.0f) x = dir > 0 ? it->mn : -1.0f;
        else if (dir > 0) x = x * 2.0f > it->mx ? it->mx : x * 2.0f;
        else x = x / 2.0f < it->mn - 0.0001f ? -1.0f : x / 2.0f;
        wchar_t v[32]; swprintf_s(v, 32, L"%d", x < 0.0f ? -1 : (int)(x + 0.5f));
        WritePrivateProfileStringW(it->sec, it->key, v, g_cfgPath);
        ReadAoConfig(); return;
    }
    if (!wcscmp(it->key, L"FrameRateLimit")) {   // game setting (-1) <-> unlimited (0) <-> 30 ... 360
        if (x < 0.0f) x = dir > 0 ? 0.0f : -1.0f;
        else if (x < 1.0f) x = dir > 0 ? it->mn : -1.0f;
        else { x += dir > 0 ? st : -st; if (x < it->mn) x = 0.0f; if (x > it->mx) x = it->mx; }
        wchar_t v[32]; swprintf_s(v, 32, L"%d", x < 0.0f ? -1 : (int)(x + 0.5f));
        WritePrivateProfileStringW(it->sec, it->key, v, g_cfgPath);
        ReadAoConfig(); return;
    }
    float mapv = -1.0f;   // exposure: leaving "map value" starts from the map's own number, not the minimum
    if (g_ao && !wcscmp(it->sec, L"Exposure"))
        mapv = g_ao->map[!wcscmp(it->key, L"MinBrightness") ? F_EX_MIN : !wcscmp(it->key, L"MaxBrightness") ? F_EX_MAX :
                         !wcscmp(it->key, L"SpeedUp") ? F_EX_MAX + 1 : F_EX_MAX + 2];
    if (it->allowMap && x < 0.0f && mapv >= 0.0f) {
        x = mapv + (dir > 0 ? st : -st);
        if (x < it->mn) x = it->mn;
    }
    else if (dir > 0) x = (it->allowMap && x < 0.0f) ? it->mn : x + st;
    else if (x >= 0.0f || !it->allowMap) { x -= st; if (x < it->mn - 0.0001f) x = it->allowMap ? -1.0f : it->mn; }
    if (x > it->mx) x = it->mx;
    wchar_t v[32];
    if (it->kind == MK_INT) swprintf_s(v, 32, L"%d", (int)(x < 0 ? -1 : x + 0.5f)); else swprintf_s(v, 32, L"%g", x);
    WritePrivateProfileStringW(it->sec, it->key, v, g_cfgPath);
    ReadAoConfig();   // apply now (AO/SSR/lightmap fields, cascades, converted-light scales)
}

static WNDPROC g_origGameProc;
static LRESULT CALLBACK GameWndHook(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_HNT_APPLYDISPLAY) { int s = SavedDisplayMode(); if (s >= 0) ApplyDisplayMode(s); return 0; }   // Game = leave it
    // Exclusive-fullscreen confirmation box: Enter (or repeating the same action: Right on Display mode / F11) confirms,
    // Esc cancels. These keys never reach the game while the box is open.
    if (g_confirmExclusive && (m == WM_KEYDOWN || m == WM_SYSKEYDOWN)) {
        int again = (w == VK_RETURN) || (HNT_DEBUG_HOTKEYS && w == VK_F11) ||
                    (g_menuOpen && w == VK_RIGHT && kMenu[g_menuSel].kind == MK_DISPLAY);
        if (!(l & (1 << 30))) {
            if (again) { g_confirmExclusive = 0; SetDisplayMode(2); }
            else if (w == VK_ESCAPE || w == g_vkMenu || w == VK_LEFT || w == VK_UP || w == VK_DOWN) g_confirmExclusive = 0;
        }
        if (again || w == VK_ESCAPE || w == g_vkMenu || w == VK_LEFT || w == VK_RIGHT || w == VK_UP || w == VK_DOWN) return 0;
    }
    if ((m == WM_KEYUP || m == WM_SYSKEYUP) && (w == VK_RETURN || w == VK_ESCAPE) && g_menuOpen) return 0;
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) {
        if (w == g_vkMenu) {
            if (!(l & (1 << 30))) {
                g_menuOpen = !g_menuOpen;
                if (!MenuVisible(g_menuSel)) g_menuSel = MenuParent(g_menuSel);   // its category was closed
                if (g_menuSel < 0) g_menuSel = 0;
            }
            return 0;
        }
#if HNT_DEBUG_HOTKEYS
        if (w == VK_F11) { if (!(l & (1 << 30))) RequestDisplayMode((g_dispMode + 1) % 3); return 0; }
#endif
        if (g_menuOpen && (w == VK_UP || w == VK_DOWN || w == VK_LEFT || w == VK_RIGHT || w == VK_RETURN)) {
            int big = GetKeyState(VK_SHIFT) < 0;
            if (w == VK_UP)   MenuMove(-1);
            if (w == VK_DOWN) MenuMove(+1);
            if (kMenu[g_menuSel].kind == MK_HEADER) {
                int open = MenuIsOpen(g_menuSel);
                if (w == VK_RIGHT && open) MenuMove(+1);                                   // step into the category
                else if (w == VK_RIGHT || (w == VK_RETURN && !open)) MenuSetOpen(g_menuSel, 1);
                else if (w == VK_LEFT || (w == VK_RETURN && open)) MenuSetOpen(g_menuSel, 0);
            } else {
                if (w == VK_LEFT) MenuAdjust(-1, big);
                if (w == VK_RIGHT) MenuAdjust(+1, big);
            }
            return 0;   // swallow: the game never sees these keys while the menu is open
        }
    }
    if ((m == WM_KEYUP || m == WM_SYSKEYUP) && (w == g_vkMenu || (HNT_DEBUG_HOTKEYS && w == VK_F11))) return 0;
    return CallWindowProcW(g_origGameProc, h, m, w, l);
}

static void BuildConfirmText(wchar_t *t, size_t n)
{
    swprintf_s(t, n,
        L"!!  EXCLUSIVE FULLSCREEN  !!\n\n"
        L"In exclusive fullscreen this menu and the overlay\n"
        L"cannot be shown.\n\n"
        L"To get out of it again:\n"
#if HNT_DEBUG_HOTKEYS
        L"    F11        -> back to Windowed\n"
#endif
        L"    Alt+Enter  -> leaves fullscreen\n"
        L"    or set Mode=Borderless in HelloNeighborTweaks.ini\n\n"
        L"[ Press ENTER again to confirm ]     [ Esc = cancel ]");
}

static void BuildMenuText(wchar_t *t, size_t n)
{
    size_t len = 0;
    len += swprintf_s(t + len, n - len, L"TWEAKS  -  Up/Down select, Right/Enter open, Left/Enter close,\n"
                                        L"           Left/Right change a value (Shift = x5), F1 close\n\n");
    int rows[MENU_N], nr = 0, selRow = 0;
    for (int i = 0; i < MENU_N; i++) if (MenuVisible(i)) { if (i == g_menuSel) selRow = nr; rows[nr++] = i; }
    // scroll: keep the selection inside the rows that fit on screen (3 title lines, 2 "more" lines)
    static int top;
    int fit = (int)g_menuMaxLines - 3;
    if (fit < 6) fit = 6;
    int span = nr <= fit ? nr : fit - 2;
    if (nr <= fit) top = 0;
    else {
        if (selRow < top) top = selRow;
        if (selRow >= top + span) top = selRow - span + 1;
        if (top > nr - span) top = nr - span;
        if (top < 0) top = 0;
    }
    if (top > 0) len += swprintf_s(t + len, n - len, L"     ... %d more above\n", top);
    for (int r = top; r < top + span && r < nr; r++) {
        int i = rows[r]; const wchar_t *sel = i == g_menuSel ? L"\x00BB" : L" ";
        if (kMenu[i].kind == MK_HEADER) {
            int cnt = 0; for (int k = i + 1; k < MENU_N && kMenu[k].kind != MK_HEADER; k++) cnt++;
            if (MenuIsOpen(i)) len += swprintf_s(t + len, n - len, L"%s v %s\n", sel, kMenu[i].label);
            else len += swprintf_s(t + len, n - len, L"%s > %s  (%d)\n", sel, kMenu[i].label, cnt);
            continue;
        }
        wchar_t val[64]; MenuValueText(&kMenu[i], val, 64);
        len += swprintf_s(t + len, n - len, L"%s     %-28s %s\n", sel, kMenu[i].label, val);
    }
    if (top + span < nr) len += swprintf_s(t + len, n - len, L"     ... %d more below\n", nr - top - span);
}

// ---------------------------------------------------------------- overlay
// A small layered, click-through, never-activated topmost window over the game's top-left corner.
// Visible only while the game window is in the foreground (borderless fullscreen / windowed).
static HWND g_gameWnd, g_ovWnd;
static wchar_t g_ovText[4096];
static HFONT g_ovFont, g_menuFont;

static BOOL CALLBACK FindGameWnd(HWND h, LPARAM lp)
{
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    wchar_t cls[64];
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && GetClassNameW(h, cls, 64) && !wcscmp(cls, L"UnrealWindow")) { *(HWND *)lp = h; return FALSE; }
    return TRUE;
}

static void BuildOverlayText(wchar_t *t, size_t n)
{
#if !HNT_DEBUG_HOTKEYS
    swprintf_s(t, n, L"Hello Neighbor Tweaks " HNT_VERSION_W L"   -   press F1 for settings");
#else
    int ready = 0; int conv = ConvCount(&ready);
    wchar_t fk[8], ck[8], live[160];
    swprintf_s(fk, 8, g_vkFog >= VK_F1 && g_vkFog <= VK_F24 ? L"F%u" : L"0x%02X", g_vkFog >= VK_F1 && g_vkFog <= VK_F24 ? g_vkFog - VK_F1 + 1 : g_vkFog);
    swprintf_s(ck, 8, g_vkContact >= VK_F1 && g_vkContact <= VK_F24 ? L"F%u" : L"0x%02X", g_vkContact >= VK_F1 && g_vkContact <= VK_F24 ? g_vkContact - VK_F1 + 1 : g_vkContact);
    swprintf_s(live, 160, L"VOLUMETRIC FOG: %s   [%s]\nCONTACT SHADOWS: %s   [%s]",
               (g_fogHotOff || g_volFog == 0) ? L"OFF" : L"ON", fk, (g_contactHotOff || (g_contactLen == 0.0f && g_lampContactLen <= 0.0f)) ? L"OFF" : L"ON", ck);
    const wchar_t *hint = g_showOverlay ? L"Press F1 for settings   (F10 = hide this, F5 = compare to original)"
                                        : L"Press F1 for settings   (F10 = keep this shown, F5 = compare to original)";
    if (g_vanilla) {
        wchar_t ck5[8];
        swprintf_s(ck5, 8, g_vkCompare >= VK_F1 && g_vkCompare <= VK_F24 ? L"F%u" : L"0x%02X", g_vkCompare >= VK_F1 && g_vkCompare <= VK_F24 ? g_vkCompare - VK_F1 + 1 : g_vkCompare);
        swprintf_s(t, n, L"COMPARE: ORIGINAL GAME   [%s = back to the mod]\n"
                         L"Off now: post-process overrides, exposure, baked fill, real-time static lights,\n"
                         L"sun cascades, SSR unlock, volumetric fog, contact shadows, lamp shadow settings,\n"
                         L"IMAGE / VIEW DISTANCE / shadow map / AO + reflection quality / texture options.\n"
                         L"Still modded until a level loads without the mod: stationary lights' real-time shadows,\n"
                         L"sun shadow distribution/resolution/sharpen. Unchanged: FOV, V-Sync, fps limit,\n"
                         L"anisotropic filtering and mouse smoothing (restart settings).", ck5);
        return;
    }
    wchar_t rt[80];
    if (g_rtFill > 0.0f) swprintf_s(rt, 80, L"REAL-TIME  (static lights live, baked fill %.0f%%)", g_rtFill * 100.0f);
    else wcscpy_s(rt, 80, L"REAL-TIME  (static lights live, lightmaps off)");
    if (g_rtl && g_rtl->enabled)
        swprintf_s(t, n, L"%s\nLIGHTING: %s   [F8]\nAO: %s   [F9]\n%s\n%d static light(s) converted%s", hint,
                   g_lmOff ? rt : L"BAKED  (lightmaps on, static lights baked)",
                   g_aoOff ? L"OFF" : L"ON", live, ready, conv != ready ? L" (pending)" : L"");
    else
        swprintf_s(t, n, L"%s\nLIGHTMAPS: %s   [F8]\nAO: %s   [F9]\n%s", hint, g_lmOff ? L"OFF" : L"ON", g_aoOff ? L"OFF" : L"ON", live);
#endif
}

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        HBRUSH bg = CreateSolidBrush(RGB(16, 16, 16)); FillRect(dc, &r, bg); DeleteObject(bg);
        HGDIOBJ of = SelectObject(dc, (g_menuOpen || g_confirmExclusive) ? g_menuFont : g_ovFont);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_confirmExclusive ? RGB(255, 150, 70) : g_menuOpen ? RGB(235, 235, 235) : (g_lmOff ? RGB(255, 210, 90) : RGB(140, 200, 255)));
        InflateRect(&r, -14, -10);
        DrawTextW(dc, g_ovText, -1, &r, DT_LEFT | DT_TOP | DT_NOPREFIX);
        SelectObject(dc, of);
        EndPaint(h, &ps); return 0;
    }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI OverlayThread(LPVOID arg)
{
    (void)arg;
    WNDCLASSW wc = {0}; wc.lpfnWndProc = OverlayProc; wc.hInstance = g_self; wc.lpszClassName = L"HNTweaksOverlay";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    g_ovFont = CreateFontW(-30, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_menuFont = CreateFontW(-28, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    g_ovWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              wc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, g_self, NULL);
    if (!g_ovWnd) return 0;
    SetLayeredWindowAttributes(g_ovWnd, 0, 215, LWA_ALPHA);
    SetTimer(g_ovWnd, 1, 100, NULL);
    int shown = 0; static wchar_t last[4096] = L""; RECT lastR = {0};
    DWORD hookedAt = 0; int displayApplied = 0;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_TIMER) {
            if (!g_gameWnd || !IsWindow(g_gameWnd)) {
                g_gameWnd = NULL; g_origGameProc = NULL;
                EnumWindows(FindGameWnd, (LPARAM)&g_gameWnd);
                if (g_gameWnd) { g_origGameProc = (WNDPROC)SetWindowLongPtrW(g_gameWnd, GWLP_WNDPROC, (LONG_PTR)GameWndHook); hookedAt = GetTickCount(); }
            }
            // re-apply the saved display mode once, ~3 s after the game window exists (runs on the game thread)
            if (g_gameWnd && !displayApplied && g_setResHolder && GetTickCount() - hookedAt > 3000) {
                displayApplied = 1; PostMessageW(g_gameWnd, WM_HNT_APPLYDISPLAY, 0, 0);
            }
            int fg = g_gameWnd && GetForegroundWindow() == g_gameWnd && !IsIconic(g_gameWnd);
            if (!fg) { g_menuOpen = 0; g_confirmExclusive = 0; }
            // exclusive fullscreen: a topmost window cannot show over it (and could knock the game out of it)
            // auto-hide: a flash request starts a 5 s window the next time the game is in front
            static DWORD flashUntil = 0;
            if (fg && g_ovFlash) { g_ovFlash = 0; flashUntil = GetTickCount() + OVERLAY_SHOW_MS; }
            if (g_menuOpen) flashUntil = 0;   // closing the menu does not bring the status back
            int flashing = flashUntil && (LONG)(flashUntil - GetTickCount()) > 0;
            if (!flashing) flashUntil = 0;
            int want = fg && (g_showOverlay || flashing || g_menuOpen || g_confirmExclusive) && g_dispMode != 2;
            if (want) {
                if (g_menuOpen) {   // how many menu rows fit on the game window (menu font line height)
                    static int lineH;
                    if (!lineH) {
                        HDC dc = GetDC(g_ovWnd); HGDIOBJ of = SelectObject(dc, g_menuFont); TEXTMETRICW tm;
                        if (GetTextMetricsW(dc, &tm)) lineH = tm.tmHeight + tm.tmExternalLeading;
                        SelectObject(dc, of); ReleaseDC(g_ovWnd, dc);
                    }
                    RECT gr0; GetClientRect(g_gameWnd, &gr0);
                    if (lineH > 0) g_menuMaxLines = (gr0.bottom - gr0.top - 24 - 20 - 24) / lineH;
                }
                if (g_confirmExclusive) BuildConfirmText(g_ovText, 4096);
                else if (g_menuOpen) BuildMenuText(g_ovText, 4096); else BuildOverlayText(g_ovText, 4096);
                HDC dc = GetDC(g_ovWnd); HGDIOBJ of = SelectObject(dc, (g_menuOpen || g_confirmExclusive) ? g_menuFont : g_ovFont);
                RECT tr = {0, 0, 0, 0}; DrawTextW(dc, g_ovText, -1, &tr, DT_CALCRECT | DT_NOPREFIX);
                SelectObject(dc, of); ReleaseDC(g_ovWnd, dc);
                RECT gr; GetWindowRect(g_gameWnd, &gr);
                RECT r = { gr.left + 24, gr.top + 24, gr.left + 24 + tr.right + 28, gr.top + 24 + tr.bottom + 20 };
                if (memcmp(&r, &lastR, sizeof r) || !shown)
                    SetWindowPos(g_ovWnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                if (wcscmp(last, g_ovText) || memcmp(&r, &lastR, sizeof r)) { InvalidateRect(g_ovWnd, NULL, TRUE); wcscpy_s(last, 4096, g_ovText); }
                lastR = r; shown = 1;
            } else if (shown) { ShowWindow(g_ovWnd, SW_HIDE); shown = 0; }
        }
        DispatchMessageW(&msg);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        InitImage((unsigned char *)GetModuleHandleW(NULL));
        PatchSSR();
        {
            DWORD n = GetModuleFileNameW(inst, g_cfgPath, MAX_PATH);
            while (n && g_cfgPath[n - 1] != L'\\') n--;
            wcscpy_s(g_cfgPath + n, MAX_PATH - n, L"HelloNeighborTweaks.ini");
        }
        // v33: create the settings file now, before the creation-time settings below are read
        { wchar_t exe0[MAX_PATH]; GetModuleFileNameW(NULL, exe0, MAX_PATH); if (wcsstr(exe0, L"HelloNeighbor-Win64-Shipping")) WriteDefaultIni(); }
        InstallAoHook();
        InstallCsmHook();
        InstallAoPassHook();
        InstallFovHook();
        InstallFogHook();
        InstallRtlHook();
        {
            wchar_t v[64]; float x;
            GetPrivateProfileStringW(L"Lighting", L"ConvertedBrightnessScale", L"1", v, 64, g_cfgPath); x = (float)wcstod(v, NULL); if (x > 0.0f) g_scaleI = x;
            GetPrivateProfileStringW(L"Lighting", L"ConvertedRadiusScale", L"1", v, 64, g_cfgPath); x = (float)wcstod(v, NULL); if (x > 0.0f) g_scaleR = x;
        }
        g_stationaryRT = GetPrivateProfileIntW(L"Lighting", L"RealtimeStationaryShadows", 1, g_cfgPath) > 0;
        g_forceShadows = GetPrivateProfileIntW(L"Lighting", L"ForceRealtimeShadows", 2, g_cfgPath);
        {
            wchar_t v[64];
            GetPrivateProfileStringW(L"Shadows", L"SunResolutionScale", L"-1", v, 64, g_cfgPath);     g_sunRes = (float)wcstod(v, NULL);
            GetPrivateProfileStringW(L"Shadows", L"SunCascadeDistribution", L"2", v, 64, g_cfgPath);  g_sunDist = (float)wcstod(v, NULL);
            GetPrivateProfileStringW(L"Shadows", L"SunShadowSharpen", L"0.3", v, 64, g_cfgPath);      g_sunSharp = (float)wcstod(v, NULL);
            GetPrivateProfileStringW(L"Shadows", L"ContactShadowLength", L"0.05", v, 64, g_cfgPath);  g_contactLen = (float)wcstod(v, NULL);
            GetPrivateProfileStringW(L"Shadows", L"LampContactShadowLength", L"-1", v, 64, g_cfgPath); g_lampContactLen = (float)wcstod(v, NULL);
            g_volFog = GetPrivateProfileIntW(L"Fog", L"VolumetricFog", 1, g_cfgPath);
            GetPrivateProfileStringW(L"Fog", L"DensityScale", L"2", v, 64, g_cfgPath);        g_fogDensityScale = (float)wcstod(v, NULL);
            GetPrivateProfileStringW(L"Fog", L"VolumetricDistance", L"-1", v, 64, g_cfgPath); g_fogVolDist = (float)wcstod(v, NULL);
        }
        if (g_rtl && GetPrivateProfileIntW(L"Lighting", L"RealtimeStaticLights", 1, g_cfgPath) > 0) {
            g_rtl->enabled = 1;
            g_lmOff = 1;   // baked copies of these lights would double up with the live ones
        }
        // Only monitor inside the game itself.
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
        if (wcsstr(exe, L"HelloNeighbor-Win64-Shipping")) {
            HANDLE th = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
            if (th) CloseHandle(th);
#if HNT_DEBUG_HOTKEYS
            th = CreateThread(NULL, 0, HotkeyThread, NULL, 0, NULL);
            if (th) CloseHandle(th);
#endif
            g_showOverlay = GetPrivateProfileIntW(L"Overlay", L"Show", 0, g_cfgPath) > 0;   // 1 = always on; 0 = 5 s then hide
            th = CreateThread(NULL, 0, OverlayThread, NULL, 0, NULL);
            if (th) CloseHandle(th);
        } else {
            char msg[400];
            sprintf_s(msg, sizeof msg, "Not running inside HelloNeighbor-Win64-Shipping.exe - monitor off.\r\nReflections: %s\r\n", g_patchMsg);
            WriteLog(msg);
        }
    }
    return TRUE;
}
