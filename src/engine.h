#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>

enum class ActionKind { Mouse, Key };
enum class ClickType { Single, Double, Hold, TimedHold, Script };
enum class RepeatMode { UntilStopped, Count };
enum class HotkeyMode { Toggle, Hold };

struct Config {
    ActionKind action = ActionKind::Mouse;
    int mouseButton = 0;  // 0 left, 1 right, 2 middle, 3 mouse4, 4 mouse5
    WORD actionVk = VK_SPACE;
    ClickType clickType = ClickType::Single;
    RepeatMode repeatMode = RepeatMode::UntilStopped;
    int repeatCount = 10;
    int64_t intervalMs = 100;
    int holdMs = 20;
    int64_t timedHoldMs = 1000;
    HotkeyMode hotkeyMode = HotkeyMode::Toggle;
    WORD hotkeyVk = VK_F6;
    WORD panicVk = VK_F12;
    bool fixedPos = false;
    int fixedX = 0;
    int fixedY = 0;
    bool lockWindow = false;
    HWND lockHwnd = nullptr;
};

enum class StopReason { Done = 0, Stopped = 1, Panic = 2 };

struct RunStats {
    unsigned long long clicks = 0;
    StopReason reason = StopReason::Done;
    int64_t elapsedMs = -1;
};

// poll returns 0 to keep going, 1 to stop, 2 for panic.
// onStatus runs on the click thread; it must not touch window controls.
struct RunControl {
    int (*poll)(void* ctx);
    void (*onStatus)(void* ctx, unsigned long long clicks, double cps, bool holding);
    void (*onEvent)(void* ctx, bool down, int64_t qpc);
    // line is 1-based. remainSec and repsLeft are negative when that step has no countdown.
    void (*onLine)(void* ctx, int line, double remainSec, int repsLeft, unsigned long long clicks);
    void* ctx;
    bool inject;
};

void EngineInit();
int64_t QpcNow();
double QpcToMs(int64_t ticks);

int64_t IntervalMsFromParts(int hours, int minutes, int seconds, int millis);
void SplitInterval(int64_t intervalMs, int& hours, int& minutes, int& seconds, int& millis);

// Shortest down-to-down period that still leaves the button up between clicks.
int64_t EffectivePeriodMs(const Config& cfg);

bool ActionConflictsWithHotkeys(const Config& cfg);

RunStats RunSession(const Config& cfg, const RunControl& ctl);

// Script runner. error receives a line message when the text cannot be run.
RunStats RunScript(const wchar_t* text, const Config& cfg, const RunControl& ctl, wchar_t* error, int errorChars);

void EngineInject(const Config& cfg, bool down, const RunControl& ctl);
int EngineWaitMs(double ms, const RunControl& ctl);
bool EngineInputAllowed(const Config& cfg);

// Writes a timestamp log. Returns 0 when pairing and interval error are within tolerance.
int RunSelfTest(const wchar_t* logPath);

// Mark on keyboard input this program injects, so its own buttons can ignore it.
constexpr ULONG_PTR kInjectedExtra = 0x434C4B31;

const wchar_t* MouseButtonName(int button);
// Caller provides a buffer of at least 64 wchars.
void KeyName(WORD vk, wchar_t* out, int outChars);

// Script token for a key or mouse button, such as "lshift" or "lef".
bool ScriptTokenForVk(int vk, wchar_t* out, int outChars);

// kind 1: the line will not run. kind 2: it runs, but the hold does not match.
struct ScriptMark {
    int line = 0;
    int kind = 0;
};

int ScriptMarks(const wchar_t* text, const Config& cfg, ScriptMark* marks, int maxMarks, wchar_t* first, int firstChars);
