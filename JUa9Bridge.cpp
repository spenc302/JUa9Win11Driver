#include <windows.h>
#include <setupapi.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdint.h>
#include <atomic>

#include "resource.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "shell32.lib")

// --- Tray icon / background-process support ------------------------------
//
// The bridge normally runs as a hidden background process controlled from
// the system tray instead of a visible console window. Diagnostic modes
// (--rumble / --autocenter) still allocate a console so their output is
// visible when run manually.

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayIconId = 1;
constexpr UINT kMenuIdExit = 1001;
constexpr wchar_t kWindowClassName[] = L"JUa9BridgeTrayWnd";

std::atomic<bool> g_running{ true };
NOTIFYICONDATAW g_trayIcon = {};
HWND g_trayWnd = nullptr;

void RemoveTrayIcon()
{
    if (g_trayIcon.cbSize != 0) {
        Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
        g_trayIcon = {};
    }
}

LRESULT CALLBACK TrayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case kTrayMessage:
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, kMenuIdExit, L"Stop JUa9 Bridge");
            SetForegroundWindow(hWnd); // required so the menu dismisses on focus loss
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
            DestroyMenu(menu);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == kMenuIdExit) {
            g_running = false;
            DestroyWindow(hWnd);
        }
        return 0;
    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

// Creates a message-only-style hidden window plus its tray icon. Returns
// nullptr on failure.
HWND CreateTrayWindow(HINSTANCE hInstance)
{
    HICON appIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (appIcon == nullptr) {
        appIcon = LoadIconW(nullptr, IDI_APPLICATION); // fallback if the .rc wasn't built in
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWindowClassName;
    wc.hIcon = appIcon;
    if (!RegisterClassW(&wc)) {
        return nullptr;
    }

    // A regular (non-message-only) hidden top-level window is used rather
    // than HWND_MESSAGE so that Shell_NotifyIcon's taskbar-recreation and
    // foreground/menu behavior work reliably across Explorer restarts.
    HWND hwnd = CreateWindowExW(
        0, kWindowClassName, L"JUa9 Bridge", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 0, 0,
        nullptr, nullptr, hInstance, nullptr);
    if (hwnd == nullptr) {
        return nullptr;
    }
    // Deliberately never shown -- ShowWindow(SW_HIDE) is the default state
    // for a window that's never had ShowWindow(SW_SHOW) called on it.

    g_trayIcon.cbSize = sizeof(g_trayIcon);
    g_trayIcon.hWnd = hwnd;
    g_trayIcon.uID = kTrayIconId;
    g_trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayIcon.uCallbackMessage = kTrayMessage;
    g_trayIcon.hIcon = appIcon;
    wcscpy_s(g_trayIcon.szTip, L"JUa9 Bridge (running)");
    Shell_NotifyIconW(NIM_ADD, &g_trayIcon);

    return hwnd;
}

// Opens a console window for diagnostic modes (--rumble / --autocenter) so
// their wprintf/fwprintf output is visible, since the app otherwise builds
// with no console at all now that entry has moved to wWinMain.
// Closes the tray window/icon from the worker thread -- used when the
// bridge loop exits on its own (device unplugged, vJoy unavailable, etc.)
// so the tray icon doesn't linger after the thread it represents has died.
void RequestTrayShutdown()
{
    g_running = false;
    if (g_trayWnd != nullptr) {
        PostMessageW(g_trayWnd, WM_CLOSE, 0, 0);
    }
}

void AttachDiagnosticConsole()
{
    AllocConsole();
    FILE* dummy = nullptr;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
    freopen_s(&dummy, "CONIN$", "r", stdin);
}

} // namespace

static const GUID kJua9Interface =
{ 0x9c1e2c0b, 0x4d72, 0x4a19, { 0x9f, 0x6a, 0x7b, 0x4d, 0x11, 0x2e, 0x6c, 0x81 } };

static const DWORD kGetLastReport =
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS);
static const DWORD kSendFrame =
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS);
static const DWORD kQueryId =
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS);

struct JoystickPositionV2 {
    BYTE device;
    LONG throttle;
    LONG rudder;
    LONG aileron;
    LONG axisX;
    LONG axisY;
    LONG axisZ;
    LONG axisXRot;
    LONG axisYRot;
    LONG axisZRot;
    LONG slider;
    LONG dial;
    LONG wheel;
    LONG axisVX;
    LONG axisVY;
    LONG axisVZ;
    LONG axisVBRX;
    LONG axisVBRY;
    LONG axisVBRZ;
    LONG buttons;
    DWORD hats;
    DWORD hatsEx1;
    DWORD hatsEx2;
    DWORD hatsEx3;
    LONG buttonsEx1;
    LONG buttonsEx2;
    LONG buttonsEx3;
};

static_assert(offsetof(JoystickPositionV2, throttle) == 4);
static_assert(offsetof(JoystickPositionV2, axisX) == 16);
static_assert(offsetof(JoystickPositionV2, axisY) == 20);
static_assert(offsetof(JoystickPositionV2, buttons) == 76);
static_assert(sizeof(JoystickPositionV2) == 108);

typedef BOOL(__cdecl* VJoyEnabledFn)();
typedef BOOL(__cdecl* AcquireVjdFn)(UINT);
typedef void(__cdecl* RelinquishVjdFn)(UINT);
typedef BOOL(__cdecl* ResetVjdFn)(UINT);
typedef BOOL(__cdecl* SetAxisFn)(LONG, UINT, UINT);
typedef BOOL(__cdecl* SetBtnFn)(BOOL, UINT, UCHAR);
typedef BOOL(__cdecl* SetContPovFn)(LONG, UINT, UCHAR);
typedef BOOL(__cdecl* GetAxisExistFn)(UINT, UINT);

static HANDLE FindJua9()
{
    HDEVINFO info = SetupDiGetClassDevsW(
        &kJua9Interface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }

    SP_DEVICE_INTERFACE_DATA interfaceData = {};
    interfaceData.cbSize = sizeof(interfaceData);
    HANDLE device = INVALID_HANDLE_VALUE;

    for (DWORD index = 0;
         SetupDiEnumDeviceInterfaces(info, nullptr, &kJua9Interface, index, &interfaceData);
         ++index) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(
            info, &interfaceData, nullptr, 0, &required, nullptr);
        auto detail = static_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, required));
        if (detail == nullptr) {
            continue;
        }
        detail->cbSize = sizeof(*detail);
        if (SetupDiGetDeviceInterfaceDetailW(
                info, &interfaceData, detail, required, nullptr, nullptr)) {
            device = CreateFileW(
                detail->DevicePath,
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
        }
        HeapFree(GetProcessHeap(), 0, detail);
        if (device != INVALID_HANDLE_VALUE) {
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(info);
    return device;
}

static LONG ScaleAxis(int16_t value)
{
    constexpr LONG kAxisMin = -1920;
    constexpr LONG kAxisMax = 1920;
    if (value <= kAxisMin) {
        return 0;
    }
    if (value >= kAxisMax) {
        return 32767;
    }
    return static_cast<LONG>(
        (static_cast<int64_t>(value) - kAxisMin) * 32767 /
        (kAxisMax - kAxisMin));
}

static bool SendPacket(HANDLE device, const BYTE* packet, BYTE length)
{
    DWORD returned = 0;
    if (!DeviceIoControl(device, kSendFrame, const_cast<BYTE*>(packet), length,
                         nullptr, 0, &returned, nullptr)) {
        fwprintf(stderr, L"USB packet 0x%02X failed: %lu\n",
                 packet[0], GetLastError());
        return false;
    }
    return true;
}

static bool QueryId(
    HANDLE device,
    BYTE request,
    BYTE* response,
    DWORD responseSize,
    DWORD* returned)
{
    if (!DeviceIoControl(device, kQueryId, &request, sizeof(request),
                         response, responseSize, returned, nullptr)) {
        fwprintf(stderr, L"USB query 0x%02X failed: %lu\n",
                 request, GetLastError());
        return false;
    }
    return *returned != 0 && response[0] == request;
}

static bool SendPacketPaced(HANDLE device, const BYTE* packet, BYTE length)
{
    // Real hardware (Intel 80930 + Immersion I-Force firmware) parses each
    // command asynchronously after the USB transaction completes; Linux's
    // driver naturally paces packets because it waits for each URB's
    // completion callback before submitting the next one. Our synchronous
    // writes complete as soon as the bus transaction acks, which can be
    // faster than the device's firmware can actually parse/store the
    // command. A short pause between sends gives the firmware time to
    // finish processing before the next command arrives.
    bool ok = SendPacket(device, packet, length);
    Sleep(20);
    return ok;
}

static bool RunRumbleTest(HANDLE device)
{
    /*
     * I-Force effect memory starts at zero. These packets mirror the
     * constant-effect upload sequence used by Linux's iforce driver.
     *
     * NOTE: the on-the-wire USB packet is [opcode byte][payload bytes...].
     * The "length" nibble in FF_CMD_* only tells the host driver how many
     * payload bytes to copy out of its internal ring buffer -- it is never
     * actually transmitted as a separate byte on the wire.
     */
    // Update with exact values captured from working XP driver:
    // XP Driver sends: 40 05 00 04 (Spring profile), 42 01 (Enable motors)
    const BYTE setProfile[] = { 0x40, 0x05, 0x00, 0x04 };
    const BYTE enable[] = { 0x42, 0x01 };
    const BYTE gain[] = { 0x43, 0x7F };
    const BYTE magnitude[] = { 0x03, 0x00, 0x00, 0x7F };
    const BYTE envelope[] = {
        0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    // Duration 0x0BB8 = 3000 ms, so a real motor response is easy to notice.
    const BYTE effect[] = {
        0x01, 0x00, 0x00, 0x20, 0xB8, 0x0B, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00
    };
    // FF_CMD_PLAY payload is [effect_id_lo, flag, repeat_count_lo]
    const BYTE playLoop[] = { 0x41, 0x00, 0x41, 0x05 };
    const BYTE stop[] = { 0x41, 0x00, 0x00, 0x00 };
    const BYTE disable[] = { 0x42, 0x00 };
    BYTE response[16] = {};
    DWORD responseLength = 0;

    const char queryIds[] = { 'O', 'M', 'P', 'B', 'N' };
    for (char id : queryIds) {
        DWORD len = 0;
        BYTE buf[16] = {};
        BOOL ok = QueryId(device, static_cast<BYTE>(id), buf, sizeof(buf), &len);
        wprintf(L"Query '%c': ok=%d len=%lu bytes=", id, ok, len);
        for (DWORD i = 0; i < len && i < sizeof(buf); ++i) {
            wprintf(L"%02X ", buf[i]);
        }
        wprintf(L"\n");
    }

    if (!QueryId(device, 'O', response, sizeof(response), &responseLength) ||
        !QueryId(device, 'B', response, sizeof(response), &responseLength) ||
        responseLength < 3 ||
        (static_cast<WORD>(response[1]) |
         (static_cast<WORD>(response[2]) << 8)) < 16 ||
        !QueryId(device, 'N', response, sizeof(response), &responseLength) ||
        responseLength < 2 || response[1] == 0) {
        fwprintf(stderr, L"Device did not report usable I-Force effect memory.\n");
        return false;
    }

    if (!SendPacketPaced(device, setProfile, sizeof(setProfile)) ||
        !SendPacketPaced(device, enable, sizeof(enable)) ||
        !SendPacketPaced(device, gain, sizeof(gain)) ||
        !SendPacketPaced(device, magnitude, sizeof(magnitude)) ||
        !SendPacketPaced(device, envelope, sizeof(envelope)) ||
        !SendPacketPaced(device, effect, sizeof(effect)) ||
        !SendPacketPaced(device, playLoop, sizeof(playLoop))) {
        SendPacket(device, disable, sizeof(disable));
        return false;
    }

    wprintf(L"Effect uploaded and playing at max strength for 3 seconds "
            L"(re-issuing play every 200 ms as a keep-alive)...\n");
    for (int i = 0; i < 30; ++i) {
        BYTE report[16] = {};
        DWORD returned = 0;
        if (DeviceIoControl(device, kGetLastReport, nullptr, 0, report,
                            sizeof(report), &returned, nullptr) &&
            returned > 0 && report[0] == 0x02) {
            wprintf(L"Status report: ");
            for (DWORD i2 = 0; i2 < returned; ++i2) {
                wprintf(L"%02X ", report[i2]);
            }
            wprintf(L"\n");
        }
        if (i % 2 == 0) {
            SendPacket(device, playLoop, sizeof(playLoop));
        }
        Sleep(100);
    }
    bool stopped = SendPacket(device, stop, sizeof(stop));
    bool disabled = SendPacket(device, disable, sizeof(disable));
    return stopped && disabled;
}

static bool RunAutocenterTest(HANDLE device)
{
    // Exact sequence verified from original XP driver USB trace:
    // 1. Query 'N' (Device ID) -> returns 4e 0a
    // 2. Query 'B' (RAM size)  -> returns 42 c8 00
    // 3. OUT EP 0x01: 40 05 00 04 (Hardware spring/profile parameters)
    // 4. OUT EP 0x01: 42 01 (Enable motors)
    const BYTE setProfile[] = { 0x40, 0x05, 0x00, 0x04 };
    const BYTE enableMotors[] = { 0x42, 0x01 };
    const BYTE gain[] = { 0x43, 0x7F };
    const BYTE disableMotors[] = { 0x42, 0x00 };

    BYTE response[16] = {};
    DWORD responseLength = 0;

    const char queryIds[] = { 'O', 'M', 'P', 'B', 'N' };
    for (char id : queryIds) {
        DWORD len = 0;
        BYTE buf[16] = {};
        BOOL ok = QueryId(device, static_cast<BYTE>(id), buf, sizeof(buf), &len);
        wprintf(L"Query '%c': ok=%d len=%lu bytes=", id, ok, len);
        for (DWORD i = 0; i < len && i < sizeof(buf); ++i) {
            wprintf(L"%02X ", buf[i]);
        }
        wprintf(L"\n");
    }

    wprintf(L"Sending XP driver exact startup packets: 40 05 00 04, then 42 01...\n");
    if (!SendPacketPaced(device, setProfile, sizeof(setProfile)) ||
        !SendPacketPaced(device, enableMotors, sizeof(enableMotors)) ||
        !SendPacketPaced(device, gain, sizeof(gain))) {
        wprintf(L"Failed to send startup packets.\n");
        return false;
    }

    wprintf(L"Hardware motor profile active for 10 seconds.\n"
            L"HOLD the joystick grip firmly (covering the optical hand sensor).\n"
            L"Live status from joystick:\n");
    for (int i = 0; i < 100; ++i) {
        BYTE report[16] = {};
        DWORD returned = 0;
        if (DeviceIoControl(device, kGetLastReport, nullptr, 0, report,
                            sizeof(report), &returned, nullptr) &&
            returned > 0) {
            if (report[0] == 0x02) {
                const wchar_t* safetyState = L"UNKNOWN";
                if (report[1] == 0x01) {
                    safetyState = L"01 (Safety OPEN / Hand OFF grip / Standby)";
                } else if (report[1] == 0x03) {
                    safetyState = L"03 (MOTORS ENGAGED / Hand ON grip / Active!)";
                }
                wprintf(L"[%02d] Status Report 0x02: State=%s raw=%02X %02X %02X %02X\n",
                        i, safetyState, report[0], report[1], report[2], report[3]);
            } else if (report[0] == 0x01) {
                int16_t x = static_cast<int16_t>(report[1] | (report[2] << 8));
                int16_t y = static_cast<int16_t>(report[3] | (report[4] << 8));
                wprintf(L"[%02d] Input Report 0x01: X=%6d Y=%6d Throttle=%3d\n",
                        i, x, y, report[5]);
            }
        }
        Sleep(100);
    }

    SendPacket(device, disableMotors, sizeof(disableMotors));
    return true;
}

// The former body of main() for the continuous vJoy-bridging mode. Runs on
// a worker thread once the tray window is up; checks g_running periodically
// so "Stop JUa9 Bridge" from the tray menu unwinds it cleanly instead of
// leaving the vJoy device or USB handle in a half-torn-down state.
DWORD WINAPI RunBridgeThread(LPVOID param)
{
    HANDLE device = static_cast<HANDLE>(param);

    HMODULE vjoy = LoadLibraryW(L"C:\\Program Files\\vJoy\\x64\\vJoyInterface.dll");
    if (vjoy == nullptr) {
        fwprintf(stderr, L"vJoyInterface.dll was not found.\n");
        CloseHandle(device);
        RequestTrayShutdown();
        return 1;
    }

    auto enabled = reinterpret_cast<VJoyEnabledFn>(GetProcAddress(vjoy, "vJoyEnabled"));
    auto acquire = reinterpret_cast<AcquireVjdFn>(GetProcAddress(vjoy, "AcquireVJD"));
    auto relinquish = reinterpret_cast<RelinquishVjdFn>(GetProcAddress(vjoy, "RelinquishVJD"));
    auto reset = reinterpret_cast<ResetVjdFn>(GetProcAddress(vjoy, "ResetVJD"));
    auto setAxis = reinterpret_cast<SetAxisFn>(GetProcAddress(vjoy, "SetAxis"));
    auto setBtn = reinterpret_cast<SetBtnFn>(GetProcAddress(vjoy, "SetBtn"));
    auto setContPov = reinterpret_cast<SetContPovFn>(
        GetProcAddress(vjoy, "SetContPov"));
    auto axisExists = reinterpret_cast<GetAxisExistFn>(
        GetProcAddress(vjoy, "GetVJDAxisExist"));
    constexpr UINT vjoyId = 1;

    if (enabled == nullptr || acquire == nullptr || relinquish == nullptr ||
        reset == nullptr || setAxis == nullptr ||
        setBtn == nullptr || setContPov == nullptr ||
        axisExists == nullptr || !enabled() ||
        !acquire(vjoyId)) {
        fwprintf(stderr, L"vJoy device 1 is unavailable or already owned.\n");
        FreeLibrary(vjoy);
        CloseHandle(device);
        RequestTrayShutdown();
        return 1;
    }

    reset(vjoyId);
    wprintf(L"vJoy axes: X=%s Y=%s Slider=%s\n",
            axisExists(vjoyId, 0x30) ? L"yes" : L"no",
            axisExists(vjoyId, 0x31) ? L"yes" : L"no",
            axisExists(vjoyId, 0x36) ? L"yes" : L"no");
    wprintf(L"J-UA9 bridge running on vJoy device 1. Use the tray icon to stop.\n");

    BYTE frame[16] = {};
    JoystickPositionV2 position = {};
    position.device = static_cast<BYTE>(vjoyId);
    position.hats = 0xFFFFFFFF;
    position.hatsEx1 = 0xFFFFFFFF;
    position.hatsEx2 = 0xFFFFFFFF;
    position.hatsEx3 = 0xFFFFFFFF;
    DWORD lastButtons = 0;
    DWORD lastReturned = 0;
    BYTE lastFrame[sizeof(frame)] = {};
    bool haveLastFrame = false;

    while (g_running) {
        DWORD returned = 0;
        if (!DeviceIoControl(device, kGetLastReport, nullptr, 0,
                             frame, sizeof(frame), &returned, nullptr)) {
            fwprintf(stderr, L"DeviceIoControl failed: %lu\n", GetLastError());
            break;
        }

        bool changed = returned != lastReturned ||
            !haveLastFrame ||
            memcmp(frame, lastFrame, returned < sizeof(frame) ? returned : sizeof(frame)) != 0;
        if (changed) {
            wprintf(L"driver report length=%lu", returned);
            for (DWORD i = 0; i < returned && i < sizeof(frame); ++i) {
                wprintf(L" %02X", frame[i]);
            }
            wprintf(L"\n");
            lastReturned = returned;
            memcpy(lastFrame, frame, sizeof(lastFrame));
            haveLastFrame = true;
        }

        if (returned >= 8 && frame[0] == 1) {
            int16_t x = static_cast<int16_t>(
                static_cast<uint16_t>(frame[1]) |
                (static_cast<uint16_t>(frame[2]) << 8));
            int16_t y = static_cast<int16_t>(
                static_cast<uint16_t>(frame[3]) |
                (static_cast<uint16_t>(frame[4]) << 8));
            position.axisX = ScaleAxis(x);
            position.axisY = ScaleAxis(y);
            position.slider = 32767L - static_cast<LONG>(frame[5]) * 257L;
            position.buttons = static_cast<LONG>(frame[6]);
            if ((frame[7] & 0x0F) == 0x01) {
                position.buttons |= 0x04L;
            }
            switch ((frame[7] >> 4) & 0x0F) {
            case 0:
                position.hats = 0xFFFFFFFF;
                break;
            case 1:
                position.hats = 4500;
                break;
            case 2:
                position.hats = 9000;
                break;
            case 3:
                position.hats = 13500;
                break;
            case 4:
                position.hats = 18000;
                break;
            case 5:
                position.hats = 22500;
                break;
            case 6:
                position.hats = 27000;
                break;
            case 7:
                position.hats = 31500;
                break;
            default:
                position.hats = 0xFFFFFFFF;
                break;
            }
            if (position.buttons != lastButtons ||
                position.axisX != 16384 ||
                position.axisY != 16384) {
                wprintf(L"buttons=0x%02lX x=%ld y=%ld slider=%ld\n",
                        position.buttons, position.axisX, position.axisY,
                        position.slider);
                lastButtons = position.buttons;
            }
            if (!setAxis(position.axisX, vjoyId, 0x30) ||
                !setAxis(position.axisY, vjoyId, 0x31) ||
                !setAxis(position.slider, vjoyId, 0x36)) {
                fwprintf(stderr, L"SetAxis failed: %lu\n", GetLastError());
            }
            for (UCHAR button = 1; button <= 8; ++button) {
                if (!setBtn((position.buttons & (1L << (button - 1))) != 0,
                            vjoyId, button)) {
                    fwprintf(stderr, L"SetBtn %u failed: %lu\n",
                             button, GetLastError());
                }
            }
            if (!setContPov(static_cast<LONG>(position.hats), vjoyId, 1)) {
                fwprintf(stderr, L"SetContPov failed: %lu\n", GetLastError());
            }
        }
        Sleep(8);
    }

    reset(vjoyId);
    relinquish(vjoyId);
    FreeLibrary(vjoy);
    CloseHandle(device);
    RequestTrayShutdown(); // no-op if the tray "Stop" item already triggered this
    return 0;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // Diagnostic modes: unchanged behavior, just now need an explicit
    // console since the app no longer has one by default.
    if (argc > 1 && (_wcsicmp(argv[1], L"--rumble") == 0 ||
                     _wcsicmp(argv[1], L"--autocenter") == 0)) {
        AttachDiagnosticConsole();
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);

        HANDLE device = FindJua9();
        if (device == INVALID_HANDLE_VALUE) {
            fwprintf(stderr, L"J-UA9 interface not found. Is the test driver started?\n");
            LocalFree(argv);
            return 1;
        }

        bool success;
        if (_wcsicmp(argv[1], L"--rumble") == 0) {
            wprintf(L"Running a max-strength 3 second rumble test.\n");
            success = RunRumbleTest(device);
        } else {
            success = RunAutocenterTest(device);
        }
        CloseHandle(device);
        wprintf(L"Press Enter to close this window...\n");
        getchar();
        LocalFree(argv);
        return success ? 0 : 1;
    }
    LocalFree(argv);

    // Normal mode: hidden window + tray icon, bridge loop on a worker thread.
    HANDLE device = FindJua9();
    if (device == INVALID_HANDLE_VALUE) {
        MessageBoxW(nullptr,
            L"J-UA9 interface not found. Is the test driver started?",
            L"JUa9 Bridge", MB_ICONERROR | MB_OK);
        return 1;
    }

    g_trayWnd = CreateTrayWindow(hInstance);
    if (g_trayWnd == nullptr) {
        MessageBoxW(nullptr, L"Failed to create tray window.", L"JUa9 Bridge",
            MB_ICONERROR | MB_OK);
        CloseHandle(device);
        return 1;
    }

    HANDLE thread = CreateThread(nullptr, 0, RunBridgeThread, device, 0, nullptr);
    if (thread == nullptr) {
        MessageBoxW(nullptr, L"Failed to start bridge thread.", L"JUa9 Bridge",
            MB_ICONERROR | MB_OK);
        DestroyWindow(g_trayWnd);
        CloseHandle(device);
        return 1;
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Give the bridge thread a moment to notice g_running==false and unwind
    // cleanly (release vJoy device, close the USB handle) before exiting.
    g_running = false;
    WaitForSingleObject(thread, 2000);
    CloseHandle(thread);
    return static_cast<int>(msg.wParam);
}
