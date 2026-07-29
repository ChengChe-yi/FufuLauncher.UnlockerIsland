#include "FreeCamera.h"

#include "../Core/SharedState.h"
#include "../Config/Config.h"
#include "../Patterns/Patterns.h"
#include "../Scanner/Scanner.h"

#include <cmath>
#include <iostream>
#include <Windows.h>

namespace FreeCamera {
    namespace {
        struct Quaternion { float x, y, z, w; };

        Quaternion QuatMul(const Quaternion& a, const Quaternion& b) {
            return {
                a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
            };
        }

        Vector3 QuatRotateVec(const Quaternion& q, const Vector3& v) {
            Quaternion qv{ v.x, v.y, v.z, 0.0f };
            Quaternion qConj{ -q.x, -q.y, -q.z, q.w };
            Quaternion r = QuatMul(QuatMul(q, qv), qConj);
            return { r.x, r.y, r.z };
        }

        Quaternion QuatFromYawPitch(float yawDeg, float pitchDeg) {
            float yaw = yawDeg * 0.0174532925f * 0.5f;
            float pitch = pitchDeg * 0.0174532925f * 0.5f;
            Quaternion qYaw{ 0, sinf(yaw), 0, cosf(yaw) };
            Quaternion qPitch{ sinf(pitch), 0, 0, cosf(pitch) };
            return QuatMul(qYaw, qPitch);
        }

        typedef void* (__fastcall *FnGetMain)();
        typedef void* (__fastcall *FnGetTransform)(void*);
        typedef void  (__fastcall *FnSetPosition)(void*, Vector3*);
        typedef void  (__fastcall *FnSetRotation)(void*, Quaternion*);
        typedef void  (__fastcall *FnGetPosition)(Vector3*, void*);

        FnGetMain      g_fnGetMain = nullptr;
        FnGetTransform g_fnGetTransform = nullptr;
        FnSetPosition  g_fnSetPosition = nullptr;
        FnSetRotation  g_fnSetRotation = nullptr;
        FnGetPosition  g_fnGetPosition = nullptr;

        void* g_CamTransform = nullptr;
        volatile bool g_Ready = false;

        volatile bool g_Active = false;
        volatile bool g_Locked = false;
        volatile float g_Yaw = 0.0f, g_Pitch = 0.0f;
        Vector3 g_FreeCamPos = { 0, 0, 0 };
        Vector3 g_LastRealPos = { 0, 0, 0 };

        volatile LONG g_MouseDX = 0;
        volatile LONG g_MouseDY = 0;
        HWND g_GameWindow = nullptr;
        WNDPROC g_OldWndProc = nullptr;
        HHOOK g_KbHook = nullptr;

        static void* SEH_GetMain(FnGetMain fn) {
            if (!fn) return nullptr;
            __try { return fn(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
        }

        static void* SEH_GetTransform(FnGetTransform fn, void* cam) {
            if (!fn || !cam) return nullptr;
            __try { return fn(cam); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
        }

        static bool SEH_GetPosition(FnGetPosition fn, Vector3* outPos, void* transform) {
            if (!fn || !outPos || !transform) return false;
            __try { fn(outPos, transform); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        static bool SEH_SetPosition(FnSetPosition fn, void* transform, Vector3* pos) {
            if (!fn || !transform || !pos) return false;
            __try { fn(transform, pos); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        static bool SEH_SetRotation(FnSetRotation fn, void* transform, Quaternion* rot) {
            if (!fn || !transform || !rot) return false;
            __try { fn(transform, rot); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        bool IsFlightKey(DWORD) {
            return g_Active && !g_Locked;
        }

        LRESULT CALLBACK KbProc(int nCode, WPARAM wParam, LPARAM lParam) {
            if (nCode >= 0 && g_Active && !g_Locked) {
                return 1;
            }
            return CallNextHookEx(g_KbHook, nCode, wParam, lParam);
        }

        LRESULT CALLBACK WndProcHook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
            if (g_Active && !g_Locked) {
                if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) {
                    if (IsFlightKey((DWORD)wParam)) return 0;
                }
                if (msg == WM_INPUT) {
                    UINT size = 0;
                    GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
                    if (size > 0 && size <= 64) {
                        BYTE buf[64];
                        if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == size) {
                            RAWINPUT* raw = (RAWINPUT*)buf;
                            if (raw->header.dwType == RIM_TYPEMOUSE && !(raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                                InterlockedExchangeAdd(&g_MouseDX, raw->data.mouse.lLastX);
                                InterlockedExchangeAdd(&g_MouseDY, raw->data.mouse.lLastY);
                                return 0;
                            }
                            if (raw->header.dwType == RIM_TYPEKEYBOARD) {
                                if (IsFlightKey(raw->data.keyboard.VKey)) return 0;
                            }
                        }
                    }
                }
            }
            return g_OldWndProc ? CallWindowProcW(g_OldWndProc, hwnd, msg, wParam, lParam)
                                 : DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        void InitRawMouseInput(HWND hwnd) {
            g_GameWindow = hwnd;
            g_OldWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)WndProcHook);

            RAWINPUTDEVICE rid[2] = {};
            rid[0].usUsagePage = 0x01;
            rid[0].usUsage     = 0x02;
            rid[0].dwFlags     = RIDEV_INPUTSINK;
            rid[0].hwndTarget  = hwnd;
            rid[1].usUsagePage = 0x01;
            rid[1].usUsage     = 0x06;
            rid[1].dwFlags     = RIDEV_INPUTSINK;
            rid[1].hwndTarget  = hwnd;
            RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
        }

        void ApplyNow() {
            if (!g_Active || !g_CamTransform || !g_fnSetPosition || !g_fnSetRotation) return;
            Vector3 p = g_FreeCamPos;
            Quaternion q = QuatFromYawPitch(g_Yaw, g_Pitch);
            
            SEH_SetPosition(g_fnSetPosition, g_CamTransform, &p);
            SEH_SetRotation(g_fnSetRotation, g_CamTransform, &q);
        }

        void ToggleActive() {
            g_Active = !g_Active;
            if (g_Active) {
                g_Locked = false;
                Vector3 realPos = g_LastRealPos;
                if (g_fnGetPosition && g_CamTransform) {
                    Vector3 tmp;
                    if (SEH_GetPosition(g_fnGetPosition, &tmp, g_CamTransform)) {
                        realPos = tmp;
                        g_LastRealPos = tmp;
                    }
                }
                g_FreeCamPos = realPos;
                InterlockedExchange(&g_MouseDX, 0);
                InterlockedExchange(&g_MouseDY, 0);
                ShowCursor(FALSE);
            } else {
                g_Locked = false;
                ShowCursor(TRUE);
            }
        }

        void ToggleLock() {
            g_Locked = !g_Locked;
            InterlockedExchange(&g_MouseDX, 0);
            InterlockedExchange(&g_MouseDY, 0);
            ShowCursor(g_Locked ? TRUE : FALSE);
        }

        DWORD WINAPI InputThread(LPVOID) {
            bool prevToggle = false;
            bool prevLock = false;
            LARGE_INTEGER freq, prevT, curT;
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&prevT);

            HWND hwnd = nullptr;
            while (!(hwnd = FindWindowA("UnityWndClass", nullptr))) Sleep(500);
            Sleep(15000);
            InitRawMouseInput(hwnd);

            while (true) {
                Sleep(10);
                auto& cfg = Config::Get();

                if (!cfg.enable_free_cam) {
                    if (g_Active) ToggleActive();
                    prevToggle = false;
                    prevLock = false;
                    QueryPerformanceCounter(&prevT);
                    continue;
                }

                bool toggle = (GetAsyncKeyState(cfg.free_cam_key) & 0x8000) != 0;
                if (toggle && !prevToggle) {
                    ToggleActive();
                }
                prevToggle = toggle;

                bool lockKey = (GetAsyncKeyState(cfg.free_cam_lock_key) & 0x8000) != 0;
                if (lockKey && !prevLock && g_Active) {
                    ToggleLock();
                }
                prevLock = lockKey;

                QueryPerformanceCounter(&curT);
                float dt = (float)(curT.QuadPart - prevT.QuadPart) / (float)freq.QuadPart;
                prevT = curT;
                if (dt > 0.1f) dt = 0.1f;

                if (!g_Active || g_Locked) continue;

                LONG dx = InterlockedExchange(&g_MouseDX, 0);
                LONG dy = InterlockedExchange(&g_MouseDY, 0);

                g_Yaw   += (float)dx * cfg.free_cam_mouse_sensitivity;
                g_Pitch += (float)dy * cfg.free_cam_mouse_sensitivity;
                if (g_Pitch > 89.0f) g_Pitch = 89.0f;
                if (g_Pitch < -89.0f) g_Pitch = -89.0f;

                Quaternion q = QuatFromYawPitch(g_Yaw, g_Pitch);
                Vector3 fwd   = QuatRotateVec(q, { 0, 0, 1 });
                Vector3 right = QuatRotateVec(q, { 1, 0, 0 });

                float speed = cfg.free_cam_move_speed;
                if (GetAsyncKeyState(VK_SHIFT) & 0x8000) speed *= cfg.free_cam_sprint_mult;
                float step = speed * dt;

                Vector3 p = g_FreeCamPos;
                if (GetAsyncKeyState('W') & 0x8000) { p.x += fwd.x * step; p.y += fwd.y * step; p.z += fwd.z * step; }
                if (GetAsyncKeyState('S') & 0x8000) { p.x -= fwd.x * step; p.y -= fwd.y * step; p.z -= fwd.z * step; }
                if (GetAsyncKeyState('D') & 0x8000) { p.x += right.x * step; p.y += right.y * step; p.z += right.z * step; }
                if (GetAsyncKeyState('A') & 0x8000) { p.x -= right.x * step; p.y -= right.y * step; p.z -= right.z * step; }
                if (GetAsyncKeyState(VK_SPACE) & 0x8000)   p.y += step;
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) p.y -= step;
                g_FreeCamPos = p;
            }
            return 0;
        }
    }

    void Init() {
        std::cout << "[SCAN] Initializing FreeCamera..." << std::endl;

        void* aMain = Scanner::ScanMainMod(Patterns::FreeCamCameraGetMain);
        void* aTf   = Scanner::ScanMainMod(Patterns::FreeCamComponentGetTransform);
        void* aGetP = Scanner::ScanMainMod(Patterns::FreeCamTransformGetPosition);
        void* aSetP = Scanner::ScanMainMod(Patterns::FreeCamTransformSetPosition);
        void* aSetR = Scanner::ScanMainMod(Patterns::FreeCamTransformSetRotation);

        if (!aMain || !aTf || !aGetP || !aSetP || !aSetR) {
            std::cout << "   -> [ERR] FreeCamera patterns not found, feature disabled." << std::endl;
            return;
        }

        g_fnGetMain      = reinterpret_cast<FnGetMain>(aMain);
        g_fnGetTransform = reinterpret_cast<FnGetTransform>(aTf);
        g_fnGetPosition  = reinterpret_cast<FnGetPosition>(aGetP);
        g_fnSetPosition  = reinterpret_cast<FnSetPosition>(aSetP);
        g_fnSetRotation  = reinterpret_cast<FnSetRotation>(aSetR);

        g_KbHook = SetWindowsHookExA(WH_KEYBOARD_LL, KbProc, GetModuleHandleA(nullptr), 0);

        CreateThread(nullptr, 0, InputThread, nullptr, 0, nullptr);

        g_Ready = true;
        std::cout << "   -> FreeCamera Ready." << std::endl;
    }

    void Tick() {
        if (!g_Ready) return;

        static ULONGLONG lastRefresh = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastRefresh > 2000) {
            lastRefresh = now;
            void* cam = SEH_GetMain(g_fnGetMain);
            if (cam) {
                void* t = SEH_GetTransform(g_fnGetTransform, cam);
                if (t) g_CamTransform = t;
            }
        }

        if (g_Active) ApplyNow();
    }
}