#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdint.h>

#pragma comment(lib, "setupapi.lib")

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
    const BYTE autocenterMagnitude[] = { 0x40, 0x03, 0x00 };
    const BYTE autocenterEnable[] = { 0x40, 0x04, 0x01 };
    const BYTE enable[] = { 0x42, 0x04 };
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
    const BYTE play[] = { 0x41, 0x00, 0x41, 0x7F };
    const BYTE stop[] = { 0x41, 0x00, 0x00, 0x00 };
    const BYTE disable[] = { 0x42, 0x01 };
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

    if (!SendPacket(device, autocenterMagnitude, sizeof(autocenterMagnitude)) ||
        !SendPacket(device, autocenterEnable, sizeof(autocenterEnable)) ||
        !SendPacket(device, enable, sizeof(enable)) ||
        !SendPacket(device, gain, sizeof(gain)) ||
        !SendPacket(device, magnitude, sizeof(magnitude)) ||
        !SendPacket(device, envelope, sizeof(envelope)) ||
        !SendPacket(device, effect, sizeof(effect)) ||
        !SendPacket(device, play, sizeof(play))) {
        SendPacket(device, disable, sizeof(disable));
        return false;
    }

    wprintf(L"Effect uploaded and playing at max strength for 3 seconds...\n");
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
        Sleep(100);
    }
    bool stopped = SendPacket(device, stop, sizeof(stop));
    bool disabled = SendPacket(device, disable, sizeof(disable));
    return stopped && disabled;
}

static bool RunAutocenterTest(HANDLE device)
{
    // Simplest possible test: enable FFB and crank the autocenter spring to
    // max. No effect memory / core-effect upload involved at all -- if the
    // stick doesn't resist being pushed off-center, the output pipe itself
    // (or the device's FFB engine) isn't responding to any command.
    const BYTE enable[] = { 0x42, 0x04 };
    const BYTE gain[] = { 0x43, 0x7F };
    const BYTE autocenterMagnitude[] = { 0x40, 0x03, 0x7F };
    const BYTE autocenterEnable[] = { 0x40, 0x04, 0x01 };
    const BYTE autocenterOff[] = { 0x40, 0x03, 0x00 };
    const BYTE disable[] = { 0x42, 0x01 };

    if (!SendPacket(device, enable, sizeof(enable)) ||
        !SendPacket(device, gain, sizeof(gain)) ||
        !SendPacket(device, autocenterMagnitude, sizeof(autocenterMagnitude)) ||
        !SendPacket(device, autocenterEnable, sizeof(autocenterEnable))) {
        SendPacket(device, disable, sizeof(disable));
        return false;
    }

    wprintf(L"Autocenter spring at max strength for 8 seconds. Slowly move the "
            L"stick through center in different directions and feel for any "
            L"resistance/stiffness...\n");
    Sleep(8000);

    bool off = SendPacket(device, autocenterOff, sizeof(autocenterOff));
    bool disabled = SendPacket(device, disable, sizeof(disable));
    return off && disabled;
}

int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    HANDLE device = FindJua9();
    if (device == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"J-UA9 interface not found. Is the test driver started?\n");
        return 1;
    }

    if (argc > 1 && _wcsicmp(argv[1], L"--rumble") == 0) {
        wprintf(L"Running a max-strength 3 second rumble test.\n");
        bool success = RunRumbleTest(device);
        CloseHandle(device);
        return success ? 0 : 1;
    }

    if (argc > 1 && _wcsicmp(argv[1], L"--autocenter") == 0) {
        bool success = RunAutocenterTest(device);
        CloseHandle(device);
        return success ? 0 : 1;
    }

    HMODULE vjoy = LoadLibraryW(L"C:\\Program Files\\vJoy\\x64\\vJoyInterface.dll");
    if (vjoy == nullptr) {
        fwprintf(stderr, L"vJoyInterface.dll was not found.\n");
        CloseHandle(device);
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
        return 1;
    }

    reset(vjoyId);
    wprintf(L"vJoy axes: X=%s Y=%s Slider=%s\n",
            axisExists(vjoyId, 0x30) ? L"yes" : L"no",
            axisExists(vjoyId, 0x31) ? L"yes" : L"no",
            axisExists(vjoyId, 0x36) ? L"yes" : L"no");
    wprintf(L"J-UA9 bridge running on vJoy device 1. Press Ctrl+C to stop.\n");

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

    for (;;) {
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
    return 0;
}
