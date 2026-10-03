/*
Copyright (c) FufuLauncher Dev Team. All rights reserved.
Licensed under the AGPL-3.0 License.
*/
#include "CameraTweaks.h"

#include "../Config/Config.h"
#include "../MinHook/MinHook.h"
#include "../Patterns/Patterns.h"
#include "../Scanner/Scanner.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace CameraTweaks {
    namespace {
        using UpdateView = void (__fastcall*)(void*);
        using BlenderTick = void (__fastcall*)(void*, float);
        UpdateView g_OriginalUpdateView = nullptr;
        BlenderTick g_OriginalBlenderTick = nullptr;

        void __fastcall HookUpdateView(void* self) {
            if (self && Config::Get().disable_camera_smooth) {
                __try {
                    auto* state = static_cast<float*>(self);
                    // Match the legacy direct-input result, while leaving
                    // delta time, sensitivity and pitch limits to the game.
                    state[30] = state[28]; // +0x78 = +0x70
                    state[31] = state[29]; // +0x7C = +0x74
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            if (g_OriginalUpdateView) g_OriginalUpdateView(self);
        }

        void __fastcall HookBlenderTick(void* self, float deltaTime) {
            if (self && deltaTime >= 0.0f && Config::Get().disable_camera_blend) {
                __try {
                    auto* state = static_cast<float*>(self);
                    // 7.1 moved duration/elapsed to +0x60/+0x64. Finish the
                    // current blend without overwriting its configured duration.
                    const float duration = state[24];
                    if (std::isfinite(duration) && duration > 0.0f) {
                        state[25] = duration;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            if (g_OriginalBlenderTick) g_OriginalBlenderTick(self, deltaTime);
        }
    }

    void Init() {
        const auto& cfg = Config::Get();
        // Like the legacy feature, install only the hooks enabled at startup.
        // No per-frame search, camera enumeration or extra input thread.
        if (cfg.disable_camera_smooth) {
            auto* code = static_cast<uint8_t*>(Scanner::ScanMainMod(Patterns::CameraUpdateView));
            // The signature also matches an inlined Update method. Verify
            // the input-only UpdateView tail before installing the old hook.
            const uint8_t tail[] = {
                0xF3, 0x0F, 0x11, 0x4E, 0x28, 0x0F, 0x28, 0x74, 0x24, 0x20,
                0x0F, 0x28, 0x7C, 0x24, 0x30, 0x48, 0x83, 0xC4, 0x40, 0x5E, 0xC3
            };
            if (code && !IsBadReadPtr(code, 0xC6 + sizeof(tail)) &&
                memcmp(code + 0xC6, tail, sizeof(tail)) == 0 &&
                MH_CreateHook(code, reinterpret_cast<void*>(HookUpdateView),
                    reinterpret_cast<void**>(&g_OriginalUpdateView)) == MH_OK) {
                std::cout << "[SCAN] Camera UpdateView Hook Ready.\n";
            } else {
                std::cout << "[WARN] Camera UpdateView unavailable or layout unsupported; input smoothing unchanged.\n";
            }
        }

        if (cfg.disable_camera_blend) {
            auto* code = static_cast<uint8_t*>(Scanner::ScanMainMod(Patterns::CameraStateBlenderTick));
            // Validate both timer fields rather than reusing the old offsets.
            const uint8_t timers[] = {
                0xF3, 0x0F, 0x10, 0x46, 0x60, 0xF3, 0x0F, 0x10, 0x76, 0x64,
                0x0F, 0x28, 0xCE, 0xF3, 0x41, 0x0F, 0x58, 0xC8,
                0xF3, 0x0F, 0x11, 0x4E, 0x64
            };
            if (code && !IsBadReadPtr(code, 0x4F + sizeof(timers)) &&
                memcmp(code + 0x4F, timers, sizeof(timers)) == 0 &&
                MH_CreateHook(code, reinterpret_cast<void*>(HookBlenderTick),
                    reinterpret_cast<void**>(&g_OriginalBlenderTick)) == MH_OK) {
                std::cout << "[SCAN] CameraStateBlender Tick Hook Ready.\n";
            } else {
                std::cout << "[WARN] CameraStateBlender unavailable or layout unsupported; state blending unchanged.\n";
            }
        }
    }
}
