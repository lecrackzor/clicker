#include "engine.h"

#include <shellapi.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr int ID_ACTION = 100;
constexpr int ID_KEY_MODE = 101;
constexpr int ID_SET_KEY = 102;
constexpr int ID_TYPE = 103;
constexpr int ID_UNTIL = 104;
constexpr int ID_COUNT = 105;
constexpr int ID_REPEAT_N = 106;
constexpr int ID_HH = 107;
constexpr int ID_MM = 108;
constexpr int ID_SS = 109;
constexpr int ID_MS = 110;
constexpr int ID_HOLD_HH = 141;
constexpr int ID_HOLD_MM = 142;
constexpr int ID_HOLD_SS = 143;
constexpr int ID_HOLD_MS = 144;
constexpr int ID_WARN = 112;
constexpr int ID_HOTMODE = 113;
constexpr int ID_HOTKEY = 114;
constexpr int ID_PANIC = 115;
constexpr int ID_FIXED = 116;
constexpr int ID_PICK = 117;
constexpr int ID_POS = 118;
constexpr int ID_START = 119;
constexpr int ID_STOP = 120;
constexpr int ID_STATUS = 121;
constexpr int ID_NOTE = 122;
constexpr int ID_LOCK = 123;
constexpr int ID_LOCK_PICK = 124;
constexpr int ID_BTN_LEFT = 130;
constexpr int ID_BTN_MIDDLE = 131;
constexpr int ID_BTN_RIGHT = 132;
constexpr int ID_TYPE_SINGLE = 135;
constexpr int ID_TYPE_DOUBLE = 136;
constexpr int ID_TYPE_HOLD = 137;
constexpr int ID_TYPE_TIMED = 140;
constexpr int ID_HOT_PRESS = 138;
constexpr int ID_HOT_TOGGLE = 139;

constexpr UINT WM_CLICKER_STATUS = WM_APP + 1;
constexpr UINT WM_CLICKER_FOCUS = WM_APP + 2;
constexpr UINT_PTR kPickTimer = 1;
constexpr UINT_PTR kCaptureTimer = 2;
constexpr UINT_PTR kLockTimer = 3;

enum class CaptureTarget { None, Action, Hotkey, Panic };

struct StatusMsg {
    unsigned long long clicks;
    double cps;
    int state;  // 0 stopped, 1 running, 2 holding, 3 panic, 4 conflict
};

struct Keys {
    bool hotWas = false;
    bool panicWas = false;
};

struct KeySample {
    bool hotEdge = false;
    bool panicEdge = false;
    bool hotDown = false;
};

struct PollCtx {
    Keys* keys = nullptr;
    bool latched = false;
    HotkeyMode mode = HotkeyMode::Toggle;
};

std::mutex g_mu;
Config g_cfg;
std::wstring g_ini;
std::atomic<HWND> g_hwnd{nullptr};
std::atomic<bool> g_quit{false};
std::atomic<bool> g_uiStart{false};
std::atomic<bool> g_uiStop{false};
std::atomic<bool> g_suspendHotkeys{false};
std::atomic<unsigned> g_hotVk{VK_F6};
std::atomic<unsigned> g_panicVk{VK_F12};
std::atomic<int> g_hotMode{0};
std::thread g_worker;

HWND g_btnLeft = nullptr;
HWND g_btnMiddle = nullptr;
HWND g_btnRight = nullptr;
HWND g_keyMode = nullptr;
HWND g_setKey = nullptr;
HWND g_typeSingle = nullptr;
HWND g_typeDouble = nullptr;
HWND g_typeHold = nullptr;
HWND g_typeTimed = nullptr;
HWND g_holdHh = nullptr;
HWND g_holdMm = nullptr;
HWND g_holdSs = nullptr;
HWND g_holdMs = nullptr;
HWND g_repeatN = nullptr;
HWND g_hh = nullptr;
HWND g_mm = nullptr;
HWND g_ss = nullptr;
HWND g_ms = nullptr;
HWND g_warn = nullptr;
HWND g_hotPress = nullptr;
HWND g_hotToggle = nullptr;
HWND g_hotkeyBtn = nullptr;
HWND g_panicBtn = nullptr;
HWND g_fixed = nullptr;
HWND g_pick = nullptr;
HWND g_lock = nullptr;
HWND g_lockPick = nullptr;
HWND g_start = nullptr;
HWND g_stop = nullptr;
HWND g_status = nullptr;
HFONT g_font = nullptr;
bool g_loading = false;
CaptureTarget g_capture = CaptureTarget::None;
int g_latchedVk = 0;
bool g_captureDown[256] = {};
bool g_pickActive = false;
bool g_pickWaitRelease = false;
bool g_lockPickActive = false;
bool g_lockWaitRelease = false;

void PostState(int state, unsigned long long clicks, double cps) {
    HWND hwnd = g_hwnd.load();
    if (!hwnd) {
        return;
    }
    auto* msg = new StatusMsg{clicks, cps, state};
    if (!PostMessageW(hwnd, WM_CLICKER_STATUS, 0, reinterpret_cast<LPARAM>(msg))) {
        delete msg;
    }
}

KeySample SampleKeys(Keys& keys) {
    SHORT hs = GetAsyncKeyState(static_cast<int>(g_hotVk.load()));
    SHORT ps = GetAsyncKeyState(static_cast<int>(g_panicVk.load()));
    bool hotDown = (hs & 0x8000) != 0;
    bool panicDown = (ps & 0x8000) != 0;
    KeySample sample;
    // Low bit catches a tap that went down and up between polls.
    sample.hotEdge = (hs & 1) != 0 || (hotDown && !keys.hotWas);
    sample.panicEdge = (ps & 1) != 0 || (panicDown && !keys.panicWas);
    sample.hotDown = hotDown;
    keys.hotWas = hotDown;
    keys.panicWas = panicDown;
    return sample;
}

int SessionPoll(void* ctx) {
    auto* poll = static_cast<PollCtx*>(ctx);
    if (g_quit.load()) {
        return 1;
    }
    KeySample sample = SampleKeys(*poll->keys);
    if (sample.panicEdge) {
        return 2;
    }
    if (g_uiStop.exchange(false)) {
        return 1;
    }
    if (poll->mode == HotkeyMode::Toggle && sample.hotEdge) {
        return 1;
    }
    if (poll->mode == HotkeyMode::Hold && !sample.hotDown && !poll->latched) {
        return 1;
    }
    return 0;
}

void WorkerMain() {
    Keys keys;
    keys.hotWas = (GetAsyncKeyState(static_cast<int>(g_hotVk.load())) & 0x8000) != 0;
    keys.panicWas = (GetAsyncKeyState(static_cast<int>(g_panicVk.load())) & 0x8000) != 0;

    while (!g_quit.load()) {
        bool start = false;
        bool latched = false;
        if (g_uiStart.exchange(false)) {
            start = true;
            latched = true;
        }
        KeySample sample = SampleKeys(keys);
        if (!g_suspendHotkeys.load()) {
            int mode = g_hotMode.load();
            if (!start && sample.hotEdge && (mode == 0 || mode == 1)) {
                start = true;
                latched = false;
            }
        }
        if (!start) {
            g_uiStop.exchange(false);
            Sleep(8);
            continue;
        }

        Config cfg;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            cfg = g_cfg;
        }
        if (ActionConflictsWithHotkeys(cfg)) {
            PostState(4, 0, 0);
            continue;
        }
        if (cfg.lockWindow && !IsWindow(cfg.lockHwnd)) {
            PostState(5, 0, 0);
            continue;
        }
        // A click on Start is still held for a moment. Wait until that physical
        // button is up so it is not merged with the first injected click.
        if (latched && cfg.action == ActionKind::Mouse) {
            for (int i = 0; i < 100 && !g_quit.load() && !g_uiStop.load(); ++i) {
                if ((GetAsyncKeyState(static_cast<int>(g_panicVk.load())) & 0x8000) != 0) {
                    g_uiStop.store(true);
                    break;
                }
                bool physicalDown =
                    (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
                    (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ||
                    (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0 ||
                    (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0 ||
                    (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0;
                if (!physicalDown) {
                    break;
                }
                Sleep(5);
            }
        }

        PollCtx poll;
        poll.keys = &keys;
        poll.latched = latched;
        poll.mode = cfg.hotkeyMode;

        RunControl ctl{};
        ctl.poll = SessionPoll;
        ctl.onStatus = [](void*, unsigned long long clicks, double cps, bool holding) {
            PostState(holding ? 2 : 1, clicks, cps);
        };
        ctl.onEvent = nullptr;
        ctl.ctx = &poll;
        ctl.inject = true;

        RunStats stats = RunSession(cfg, ctl);
        g_uiStart.exchange(false);
        PostState(stats.reason == StopReason::Panic ? 3 : 0, stats.clicks, 0);
    }
}

std::wstring SettingsPath() {
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    std::wstring dir = (n == 0 || n >= MAX_PATH) ? L"." : std::wstring(base) + L"\\Clicker";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

void WriteInt(const wchar_t* key, int value) {
    wchar_t buf[32];
    swprintf_s(buf, L"%d", value);
    WritePrivateProfileStringW(L"clicker", key, buf, g_ini.c_str());
}

int ReadEdit(HWND edit, int fallback, int minValue, int maxValue) {
    wchar_t buf[16] = {};
    GetWindowTextW(edit, buf, 16);
    if (buf[0] == 0) {
        return fallback;
    }
    int value = _wtoi(buf);
    if (value < minValue) {
        value = minValue;
    }
    if (value > maxValue) {
        value = maxValue;
    }
    return value;
}

void SetEditInt(HWND edit, int value) {
    wchar_t buf[16];
    swprintf_s(buf, L"%d", value);
    SetWindowTextW(edit, buf);
}

Config ReadUi() {
    Config cfg = g_cfg;
    if (SendMessageW(g_keyMode, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        cfg.action = ActionKind::Key;
    } else {
        cfg.action = ActionKind::Mouse;
        if (SendMessageW(g_btnRight, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            cfg.mouseButton = 1;
        } else if (SendMessageW(g_btnMiddle, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            cfg.mouseButton = 2;
        } else {
            cfg.mouseButton = 0;
        }
    }
    if (SendMessageW(g_typeDouble, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        cfg.clickType = ClickType::Double;
    } else if (SendMessageW(g_typeHold, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        cfg.clickType = ClickType::Hold;
    } else if (SendMessageW(g_typeTimed, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        cfg.clickType = ClickType::TimedHold;
    } else {
        cfg.clickType = ClickType::Single;
    }
    int repeat = ReadEdit(g_repeatN, 0, 0, 1000000);
    if (repeat <= 0) {
        cfg.repeatMode = RepeatMode::UntilStopped;
        cfg.repeatCount = 1;
    } else {
        cfg.repeatMode = RepeatMode::Count;
        cfg.repeatCount = repeat;
    }
    int hours = ReadEdit(g_hh, 0, 0, 999);
    int minutes = ReadEdit(g_mm, 0, 0, 999);
    int seconds = ReadEdit(g_ss, 0, 0, 999);
    int millis = ReadEdit(g_ms, 0, 0, 999);
    cfg.intervalMs = IntervalMsFromParts(hours, minutes, seconds, millis);
    cfg.timedHoldMs = IntervalMsFromParts(ReadEdit(g_holdHh, 0, 0, 999), ReadEdit(g_holdMm, 0, 0, 999),
                                          ReadEdit(g_holdSs, 0, 0, 999), ReadEdit(g_holdMs, 0, 0, 999));
    if (cfg.timedHoldMs < 1) {
        cfg.timedHoldMs = 1;
    }
    cfg.hotkeyMode = SendMessageW(g_hotPress, BM_GETCHECK, 0, 0) == BST_CHECKED
                         ? HotkeyMode::Hold
                         : HotkeyMode::Toggle;
    cfg.fixedPos = SendMessageW(g_fixed, BM_GETCHECK, 0, 0) == BST_CHECKED;
    cfg.lockWindow = SendMessageW(g_lock, BM_GETCHECK, 0, 0) == BST_CHECKED;
    return cfg;
}

void UpdateWarning(const Config& cfg) {
    if (cfg.clickType == ClickType::Hold || cfg.clickType == ClickType::TimedHold) {
        SetWindowTextW(g_warn, L"");
        return;
    }
    int64_t effective = EffectivePeriodMs(cfg);
    if (cfg.intervalMs < effective) {
        wchar_t buf[160];
        swprintf_s(buf, L"Interval shorter than the click; using %lld ms.",
                   static_cast<long long>(effective));
        SetWindowTextW(g_warn, buf);
    } else {
        SetWindowTextW(g_warn, L"");
    }
}

void UpdateKeyLabel() {
    wchar_t name[64];
    KeyName(g_cfg.actionVk, name, 64);
    SetWindowTextW(g_setKey, name);
}

void UpdateInputMode(bool editable) {
    bool key = SendMessageW(g_keyMode, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(g_setKey, editable && key ? TRUE : FALSE);
}

void UpdateHotkeyButtons() {
    wchar_t name[64];
    KeyName(g_cfg.hotkeyVk, name, 64);
    SetWindowTextW(g_hotkeyBtn, name);
    KeyName(g_cfg.panicVk, name, 64);
    SetWindowTextW(g_panicBtn, name);
}

void UpdateLockLabel() {
    wchar_t title[64];
    if (!g_cfg.lockHwnd || !IsWindow(g_cfg.lockHwnd) ||
        GetWindowTextW(g_cfg.lockHwnd, title, 64) == 0 || title[0] == 0) {
        SetWindowTextW(g_lockPick, L"Choose window");
        return;
    }
    if (wcslen(title) > 16) {
        title[15] = L'.';
        title[16] = 0;
    }
    SetWindowTextW(g_lockPick, title);
}

void UpdatePositionLabel() {
    if (g_cfg.fixedX == 0 && g_cfg.fixedY == 0 && !g_cfg.fixedPos) {
        SetWindowTextW(g_pick, L"Choose spot");
        return;
    }
    wchar_t buf[64];
    swprintf_s(buf, L"%d,%d", g_cfg.fixedX, g_cfg.fixedY);
    SetWindowTextW(g_pick, buf);
}

void ApplyUi(const Config& cfg) {
    g_loading = true;
    bool key = cfg.action == ActionKind::Key;
    SendMessageW(g_btnLeft, BM_SETCHECK, !key && cfg.mouseButton == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_btnMiddle, BM_SETCHECK, !key && cfg.mouseButton == 2 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_btnRight, BM_SETCHECK, !key && cfg.mouseButton == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_keyMode, BM_SETCHECK, key ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_typeSingle, BM_SETCHECK, cfg.clickType == ClickType::Single ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_typeDouble, BM_SETCHECK, cfg.clickType == ClickType::Double ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_typeHold, BM_SETCHECK, cfg.clickType == ClickType::Hold ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_typeTimed, BM_SETCHECK, cfg.clickType == ClickType::TimedHold ? BST_CHECKED : BST_UNCHECKED, 0);
    SetEditInt(g_repeatN, cfg.repeatMode == RepeatMode::Count ? cfg.repeatCount : 0);
    int hours = 0, minutes = 0, seconds = 0, millis = 0;
    SplitInterval(cfg.intervalMs, hours, minutes, seconds, millis);
    SetEditInt(g_hh, hours);
    SetEditInt(g_mm, minutes);
    SetEditInt(g_ss, seconds);
    SetEditInt(g_ms, millis);
    int holdHours = 0, holdMinutes = 0, holdSeconds = 0, holdMillis = 0;
    SplitInterval(cfg.timedHoldMs, holdHours, holdMinutes, holdSeconds, holdMillis);
    SetEditInt(g_holdHh, holdHours);
    SetEditInt(g_holdMm, holdMinutes);
    SetEditInt(g_holdSs, holdSeconds);
    SetEditInt(g_holdMs, holdMillis);
    SendMessageW(g_hotPress, BM_SETCHECK, cfg.hotkeyMode == HotkeyMode::Hold ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_hotToggle, BM_SETCHECK, cfg.hotkeyMode == HotkeyMode::Toggle ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_fixed, BM_SETCHECK, cfg.fixedPos ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_lock, BM_SETCHECK, cfg.lockWindow ? BST_CHECKED : BST_UNCHECKED, 0);
    UpdateLockLabel();
    UpdateKeyLabel();
    UpdateHotkeyButtons();
    UpdatePositionLabel();
    UpdateWarning(cfg);
    UpdateInputMode(true);
    g_loading = false;
}

void SaveIni(const Config& cfg) {
    int hours = 0, minutes = 0, seconds = 0, millis = 0;
    SplitInterval(cfg.intervalMs, hours, minutes, seconds, millis);
    WriteInt(L"action", cfg.action == ActionKind::Key ? 5 : cfg.mouseButton);
    WriteInt(L"action_vk", cfg.actionVk);
    int clickType = 0;
    if (cfg.clickType == ClickType::Double) {
        clickType = 1;
    } else if (cfg.clickType == ClickType::Hold) {
        clickType = 2;
    } else if (cfg.clickType == ClickType::TimedHold) {
        clickType = 3;
    }
    WriteInt(L"click_type", clickType);
    WriteInt(L"repeat_mode", cfg.repeatMode == RepeatMode::Count ? 1 : 0);
    WriteInt(L"repeat_count", cfg.repeatCount);
    WriteInt(L"hours", hours);
    WriteInt(L"minutes", minutes);
    WriteInt(L"seconds", seconds);
    WriteInt(L"millis", millis);
    int holdHours = 0, holdMinutes = 0, holdSeconds = 0, holdMillis = 0;
    SplitInterval(cfg.timedHoldMs, holdHours, holdMinutes, holdSeconds, holdMillis);
    WriteInt(L"hold_for_hours", holdHours);
    WriteInt(L"hold_for_minutes", holdMinutes);
    WriteInt(L"hold_for_seconds", holdSeconds);
    WriteInt(L"hold_for_millis", holdMillis);
    WriteInt(L"hold_ms", cfg.holdMs);
    WriteInt(L"hotkey_mode", cfg.hotkeyMode == HotkeyMode::Hold ? 1 : 0);
    WriteInt(L"hotkey_vk", cfg.hotkeyVk);
    WriteInt(L"panic_vk", cfg.panicVk);
    WriteInt(L"fixed", cfg.fixedPos ? 1 : 0);
    WriteInt(L"fixed_x", cfg.fixedX);
    WriteInt(L"fixed_y", cfg.fixedY);
    WriteInt(L"lock_window", cfg.lockWindow ? 1 : 0);
}

void CommitFromUi() {
    if (g_loading) {
        return;
    }
    Config cfg = ReadUi();
    cfg.actionVk = g_cfg.actionVk;
    cfg.hotkeyVk = g_cfg.hotkeyVk;
    cfg.panicVk = g_cfg.panicVk;
    cfg.fixedX = g_cfg.fixedX;
    cfg.fixedY = g_cfg.fixedY;
    cfg.lockHwnd = g_cfg.lockHwnd;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_cfg = cfg;
    }
    g_hotVk.store(cfg.hotkeyVk);
    g_panicVk.store(cfg.panicVk);
    g_hotMode.store(cfg.hotkeyMode == HotkeyMode::Hold ? 1 : 0);
    SaveIni(cfg);
    UpdateWarning(cfg);
}

void LoadIni() {
    g_ini = SettingsPath();
    Config cfg;
    int action = GetPrivateProfileIntW(L"clicker", L"action", 0, g_ini.c_str());
    if (action >= 5) {
        cfg.action = ActionKind::Key;
        cfg.mouseButton = 0;
    } else {
        cfg.action = ActionKind::Mouse;
        cfg.mouseButton = action;
    }
    cfg.actionVk = static_cast<WORD>(GetPrivateProfileIntW(L"clicker", L"action_vk", VK_SPACE, g_ini.c_str()));
    if (cfg.action == ActionKind::Mouse && cfg.mouseButton == 3) {
        cfg.action = ActionKind::Key;
        cfg.actionVk = VK_XBUTTON1;
        cfg.mouseButton = 0;
    } else if (cfg.action == ActionKind::Mouse && cfg.mouseButton == 4) {
        cfg.action = ActionKind::Key;
        cfg.actionVk = VK_XBUTTON2;
        cfg.mouseButton = 0;
    } else if (cfg.mouseButton < 0 || cfg.mouseButton > 2) {
        cfg.mouseButton = 0;
    }
    int type = GetPrivateProfileIntW(L"clicker", L"click_type", 0, g_ini.c_str());
    if (type == 1) cfg.clickType = ClickType::Double;
    else if (type == 2) cfg.clickType = ClickType::Hold;
    else if (type == 3) cfg.clickType = ClickType::TimedHold;
    else cfg.clickType = ClickType::Single;
    cfg.repeatMode = GetPrivateProfileIntW(L"clicker", L"repeat_mode", 0, g_ini.c_str()) == 1
                         ? RepeatMode::Count
                         : RepeatMode::UntilStopped;
    cfg.repeatCount = GetPrivateProfileIntW(L"clicker", L"repeat_count", 10, g_ini.c_str());
    if (cfg.repeatCount < 1) cfg.repeatCount = 1;
    int hours = GetPrivateProfileIntW(L"clicker", L"hours", 0, g_ini.c_str());
    int minutes = GetPrivateProfileIntW(L"clicker", L"minutes", 0, g_ini.c_str());
    int seconds = GetPrivateProfileIntW(L"clicker", L"seconds", 0, g_ini.c_str());
    int millis = GetPrivateProfileIntW(L"clicker", L"millis", 100, g_ini.c_str());
    cfg.intervalMs = IntervalMsFromParts(hours, minutes, seconds, millis);
    cfg.holdMs = GetPrivateProfileIntW(L"clicker", L"hold_ms", 20, g_ini.c_str());
    if (cfg.holdMs < 1) cfg.holdMs = 1;
    wchar_t holdProbe[8] = {};
    DWORD holdProbeLen = GetPrivateProfileStringW(L"clicker", L"hold_for_millis", L"", holdProbe, 8, g_ini.c_str());
    if (holdProbeLen == 0) {
        cfg.timedHoldMs = 1000;
    } else {
        int holdHours = GetPrivateProfileIntW(L"clicker", L"hold_for_hours", 0, g_ini.c_str());
        int holdMinutes = GetPrivateProfileIntW(L"clicker", L"hold_for_minutes", 0, g_ini.c_str());
        int holdSeconds = GetPrivateProfileIntW(L"clicker", L"hold_for_seconds", 0, g_ini.c_str());
        int holdMillis = GetPrivateProfileIntW(L"clicker", L"hold_for_millis", 0, g_ini.c_str());
        cfg.timedHoldMs = IntervalMsFromParts(holdHours, holdMinutes, holdSeconds, holdMillis);
        if (cfg.timedHoldMs < 1) cfg.timedHoldMs = 1;
    }
    cfg.hotkeyMode = GetPrivateProfileIntW(L"clicker", L"hotkey_mode", 0, g_ini.c_str()) == 1
                         ? HotkeyMode::Hold
                         : HotkeyMode::Toggle;
    cfg.hotkeyVk = static_cast<WORD>(GetPrivateProfileIntW(L"clicker", L"hotkey_vk", VK_F6, g_ini.c_str()));
    cfg.panicVk = static_cast<WORD>(GetPrivateProfileIntW(L"clicker", L"panic_vk", VK_F12, g_ini.c_str()));
    cfg.fixedPos = GetPrivateProfileIntW(L"clicker", L"fixed", 0, g_ini.c_str()) != 0;
    cfg.lockWindow = GetPrivateProfileIntW(L"clicker", L"lock_window", 0, g_ini.c_str()) != 0;
    cfg.fixedX = GetPrivateProfileIntW(L"clicker", L"fixed_x", 0, g_ini.c_str());
    cfg.fixedY = GetPrivateProfileIntW(L"clicker", L"fixed_y", 0, g_ini.c_str());
    g_cfg = cfg;
    g_hotVk.store(cfg.hotkeyVk);
    g_panicVk.store(cfg.panicVk);
    g_hotMode.store(cfg.hotkeyMode == HotkeyMode::Hold ? 1 : 0);
}

HMENU MenuId(int id) {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

HWND MakeLabel(HWND parent, HINSTANCE inst, const wchar_t* text, int x, int y, int w, int h, int id = 0) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_EDITCONTROL | SS_NOPREFIX,
                           x, y, w, h, parent, MenuId(id), inst, nullptr);
}

HWND MakeEdit(HWND parent, HINSTANCE inst, int x, int y, int w, int id) {
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_CENTER,
                                x, y, w, 18, parent, MenuId(id), inst, nullptr);
    SendMessageW(edit, EM_SETLIMITTEXT, 3, 0);
    return edit;
}

void SetRunningUi(bool running) {
    EnableWindow(g_start, running ? FALSE : TRUE);
    EnableWindow(g_stop, running ? TRUE : FALSE);
    const BOOL edit = running ? FALSE : TRUE;
    EnableWindow(g_btnLeft, edit);
    EnableWindow(g_btnMiddle, edit);
    EnableWindow(g_btnRight, edit);
    EnableWindow(g_keyMode, edit);
    UpdateInputMode(edit == TRUE);
    EnableWindow(g_typeSingle, edit);
    EnableWindow(g_typeDouble, edit);
    EnableWindow(g_typeHold, edit);
    EnableWindow(g_typeTimed, edit);
    EnableWindow(g_holdHh, edit);
    EnableWindow(g_holdMm, edit);
    EnableWindow(g_holdSs, edit);
    EnableWindow(g_holdMs, edit);
    EnableWindow(g_repeatN, edit);
    EnableWindow(g_hh, edit);
    EnableWindow(g_mm, edit);
    EnableWindow(g_ss, edit);
    EnableWindow(g_ms, edit);
    EnableWindow(g_hotPress, edit);
    EnableWindow(g_hotToggle, edit);
    EnableWindow(g_hotkeyBtn, edit);
    EnableWindow(g_panicBtn, edit);
    EnableWindow(g_fixed, edit);
    EnableWindow(g_pick, edit);
    EnableWindow(g_lock, edit);
    EnableWindow(g_lockPick, edit);
}

void ShowStatus(const StatusMsg& msg) {
    wchar_t text[160];
    if (msg.state == 4) {
        wcscpy_s(text, L"Action key matches the hotkey or panic key");
        SetRunningUi(false);
    } else if (msg.state == 5) {
        wcscpy_s(text, L"Choose a window to lock");
        SetRunningUi(false);
    } else if (msg.state == 2) {
        wcscpy_s(text, L"Holding - release with hotkey, Stop, or panic");
        SetRunningUi(true);
    } else if (msg.state == 1) {
        swprintf_s(text, L"Running - %llu clicks - %.1f cps", msg.clicks, msg.cps);
        SetRunningUi(true);
    } else if (msg.state == 3) {
        swprintf_s(text, L"Panic - %llu clicks", msg.clicks);
        SetRunningUi(false);
    } else if (msg.clicks > 0) {
        swprintf_s(text, L"Stopped - %llu clicks", msg.clicks);
        SetRunningUi(false);
    } else {
        wcscpy_s(text, L"status: idle");
        SetRunningUi(false);
    }
    SetWindowTextW(g_status, text);
}

void EndCapture(HWND hwnd) {
    g_capture = CaptureTarget::None;
    g_latchedVk = 0;
    g_suspendHotkeys.store(false);
    KillTimer(hwnd, kCaptureTimer);
    UpdateKeyLabel();
    UpdateHotkeyButtons();
}

bool CaptureWants(int vk) {
    if (vk < VK_XBUTTON1 || vk > 255) {
        return false;
    }
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) {
        return false;
    }
    if (g_capture != CaptureTarget::Action && (vk == VK_XBUTTON1 || vk == VK_XBUTTON2)) {
        return false;
    }
    return true;
}

void ShowLatchedKey(int vk) {
    wchar_t name[64];
    KeyName(static_cast<WORD>(vk), name, 64);
    HWND button = g_setKey;
    if (g_capture == CaptureTarget::Hotkey) {
        button = g_hotkeyBtn;
    } else if (g_capture == CaptureTarget::Panic) {
        button = g_panicBtn;
    }
    SetWindowTextW(button, name);
}

// Remember the key on the way down. Saving it while it is still held makes a
// focused button treat the release as another click, which restarts capture.
void LatchCapture(HWND hwnd, int vk) {
    if (g_capture == CaptureTarget::None) {
        return;
    }
    if (vk == VK_ESCAPE) {
        EndCapture(hwnd);
        return;
    }
    if (g_latchedVk != 0 || !CaptureWants(vk)) {
        return;
    }
    g_latchedVk = vk;
    ShowLatchedKey(vk);
}

void AcceptCapture(HWND hwnd, int vk) {
    if (g_capture == CaptureTarget::None || !CaptureWants(vk)) {
        return;
    }
    if (vk == VK_ESCAPE) {
        EndCapture(hwnd);
        return;
    }
    WORD key = static_cast<WORD>(vk);
    if (g_capture == CaptureTarget::Action) {
        g_cfg.actionVk = key;
        g_loading = true;
        SendMessageW(g_keyMode, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(g_btnLeft, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(g_btnMiddle, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(g_btnRight, BM_SETCHECK, BST_UNCHECKED, 0);
        g_loading = false;
    } else if (g_capture == CaptureTarget::Hotkey) {
        g_cfg.hotkeyVk = key;
    } else if (g_capture == CaptureTarget::Panic) {
        g_cfg.panicVk = key;
    }
    EndCapture(hwnd);
    CommitFromUi();
}

void BeginCapture(HWND hwnd, CaptureTarget target) {
    g_capture = target;
    g_latchedVk = 0;
    g_suspendHotkeys.store(true);
    // The button click that opened capture sets focus back after we return.
    PostMessageW(hwnd, WM_CLICKER_FOCUS, 0, 0);
    for (int vk = 0; vk < 256; ++vk) {
        g_captureDown[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
    if (target == CaptureTarget::Action) {
        SetWindowTextW(g_setKey, L"Press...");
    } else if (target == CaptureTarget::Hotkey) {
        SetWindowTextW(g_hotkeyBtn, L"Press...");
    } else if (target == CaptureTarget::Panic) {
        SetWindowTextW(g_panicBtn, L"Press...");
    }
    SetTimer(hwnd, kCaptureTimer, 10, nullptr);
}

void PollCapture(HWND hwnd) {
    if (g_capture == CaptureTarget::None) {
        return;
    }
    if (g_latchedVk != 0) {
        bool held = (GetAsyncKeyState(g_latchedVk) & 0x8000) != 0;
        if (!held) {
            AcceptCapture(hwnd, g_latchedVk);
        }
        return;
    }
    for (int vk = VK_XBUTTON1; vk < 256; ++vk) {
        if (!CaptureWants(vk)) {
            continue;
        }
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (g_captureDown[vk]) {
            if (!down) {
                g_captureDown[vk] = false;
            }
            continue;
        }
        if (down) {
            LatchCapture(hwnd, vk);
            return;
        }
    }
}

void PollPick(HWND hwnd) {
    if (!g_pickActive) {
        return;
    }
    bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (g_pickWaitRelease) {
        if (!down) {
            g_pickWaitRelease = false;
        }
        return;
    }
    if (!down) {
        return;
    }
    POINT point;
    GetCursorPos(&point);
    g_cfg.fixedX = point.x;
    g_cfg.fixedY = point.y;
    g_cfg.fixedPos = true;
    g_pickActive = false;
    KillTimer(hwnd, kPickTimer);
    g_loading = true;
    SendMessageW(g_fixed, BM_SETCHECK, BST_CHECKED, 0);
    g_loading = false;
    UpdatePositionLabel();
    CommitFromUi();
    SetWindowTextW(g_status, L"Position set");
}

void PollLock(HWND hwnd) {
    if (!g_lockPickActive) {
        return;
    }
    bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (g_lockWaitRelease) {
        if (!down) {
            g_lockWaitRelease = false;
        }
        return;
    }
    if (!down) {
        return;
    }
    POINT point;
    GetCursorPos(&point);
    HWND under = WindowFromPoint(point);
    HWND root = under ? GetAncestor(under, GA_ROOT) : nullptr;
    if (!root) {
        root = under;
    }
    HWND ours = GetAncestor(hwnd, GA_ROOT);
    g_lockPickActive = false;
    KillTimer(hwnd, kLockTimer);
    if (!root || root == ours || root == hwnd) {
        UpdateLockLabel();
        SetWindowTextW(g_status, L"Window pick cancelled");
        return;
    }
    g_cfg.lockHwnd = root;
    g_cfg.lockWindow = true;
    g_loading = true;
    SendMessageW(g_lock, BM_SETCHECK, BST_CHECKED, 0);
    g_loading = false;
    UpdateLockLabel();
    CommitFromUi();
    SetWindowTextW(g_status, L"Window locked");
}

BOOL CALLBACK SetChildFont(HWND child, LPARAM font) {
    SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
    return TRUE;
}

WNDPROC g_buttonProc = nullptr;
WNDPROC g_editProc = nullptr;

bool IsInjectedKey() {
    return GetMessageExtraInfo() == static_cast<LPARAM>(kInjectedExtra);
}

LRESULT CALLBACK ButtonProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (IsInjectedKey() && (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR ||
                            msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)) {
        return 0;
    }
    // Space on a focused button is a click. While choosing a key, keep that
    // press for the chooser and wait until it is released.
    if (g_capture != CaptureTarget::None &&
        (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)) {
        return SendMessageW(GetParent(hwnd), msg, wParam, lParam);
    }
    return CallWindowProcW(g_buttonProc, hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (IsInjectedKey() && (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR ||
                            msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)) {
        return 0;
    }
    return CallWindowProcW(g_editProc, hwnd, msg, wParam, lParam);
}

BOOL CALLBACK SubclassInputChild(HWND child, LPARAM) {
    wchar_t cls[16];
    if (GetClassNameW(child, cls, 16) == 0) {
        return TRUE;
    }
    if (wcscmp(cls, L"Button") == 0) {
        if (!g_buttonProc) {
            g_buttonProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(child, GWLP_WNDPROC));
        }
        SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ButtonProc));
    } else if (wcscmp(cls, L"Edit") == 0) {
        if (!g_editProc) {
            g_editProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(child, GWLP_WNDPROC));
        }
        SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc));
    }
    return TRUE;
}

HWND MakeGroup(HWND hwnd, HINSTANCE inst, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"BUTTON", text,
                           WS_CHILD | WS_VISIBLE | BS_GROUPBOX | WS_CLIPSIBLINGS,
                           x, y, w, h, hwnd, nullptr, inst, nullptr);
}

HWND MakeRadio(HWND hwnd, HINSTANCE inst, const wchar_t* text, int x, int y, int w, int id, bool group) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON;
    if (group) {
        style |= WS_GROUP;
    }
    return CreateWindowExW(0, L"BUTTON", text, style, x, y, w, 16, hwnd, MenuId(id), inst, nullptr);
}

void CreateControls(HWND hwnd, HINSTANCE inst) {
    g_status = MakeLabel(hwnd, inst, L"status: idle", 8, 4, 188, 28, ID_STATUS);

    MakeGroup(hwnd, inst, L"HH:MM:SS:ms", 6, 34, 192, 44);
    g_hh = MakeEdit(hwnd, inst, 14, 52, 32, ID_HH);
    MakeLabel(hwnd, inst, L":", 48, 54, 8, 16);
    g_mm = MakeEdit(hwnd, inst, 56, 52, 32, ID_MM);
    MakeLabel(hwnd, inst, L":", 90, 54, 8, 16);
    g_ss = MakeEdit(hwnd, inst, 98, 52, 32, ID_SS);
    MakeLabel(hwnd, inst, L":", 132, 54, 8, 16);
    g_ms = MakeEdit(hwnd, inst, 140, 52, 36, ID_MS);

    MakeLabel(hwnd, inst, L"stop after", 8, 86, 62, 16);
    g_repeatN = MakeEdit(hwnd, inst, 72, 84, 40, ID_REPEAT_N);
    MakeLabel(hwnd, inst, L"0 = no limit", 118, 86, 76, 16);
    SendMessageW(g_repeatN, EM_SETLIMITTEXT, 7, 0);

    g_start = CreateWindowExW(0, L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                              8, 110, 188, 24, hwnd, MenuId(ID_START), inst, nullptr);
    g_stop = CreateWindowExW(0, L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                             8, 138, 188, 26, hwnd, MenuId(ID_STOP), inst, nullptr);
    EnableWindow(g_stop, FALSE);

    MakeGroup(hwnd, inst, L"press / toggle", 6, 172, 192, 100);
    g_hotPress = MakeRadio(hwnd, inst, L"press", 18, 190, 168, ID_HOT_PRESS, true);
    g_hotToggle = MakeRadio(hwnd, inst, L"toggle", 18, 208, 168, ID_HOT_TOGGLE, false);
    MakeLabel(hwnd, inst, L"trigger", 18, 230, 50, 16);
    g_hotkeyBtn = CreateWindowExW(0, L"BUTTON", L"F6", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  70, 226, 116, 20, hwnd, MenuId(ID_HOTKEY), inst, nullptr);
    MakeLabel(hwnd, inst, L"panic", 18, 252, 50, 16);
    g_panicBtn = CreateWindowExW(0, L"BUTTON", L"F12", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 70, 248, 116, 20, hwnd, MenuId(ID_PANIC), inst, nullptr);

    MakeGroup(hwnd, inst, L"mouse / key", 6, 280, 192, 118);
    g_btnLeft = MakeRadio(hwnd, inst, L"left", 18, 298, 168, ID_BTN_LEFT, true);
    g_btnMiddle = MakeRadio(hwnd, inst, L"middle", 18, 316, 168, ID_BTN_MIDDLE, false);
    g_btnRight = MakeRadio(hwnd, inst, L"right", 18, 334, 168, ID_BTN_RIGHT, false);
    g_keyMode = MakeRadio(hwnd, inst, L"key", 18, 352, 168, ID_KEY_MODE, false);
    g_setKey = CreateWindowExW(0, L"BUTTON", L"Space", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               32, 370, 150, 20, hwnd, MenuId(ID_SET_KEY), inst, nullptr);

    MakeGroup(hwnd, inst, L"click", 6, 406, 192, 128);
    g_typeSingle = MakeRadio(hwnd, inst, L"single", 18, 424, 168, ID_TYPE_SINGLE, true);
    g_typeDouble = MakeRadio(hwnd, inst, L"double", 18, 442, 168, ID_TYPE_DOUBLE, false);
    g_typeHold = MakeRadio(hwnd, inst, L"until stop", 18, 460, 168, ID_TYPE_HOLD, false);
    g_typeTimed = MakeRadio(hwnd, inst, L"for", 18, 478, 168, ID_TYPE_TIMED, false);
    g_holdHh = MakeEdit(hwnd, inst, 18, 498, 32, ID_HOLD_HH);
    MakeLabel(hwnd, inst, L":", 52, 500, 8, 16);
    g_holdMm = MakeEdit(hwnd, inst, 60, 498, 32, ID_HOLD_MM);
    MakeLabel(hwnd, inst, L":", 94, 500, 8, 16);
    g_holdSs = MakeEdit(hwnd, inst, 102, 498, 32, ID_HOLD_SS);
    MakeLabel(hwnd, inst, L":", 136, 500, 8, 16);
    g_holdMs = MakeEdit(hwnd, inst, 144, 498, 36, ID_HOLD_MS);

    MakeGroup(hwnd, inst, L"target", 6, 542, 192, 112);
    g_fixed = CreateWindowExW(0, L"BUTTON", L"Fixed spot",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTOCHECKBOX,
                              16, 560, 160, 18, hwnd, MenuId(ID_FIXED), inst, nullptr);
    g_pick = CreateWindowExW(0, L"BUTTON", L"Choose spot", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                             28, 580, 156, 20, hwnd, MenuId(ID_PICK), inst, nullptr);
    g_lock = CreateWindowExW(0, L"BUTTON", L"One window",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                             16, 606, 160, 18, hwnd, MenuId(ID_LOCK), inst, nullptr);
    g_lockPick = CreateWindowExW(0, L"BUTTON", L"Choose window", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 28, 626, 156, 20, hwnd, MenuId(ID_LOCK_PICK), inst, nullptr);

    g_warn = MakeLabel(hwnd, inst, L"", 8, 662, 188, 28, ID_WARN);
    EnumChildWindows(hwnd, SubclassInputChild, 0);
}

void OnStart() {
    CommitFromUi();
    if (ActionConflictsWithHotkeys(g_cfg)) {
        SetWindowTextW(g_status, L"Action key matches the hotkey or panic key");
        return;
    }
    if (g_cfg.lockWindow && !IsWindow(g_cfg.lockHwnd)) {
        SetWindowTextW(g_status, L"Choose a window to lock");
        return;
    }
    if (HWND owner = g_hwnd.load()) {
        SetFocus(owner);
    }
    SetRunningUi(true);
    g_uiStop.store(false);
    g_uiStart.store(true);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        CreateControls(hwnd, reinterpret_cast<LPCREATESTRUCT>(lParam)->hInstance);
        g_font = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"MS Shell Dlg");
        EnumChildWindows(hwnd, SetChildFont, reinterpret_cast<LPARAM>(g_font));
        g_hwnd.store(hwnd);
        LoadIni();
        ApplyUi(g_cfg);
        g_worker = std::thread(WorkerMain);
        return 0;
    }
    case WM_KEYDOWN:
        if (g_capture != CaptureTarget::None) {
            if ((lParam & (1 << 30)) == 0) {
                LatchCapture(hwnd, static_cast<int>(wParam));
            }
            return 0;
        }
        break;
    case WM_KEYUP:
        if (g_capture != CaptureTarget::None && static_cast<int>(wParam) == g_latchedVk) {
            AcceptCapture(hwnd, g_latchedVk);
            return 0;
        }
        break;
    case WM_XBUTTONDOWN:
        if (g_capture == CaptureTarget::Action) {
            int vk = HIWORD(wParam) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1;
            LatchCapture(hwnd, vk);
            return TRUE;
        }
        break;
    case WM_XBUTTONUP:
        if (g_capture == CaptureTarget::Action) {
            int vk = HIWORD(wParam) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1;
            if (vk == g_latchedVk) {
                AcceptCapture(hwnd, vk);
            }
            return TRUE;
        }
        break;
    case WM_CLICKER_FOCUS:
        if (g_capture != CaptureTarget::None) {
            SetFocus(hwnd);
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
        if (code == BN_CLICKED && IsInjectedKey()) {
            return 0;
        }
        if (id == ID_START && code == BN_CLICKED) {
            OnStart();
            return 0;
        }
        if (id == ID_STOP && code == BN_CLICKED) {
            g_uiStop.store(true);
            return 0;
        }
        if (id == ID_SET_KEY && code == BN_CLICKED) {
            if (g_capture != CaptureTarget::Action) {
                BeginCapture(hwnd, CaptureTarget::Action);
            }
            return 0;
        }
        if (id == ID_HOTKEY && code == BN_CLICKED) {
            if (g_capture != CaptureTarget::Hotkey) {
                BeginCapture(hwnd, CaptureTarget::Hotkey);
            }
            return 0;
        }
        if (id == ID_PANIC && code == BN_CLICKED) {
            if (g_capture != CaptureTarget::Panic) {
                BeginCapture(hwnd, CaptureTarget::Panic);
            }
            return 0;
        }
        if (id == ID_PICK && code == BN_CLICKED) {
            g_pickActive = true;
            g_pickWaitRelease = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            SetWindowTextW(g_pick, L"...");
            SetWindowTextW(g_status, L"Click the spot to use");
            SetTimer(hwnd, kPickTimer, 10, nullptr);
            return 0;
        }
        if (id == ID_LOCK_PICK && code == BN_CLICKED) {
            g_lockPickActive = true;
            g_lockWaitRelease = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            SetWindowTextW(g_lockPick, L"...");
            SetWindowTextW(g_status, L"Click the window to stay in");
            SetTimer(hwnd, kLockTimer, 10, nullptr);
            return 0;
        }
        if (code == EN_CHANGE || code == BN_CLICKED || code == CBN_SELCHANGE) {
            if (id == ID_BTN_LEFT || id == ID_BTN_MIDDLE || id == ID_BTN_RIGHT || id == ID_KEY_MODE) {
                UpdateInputMode(true);
            }
            CommitFromUi();
        }
        return 0;
    }
    case WM_TIMER:
        if (wParam == kCaptureTimer) {
            PollCapture(hwnd);
        } else if (wParam == kPickTimer) {
            PollPick(hwnd);
        } else if (wParam == kLockTimer) {
            PollLock(hwnd);
        }
        return 0;
    case WM_CLICKER_STATUS: {
        auto* status = reinterpret_cast<StatusMsg*>(lParam);
        if (status) {
            ShowStatus(*status);
            delete status;
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        if (reinterpret_cast<HWND>(lParam) == g_warn) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, RGB(170, 55, 40));
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        break;
    }
    case WM_DESTROY:
        g_pickActive = false;
        EndCapture(hwnd);
        CommitFromUi();
        g_quit.store(true);
        if (g_worker.joinable()) {
            g_worker.join();
        }
        g_hwnd.store(nullptr);
        if (g_font) {
            DeleteObject(g_font);
            g_font = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int RunWindow(HINSTANCE inst) {
    EngineInit();
    SetProcessDPIAware();

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"ClickerWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    RECT rect{0, 0, 204, 698};
    AdjustWindowRect(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND hwnd = CreateWindowExW(
        0, L"ClickerWindow", L"Clicker",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

}  // namespace

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool selfTest = false;
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--self-test") == 0) {
                selfTest = true;
            }
        }
        LocalFree(argv);
    }
    if (selfTest) {
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            return 1;
        }
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) {
            wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), L"selftest.log");
        }
        return RunSelfTest(path);
    }
    return RunWindow(inst);
}
