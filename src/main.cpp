#include "engine.h"

#include <commctrl.h>
#include <objbase.h>
#include <richedit.h>
#include <richole.h>
#include <tom.h>
#include <shellapi.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
constexpr int ID_TYPE_SCRIPT = 145;
constexpr int ID_SCRIPT = 146;
constexpr int ID_HELP = 147;
constexpr int ID_ADD_KEY = 148;
constexpr int ID_HELP_CLOSE = 149;
constexpr int ID_SCRIPT_LIST = 150;
constexpr int ID_SCRIPT_SAVE = 151;
constexpr int ID_SCRIPT_DEL = 152;
constexpr int ID_SCRIPT_KEY = 153;
constexpr int ID_HOT_PRESS = 138;
constexpr int ID_HOT_TOGGLE = 139;

constexpr UINT WM_CLICKER_STATUS = WM_APP + 1;
constexpr UINT WM_CLICKER_FOCUS = WM_APP + 2;
constexpr UINT WM_CLICKER_LOAD = WM_APP + 3;
constexpr UINT_PTR kPickTimer = 1;
constexpr UINT_PTR kCaptureTimer = 2;
constexpr UINT_PTR kLockTimer = 3;

enum class CaptureTarget { None, Action, Hotkey, Panic, Script, ScriptKey };

struct StatusMsg {
    unsigned long long clicks;
    double cps;
    int state;  // 0 stopped, 1 running, 2 holding, 3 panic, 4 conflict
    int64_t elapsedMs;
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
std::wstring g_script;
std::wstring g_scriptError;
std::wstring g_scriptLive;
int g_runLine = 0;
int g_paintedLine = -1;
bool g_coloring = false;
std::wstring g_ini;

struct NamedScript {
    std::wstring name;
    WORD key = 0;
};

std::vector<NamedScript> g_named;
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
HWND g_typeScript = nullptr;
HWND g_scriptEdit = nullptr;
bool g_scriptPlaced = false;
DWORD g_scriptCaret = 0;
ITextDocument* g_scriptDoc = nullptr;
HWND g_scriptList = nullptr;
HWND g_scriptSave = nullptr;
HWND g_scriptDel = nullptr;
HWND g_scriptKey = nullptr;
HWND g_helpBtn = nullptr;
HWND g_addKey = nullptr;
HWND g_helpWnd = nullptr;
HWND g_holdHh = nullptr;
HWND g_holdMm = nullptr;
HWND g_holdSs = nullptr;
HWND g_holdMs = nullptr;
HWND g_holdSep1 = nullptr;
HWND g_holdSep2 = nullptr;
HWND g_holdSep3 = nullptr;
HWND g_mouseGroup = nullptr;
HWND g_repeatLabel = nullptr;
HWND g_repeatHint = nullptr;
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
HFONT g_scriptFont = nullptr;
HWND g_tip = nullptr;
WNDPROC g_buttonProc = nullptr;
bool g_loading = false;
CaptureTarget g_capture = CaptureTarget::None;
int g_latchedVk = 0;
bool g_captureDown[256] = {};
bool g_pickActive = false;
bool g_pickWaitRelease = false;
bool g_lockPickActive = false;
bool g_lockWaitRelease = false;

void PostState(int state, unsigned long long clicks, double cps, int64_t elapsedMs = -1) {
    HWND hwnd = g_hwnd.load();
    if (!hwnd) {
        return;
    }
    auto* msg = new StatusMsg{clicks, cps, state, elapsedMs};
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

void PollScriptKeys(bool scriptWas[256]) {
    WORD vks[64];
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (const NamedScript& item : g_named) {
            if (item.key != 0 && count < 64) {
                bool seen = false;
                for (int i = 0; i < count; ++i) {
                    if (vks[i] == item.key) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) {
                    vks[count++] = item.key;
                }
            }
        }
    }
    bool suspended = g_suspendHotkeys.load();
    HWND hwnd = g_hwnd.load();
    for (int i = 0; i < count; ++i) {
        WORD vk = vks[i];
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !scriptWas[vk] && !suspended && hwnd) {
            PostMessageW(hwnd, WM_CLICKER_LOAD, vk, 0);
        }
        scriptWas[vk] = down;
    }
}

void WorkerMain() {
    Keys keys;
    keys.hotWas = (GetAsyncKeyState(static_cast<int>(g_hotVk.load())) & 0x8000) != 0;
    keys.panicWas = (GetAsyncKeyState(static_cast<int>(g_panicVk.load())) & 0x8000) != 0;
    bool scriptWas[256] = {};
    for (int vk = 1; vk < 256; ++vk) {
        scriptWas[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    }

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
            PollScriptKeys(scriptWas);
            g_uiStop.exchange(false);
            Sleep(8);
            continue;
        }

        Config cfg;
        std::wstring script;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            cfg = g_cfg;
            script = g_script;
        }
        if (cfg.clickType != ClickType::Script && ActionConflictsWithHotkeys(cfg)) {
            PostState(4, 0, 0);
            continue;
        }
        if (cfg.lockWindow && !IsWindow(cfg.lockHwnd)) {
            PostState(5, 0, 0);
            continue;
        }
        // A click on Start is still held for a moment. Wait until that physical
        // button is up so it is not merged with the first injected click.
        if (latched && (cfg.action == ActionKind::Mouse || cfg.clickType == ClickType::Script)) {
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
        ctl.onLine = [](void*, int line, double remain, int repsLeft, unsigned long long clicks) {
            wchar_t buf[96];
            if (remain >= 0 && repsLeft >= 0) {
                swprintf_s(buf, L"Line %d — %.1fs — %d left", line, remain, repsLeft);
            } else if (remain >= 0) {
                swprintf_s(buf, L"Line %d — %.1fs", line, remain);
            } else if (repsLeft >= 0) {
                swprintf_s(buf, L"Line %d — %d left", line, repsLeft);
            } else {
                swprintf_s(buf, L"Line %d", line);
            }
            {
                std::lock_guard<std::mutex> lock(g_mu);
                g_scriptLive = buf;
                g_runLine = line;
            }
            PostState(1, clicks, 0);
        };
        ctl.onEvent = nullptr;
        ctl.ctx = &poll;
        ctl.inject = true;

        RunStats stats;
        wchar_t scriptError[160] = {};
        if (cfg.clickType == ClickType::Script) {
            {
                std::lock_guard<std::mutex> lock(g_mu);
                g_scriptLive.clear();
                g_runLine = 0;
            }
            stats = RunScript(script.c_str(), cfg, ctl, scriptError, 160);
        } else {
            stats = RunSession(cfg, ctl);
        }
        g_uiStart.exchange(false);
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_scriptLive.clear();
            g_runLine = 0;
        }
        if (scriptError[0]) {
            std::lock_guard<std::mutex> lock(g_mu);
            g_scriptError = scriptError;
            PostState(6, stats.clicks, 0, stats.elapsedMs);
        } else {
            PostState(stats.reason == StopReason::Panic ? 3 : 0, stats.clicks, 0, stats.elapsedMs);
        }
        for (int vk = 1; vk < 256; ++vk) {
            scriptWas[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        }
    }
}

std::wstring SettingsPath() {
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    std::wstring dir = (n == 0 || n >= MAX_PATH) ? L"." : std::wstring(base) + L"\\Clicker";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

const wchar_t* kDefaultScript =
    L"rep\r\n"
    L"\tlef\r\n"
    L"\twait 0.5s\r\n";

const wchar_t* kHelpText =
    L"lef            click left once\r\n"
    L"lef 5s         keep clicking left for 5 seconds\r\n"
    L"lef+           hold left down\r\n"
    L"lef+ 5s        hold left for 5 seconds, then release\r\n"
    L"lef-           release left\r\n"
    L"\r\n"
    L"rig, mid, x1 and x2 work the same way.\r\n"
    L"Keys do too: a, 5, spc, enter, lshift, insert, num5.\r\n"
    L"5 is the top-row 5 key. num5 is the number pad.\r\n"
    L"Add key writes that name where the cursor was.\r\n"
    L"If you have not clicked in the script, it goes at the bottom.\r\n"
    L"\r\n"
    L"lshift a       Shift+A once\r\n"
    L"lshift 5       Shift+5 once\r\n"
    L"lctrl lshift c Ctrl+Shift+C once\r\n"
    L"lshift a 5s    keep doing Shift+A for 5 seconds\r\n"
    L"lshift+        hold shift across the next lines\r\n"
    L"lshift-        let shift go\r\n"
    L"\r\n"
    L"wait 5s        pause. 500ms, 2m, 1h, and 0:0:5:0 too.\r\n"
    L"wait 5         an error. 5 is a key, and there is no time.\r\n"
    L"rep            repeat the tabbed lines until stop\r\n"
    L"rep 4          repeat them 4 times, not for 4 seconds\r\n"
    L"\r\n"
    L"Tab steps the selected lines in. Shift+Tab steps them out.\r\n"
    L"The time at the top is the gap used by lef 5s.\r\n"
    L"The selected line is the step running now.\r\n"
    L"A red line will not do what it says.\r\n"
    L"# starts a comment.\r\n"
    L"\r\n"
    L"Save stores this script under the name in the box.\r\n"
    L"Its key loads that script while the clicker is stopped.\r\n";

std::wstring ClickerDir() {
    std::wstring ini = g_ini.empty() ? SettingsPath() : g_ini;
    size_t slash = ini.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
        return L".";
    }
    return ini.substr(0, slash);
}

std::wstring ScriptPath() {
    return ClickerDir() + L"\\script.txt";
}

std::wstring ScriptsDir() {
    std::wstring dir = ClickerDir() + L"\\scripts";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring ScriptIndexPath() {
    return ClickerDir() + L"\\scripts.ini";
}

std::wstring ScriptFileFor(const std::wstring& name) {
    return ScriptsDir() + L"\\" + name + L".txt";
}

bool ScriptNameOk(const std::wstring& name) {
    if (name.empty() || name.size() > 32) {
        return false;
    }
    if (name.front() == L' ' || name.back() == L' ') {
        return false;
    }
    for (wchar_t c : name) {
        bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
                  c == L' ' || c == L'-' || c == L'_';
        if (!ok) {
            return false;
        }
    }
    std::wstring upper = name;
    for (wchar_t& c : upper) {
        if (c >= L'a' && c <= L'z') {
            c = static_cast<wchar_t>(c - L'a' + L'A');
        }
    }
    if (upper == L"CON" || upper == L"PRN" || upper == L"AUX" || upper == L"NUL") {
        return false;
    }
    if (upper.size() == 4 && (upper.compare(0, 3, L"COM") == 0 || upper.compare(0, 3, L"LPT") == 0) &&
        upper[3] >= L'1' && upper[3] <= L'9') {
        return false;
    }
    return true;
}

bool ReadTextFile(const std::wstring& path, std::wstring& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD size = GetFileSize(file, nullptr);
    if (size == INVALID_FILE_SIZE || size < 2 || size > 1000000) {
        CloseHandle(file);
        return false;
    }
    std::string bytes(size, '\0');
    DWORD read = 0;
    ReadFile(file, bytes.data(), size, &read, nullptr);
    CloseHandle(file);
    bytes.resize(read);
    if (read >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        size_t chars = (read - 2) / sizeof(wchar_t);
        out.assign(chars, L'\0');
        memcpy(out.data(), bytes.data() + 2, chars * sizeof(wchar_t));
        while (!out.empty() && out.back() == 0) {
            out.pop_back();
        }
        return true;
    }
    int wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(read), nullptr, 0);
    if (wide <= 0) {
        return false;
    }
    out.assign(static_cast<size_t>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(read), out.data(), wide);
    return true;
}

std::wstring LoadScriptFile() {
    std::wstring text;
    if (!ReadTextFile(ScriptPath(), text)) {
        return kDefaultScript;
    }
    return text;
}

bool SaveTextFile(const std::wstring& path, const std::wstring& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    unsigned short bom = 0xFEFF;
    DWORD written = 0;
    if (!WriteFile(file, &bom, sizeof(bom), &written, nullptr) || written != sizeof(bom)) {
        CloseHandle(file);
        return false;
    }
    if (!text.empty()) {
        DWORD bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
        if (!WriteFile(file, text.data(), bytes, &written, nullptr) || written != bytes) {
            CloseHandle(file);
            return false;
        }
    }
    CloseHandle(file);
    return true;
}

void SaveScript(const std::wstring& text) {
    SaveTextFile(ScriptPath(), text);
}

void LoadNamedScripts() {
    std::wstring path = ScriptIndexPath();
    int count = GetPrivateProfileIntW(L"scripts", L"count", 0, path.c_str());
    if (count < 0) {
        count = 0;
    }
    if (count > 200) {
        count = 200;
    }
    std::vector<NamedScript> loaded;
    for (int i = 0; i < count; ++i) {
        wchar_t keyName[16];
        swprintf_s(keyName, L"name%d", i);
        wchar_t name[40] = {};
        GetPrivateProfileStringW(L"scripts", keyName, L"", name, 40, path.c_str());
        if (!ScriptNameOk(name)) {
            continue;
        }
        swprintf_s(keyName, L"key%d", i);
        int vk = GetPrivateProfileIntW(L"scripts", keyName, 0, path.c_str());
        NamedScript item;
        item.name = name;
        if (vk > 0 && vk < 256) {
            item.key = static_cast<WORD>(vk);
        }
        if (item.key == g_cfg.hotkeyVk || item.key == g_cfg.panicVk) {
            item.key = 0;
        }
        for (const NamedScript& prior : loaded) {
            if (item.key != 0 && prior.key == item.key) {
                item.key = 0;
                break;
            }
        }
        loaded.push_back(item);
    }
    std::lock_guard<std::mutex> lock(g_mu);
    g_named = std::move(loaded);
}

void SaveNamedIndex() {
    std::vector<NamedScript> copy;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        copy = g_named;
    }
    std::wstring path = ScriptIndexPath();
    int old = GetPrivateProfileIntW(L"scripts", L"count", 0, path.c_str());
    wchar_t buf[16];
    swprintf_s(buf, L"%d", static_cast<int>(copy.size()));
    WritePrivateProfileStringW(L"scripts", L"count", buf, path.c_str());
    for (int i = 0; i < static_cast<int>(copy.size()); ++i) {
        wchar_t keyName[16];
        swprintf_s(keyName, L"name%d", i);
        WritePrivateProfileStringW(L"scripts", keyName, copy[static_cast<size_t>(i)].name.c_str(), path.c_str());
        swprintf_s(keyName, L"key%d", i);
        swprintf_s(buf, L"%d", copy[static_cast<size_t>(i)].key);
        WritePrivateProfileStringW(L"scripts", keyName, buf, path.c_str());
    }
    for (int i = static_cast<int>(copy.size()); i < old; ++i) {
        wchar_t keyName[16];
        swprintf_s(keyName, L"name%d", i);
        WritePrivateProfileStringW(L"scripts", keyName, nullptr, path.c_str());
        swprintf_s(keyName, L"key%d", i);
        WritePrivateProfileStringW(L"scripts", keyName, nullptr, path.c_str());
    }
}

std::wstring ReadScriptEdit() {
    if (!g_scriptEdit) {
        return g_script;
    }
    int n = GetWindowTextLengthW(g_scriptEdit);
    if (n < 0) {
        n = 0;
    }
    std::wstring text(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(g_scriptEdit, text.data(), n + 1);
    text.resize(static_cast<size_t>(n));
    return text;
}

void CommitFromUi();
void PaintScript();
void ApplyScriptTabs(HWND edit);
void FillScriptList();
void UpdateScriptKeyButton();
void EndCapture(HWND hwnd);
void AddTip(HWND tool, const wchar_t* text);
void RelayTipMouse(HWND coordHwnd, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK ButtonProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

void RememberScriptCaret() {
    if (!g_scriptEdit) {
        return;
    }
    DWORD start = 0;
    DWORD end = 0;
    SendMessageW(g_scriptEdit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    g_scriptCaret = start;
    g_scriptPlaced = true;
}

void PlaceScriptCaretAtClick(HWND hwnd, LPARAM lParam) {
    POINTL pt;
    pt.x = static_cast<LONG>(static_cast<short>(LOWORD(lParam)));
    pt.y = static_cast<LONG>(static_cast<short>(HIWORD(lParam)));
    LRESULT cp = SendMessageW(hwnd, EM_CHARFROMPOS, 0, reinterpret_cast<LPARAM>(&pt));
    if (cp < 0) {
        return;
    }
    SendMessageW(hwnd, EM_SETSEL, static_cast<WPARAM>(cp), static_cast<LPARAM>(cp));
    RememberScriptCaret();
}

std::wstring ReadScriptForInsert() {
    GETTEXTLENGTHEX length{};
    length.flags = GTL_PRECISE | GTL_NUMCHARS;
    length.codepage = 1200;
    LRESULT n = SendMessageW(g_scriptEdit, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&length), 0);
    if (n < 0) {
        n = 0;
    }
    std::wstring text(static_cast<size_t>(n) + 1, L'\0');
    GETTEXTEX get{};
    get.cb = static_cast<DWORD>((static_cast<size_t>(n) + 1) * sizeof(wchar_t));
    get.codepage = 1200;
    SendMessageW(g_scriptEdit, EM_GETTEXTEX, reinterpret_cast<WPARAM>(&get), reinterpret_cast<LPARAM>(text.data()));
    text.resize(wcslen(text.c_str()));
    return text;
}

void ForgetScriptCaret() {
    g_scriptPlaced = false;
    g_scriptCaret = 0;
}

void InsertScriptLine(const wchar_t* name) {
    if (!g_scriptEdit || !name || !name[0]) {
        return;
    }
    if (g_scriptPlaced) {
        g_coloring = true;
        SendMessageW(g_scriptEdit, EM_SETSEL, g_scriptCaret, g_scriptCaret);
        SendMessageW(g_scriptEdit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(name));
        g_coloring = false;
        RememberScriptCaret();
        SendMessageW(g_scriptEdit, EM_SCROLLCARET, 0, 0);
        CommitFromUi();
        return;
    }
    std::wstring text = ReadScriptForInsert();
    size_t caret = text.size();
    auto isBreak = [](wchar_t c) { return c == L'\n' || c == L'\r'; };
    size_t lineStart = 0;
    if (caret > 0) {
        size_t br = text.find_last_of(L"\r\n", caret - 1);
        lineStart = br == std::wstring::npos ? 0 : br + 1;
    }
    size_t lineEnd = text.find_first_of(L"\r\n", caret);
    if (lineEnd == std::wstring::npos) {
        lineEnd = text.size();
    }
    size_t contentEnd = lineEnd;
    std::wstring indent;
    bool blank = true;
    for (size_t i = lineStart; i < contentEnd; ++i) {
        if (text[i] == L' ' || text[i] == L'\t') {
            if (blank) {
                indent.push_back(text[i]);
            }
        } else {
            blank = false;
        }
    }
    size_t bodyBegin = lineStart;
    while (bodyBegin < contentEnd && (text[bodyBegin] == L' ' || text[bodyBegin] == L'\t')) {
        ++bodyBegin;
    }
    size_t bodyEnd = contentEnd;
    while (bodyEnd > bodyBegin && (text[bodyEnd - 1] == L' ' || text[bodyEnd - 1] == L'\t')) {
        --bodyEnd;
    }
    std::wstring body = text.substr(bodyBegin, bodyEnd - bodyBegin);
    bool onRep = body.size() >= 3 && _wcsnicmp(body.c_str(), L"rep", 3) == 0 &&
                 (body.size() == 3 || body[3] == L' ' || body[3] == L'\t');
    if (onRep) {
        indent.push_back(L'\t');
    }
    std::wstring line = indent + name;
    size_t caretAt = 0;
    if (text.empty() || blank) {
        text.replace(lineStart, contentEnd - lineStart, line);
        caretAt = lineStart + line.size();
    } else {
        size_t at = lineEnd;
        std::wstring extra;
        if (at < text.size() && isBreak(text[at])) {
            ++at;
            if (at < text.size() && text[at - 1] == L'\r' && text[at] == L'\n') {
                ++at;
            }
            extra = line + L"\r";
            caretAt = at + line.size();
        } else {
            extra = L"\r" + line;
            caretAt = at + extra.size();
        }
        text.insert(at, extra);
    }
    SetWindowTextW(g_scriptEdit, text.c_str());
    ApplyScriptTabs(g_scriptEdit);
    SendMessageW(g_scriptEdit, EM_SETSEL, caretAt, caretAt);
    SendMessageW(g_scriptEdit, EM_SCROLLCARET, 0, 0);
    g_scriptCaret = static_cast<DWORD>(caretAt);
    g_scriptPlaced = true;
    CommitFromUi();
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
    } else if (SendMessageW(g_typeScript, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        cfg.clickType = ClickType::Script;
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
    if (cfg.clickType == ClickType::Hold || cfg.clickType == ClickType::TimedHold ||
        cfg.clickType == ClickType::Script) {
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

void ApplyUnusedControls(bool editable) {
    bool script = g_typeScript && SendMessageW(g_typeScript, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const BOOL on = (editable && !script) ? TRUE : FALSE;
    EnableWindow(g_mouseGroup, on);
    EnableWindow(g_btnLeft, on);
    EnableWindow(g_btnMiddle, on);
    EnableWindow(g_btnRight, on);
    EnableWindow(g_keyMode, on);
    EnableWindow(g_repeatLabel, on);
    EnableWindow(g_repeatHint, on);
    EnableWindow(g_repeatN, on);
    EnableWindow(g_holdHh, on);
    EnableWindow(g_holdMm, on);
    EnableWindow(g_holdSs, on);
    EnableWindow(g_holdMs, on);
    EnableWindow(g_holdSep1, on);
    EnableWindow(g_holdSep2, on);
    EnableWindow(g_holdSep3, on);
    if (on) {
        UpdateInputMode(true);
    } else {
        EnableWindow(g_setKey, FALSE);
    }
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

HMENU MenuId(int id);

LRESULT CALLBACK HelpProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        int lines = 1;
        int widest = 0;
        HDC dc = GetDC(hwnd);
        HFONT oldFont = nullptr;
        if (g_scriptFont) {
            oldFont = static_cast<HFONT>(SelectObject(dc, g_scriptFont));
        }
        TEXTMETRICW metric{};
        GetTextMetricsW(dc, &metric);
        const wchar_t* scan = kHelpText;
        while (*scan) {
            const wchar_t* end = scan;
            while (*end && *end != L'\n') {
                ++end;
            }
            int chars = static_cast<int>(end - scan);
            if (chars > 0 && end[-1] == L'\r') {
                --chars;
            }
            SIZE measured{};
            if (chars > 0) {
                GetTextExtentPoint32W(dc, scan, chars, &measured);
                if (measured.cx > widest) {
                    widest = measured.cx;
                }
            }
            if (*end == L'\n') {
                ++lines;
                scan = end + 1;
            } else {
                scan = end;
            }
        }
        if (kHelpText[0] != 0 && kHelpText[wcslen(kHelpText) - 1] == L'\n' && lines > 1) {
            --lines;
        }
        if (oldFont) {
            SelectObject(dc, oldFont);
        }
        ReleaseDC(hwnd, dc);
        int lineHeight = metric.tmHeight + metric.tmExternalLeading + 2;
        if (lineHeight < 1) {
            lineHeight = 18;
        }
        int editW = widest + 24;
        int editH = lines * lineHeight + 6;
        HWND edit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", kHelpText,
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY,
            8, 8, editW, editH, hwnd, nullptr, nullptr, nullptr);
        if (g_scriptFont) {
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_scriptFont), FALSE);
        }
        HWND close = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     8, 16 + editH, 72, 22, hwnd, MenuId(ID_HELP_CLOSE), nullptr, nullptr);
        if (g_font) {
            SendMessageW(close, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
        }
        if (g_buttonProc) {
            SetWindowLongPtrW(close, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ButtonProc));
        }
        AddTip(close, L"Close this guide.");
        RECT client{0, 0, 16 + editW, 16 + editH + 8 + 22 + 8};
        RECT framed = client;
        AdjustWindowRect(&framed, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
        int width = framed.right - framed.left;
        int height = framed.bottom - framed.top;
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        RECT placed{};
        GetWindowRect(hwnd, &placed);
        int x = placed.left;
        int y = placed.top;
        if (x + width > work.right) {
            x = work.right - width;
        }
        if (y + height > work.bottom) {
            y = work.bottom - height;
        }
        if (x < work.left) {
            x = work.left;
        }
        if (y < work.top) {
            y = work.top;
        }
        SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_HELP_CLOSE) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_helpWnd == hwnd) {
            g_helpWnd = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ShowHelp(HWND owner) {
    if (g_helpWnd && IsWindow(g_helpWnd)) {
        ShowWindow(g_helpWnd, SW_SHOW);
        SetForegroundWindow(g_helpWnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = HelpProc;
        wc.hInstance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
        wc.lpszClassName = L"ClickerHelp";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        registered = true;
    }
    RECT rect{0, 0, 500, 450};
    AdjustWindowRect(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    g_helpWnd = CreateWindowExW(
        0, L"ClickerHelp", L"Script help",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        ownerRect.left + 24, ownerRect.top + 24,
        rect.right - rect.left, rect.bottom - rect.top,
        owner, nullptr, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE)), nullptr);
    ShowWindow(g_helpWnd, SW_SHOW);
}

std::wstring ComboName() {
    wchar_t raw[48] = {};
    if (g_scriptList) {
        GetWindowTextW(g_scriptList, raw, 48);
    }
    std::wstring name = raw;
    size_t begin = 0;
    while (begin < name.size() && name[begin] == L' ') {
        ++begin;
    }
    size_t end = name.size();
    while (end > begin && name[end - 1] == L' ') {
        --end;
    }
    return name.substr(begin, end - begin);
}

int NamedIndexOf(const std::wstring& name) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (int i = 0; i < static_cast<int>(g_named.size()); ++i) {
        if (_wcsicmp(g_named[static_cast<size_t>(i)].name.c_str(), name.c_str()) == 0) {
            return i;
        }
    }
    return -1;
}

bool ClickerRunning() {
    return g_stop && IsWindowEnabled(g_stop);
}

void UpdateScriptKeyButton() {
    if (!g_scriptKey || g_capture == CaptureTarget::ScriptKey) {
        return;
    }
    int index = NamedIndexOf(ComboName());
    WORD key = 0;
    if (index >= 0) {
        std::lock_guard<std::mutex> lock(g_mu);
        if (index < static_cast<int>(g_named.size())) {
            key = g_named[static_cast<size_t>(index)].key;
        }
    }
    if (key == 0) {
        SetWindowTextW(g_scriptKey, L"Key");
        return;
    }
    wchar_t label[64];
    KeyName(key, label, 64);
    SetWindowTextW(g_scriptKey, label);
}

void FillScriptList() {
    if (!g_scriptList) {
        return;
    }
    std::wstring current = ComboName();
    std::vector<NamedScript> copy;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        copy = g_named;
    }
    bool nested = g_loading;
    g_loading = true;
    SendMessageW(g_scriptList, CB_RESETCONTENT, 0, 0);
    int select = -1;
    for (int i = 0; i < static_cast<int>(copy.size()); ++i) {
        SendMessageW(g_scriptList, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(copy[static_cast<size_t>(i)].name.c_str()));
        if (_wcsicmp(copy[static_cast<size_t>(i)].name.c_str(), current.c_str()) == 0) {
            select = i;
        }
    }
    if (select >= 0) {
        SendMessageW(g_scriptList, CB_SETCURSEL, select, 0);
    } else {
        SetWindowTextW(g_scriptList, current.c_str());
    }
    g_loading = nested;
    UpdateScriptKeyButton();
}

void SelectComboName(const std::wstring& name) {
    if (!g_scriptList) {
        return;
    }
    int count = static_cast<int>(SendMessageW(g_scriptList, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        wchar_t item[40] = {};
        SendMessageW(g_scriptList, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(item));
        if (_wcsicmp(item, name.c_str()) == 0) {
            SendMessageW(g_scriptList, CB_SETCURSEL, i, 0);
            return;
        }
    }
    SetWindowTextW(g_scriptList, name.c_str());
}

void LoadNamedByName(const std::wstring& name) {
    if (ClickerRunning()) {
        return;
    }
    if (g_suspendHotkeys.load() && GetFocus() == g_scriptEdit) {
        return;
    }
    std::wstring text;
    if (!ReadTextFile(ScriptFileFor(name), text)) {
        SetWindowTextW(g_status, L"That script file is missing");
        return;
    }
    g_loading = true;
    SetWindowTextW(g_scriptEdit, text.c_str());
    ForgetScriptCaret();
    ApplyScriptTabs(g_scriptEdit);
    SelectComboName(name);
    g_loading = false;
    CommitFromUi();
    UpdateScriptKeyButton();
    wchar_t note[80];
    swprintf_s(note, L"Loaded %s", name.c_str());
    SetWindowTextW(g_status, note);
}

void LoadNamedByKey(WORD vk) {
    if (vk == 0 || ClickerRunning() || g_suspendHotkeys.load()) {
        return;
    }
    std::wstring name;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (const NamedScript& item : g_named) {
            if (item.key == vk) {
                name = item.name;
                break;
            }
        }
    }
    if (!name.empty()) {
        LoadNamedByName(name);
    }
}

void SaveCurrentNamed() {
    std::wstring name = ComboName();
    if (!ScriptNameOk(name)) {
        SetWindowTextW(g_status, L"Use a name of letters, numbers, spaces, - or _");
        return;
    }
    if (!SaveTextFile(ScriptFileFor(name), ReadScriptEdit())) {
        SetWindowTextW(g_status, L"Could not save that script");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mu);
        bool found = false;
        for (NamedScript& item : g_named) {
            if (_wcsicmp(item.name.c_str(), name.c_str()) == 0) {
                item.name = name;
                found = true;
                break;
            }
        }
        if (!found) {
            NamedScript item;
            item.name = name;
            g_named.push_back(item);
        }
    }
    SaveNamedIndex();
    FillScriptList();
    CommitFromUi();
    wchar_t note[80];
    swprintf_s(note, L"Saved %s", name.c_str());
    SetWindowTextW(g_status, note);
}

void DeleteCurrentNamed() {
    std::wstring name = ComboName();
    int index = NamedIndexOf(name);
    if (index < 0) {
        SetWindowTextW(g_status, L"Pick a saved script");
        return;
    }
    DeleteFileW(ScriptFileFor(name).c_str());
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (index < static_cast<int>(g_named.size())) {
            g_named.erase(g_named.begin() + index);
        }
    }
    SaveNamedIndex();
    g_loading = true;
    SetWindowTextW(g_scriptList, L"");
    g_loading = false;
    FillScriptList();
    wchar_t note[80];
    swprintf_s(note, L"Deleted %s", name.c_str());
    SetWindowTextW(g_status, note);
}

void ClearNamedKey(HWND hwnd) {
    std::wstring name = ComboName();
    int index = NamedIndexOf(name);
    if (index >= 0) {
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (index < static_cast<int>(g_named.size())) {
                g_named[static_cast<size_t>(index)].key = 0;
            }
        }
        SaveNamedIndex();
    }
    EndCapture(hwnd);
    if (index < 0) {
        SetWindowTextW(g_status, L"Save the script first");
    } else {
        SetWindowTextW(g_status, L"Select key cleared");
    }
}

void LayoutForScript(bool script) {
    HWND hwnd = g_hwnd.load();
    if (!hwnd) {
        return;
    }
    const int width = script ? 430 : 204;
    const int height = 716;
    RECT rect{0, 0, width, height};
    DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    AdjustWindowRect(&rect, style, FALSE);
    RECT cur{};
    GetWindowRect(hwnd, &cur);
    SetWindowPos(hwnd, nullptr, cur.left, cur.top, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_status) {
        SetWindowPos(g_status, nullptr, 8, 4, script ? 414 : 188, 28, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g_scriptEdit) {
        ShowWindow(g_scriptEdit, script ? SW_SHOW : SW_HIDE);
        EnableWindow(g_scriptEdit, script ? TRUE : FALSE);
    }
    if (g_helpBtn) {
        ShowWindow(g_helpBtn, script ? SW_SHOW : SW_HIDE);
        EnableWindow(g_helpBtn, script ? TRUE : FALSE);
    }
    if (g_addKey) {
        ShowWindow(g_addKey, script ? SW_SHOW : SW_HIDE);
        EnableWindow(g_addKey, script ? TRUE : FALSE);
    }
    if (g_scriptList) {
        ShowWindow(g_scriptList, script ? SW_SHOW : SW_HIDE);
        EnableWindow(g_scriptList, script ? TRUE : FALSE);
    }
    if (g_scriptSave) {
        ShowWindow(g_scriptSave, script ? SW_SHOW : SW_HIDE);
        EnableWindow(g_scriptSave, script ? TRUE : FALSE);
    }
    if (g_scriptDel) {
        ShowWindow(g_scriptDel, script ? SW_SHOW : SW_HIDE);
        EnableWindow(g_scriptDel, script ? TRUE : FALSE);
    }
    if (g_scriptKey) {
        ShowWindow(g_scriptKey, script ? SW_SHOW : SW_HIDE);
        EnableWindow(g_scriptKey, script ? TRUE : FALSE);
    }
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
    SendMessageW(g_typeScript, BM_SETCHECK, cfg.clickType == ClickType::Script ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_scriptEdit) {
        SetWindowTextW(g_scriptEdit, g_script.c_str());
        ForgetScriptCaret();
        ApplyScriptTabs(g_scriptEdit);
    }
    FillScriptList();
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
    ApplyUnusedControls(true);
    LayoutForScript(cfg.clickType == ClickType::Script);
    g_loading = false;
    PaintScript();
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
    } else if (cfg.clickType == ClickType::Script) {
        clickType = 4;
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
        g_script = ReadScriptEdit();
    }
    g_hotVk.store(cfg.hotkeyVk);
    g_panicVk.store(cfg.panicVk);
    g_hotMode.store(cfg.hotkeyMode == HotkeyMode::Hold ? 1 : 0);
    SaveIni(cfg);
    SaveScript(g_script);
    UpdateWarning(cfg);
    PaintScript();
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
    else if (type == 4) cfg.clickType = ClickType::Script;
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
    cfg.holdMs = 20;
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
    g_script = LoadScriptFile();
    LoadNamedScripts();
    g_hotVk.store(cfg.hotkeyVk);
    g_panicVk.store(cfg.panicVk);
    g_hotMode.store(cfg.hotkeyMode == HotkeyMode::Hold ? 1 : 0);
}

HMENU MenuId(int id) {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

HWND MakeLabel(HWND parent, HINSTANCE inst, const wchar_t* text, int x, int y, int w, int h, int id = 0) {
    return CreateWindowExW(0, L"STATIC", text,
                           WS_CHILD | WS_VISIBLE | SS_LEFT | SS_EDITCONTROL | SS_NOPREFIX | SS_NOTIFY,
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
    ApplyUnusedControls(edit == TRUE);
    EnableWindow(g_typeSingle, edit);
    EnableWindow(g_typeDouble, edit);
    EnableWindow(g_typeHold, edit);
    EnableWindow(g_typeTimed, edit);
    EnableWindow(g_typeScript, edit);
    bool scriptOn = g_typeScript && SendMessageW(g_typeScript, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(g_scriptEdit, scriptOn ? TRUE : FALSE);
    if (g_scriptEdit) {
        SendMessageW(g_scriptEdit, EM_SETREADONLY, (running && scriptOn) ? TRUE : FALSE, 0);
    }
    EnableWindow(g_addKey, (edit && scriptOn) ? TRUE : FALSE);
    EnableWindow(g_helpBtn, scriptOn ? TRUE : FALSE);
    EnableWindow(g_scriptList, (edit && scriptOn) ? TRUE : FALSE);
    EnableWindow(g_scriptSave, (edit && scriptOn) ? TRUE : FALSE);
    EnableWindow(g_scriptDel, (edit && scriptOn) ? TRUE : FALSE);
    EnableWindow(g_scriptKey, (edit && scriptOn) ? TRUE : FALSE);
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

void FormatDuration(int64_t elapsedMs, wchar_t* out, int outChars) {
    if (elapsedMs < 0) {
        elapsedMs = 0;
    }
    if (elapsedMs < 10000) {
        swprintf_s(out, outChars, L"%.1fs", elapsedMs / 1000.0);
        return;
    }
    int64_t totalSeconds = elapsedMs / 1000;
    int64_t seconds = totalSeconds % 60;
    int64_t minutes = (totalSeconds / 60) % 60;
    int64_t hours = totalSeconds / 3600;
    if (hours > 0) {
        swprintf_s(out, outChars, L"%lldh %lldm", static_cast<long long>(hours), static_cast<long long>(minutes));
    } else {
        swprintf_s(out, outChars, L"%lldm %llds", static_cast<long long>(minutes), static_cast<long long>(seconds));
    }
}

void ApplyScriptTabs(HWND edit) {
    if (!edit) {
        return;
    }
    CHARRANGE previous{};
    SendMessageW(edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&previous));
    SendMessageW(edit, WM_SETREDRAW, FALSE, 0);
    SendMessageW(edit, EM_HIDESELECTION, TRUE, FALSE);
    SendMessageW(edit, EM_SETSEL, 0, -1);
    PARAFORMAT2 format{};
    format.cbSize = sizeof(format);
    format.dwMask = PFM_TABSTOPS;
    format.cTabCount = MAX_TAB_STOPS;
    for (int i = 0; i < MAX_TAB_STOPS; ++i) {
        format.rgxTabs[i] = (i + 1) * 480;
    }
    SendMessageW(edit, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&format));
    SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&previous));
    SendMessageW(edit, EM_HIDESELECTION, FALSE, FALSE);
    SendMessageW(edit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(edit, nullptr, TRUE);
}

bool EnsureScriptDoc() {
    if (g_scriptDoc) {
        return true;
    }
    if (!g_scriptEdit) {
        return false;
    }
    IRichEditOle* ole = nullptr;
    SendMessageW(g_scriptEdit, EM_GETOLEINTERFACE, 0, reinterpret_cast<LPARAM>(&ole));
    if (!ole) {
        return false;
    }
    HRESULT hr = ole->QueryInterface(__uuidof(ITextDocument), reinterpret_cast<void**>(&g_scriptDoc));
    ole->Release();
    return SUCCEEDED(hr) && g_scriptDoc != nullptr;
}

void ColorScriptLine(int line, bool error) {
    if (!g_scriptDoc) {
        return;
    }
    int start = static_cast<int>(SendMessageW(g_scriptEdit, EM_LINEINDEX, line, 0));
    if (start < 0) {
        return;
    }
    int len = static_cast<int>(SendMessageW(g_scriptEdit, EM_LINELENGTH, start, 0));
    if (len < 0) {
        len = 0;
    }
    ITextRange* range = nullptr;
    if (FAILED(g_scriptDoc->Range(start, start + len, &range)) || !range) {
        return;
    }
    ITextFont* font = nullptr;
    if (SUCCEEDED(range->GetFont(&font)) && font) {
        long color = tomUndefined;
        font->GetForeColor(&color);
        const long errorColor = static_cast<long>(RGB(170, 55, 40));
        const bool uniformError = color == errorColor;
        const bool uniformOk = color == tomAutoColor || color == static_cast<long>(GetSysColor(COLOR_WINDOWTEXT));
        if (error && !uniformError) {
            font->SetBackColor(tomAutoColor);
            font->SetForeColor(errorColor);
        } else if (!error && !uniformOk) {
            font->SetBackColor(tomAutoColor);
            font->SetForeColor(tomAutoColor);
        }
        font->Release();
    }
    range->Release();
}

void PaintScript() {
    if (!g_scriptEdit || g_coloring) {
        return;
    }
    g_coloring = true;
    bool scriptOn = g_typeScript && SendMessageW(g_typeScript, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ScriptMark marks[500];
    wchar_t first[160] = {};
    int count = 0;
    if (scriptOn) {
        count = ScriptMarks(ReadScriptEdit().c_str(), g_cfg, marks, 500, first, 160);
        SetWindowTextW(g_warn, first);
    }
    if (!EnsureScriptDoc()) {
        g_coloring = false;
        return;
    }
    long frozen = 0;
    long ignored = 0;
    g_scriptDoc->Undo(tomSuspend, &ignored);
    g_scriptDoc->Freeze(&frozen);
    int lines = static_cast<int>(SendMessageW(g_scriptEdit, EM_GETLINECOUNT, 0, 0));
    for (int line = 0; line < lines; ++line) {
        bool bad = false;
        for (int i = 0; i < count; ++i) {
            if (marks[i].line == line + 1) {
                bad = true;
                break;
            }
        }
        ColorScriptLine(line, bad);
    }
    g_scriptDoc->Unfreeze(&frozen);
    g_scriptDoc->Undo(tomResume, &ignored);
    g_coloring = false;
}

void ShowRunLine(int line) {
    if (!g_scriptEdit) {
        return;
    }
    g_paintedLine = line;
    if (line <= 0) {
        CHARRANGE current{};
        SendMessageW(g_scriptEdit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&current));
        current.cpMax = current.cpMin;
        SendMessageW(g_scriptEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&current));
        return;
    }
    int index = line - 1;
    int start = static_cast<int>(SendMessageW(g_scriptEdit, EM_LINEINDEX, index, 0));
    if (start < 0) {
        return;
    }
    int len = static_cast<int>(SendMessageW(g_scriptEdit, EM_LINELENGTH, start, 0));
    CHARRANGE range;
    range.cpMin = start;
    range.cpMax = start + len;
    SendMessageW(g_scriptEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
    SendMessageW(g_scriptEdit, EM_SCROLLCARET, 0, 0);
}

void IndentScriptLines(bool outdent) {
    if (!g_scriptEdit) {
        return;
    }
    CHARRANGE sel{};
    SendMessageW(g_scriptEdit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&sel));
    int line1 = static_cast<int>(SendMessageW(g_scriptEdit, EM_EXLINEFROMCHAR, 0, sel.cpMin));
    int line2 = static_cast<int>(SendMessageW(g_scriptEdit, EM_EXLINEFROMCHAR, 0, sel.cpMax));
    if (line1 < 0) {
        line1 = 0;
    }
    if (line2 < 0) {
        line2 = line1;
    }
    if (sel.cpMax > sel.cpMin) {
        int line2Start = static_cast<int>(SendMessageW(g_scriptEdit, EM_LINEINDEX, line2, 0));
        if (line2Start == sel.cpMax && line2 > line1) {
            --line2;
        }
    }
    if (line2 < line1) {
        line2 = line1;
    }
    g_coloring = true;
    SendMessageW(g_scriptEdit, WM_SETREDRAW, FALSE, 0);
    for (int line = line2; line >= line1; --line) {
        int start = static_cast<int>(SendMessageW(g_scriptEdit, EM_LINEINDEX, line, 0));
        if (start < 0) {
            continue;
        }
        int n = static_cast<int>(SendMessageW(g_scriptEdit, EM_LINELENGTH, start, 0));
        if (n < 0) {
            n = 0;
        }
        std::wstring buf(static_cast<size_t>(n) + 1, L'\0');
        TEXTRANGEW textRange{};
        textRange.chrg.cpMin = start;
        textRange.chrg.cpMax = start + n;
        textRange.lpstrText = buf.data();
        if (n > 0) {
            SendMessageW(g_scriptEdit, EM_GETTEXTRANGE, 0, reinterpret_cast<LPARAM>(&textRange));
        }
        int drop = 0;
        if (outdent && n > 0 && (buf[0] == L'\t' || buf[0] == L' ')) {
            drop = 1;
        }
        std::wstring next;
        if (!outdent) {
            next.push_back(L'\t');
        }
        if (n > drop) {
            next.append(buf.data() + drop, buf.data() + n);
        }
        CHARRANGE range;
        range.cpMin = start;
        range.cpMax = start + n;
        SendMessageW(g_scriptEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
        SendMessageW(g_scriptEdit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(next.c_str()));
    }
    int start = static_cast<int>(SendMessageW(g_scriptEdit, EM_LINEINDEX, line1, 0));
    int endStart = static_cast<int>(SendMessageW(g_scriptEdit, EM_LINEINDEX, line2, 0));
    int endLen = endStart < 0 ? 0 : static_cast<int>(SendMessageW(g_scriptEdit, EM_LINELENGTH, endStart, 0));
    if (start < 0) {
        start = 0;
    }
    if (endStart < 0) {
        endStart = start;
    }
    CHARRANGE again;
    again.cpMin = start;
    again.cpMax = endStart + endLen;
    SendMessageW(g_scriptEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&again));
    SendMessageW(g_scriptEdit, WM_SETREDRAW, TRUE, 0);
    g_coloring = false;
    CommitFromUi();
    InvalidateRect(g_scriptEdit, nullptr, TRUE);
}

void ShowStatus(const StatusMsg& msg) {
    wchar_t text[160];
    wchar_t duration[32];
    FormatDuration(msg.elapsedMs, duration, 32);
    if (msg.state == 4) {
        wcscpy_s(text, L"Action key matches the hotkey or panic key");
        SetRunningUi(false);
    } else if (msg.state == 5) {
        wcscpy_s(text, L"Choose a window to lock");
        SetRunningUi(false);
    } else if (msg.state == 6) {
        std::wstring err;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            err = g_scriptError;
        }
        if (err.empty()) {
            err = L"script error";
        }
        SetWindowTextW(g_status, err.c_str());
        SetRunningUi(false);
        if (g_paintedLine > 0) {
            ShowRunLine(0);
        }
        PaintScript();
        return;
    } else if (msg.state == 2) {
        wcscpy_s(text, L"Holding - release with hotkey, Stop, or panic");
        SetRunningUi(true);
    } else if (msg.state == 1) {
        std::wstring live;
        int runLine = 0;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            live = g_scriptLive;
            runLine = g_runLine;
        }
        if (!live.empty()) {
            swprintf_s(text, L"%s — %llu clicks", live.c_str(), msg.clicks);
        } else {
            swprintf_s(text, L"Running - %llu clicks - %.1f cps", msg.clicks, msg.cps);
        }
        SetRunningUi(true);
        if (runLine != g_paintedLine) {
            ShowRunLine(runLine);
        }
    } else if (msg.state == 3) {
        swprintf_s(text, L"Panic - %llu clicks\n%s", msg.clicks, duration);
        SetRunningUi(false);
    } else if (msg.elapsedMs >= 0) {
        swprintf_s(text, L"Stopped - %llu clicks\n%s", msg.clicks, duration);
        SetRunningUi(false);
    } else if (msg.clicks > 0) {
        swprintf_s(text, L"Stopped - %llu clicks", msg.clicks);
        SetRunningUi(false);
    } else {
        wcscpy_s(text, L"status: idle");
        SetRunningUi(false);
    }
    SetWindowTextW(g_status, text);
    if (msg.state != 1 && msg.state != 6 && g_paintedLine > 0) {
        ShowRunLine(0);
    }
}

void EndCapture(HWND hwnd) {
    g_capture = CaptureTarget::None;
    g_latchedVk = 0;
    g_suspendHotkeys.store(false);
    KillTimer(hwnd, kCaptureTimer);
    UpdateKeyLabel();
    UpdateHotkeyButtons();
    if (g_addKey) {
        SetWindowTextW(g_addKey, L"Add key");
    }
    UpdateScriptKeyButton();
}

bool CaptureWants(int vk) {
    if (g_capture == CaptureTarget::Script &&
        (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2)) {
        return true;
    }
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
    if (g_capture == CaptureTarget::Script) {
        wchar_t token[32];
        if (!ScriptTokenForVk(vk, token, 32)) {
            wcscpy_s(token, L"?");
        }
        SetWindowTextW(g_addKey, token);
        return;
    }
    wchar_t name[64];
    KeyName(static_cast<WORD>(vk), name, 64);
    HWND button = g_setKey;
    if (g_capture == CaptureTarget::Hotkey) {
        button = g_hotkeyBtn;
    } else if (g_capture == CaptureTarget::Panic) {
        button = g_panicBtn;
    } else if (g_capture == CaptureTarget::ScriptKey) {
        button = g_scriptKey;
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
        if (g_capture == CaptureTarget::ScriptKey) {
            ClearNamedKey(hwnd);
            return;
        }
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
        if (g_capture == CaptureTarget::ScriptKey) {
            ClearNamedKey(hwnd);
            return;
        }
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
    } else if (g_capture == CaptureTarget::Script) {
        wchar_t token[32];
        EndCapture(hwnd);
        if (!ScriptTokenForVk(key, token, 32)) {
            SetWindowTextW(g_status, L"No script name for that key");
            return;
        }
        InsertScriptLine(token);
        wchar_t note[64];
        swprintf_s(note, L"Added %s", token);
        SetWindowTextW(g_status, note);
        SetFocus(g_scriptEdit);
        return;
    } else if (g_capture == CaptureTarget::ScriptKey) {
        std::wstring name = ComboName();
        int index = NamedIndexOf(name);
        EndCapture(hwnd);
        if (index < 0) {
            SetWindowTextW(g_status, L"Save the script first");
            return;
        }
        if (key == g_cfg.hotkeyVk) {
            SetWindowTextW(g_status, L"That key is the trigger");
            return;
        }
        if (key == g_cfg.panicVk) {
            SetWindowTextW(g_status, L"That key is the panic key");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_mu);
            for (int i = 0; i < static_cast<int>(g_named.size()); ++i) {
                if (i != index && g_named[static_cast<size_t>(i)].key == key) {
                    SetWindowTextW(g_status, L"That key selects another script");
                    return;
                }
            }
            if (index < static_cast<int>(g_named.size())) {
                g_named[static_cast<size_t>(index)].key = key;
            }
        }
        SaveNamedIndex();
        UpdateScriptKeyButton();
        wchar_t note[80];
        swprintf_s(note, L"Select key set for %s", name.c_str());
        SetWindowTextW(g_status, note);
        return;
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
    } else if (target == CaptureTarget::Script) {
        SetWindowTextW(g_addKey, L"Press...");
    } else if (target == CaptureTarget::ScriptKey) {
        SetWindowTextW(g_scriptKey, L"Press...");
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
    int first = g_capture == CaptureTarget::Script ? VK_LBUTTON : VK_XBUTTON1;
    for (int vk = first; vk < 256; ++vk) {
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

WNDPROC g_editProc = nullptr;
WNDPROC g_richProc = nullptr;
WNDPROC g_comboProc = nullptr;
WNDPROC g_staticProc = nullptr;

bool IsInjectedKey() {
    return GetMessageExtraInfo() == static_cast<LPARAM>(kInjectedExtra);
}

bool IsInjectedMouse(UINT msg) {
    if (!IsInjectedKey()) {
        return false;
    }
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        return true;
    default:
        return false;
    }
}

LRESULT CALLBACK ButtonProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (IsInjectedMouse(msg)) {
        return 0;
    }
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
    if (IsInjectedMouse(msg)) {
        return 0;
    }
    if (IsInjectedKey() && (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR ||
                            msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)) {
        return 0;
    }
    if (hwnd == g_scriptEdit && g_capture == CaptureTarget::Script &&
        (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)) {
        return SendMessageW(GetParent(hwnd), msg, wParam, lParam);
    }
    if (hwnd == g_scriptEdit && msg == WM_MOUSEACTIVATE) {
        return MA_ACTIVATE;
    }
    if (hwnd == g_scriptEdit && wParam == VK_TAB &&
        (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR)) {
        if (msg == WM_KEYDOWN && (GetWindowLongPtrW(hwnd, GWL_STYLE) & ES_READONLY) == 0) {
            IndentScriptLines((GetKeyState(VK_SHIFT) & 0x8000) != 0);
            RememberScriptCaret();
        }
        return 0;
    }
    WNDPROC next = hwnd == g_scriptEdit ? g_richProc : g_editProc;
    if (hwnd == g_scriptEdit && msg == WM_LBUTTONDOWN && (GetKeyState(VK_SHIFT) & 0x8000) == 0) {
        LRESULT result = CallWindowProcW(next, hwnd, msg, wParam, lParam);
        PlaceScriptCaretAtClick(hwnd, lParam);
        return result;
    }
    LRESULT result = CallWindowProcW(next, hwnd, msg, wParam, lParam);
    if (hwnd == g_scriptEdit && !IsInjectedKey() && (msg == WM_KEYUP || msg == WM_CHAR)) {
        RememberScriptCaret();
    }
    return result;
}

LRESULT CALLBACK ComboProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (IsInjectedMouse(msg)) {
        return 0;
    }
    return CallWindowProcW(g_comboProc, hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK StaticProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return CallWindowProcW(g_staticProc, hwnd, msg, wParam, lParam);
}

BOOL CALLBACK SubclassInputChild(HWND child, LPARAM) {
    wchar_t cls[32];
    if (GetClassNameW(child, cls, 32) == 0) {
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
    } else if (wcscmp(cls, L"RICHEDIT50W") == 0) {
        g_richProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(child, GWLP_WNDPROC));
        SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc));
    } else if (wcscmp(cls, L"ComboBox") == 0) {
        if (!g_comboProc) {
            g_comboProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(child, GWLP_WNDPROC));
        }
        SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ComboProc));
    } else if (wcscmp(cls, L"Static") == 0) {
        if (!g_staticProc) {
            g_staticProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(child, GWLP_WNDPROC));
        }
        SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(StaticProc));
    }
    return TRUE;
}

struct TipSpec {
    HWND tool;
    const wchar_t* text;
};

std::vector<TipSpec> g_tips;
bool g_tipsReady = false;

void InstallTip(HWND tool, const wchar_t* text) {
    TOOLINFOW info{};
    info.cbSize = TTTOOLINFOW_V2_SIZE;
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = GetParent(tool);
    info.uId = reinterpret_cast<UINT_PTR>(tool);
    info.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}

void AddTip(HWND tool, const wchar_t* text) {
    if (!g_tip || !tool || !text || !text[0]) {
        return;
    }
    g_tips.push_back({tool, text});
    if (g_tipsReady) {
        InstallTip(tool, text);
    }
}

void RehookTips() {
    g_tipsReady = true;
    for (const TipSpec& tip : g_tips) {
        if (IsWindow(tip.tool)) {
            InstallTip(tip.tool, tip.text);
        }
    }
}

void EnsureTip(HWND hwnd, HINSTANCE inst) {
    if (g_tip) {
        return;
    }
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&common);
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr,
                            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                            hwnd, nullptr, inst, nullptr);
    if (!g_tip) {
        return;
    }
    SetWindowPos(g_tip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 280);
    SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_INITIAL, 400);
    SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 12000);
}

bool TipWindow(HWND hwnd) {
    if (!hwnd) {
        return false;
    }
    HWND root = GetAncestor(hwnd, GA_ROOT);
    return root == g_hwnd.load() || (g_helpWnd && root == g_helpWnd);
}

void RelayTipMouse(HWND coordHwnd, WPARAM wParam, LPARAM lParam) {
    if (!g_tip || !coordHwnd) {
        return;
    }
    HWND owner = coordHwnd;
    HWND parent = GetParent(coordHwnd);
    if (parent && TipWindow(parent)) {
        owner = parent;
    }
    POINT screen{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
    ClientToScreen(coordHwnd, &screen);
    POINT client = screen;
    ScreenToClient(owner, &client);
    MSG relay{};
    relay.hwnd = owner;
    relay.message = WM_MOUSEMOVE;
    relay.wParam = wParam;
    relay.lParam = MAKELPARAM(client.x, client.y);
    relay.time = GetMessageTime();
    relay.pt = screen;
    SendMessageW(g_tip, TTM_RELAYEVENT, GetMessageExtraInfo(), reinterpret_cast<LPARAM>(&relay));
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
    EnsureTip(hwnd, inst);
    const wchar_t* repeatTip = L"How many clicks to make, then stop. 0 runs until you stop it.";
    const wchar_t* triggerTip = L"Starts and stops the run. Click, then press a key.";
    const wchar_t* panicTip = L"Stops immediately. Click, then press a key.";
    const wchar_t* holdTip = L"How long the hold lasts. Hours, minutes, seconds, milliseconds.";
    const wchar_t* nameTip = L"Saved scripts. Pick one to load it, or type a new name.";

    g_status = MakeLabel(hwnd, inst, L"status: idle", 8, 4, 188, 28, ID_STATUS);
    AddTip(g_status, L"What the clicker is doing, and how long the last run lasted.");

    AddTip(MakeGroup(hwnd, inst, L"HH:MM:SS:ms", 6, 34, 192, 44),
           L"Time between clicks. Hours, minutes, seconds, milliseconds.");
    g_hh = MakeEdit(hwnd, inst, 14, 52, 32, ID_HH);
    AddTip(g_hh, L"Hours between clicks.");
    MakeLabel(hwnd, inst, L":", 48, 54, 8, 16);
    g_mm = MakeEdit(hwnd, inst, 56, 52, 32, ID_MM);
    AddTip(g_mm, L"Minutes between clicks.");
    MakeLabel(hwnd, inst, L":", 90, 54, 8, 16);
    g_ss = MakeEdit(hwnd, inst, 98, 52, 32, ID_SS);
    AddTip(g_ss, L"Seconds between clicks.");
    MakeLabel(hwnd, inst, L":", 132, 54, 8, 16);
    g_ms = MakeEdit(hwnd, inst, 140, 52, 36, ID_MS);
    AddTip(g_ms, L"Milliseconds between clicks.");

    g_repeatLabel = MakeLabel(hwnd, inst, L"stop after", 8, 86, 62, 16);
    AddTip(g_repeatLabel, repeatTip);
    g_repeatN = MakeEdit(hwnd, inst, 72, 84, 40, ID_REPEAT_N);
    AddTip(g_repeatN, repeatTip);
    g_repeatHint = MakeLabel(hwnd, inst, L"0 = no limit", 118, 86, 76, 16);
    AddTip(g_repeatHint, repeatTip);
    SendMessageW(g_repeatN, EM_SETLIMITTEXT, 7, 0);

    g_start = CreateWindowExW(0, L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                              8, 110, 188, 24, hwnd, MenuId(ID_START), inst, nullptr);
    AddTip(g_start, L"Start clicking.");
    g_stop = CreateWindowExW(0, L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                             8, 138, 188, 26, hwnd, MenuId(ID_STOP), inst, nullptr);
    AddTip(g_stop, L"Stop, and let go of anything still held down.");
    EnableWindow(g_stop, FALSE);

    AddTip(MakeGroup(hwnd, inst, L"press / toggle", 6, 172, 192, 100),
           L"How the trigger key starts and stops.");
    g_hotPress = MakeRadio(hwnd, inst, L"press", 18, 190, 168, ID_HOT_PRESS, true);
    AddTip(g_hotPress, L"Run only while the trigger key is held down.");
    g_hotToggle = MakeRadio(hwnd, inst, L"toggle", 18, 208, 168, ID_HOT_TOGGLE, false);
    AddTip(g_hotToggle, L"Press the trigger once to start. Press it again to stop.");
    AddTip(MakeLabel(hwnd, inst, L"trigger", 18, 230, 50, 16), triggerTip);
    g_hotkeyBtn = CreateWindowExW(0, L"BUTTON", L"F6", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  70, 226, 116, 20, hwnd, MenuId(ID_HOTKEY), inst, nullptr);
    AddTip(g_hotkeyBtn, triggerTip);
    AddTip(MakeLabel(hwnd, inst, L"panic", 18, 252, 50, 16), panicTip);
    g_panicBtn = CreateWindowExW(0, L"BUTTON", L"F12", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 70, 248, 116, 20, hwnd, MenuId(ID_PANIC), inst, nullptr);
    AddTip(g_panicBtn, panicTip);

    g_mouseGroup = MakeGroup(hwnd, inst, L"mouse / key", 6, 280, 192, 118);
    AddTip(g_mouseGroup, L"What gets pressed.");
    g_btnLeft = MakeRadio(hwnd, inst, L"left", 18, 298, 168, ID_BTN_LEFT, true);
    AddTip(g_btnLeft, L"Click the left mouse button.");
    g_btnMiddle = MakeRadio(hwnd, inst, L"middle", 18, 316, 168, ID_BTN_MIDDLE, false);
    AddTip(g_btnMiddle, L"Click the middle mouse button.");
    g_btnRight = MakeRadio(hwnd, inst, L"right", 18, 334, 168, ID_BTN_RIGHT, false);
    AddTip(g_btnRight, L"Click the right mouse button.");
    g_keyMode = MakeRadio(hwnd, inst, L"key", 18, 352, 168, ID_KEY_MODE, false);
    AddTip(g_keyMode, L"Press a keyboard key instead of a mouse button.");
    g_setKey = CreateWindowExW(0, L"BUTTON", L"Space", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               32, 370, 150, 20, hwnd, MenuId(ID_SET_KEY), inst, nullptr);
    AddTip(g_setKey, L"The key that gets pressed. Click, then press a key.");

    AddTip(MakeGroup(hwnd, inst, L"click", 6, 406, 192, 146),
           L"One click, a double click, a hold, or the script.");
    g_typeSingle = MakeRadio(hwnd, inst, L"single", 18, 424, 168, ID_TYPE_SINGLE, true);
    AddTip(g_typeSingle, L"One click each time.");
    g_typeDouble = MakeRadio(hwnd, inst, L"double", 18, 442, 168, ID_TYPE_DOUBLE, false);
    AddTip(g_typeDouble, L"Two clicks each time.");
    g_typeHold = MakeRadio(hwnd, inst, L"until stop", 18, 460, 168, ID_TYPE_HOLD, false);
    AddTip(g_typeHold, L"Hold the button down until you stop.");
    g_typeTimed = MakeRadio(hwnd, inst, L"for", 18, 478, 168, ID_TYPE_TIMED, false);
    AddTip(g_typeTimed, L"Hold the button down for the time below, then let go.");
    g_holdHh = MakeEdit(hwnd, inst, 18, 498, 32, ID_HOLD_HH);
    AddTip(g_holdHh, holdTip);
    g_holdSep1 = MakeLabel(hwnd, inst, L":", 52, 500, 8, 16);
    g_holdMm = MakeEdit(hwnd, inst, 60, 498, 32, ID_HOLD_MM);
    AddTip(g_holdMm, holdTip);
    g_holdSep2 = MakeLabel(hwnd, inst, L":", 94, 500, 8, 16);
    g_holdSs = MakeEdit(hwnd, inst, 102, 498, 32, ID_HOLD_SS);
    AddTip(g_holdSs, holdTip);
    g_holdSep3 = MakeLabel(hwnd, inst, L":", 136, 500, 8, 16);
    g_holdMs = MakeEdit(hwnd, inst, 144, 498, 36, ID_HOLD_MS);
    AddTip(g_holdMs, holdTip);
    g_typeScript = MakeRadio(hwnd, inst, L"script", 18, 522, 168, ID_TYPE_SCRIPT, false);
    AddTip(g_typeScript, L"Run the script instead of one repeated click.");

    AddTip(MakeGroup(hwnd, inst, L"target", 6, 560, 192, 112), L"Where the click is allowed to land.");
    g_fixed = CreateWindowExW(0, L"BUTTON", L"Fixed spot",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTOCHECKBOX,
                              16, 578, 160, 18, hwnd, MenuId(ID_FIXED), inst, nullptr);
    AddTip(g_fixed, L"Move the cursor to one spot before each click.");
    g_pick = CreateWindowExW(0, L"BUTTON", L"Choose spot", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                             28, 598, 156, 20, hwnd, MenuId(ID_PICK), inst, nullptr);
    AddTip(g_pick, L"Click the spot on the screen to use.");
    g_lock = CreateWindowExW(0, L"BUTTON", L"One window",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                             16, 624, 160, 18, hwnd, MenuId(ID_LOCK), inst, nullptr);
    AddTip(g_lock, L"Click only while the chosen window is in front.");
    g_lockPick = CreateWindowExW(0, L"BUTTON", L"Choose window", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 28, 644, 156, 20, hwnd, MenuId(ID_LOCK_PICK), inst, nullptr);
    AddTip(g_lockPick, L"Click the window to stay in.");

    g_warn = MakeLabel(hwnd, inst, L"", 8, 680, 188, 28, ID_WARN);
    AddTip(g_warn, L"The first problem in the script, or a note about the timing.");
    g_scriptList = CreateWindowExW(
        0, L"COMBOBOX", L"",
        WS_CHILD | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_CLIPSIBLINGS,
        214, 36, 208, 180, hwnd, MenuId(ID_SCRIPT_LIST), inst, nullptr);
    SendMessageW(g_scriptList, CB_LIMITTEXT, 32, 0);
    AddTip(g_scriptList, nameTip);
    COMBOBOXINFO combo{};
    combo.cbSize = sizeof(combo);
    if (GetComboBoxInfo(g_scriptList, &combo)) {
        AddTip(combo.hwndItem, nameTip);
        AddTip(combo.hwndList, nameTip);
    }
    g_scriptSave = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS,
                                   214, 64, 66, 22, hwnd, MenuId(ID_SCRIPT_SAVE), inst, nullptr);
    AddTip(g_scriptSave, L"Save the box under that name. The same name overwrites.");
    g_scriptDel = CreateWindowExW(0, L"BUTTON", L"Delete", WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS,
                                  284, 64, 64, 22, hwnd, MenuId(ID_SCRIPT_DEL), inst, nullptr);
    AddTip(g_scriptDel, L"Delete that saved script and its key.");
    g_scriptKey = CreateWindowExW(0, L"BUTTON", L"Key", WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS,
                                  352, 64, 70, 22, hwnd, MenuId(ID_SCRIPT_KEY), inst, nullptr);
    AddTip(g_scriptKey, L"Loads this script while stopped. Click, then press a key. Escape clears it.");
    g_helpBtn = CreateWindowExW(0, L"BUTTON", L"Help", WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS,
                                214, 90, 100, 22, hwnd, MenuId(ID_HELP), inst, nullptr);
    AddTip(g_helpBtn, L"Open a short guide to the script.");
    g_addKey = CreateWindowExW(0, L"BUTTON", L"Add key", WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS,
                               318, 90, 104, 22, hwnd, MenuId(ID_ADD_KEY), inst, nullptr);
    AddTip(g_addKey, L"Press a key or mouse button and write its name where the cursor was. If you have not clicked in the script, it goes at the bottom.");
    LoadLibraryW(L"Msftedit.dll");
    g_scriptEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"RICHEDIT50W", L"",
        WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
            WS_VSCROLL | WS_HSCROLL | ES_NOHIDESEL,
        214, 116, 208, 592, hwnd, MenuId(ID_SCRIPT), inst, nullptr);
    AddTip(g_scriptEdit, L"The script that runs. Tab indents the selected lines. Shift+Tab pulls them back.");
    SetWindowPos(g_scriptList, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SendMessageW(g_scriptEdit, EM_EXLIMITTEXT, 0, 8000);
    SendMessageW(g_scriptEdit, EM_SETEVENTMASK, 0, ENM_CHANGE);
    EnumChildWindows(hwnd, SubclassInputChild, 0);
    RehookTips();
}

void OnStart() {
    CommitFromUi();
    if (g_cfg.clickType != ClickType::Script && ActionConflictsWithHotkeys(g_cfg)) {
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
        g_scriptFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                   FIXED_PITCH | FF_MODERN, L"Consolas");
        SendMessageW(g_scriptEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_scriptFont), FALSE);
        if (g_tip) {
            SendMessageW(g_tip, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
        }
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
        if (id == ID_HELP && code == BN_CLICKED) {
            ShowHelp(hwnd);
            return 0;
        }
        if (id == ID_ADD_KEY && code == BN_CLICKED) {
            if (g_capture != CaptureTarget::Script) {
                BeginCapture(hwnd, CaptureTarget::Script);
                SetWindowTextW(g_status, L"Press a key or mouse button");
            }
            return 0;
        }
        if (id == ID_SCRIPT_SAVE && code == BN_CLICKED) {
            SaveCurrentNamed();
            return 0;
        }
        if (id == ID_SCRIPT_DEL && code == BN_CLICKED) {
            DeleteCurrentNamed();
            return 0;
        }
        if (id == ID_SCRIPT_KEY && code == BN_CLICKED) {
            if (g_capture != CaptureTarget::ScriptKey) {
                if (NamedIndexOf(ComboName()) < 0) {
                    SetWindowTextW(g_status, L"Save the script first");
                } else {
                    BeginCapture(hwnd, CaptureTarget::ScriptKey);
                    SetWindowTextW(g_status, L"Press a key. Escape clears it.");
                }
            }
            return 0;
        }
        if (id == ID_SCRIPT_LIST && code == CBN_SETFOCUS) {
            g_suspendHotkeys.store(true);
            return 0;
        }
        if (id == ID_SCRIPT_LIST && code == CBN_KILLFOCUS) {
            if (g_capture == CaptureTarget::None && GetFocus() != g_scriptEdit) {
                g_suspendHotkeys.store(false);
            }
            return 0;
        }
        if (id == ID_SCRIPT_LIST && code == CBN_DROPDOWN) {
            SetWindowPos(g_scriptList, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            return 0;
        }
        if (id == ID_SCRIPT_LIST && code == CBN_CLOSEUP) {
            SetWindowPos(g_scriptList, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            return 0;
        }
        if (id == ID_SCRIPT_LIST && code == CBN_SELCHANGE) {
            if (!g_loading) {
                int sel = static_cast<int>(SendMessageW(g_scriptList, CB_GETCURSEL, 0, 0));
                if (sel >= 0) {
                    wchar_t item[40] = {};
                    SendMessageW(g_scriptList, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(item));
                    LoadNamedByName(item);
                }
            }
            return 0;
        }
        if (id == ID_SCRIPT && code == EN_SETFOCUS) {
            g_suspendHotkeys.store(true);
            return 0;
        }
        if (id == ID_SCRIPT && code == EN_KILLFOCUS) {
            if (g_capture == CaptureTarget::None) {
                g_suspendHotkeys.store(false);
            }
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
        if (g_coloring && id == ID_SCRIPT) {
            return 0;
        }
        if (code == EN_CHANGE || code == BN_CLICKED || code == CBN_SELCHANGE) {
            if (id == ID_BTN_LEFT || id == ID_BTN_MIDDLE || id == ID_BTN_RIGHT || id == ID_KEY_MODE) {
                UpdateInputMode(true);
            }
            CommitFromUi();
            if (id == ID_TYPE_SINGLE || id == ID_TYPE_DOUBLE || id == ID_TYPE_HOLD || id == ID_TYPE_TIMED ||
                id == ID_TYPE_SCRIPT) {
                bool script = SendMessageW(g_typeScript, BM_GETCHECK, 0, 0) == BST_CHECKED;
                LayoutForScript(script);
                ApplyUnusedControls(true);
            }
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
    case WM_MOUSEMOVE: {
        POINT pt{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
        HWND child = ChildWindowFromPointEx(hwnd, pt, CWP_SKIPINVISIBLE);
        if (!child || child == hwnd || !IsWindowEnabled(child)) {
            RelayTipMouse(hwnd, wParam, lParam);
        }
        break;
    }
    case WM_CLICKER_LOAD:
        LoadNamedByKey(static_cast<WORD>(wParam));
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
        if (g_scriptDoc) {
            g_scriptDoc->Release();
            g_scriptDoc = nullptr;
        }
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
        if (g_scriptFont) {
            DeleteObject(g_scriptFont);
            g_scriptFont = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int RunWindow(HINSTANCE inst) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
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

    RECT rect{0, 0, 204, 716};
    AdjustWindowRect(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND hwnd = CreateWindowExW(
        0, L"ClickerWindow", L"Clicker",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        CoUninitialize();
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    int code = static_cast<int>(message.wParam);
    CoUninitialize();
    return code;
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
