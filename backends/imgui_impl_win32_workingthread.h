// dear imgui: Platform Backend for Windows (standard windows API for 32 and 64 bits applications)
// This needs to be used along with a Renderer (e.g. DirectX11, OpenGL3, Vulkan..)

// Implemented features:
//  [X] Platform: Clipboard support (for Win32 this is actually part of core dear imgui)
//  [X] Platform: Mouse cursor shape and visibility. Disable with 'io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange'.
//  [X] Platform: Keyboard arrays indexed using VK_* Virtual Key Codes, e.g. ImGui::IsKeyPressed(VK_SPACE).
//  [X] Platform: Gamepad support. Enabled with 'io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad'.

// Experimental features:
//  [X] Platform: It's safer to using this implement on your working thread (different to Win32 "GUI thread").

// Warning:
//  1. This is a experimental implement and may have many bugs.
//  2. Lock free message exchange queue (g_Win32MSG) may be full and some Win32 messages will be missed.
//  3. Mouse capture (Win32 API SetCapture, GetCapture and ReleaseCapture) may not working because it is asynchronous (execution order is not guaranteed).
//  4. Input delay may be large.
//  5. Support ImGuiBackendFlags_HasSetMousePos, but the reason same as (3), it may not working.

// You can copy and use unmodified imgui_impl_* files in your project. See examples/ folder for examples of using this.
// If you are new to Dear ImGui, read documentation from the docs/ folder + read the top of imgui.cpp.
// Read online: https://github.com/ocornut/imgui/tree/master/docs

#pragma once
#include "imgui.h"      // IMGUI_IMPL_API

IMGUI_IMPL_API bool     ImGui_ImplWin32WorkingThread_Init(void* hwnd);
IMGUI_IMPL_API void     ImGui_ImplWin32WorkingThread_Shutdown();
IMGUI_IMPL_API void     ImGui_ImplWin32WorkingThread_NewFrame();

// Configuration
// - Disable gamepad support or linking with xinput.lib
//#define IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
//#define IMGUI_IMPL_WIN32_DISABLE_LINKING_XINPUT

// Win32 user define message
//  - If it conflicts with yours, define a new one to override it.
#ifndef WM_USER_IMGUI_IMPL_WIN32WORKINGTHREAD
#define WM_USER_IMGUI_IMPL_WIN32WORKINGTHREAD 0x0400 // WM_USER
#endif

// Win32 message handler your application need to call.
// - Intentionally commented out in a '#if 0' block to avoid dragging dependencies on <windows.h> from this helper.
// - You should COPY the line below into your .cpp code to forward declare the function and then you can call it.
#if 0
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32WorkingThread_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

// DPI-related helpers (optional)
// - Use to enable DPI awareness without having to create an application manifest.
// - Your own app may already do this via a manifest or explicit calls. This is mostly useful for our examples/ apps.
// - In theory we could call simple functions from Windows SDK such as SetProcessDPIAware(), SetProcessDpiAwareness(), etc.
//   but most of the functions provided by Microsoft require Windows 8.1/10+ SDK at compile time and Windows 8/10+ at runtime,
//   neither we want to require the user to have. So we dynamically select and load those functions to avoid dependencies.
IMGUI_IMPL_API void     ImGui_ImplWin32WorkingThread_EnableDpiAwareness();
IMGUI_IMPL_API float    ImGui_ImplWin32WorkingThread_GetDpiScaleForHwnd(void* hwnd);       // HWND hwnd
IMGUI_IMPL_API float    ImGui_ImplWin32WorkingThread_GetDpiScaleForMonitor(void* monitor); // HMONITOR monitor
