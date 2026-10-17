#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>

enum class ActionKind { Mouse, Key };
enum class ClickType { Single, Double, Hold };
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
};

// poll returns 0 to keep going, 1 to stop, 2 for panic.
// onStatus runs on the click thread; it must not touch window controls.
struct RunControl {
    int (*poll)(void* ctx);
    void (*onStatus)(void* ctx, unsigned long long clicks, double cps, bool holding);
    void (*onEvent)(void* ctx, bool down, int64_t qpc);
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

// Writes a timestamp log. Returns 0 when pairing and interval error are within tolerance.
int RunSelfTest(const wchar_t* logPath);

const wchar_t* MouseButtonName(int button);
// Caller provides a buffer of at least 64 wchars.
void KeyName(WORD vk, wchar_t* out, int outChars);
