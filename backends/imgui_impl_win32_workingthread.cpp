// dear imgui: Platform Backend for Windows (standard windows API for 32 and 64 bits applications)
// This needs to be used along with a Renderer (e.g. DirectX11, OpenGL3, Vulkan..)

// Implemented features:
//  [X] Platform: Clipboard support (for Win32 this is actually part of core dear imgui)
//  [X] Platform: Mouse cursor shape and visibility. Disable with 'io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange'.
//  [X] Platform: Keyboard arrays indexed using VK_* Virtual Key Codes, e.g. ImGui::IsKeyPressed(VK_SPACE).
//  [X] Platform: Gamepad support. Enabled with 'io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad'.

// Experimental features:
//  [X] Platform: It's safety to using this implement on working thread (different to "Win32 GUI thread").

// Warning:
//  1. This is a experimental implement and may have many bugs.
//  2. Lock free message exchange queue (g_vWin32MSG) may be full and some Win32 messages will be missed.
//  3. Mouse capture (Win32 API SetCapture, GetCapture and ReleaseCapture) may not working because it is asynchronous (execution order is not guaranteed).
//  4. Input delay may be large.
//  5. Support ImGuiBackendFlags_HasSetMousePos, but the reason same as (3), it may not working.

// You can copy and use unmodified imgui_impl_* files in your project. See examples/ folder for examples of using this.
// If you are new to Dear ImGui, read documentation from the docs/ folder + read the top of imgui.cpp.
// Read online: https://github.com/ocornut/imgui/tree/master/docs

#include "imgui.h"
#include "imgui_impl_win32_workingthread.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <vector>
#include <Windows.h>
#include <windowsx.h>
#include <tchar.h>

// Using XInput library for gamepad (with recent Windows SDK this may leads to executables which won't run on Windows 7)
#ifndef IMGUI_IMPL_WIN32WORKINGTHREAD_DISABLE_GAMEPAD
#include <XInput.h>
#else
#define IMGUI_IMPL_WIN32WORKINGTHREAD_DISABLE_LINKING_XINPUT
#endif
#if defined(_MSC_VER) && !defined(IMGUI_IMPL_WIN32WORKINGTHREAD_DISABLE_LINKING_XINPUT)
#pragma comment(lib, "xinput")
//#pragma comment(lib, "Xinput9_1_0")
#endif

// Allow compilation with old Windows SDK. MinGW doesn't have default _WIN32_WINNT/WINVER versions.
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif
#ifndef DBT_DEVNODES_CHANGED
#define DBT_DEVNODES_CHANGED 0x0007
#endif

// CHANGELOG
//  2020-11-01: First implement.

// Data Queue
enum class wt_queue_data_state : uint8_t
{
    unknown     = 0b00000000,
    write_start = 0b00000001,
    write_end   = 0b00000010,
    read_start  = 0b00000100,
    read_end    = 0b00001000,
};
template<typename T, size_t N>
class wt_queue
{
private:
    struct wtf_queue_data
    {
        wtf_queue_data* next;
        wt_queue_data_state state;
        T data;
    };
    wtf_queue_data _buffer[N];
    wtf_queue_data* _pread;
    wtf_queue_data* _pwrite;
public:
    bool write(const T& v)
    {
        if ((uint8_t)_pwrite->state & (uint8_t)wt_queue_data_state::read_end)
        {
            _pwrite->state = wt_queue_data_state::unknown;
            _pwrite->state = wt_queue_data_state::write_start;
            _pwrite->data = v;
            _pwrite->state = wt_queue_data_state::unknown;
            _pwrite->state = wt_queue_data_state::write_end;
            _pwrite = _pwrite->next;
            return true;
        }
        return false;
    }
    bool read(T& v)
    {
        if ((uint8_t)_pread->state & (uint8_t)wt_queue_data_state::write_end)
        {
            _pread->state = wt_queue_data_state::unknown;
            _pread->state = wt_queue_data_state::read_start;
            v = _pread->data;
            _pread->state = wt_queue_data_state::unknown;
            _pread->state = wt_queue_data_state::read_end;
            _pread = _pread->next;
            return true;
        }
        return false;
    }
public:
    wt_queue()
    {
        for (size_t idx = 0; idx < (N - 1); idx += 1)
        {
            _buffer[idx].next = &_buffer[idx + 1];
            _buffer[idx].state = wt_queue_data_state::read_end;
        }
        _buffer[N - 1].next = &_buffer[0];
        _buffer[N - 1].state = wt_queue_data_state::read_end;
        _pread = &_buffer[0];
        _pwrite = &_buffer[0];
    }
    ~wt_queue() = default;
};

// ImGui Data
static void (*g_fLastSetIMEPosFn)(int x, int y) = NULL;

// Win32 Data
static HWND                 g_hWnd = NULL;
static INT64                g_Time = 0;
static INT64                g_TicksPerSecond = 0;
static ImGuiMouseCursor     g_LastMouseCursor = ImGuiMouseCursor_COUNT;
static bool                 g_HasGamepad = false;
static bool                 g_WantUpdateHasGamepad = true;

// Win32 ImGui Data Exchange
struct WNDMSG {
    HWND        hwnd;
    UINT        message;
    WPARAM      wParam;
    LPARAM      lParam;
};
using wt_msg_queue = wt_queue<WNDMSG, 1024>;

constexpr UINT MSG_NONE             = WM_USER_IMGUI_IMPL_WIN32WORKINGTHREAD;
constexpr UINT MSG_MOUSE_CAPTURE    = WM_USER_IMGUI_IMPL_WIN32WORKINGTHREAD + 1;
constexpr UINT MSG_SET_MOUSE_POS    = WM_USER_IMGUI_IMPL_WIN32WORKINGTHREAD + 2;
constexpr UINT MSG_SET_MOUSE_CURSOR = WM_USER_IMGUI_IMPL_WIN32WORKINGTHREAD + 3;
constexpr UINT MSG_SET_IME_POS      = WM_USER_IMGUI_IMPL_WIN32WORKINGTHREAD + 4;

constexpr WPARAM MSG_MOUSE_CAPTURE_SET     = 1;
constexpr WPARAM MSG_MOUSE_CAPTURE_RELEASE = 2;

static bool             g_bWindowFocus = true;
static wt_msg_queue     g_vWin32MSG;
static bool             g_bUpdateCursor = false;
static LPCWSTR          g_sCursorName = IDC_ARROW;

void ImGui_ImplWin32WorkingThread_ProcessMessage()
{
    ImGuiIO& io = ImGui::GetIO();
    WNDMSG msg;
    
    auto resetImGuiInput = [&]() -> void
    {
        #ifdef IMGUI_IMPL_WIN32WORKINGTHREAD_RESET_INPUT_WHEN_WINDOW_FOCUS_LOSE
            std::memset(&io.MouseDown, 0, sizeof(io.MouseDown));
            io.MouseWheel = 0;
            io.MouseWheelH = 0;
            io.MousePos = ImVec2(0.0f, 0.0f);
            PostMessageW(g_hWnd, MSG_MOUSE_CAPTURE, MSG_MOUSE_CAPTURE_RELEASE, 0); // cancel capture
            
            std::memset(&io.KeysDown, 0, sizeof(io.KeysDown));
            io.KeyShift = false;
            io.KeyCtrl = false;
            io.KeyAlt = false;
        #endif
    };
    
    auto updateMousePosition = [&]() -> void
    {
        io.MousePos = ImVec2(
            (float)GET_X_LPARAM(msg.lParam),
            (float)GET_Y_LPARAM(msg.lParam));
    };
    
    auto updateKeyboardKey = [&](bool down) -> void
    {
        // https://github.com/Microsoft/DirectXTK
        UINT vk = msg.wParam;
        switch (vk)
        {
        case VK_SHIFT:
            vk = MapVirtualKeyW((msg.lParam & 0x00ff0000) >> 16, MAPVK_VSC_TO_VK_EX);
            if (!down)
            {
                // Workaround to ensure left vs. right shift get cleared when both were pressed at same time
                io.KeysDown[VK_LSHIFT] = down;
                io.KeysDown[VK_RSHIFT] = down;
            }
            io.KeyShift = down;
            break;
        case VK_CONTROL:
            vk = (msg.lParam & 0x01000000) ? VK_RCONTROL : VK_LCONTROL;
            io.KeyCtrl = down;
            break;
        case VK_MENU:
            vk = (msg.lParam & 0x01000000) ? VK_RMENU : VK_LMENU;
            io.KeyAlt = down;
            break;
        }
        if (vk < 256)
            io.KeysDown[vk] = down;
    };
    
    auto processInputEvent = [&]() -> bool
    {
        switch (msg.message)
        {
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
            {
                int button = 0;
                if (msg.message == WM_LBUTTONDOWN || msg.message == WM_LBUTTONDBLCLK)
                    button = 0;
                if (msg.message == WM_RBUTTONDOWN || msg.message == WM_RBUTTONDBLCLK)
                    button = 1;
                if (msg.message == WM_MBUTTONDOWN || msg.message == WM_MBUTTONDBLCLK)
                    button = 2;
                if (msg.message == WM_XBUTTONDOWN || msg.message == WM_XBUTTONDBLCLK)
                    button = (GET_XBUTTON_WPARAM(msg.wParam) == XBUTTON1) ? 3 : 4;
                if (!ImGui::IsAnyMouseDown())
                {
                    PostMessageW(g_hWnd, MSG_MOUSE_CAPTURE, MSG_MOUSE_CAPTURE_SET, 0);
                }
                io.MouseDown[button] = true;
            }
            updateMousePosition();
            return true;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP:
            {
                int button = 0;
                if (msg.message == WM_LBUTTONUP)
                    button = 0;
                if (msg.message == WM_RBUTTONUP)
                    button = 1;
                if (msg.message == WM_MBUTTONUP)
                    button = 2;
                if (msg.message == WM_XBUTTONUP)
                    button = (GET_XBUTTON_WPARAM(msg.wParam) == XBUTTON1) ? 3 : 4;
                io.MouseDown[button] = false;
                if (!ImGui::IsAnyMouseDown())
                {
                    PostMessageW(g_hWnd, MSG_MOUSE_CAPTURE, MSG_MOUSE_CAPTURE_RELEASE, 0);
                }
            }
            updateMousePosition();
            return true;
        case WM_MOUSEMOVE:
        case WM_MOUSEHOVER:
            updateMousePosition();
            return true;
        case WM_MOUSEWHEEL:
            io.MouseWheel  += (float)GET_WHEEL_DELTA_WPARAM(msg.wParam) / (float)WHEEL_DELTA;
            return true;
        case WM_MOUSEHWHEEL:
            io.MouseWheelH += (float)GET_WHEEL_DELTA_WPARAM(msg.wParam) / (float)WHEEL_DELTA;
            return true;
        
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            updateKeyboardKey(true);
            return true;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            updateKeyboardKey(false);
            return true;
        case WM_CHAR:
            if (msg.wParam > 0 && msg.wParam < 0x10000)
                io.AddInputCharacterUTF16((ImWchar16)msg.wParam);
            return true;
        }
        return false;
    };
    
    auto processOtherEvent = [&]() -> bool
    {
        switch (msg.message)
        {
        case WM_ACTIVATEAPP:
            #ifndef IMGUI_IMPL_WIN32WORKINGTHREAD_REVICE_INPUT_WHEN_WINDOW_FOCUS_LOST
                switch(msg.wParam)
                {
                case TRUE:
                    g_bWindowFocus = true;
                    break;
                case FALSE:
                    g_bWindowFocus = false;
                    break;
                }
            #endif
            resetImGuiInput();
            return true;
        case WM_SIZE:
            io.DisplaySize = ImVec2((float)(LOWORD(msg.lParam)), (float)(HIWORD(msg.lParam)));
            return true;
        case WM_DEVICECHANGE:
            if ((UINT)msg.wParam == DBT_DEVNODES_CHANGED)
            {
                g_WantUpdateHasGamepad = true;
            }
            return true;
        }
        return false;
    };
    
    while (g_vWin32MSG.read(msg))
    {
        if (g_bWindowFocus)
        {
            if (!processInputEvent())
            {
                processOtherEvent();
            }
        }
        else
        {
            processOtherEvent();
        }
    }
}

// Functions
static_assert(sizeof(LPARAM) >= sizeof(ptrdiff_t), "If you compiler hit this assert, please tell me.");
bool ImGui_ImplWin32_UpdateMouseCursor()
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange)
    {
        g_bUpdateCursor = false;
        return false;
    }
    else
    {
        g_bUpdateCursor = true;
    }
    
    ImGuiMouseCursor imgui_cursor = ImGui::GetMouseCursor();
    if (imgui_cursor == ImGuiMouseCursor_None || io.MouseDrawCursor)
    {
        // Hide OS mouse cursor if imgui is drawing it or if it wants no cursor
        g_sCursorName = NULL;
        PostMessageW(g_hWnd, MSG_SET_MOUSE_CURSOR, 0, 0);
    }
    else
    {
        // Show OS mouse cursor
        g_sCursorName = IDC_ARROW;
        switch (imgui_cursor)
        {
        case ImGuiMouseCursor_Arrow:        g_sCursorName = IDC_ARROW;       break;
        case ImGuiMouseCursor_TextInput:    g_sCursorName = IDC_IBEAM;       break;
        case ImGuiMouseCursor_ResizeAll:    g_sCursorName = IDC_SIZEALL;     break;
        case ImGuiMouseCursor_ResizeEW:     g_sCursorName = IDC_SIZEWE;      break;
        case ImGuiMouseCursor_ResizeNS:     g_sCursorName = IDC_SIZENS;      break;
        case ImGuiMouseCursor_ResizeNESW:   g_sCursorName = IDC_SIZENESW;    break;
        case ImGuiMouseCursor_ResizeNWSE:   g_sCursorName = IDC_SIZENWSE;    break;
        case ImGuiMouseCursor_Hand:         g_sCursorName = IDC_HAND;        break;
        case ImGuiMouseCursor_NotAllowed:   g_sCursorName = IDC_NO;          break;
        }
        PostMessageW(g_hWnd, MSG_SET_MOUSE_CURSOR, 0, 0);
    }
    
    return true;
}
void ImGui_ImplWin32_UpdateMousePos()
{
    ImGuiIO& io = ImGui::GetIO();

    // Set OS mouse position if requested (rarely used, only when ImGuiConfigFlags_NavEnableSetMousePos is enabled by user)
    if (io.WantSetMousePos)
    {
        static POINT mouse_pos[16];
        static size_t mouse_pos_idx = 0;
        
        mouse_pos[mouse_pos_idx] = { (LONG)(io.MousePos.x), (LONG)(io.MousePos.y) };
        PostMessageW(g_hWnd, MSG_SET_MOUSE_POS, 0, (LPARAM)(ptrdiff_t)(&mouse_pos[mouse_pos_idx]));
        
        mouse_pos_idx = (mouse_pos_idx + 1) % 16;
    }
}
void ImGui_ImplWin32_UpdateGamepads()
{
#ifndef IMGUI_IMPL_WIN32WORKINGTHREAD_DISABLE_GAMEPAD
    ImGuiIO& io = ImGui::GetIO();
    memset(io.NavInputs, 0, sizeof(io.NavInputs));
    if ((io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) == 0)
        return;

    // Calling XInputGetState() every frame on disconnected gamepads is unfortunately too slow.
    // Instead we refresh gamepad availability by calling XInputGetCapabilities() _only_ after receiving WM_DEVICECHANGE.
    if (g_WantUpdateHasGamepad)
    {
        XINPUT_CAPABILITIES caps;
        g_HasGamepad = (XInputGetCapabilities(0, XINPUT_FLAG_GAMEPAD, &caps) == ERROR_SUCCESS);
        g_WantUpdateHasGamepad = false;
    }

    XINPUT_STATE xinput_state;
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    if (g_HasGamepad && XInputGetState(0, &xinput_state) == ERROR_SUCCESS)
    {
        const XINPUT_GAMEPAD& gamepad = xinput_state.Gamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

        #define MAP_BUTTON(NAV_NO, BUTTON_ENUM)     { io.NavInputs[NAV_NO] = (gamepad.wButtons & BUTTON_ENUM) ? 1.0f : 0.0f; }
        #define MAP_ANALOG(NAV_NO, VALUE, V0, V1)   { float vn = (float)(VALUE - V0) / (float)(V1 - V0); if (vn > 1.0f) vn = 1.0f; if (vn > 0.0f && io.NavInputs[NAV_NO] < vn) io.NavInputs[NAV_NO] = vn; }
        MAP_BUTTON(ImGuiNavInput_Activate,      XINPUT_GAMEPAD_A);              // Cross / A
        MAP_BUTTON(ImGuiNavInput_Cancel,        XINPUT_GAMEPAD_B);              // Circle / B
        MAP_BUTTON(ImGuiNavInput_Menu,          XINPUT_GAMEPAD_X);              // Square / X
        MAP_BUTTON(ImGuiNavInput_Input,         XINPUT_GAMEPAD_Y);              // Triangle / Y
        MAP_BUTTON(ImGuiNavInput_DpadLeft,      XINPUT_GAMEPAD_DPAD_LEFT);      // D-Pad Left
        MAP_BUTTON(ImGuiNavInput_DpadRight,     XINPUT_GAMEPAD_DPAD_RIGHT);     // D-Pad Right
        MAP_BUTTON(ImGuiNavInput_DpadUp,        XINPUT_GAMEPAD_DPAD_UP);        // D-Pad Up
        MAP_BUTTON(ImGuiNavInput_DpadDown,      XINPUT_GAMEPAD_DPAD_DOWN);      // D-Pad Down
        MAP_BUTTON(ImGuiNavInput_FocusPrev,     XINPUT_GAMEPAD_LEFT_SHOULDER);  // L1 / LB
        MAP_BUTTON(ImGuiNavInput_FocusNext,     XINPUT_GAMEPAD_RIGHT_SHOULDER); // R1 / RB
        MAP_BUTTON(ImGuiNavInput_TweakSlow,     XINPUT_GAMEPAD_LEFT_SHOULDER);  // L1 / LB
        MAP_BUTTON(ImGuiNavInput_TweakFast,     XINPUT_GAMEPAD_RIGHT_SHOULDER); // R1 / RB
        MAP_ANALOG(ImGuiNavInput_LStickLeft,    gamepad.sThumbLX,  -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
        MAP_ANALOG(ImGuiNavInput_LStickRight,   gamepad.sThumbLX,  +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
        MAP_ANALOG(ImGuiNavInput_LStickUp,      gamepad.sThumbLY,  +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
        MAP_ANALOG(ImGuiNavInput_LStickDown,    gamepad.sThumbLY,  -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32767);
        #undef MAP_BUTTON
        #undef MAP_ANALOG
    }
#endif // #ifndef IMGUI_IMPL_WIN32WORKINGTHREAD_DISABLE_GAMEPAD
}
void ImGui_ImplWin32_UpdateIMEPos(int x, int y)
{
    static POINT ime_pos[16];
    static size_t ime_pos_idx = 0;
    
    ime_pos[ime_pos_idx] = { (LONG)x, (LONG)y };
    PostMessageW(g_hWnd, MSG_SET_IME_POS, 0, (LPARAM)(ptrdiff_t)(&ime_pos[ime_pos_idx]));
    
    ime_pos_idx = (ime_pos_idx + 1) % 16;
}

// API
bool    ImGui_ImplWin32WorkingThread_Init(void* hwnd)
{
    if (FALSE == QueryPerformanceFrequency((LARGE_INTEGER*)&g_TicksPerSecond))
        return false;
    if (FALSE == QueryPerformanceCounter((LARGE_INTEGER*)&g_Time))
        return false;
    
    // Setup backend capabilities flags
    g_hWnd = (HWND)hwnd;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;         // We can honor GetMouseCursor() values (optional)
    io.BackendFlags |= ImGuiBackendFlags_HasSetMousePos;          // We can honor io.WantSetMousePos requests (optional, rarely used)
    io.BackendPlatformName = "imgui_impl_win32_workingthread";
    
    // Setup backend IME support
    io.ImeWindowHandle = hwnd;
    g_fLastSetIMEPosFn = io.ImeSetInputScreenPosFn;
    io.ImeSetInputScreenPosFn = &ImGui_ImplWin32_UpdateIMEPos;
    
    // Keyboard mapping. ImGui will use those indices to peek into the io.KeysDown[] array that we will update during the application lifetime.
    io.KeyMap[ImGuiKey_Tab] = VK_TAB;
    io.KeyMap[ImGuiKey_LeftArrow] = VK_LEFT;
    io.KeyMap[ImGuiKey_RightArrow] = VK_RIGHT;
    io.KeyMap[ImGuiKey_UpArrow] = VK_UP;
    io.KeyMap[ImGuiKey_DownArrow] = VK_DOWN;
    io.KeyMap[ImGuiKey_PageUp] = VK_PRIOR;
    io.KeyMap[ImGuiKey_PageDown] = VK_NEXT;
    io.KeyMap[ImGuiKey_Home] = VK_HOME;
    io.KeyMap[ImGuiKey_End] = VK_END;
    io.KeyMap[ImGuiKey_Insert] = VK_INSERT;
    io.KeyMap[ImGuiKey_Delete] = VK_DELETE;
    io.KeyMap[ImGuiKey_Backspace] = VK_BACK;
    io.KeyMap[ImGuiKey_Space] = VK_SPACE;
    io.KeyMap[ImGuiKey_Enter] = VK_RETURN;
    io.KeyMap[ImGuiKey_Escape] = VK_ESCAPE;
    io.KeyMap[ImGuiKey_KeyPadEnter] = VK_RETURN;
    io.KeyMap[ImGuiKey_A] = 'A';
    io.KeyMap[ImGuiKey_C] = 'C';
    io.KeyMap[ImGuiKey_V] = 'V';
    io.KeyMap[ImGuiKey_X] = 'X';
    io.KeyMap[ImGuiKey_Y] = 'Y';
    io.KeyMap[ImGuiKey_Z] = 'Z';
    
    return true;
}
void    ImGui_ImplWin32WorkingThread_Shutdown()
{
    ImGuiIO& io = ImGui::GetIO();
    io.ImeWindowHandle = NULL;
    io.ImeSetInputScreenPosFn = g_fLastSetIMEPosFn;
    g_fLastSetIMEPosFn = NULL;
    
    g_hWnd = NULL;
    g_Time = 0;
    g_TicksPerSecond = 0;
    g_LastMouseCursor = ImGuiMouseCursor_COUNT;
    g_HasGamepad = false;
    g_WantUpdateHasGamepad = true;
    
    g_bWindowFocus = true;
    g_vWin32MSG = wt_msg_queue(); // clean
    g_bUpdateCursor = false;
    g_sCursorName = IDC_ARROW;
}
void    ImGui_ImplWin32WorkingThread_NewFrame()
{
    ImGuiIO& io = ImGui::GetIO();
    IM_ASSERT(io.Fonts->IsBuilt() && "Font atlas not built! It is generally built by the renderer backend. Missing call to renderer _NewFrame() function? e.g. ImGui_ImplOpenGL3_NewFrame().");
    
    // Setup time step
    INT64 current_time;
    ::QueryPerformanceCounter((LARGE_INTEGER*)&current_time);
    io.DeltaTime = (float)(current_time - g_Time) / g_TicksPerSecond;
    g_Time = current_time;
    
    // Setup display size (every frame to accommodate for window resizing)
    RECT rect;
    ::GetClientRect(g_hWnd, &rect); // may not thread safe?
    io.DisplaySize = ImVec2((float)(rect.right - rect.left), (float)(rect.bottom - rect.top));
    
    // Now process all messages
    io.KeySuper = false;
    ImGui_ImplWin32_UpdateMousePos(); // set position first
    ImGui_ImplWin32WorkingThread_ProcessMessage();
    
    // Update OS mouse cursor with the cursor requested by imgui
    ImGuiMouseCursor mouse_cursor = io.MouseDrawCursor ? ImGuiMouseCursor_None : ImGui::GetMouseCursor();
    if (g_LastMouseCursor != mouse_cursor)
    {
        g_LastMouseCursor = mouse_cursor;
        ImGui_ImplWin32_UpdateMouseCursor();
    }
    
    // Update game controllers (if enabled and available)
    ImGui_ImplWin32_UpdateGamepads();
}


// Win32 message handler (process Win32 mouse/keyboard inputs, etc.)
// Call from your application's message handler.
// When implementing your own backend, you can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if Dear ImGui wants to use your inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application.
// Generally you may always pass all inputs to Dear ImGui, and hide them from your application based on those two flags.
// PS: In this Win32 handler, we use the capture API (GetCapture/SetCapture/ReleaseCapture) to be able to read mouse coordinates when dragging mouse outside of our window bounds.
// PS: We treat DBLCLK messages as regular mouse down messages, so this code will work on windows classes that have the CS_DBLCLKS flag set. Our own example app code doesn't set this flag.
#if 0
// Copy this line into your .cpp file to forward declare the function.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32WorkingThread_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif
IMGUI_IMPL_API LRESULT ImGui_ImplWin32WorkingThread_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui::GetCurrentContext() == NULL)
        return 0;
    if (hwnd != g_hWnd)
        return 0;
    
    auto dispatch = [&]() -> void
    {
        WNDMSG msg_v = { hwnd, msg, wParam, lParam };
        g_vWin32MSG.write(msg_v); // what will happen if queue is full ???
    };
    
    ImGuiIO& io = ImGui::GetIO();
    switch (msg)
    {
    case WM_ACTIVATEAPP:
        dispatch();
        #ifndef IMGUI_IMPL_WIN32WORKINGTHREAD_REVICE_INPUT_WHEN_WINDOW_FOCUS_LOST
            return 1;
        #else
            return 0;
        #endif
    
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_MOUSEMOVE:
    case WM_MOUSEHOVER:
    
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_CHAR:
    
    case WM_SIZE:
    case WM_DEVICECHANGE:
        dispatch();
        return 0;
    
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && g_bUpdateCursor)
        {
            if (g_sCursorName != NULL)
                SetCursor(LoadCursorW(NULL, g_sCursorName));
            else
                SetCursor(NULL);
            return 1;
        }
        return 0;
    case MSG_MOUSE_CAPTURE:
        switch(wParam)
        {
        case MSG_MOUSE_CAPTURE_SET:
            if (GetCapture() == NULL)
            {
                SetCapture(hwnd);
            }
            break;
        case MSG_MOUSE_CAPTURE_RELEASE:
            if (GetCapture() == hwnd)
            {
                ReleaseCapture();
            }
            break;
        }
        return 0;
    case MSG_SET_MOUSE_POS:
        {
            POINT* ptr = (POINT*)(ptrdiff_t)lParam;
            POINT pos = *ptr;
            ClientToScreen(hwnd, &pos);
            SetCursorPos(pos.x, pos.y);
        }
        return 0;
    case MSG_SET_MOUSE_CURSOR:
        {
            if (g_sCursorName != NULL)
                SetCursor(LoadCursorW(NULL, g_sCursorName));
            else
                SetCursor(NULL);
        }
        return 1;
    case MSG_SET_IME_POS:
        {
            POINT* ptr = (POINT*)(ptrdiff_t)lParam;
            if (HIMC himc = ImmGetContext(hwnd))
            {
                COMPOSITIONFORM cf;
                cf.ptCurrentPos = *ptr;
                cf.dwStyle = CFS_FORCE_POSITION;
                ImmSetCompositionWindow(himc, &cf);
                ImmReleaseContext(hwnd, himc);
            }
        }
        return 0;
    }
    
    return 0;
}


//--------------------------------------------------------------------------------------------------------
// DPI-related helpers (optional)
//--------------------------------------------------------------------------------------------------------
// - Use to enable DPI awareness without having to create an application manifest.
// - Your own app may already do this via a manifest or explicit calls. This is mostly useful for our examples/ apps.
// - In theory we could call simple functions from Windows SDK such as SetProcessDPIAware(), SetProcessDpiAwareness(), etc.
//   but most of the functions provided by Microsoft require Windows 8.1/10+ SDK at compile time and Windows 8/10+ at runtime,
//   neither we want to require the user to have. So we dynamically select and load those functions to avoid dependencies.
//---------------------------------------------------------------------------------------------------------
// This is the scheme successfully used by GLFW (from which we borrowed some of the code) and other apps aiming to be highly portable.
// ImGui_ImplWin32_EnableDpiAwareness() is just a helper called by main.cpp, we don't call it automatically.
// If you are trying to implement your own backend for your own engine, you may ignore that noise.
//---------------------------------------------------------------------------------------------------------

// Implement some of the functions and types normally declared in recent Windows SDK.
#if !defined(_versionhelpers_H_INCLUDED_) && !defined(_INC_VERSIONHELPERS)
static BOOL IsWindowsVersionOrGreater(WORD major, WORD minor, WORD sp)
{
    OSVERSIONINFOEXW osvi = { sizeof(osvi), major, minor, 0, 0, { 0 }, sp, 0, 0, 0, 0 };
    DWORD mask = VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR;
    ULONGLONG cond = ::VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL);
    cond = ::VerSetConditionMask(cond, VER_MINORVERSION, VER_GREATER_EQUAL);
    cond = ::VerSetConditionMask(cond, VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL);
    return ::VerifyVersionInfoW(&osvi, mask, cond);
}
#define IsWindows8Point1OrGreater()  IsWindowsVersionOrGreater(HIBYTE(0x0602), LOBYTE(0x0602), 0) // _WIN32_WINNT_WINBLUE
#endif

#ifndef DPI_ENUMS_DECLARED
typedef enum { PROCESS_DPI_UNAWARE = 0, PROCESS_SYSTEM_DPI_AWARE = 1, PROCESS_PER_MONITOR_DPI_AWARE = 2 } PROCESS_DPI_AWARENESS;
typedef enum { MDT_EFFECTIVE_DPI = 0, MDT_ANGULAR_DPI = 1, MDT_RAW_DPI = 2, MDT_DEFAULT = MDT_EFFECTIVE_DPI } MONITOR_DPI_TYPE;
#endif
#ifndef _DPI_AWARENESS_CONTEXTS_
DECLARE_HANDLE(DPI_AWARENESS_CONTEXT);
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE    (DPI_AWARENESS_CONTEXT)-3
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 (DPI_AWARENESS_CONTEXT)-4
#endif
typedef HRESULT(WINAPI* PFN_SetProcessDpiAwareness)(PROCESS_DPI_AWARENESS);                     // Shcore.lib + dll, Windows 8.1+
typedef HRESULT(WINAPI* PFN_GetDpiForMonitor)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);        // Shcore.lib + dll, Windows 8.1+
typedef DPI_AWARENESS_CONTEXT(WINAPI* PFN_SetThreadDpiAwarenessContext)(DPI_AWARENESS_CONTEXT); // User32.lib + dll, Windows 10 v1607+ (Creators Update)

// Helper function to enable DPI awareness without setting up a manifest
void ImGui_ImplWin32WorkingThread_EnableDpiAwareness()
{
    // if (IsWindows10OrGreater()) // This needs a manifest to succeed. Instead we try to grab the function pointer!
    {
        static HINSTANCE user32_dll = ::LoadLibraryA("user32.dll"); // Reference counted per-process
        if (PFN_SetThreadDpiAwarenessContext SetThreadDpiAwarenessContextFn = (PFN_SetThreadDpiAwarenessContext)::GetProcAddress(user32_dll, "SetThreadDpiAwarenessContext"))
        {
            SetThreadDpiAwarenessContextFn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            return;
        }
    }
    if (IsWindows8Point1OrGreater())
    {
        static HINSTANCE shcore_dll = ::LoadLibraryA("shcore.dll"); // Reference counted per-process
        if (PFN_SetProcessDpiAwareness SetProcessDpiAwarenessFn = (PFN_SetProcessDpiAwareness)::GetProcAddress(shcore_dll, "SetProcessDpiAwareness"))
        {
            SetProcessDpiAwarenessFn(PROCESS_PER_MONITOR_DPI_AWARE);
            return;
        }
    }
#if _WIN32_WINNT >= 0x0600
    ::SetProcessDPIAware();
#endif
}

#if defined(_MSC_VER) && !defined(NOGDI)
#pragma comment(lib, "gdi32")   // Link with gdi32.lib for GetDeviceCaps()
#endif

float ImGui_ImplWin32WorkingThread_GetDpiScaleForMonitor(void* monitor)
{
    UINT xdpi = 96, ydpi = 96;
    static BOOL bIsWindows8Point1OrGreater = IsWindows8Point1OrGreater();
    if (bIsWindows8Point1OrGreater)
    {
        static HINSTANCE shcore_dll = ::LoadLibraryA("shcore.dll"); // Reference counted per-process
        if (PFN_GetDpiForMonitor GetDpiForMonitorFn = (PFN_GetDpiForMonitor)::GetProcAddress(shcore_dll, "GetDpiForMonitor"))
            GetDpiForMonitorFn((HMONITOR)monitor, MDT_EFFECTIVE_DPI, &xdpi, &ydpi);
    }
#ifndef NOGDI
    else
    {
        const HDC dc = ::GetDC(NULL);
        xdpi = ::GetDeviceCaps(dc, LOGPIXELSX);
        ydpi = ::GetDeviceCaps(dc, LOGPIXELSY);
        ::ReleaseDC(NULL, dc);
    }
#endif
    IM_ASSERT(xdpi == ydpi); // Please contact me if you hit this assert!
    return xdpi / 96.0f;
}

float ImGui_ImplWin32WorkingThread_GetDpiScaleForHwnd(void* hwnd)
{
    HMONITOR monitor = ::MonitorFromWindow((HWND)hwnd, MONITOR_DEFAULTTONEAREST);
    return ImGui_ImplWin32WorkingThread_GetDpiScaleForMonitor(monitor);
}

//---------------------------------------------------------------------------------------------------------
