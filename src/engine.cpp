#include "engine.h"

#include <timeapi.h>

#include <cstdio>
#include <cstring>

namespace {

int64_t g_freq = 0;

constexpr int64_t kDoubleGapMs = 30;
constexpr UINT kTimerFlagsHighRes = 0x00000002;  // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION

bool IsExtendedVk(WORD vk) {
    switch (vk) {
    case VK_RMENU:
    case VK_RCONTROL:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_UP:
    case VK_RIGHT:
    case VK_DOWN:
    case VK_NUMLOCK:
    case VK_DIVIDE:
    case VK_SNAPSHOT:
    case VK_LWIN:
    case VK_RWIN:
    case VK_APPS:
        return true;
    default:
        return false;
    }
}

HANDLE CreateHiResTimer() {
    HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, kTimerFlagsHighRes, TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (!timer) {
        timer = CreateWaitableTimerExW(
            nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
    }
    return timer;
}

void SleepQpc(HANDLE timer, int64_t ticks) {
    if (ticks <= 0 || g_freq <= 0) {
        return;
    }
    int64_t hundredNs = (ticks * 10000000) / g_freq;
    if (hundredNs < 1) {
        hundredNs = 1;
    }
    LARGE_INTEGER due;
    due.QuadPart = -hundredNs;
    if (!timer || !SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        DWORD ms = static_cast<DWORD>((ticks * 1000) / g_freq);
        if (ms < 1) {
            ms = 1;
        }
        Sleep(ms);
        return;
    }
    WaitForSingleObject(timer, INFINITE);
}

// Returns 0 when the deadline is reached. Otherwise returns the poll stop code.
// The last few milliseconds are spun so a coarse sleep cannot swallow a short hold.
int WaitUntil(HANDLE timer, int64_t deadline, const RunControl& ctl) {
    const int64_t spinTail = (g_freq * 3) / 1000;
    const int64_t maxChunk = g_freq / 100;  // 10 ms, so panic is noticed quickly
    while (QpcNow() < deadline) {
        if (ctl.poll) {
            int code = ctl.poll(ctl.ctx);
            if (code != 0) {
                return code;
            }
        }
        int64_t remain = deadline - QpcNow();
        if (remain <= spinTail) {
            while (QpcNow() < deadline) {
                YieldProcessor();
            }
            return 0;
        }
        int64_t chunk = remain - spinTail;
        if (chunk > maxChunk) {
            chunk = maxChunk;
        }
        SleepQpc(timer, chunk);
    }
    return 0;
}

void SendMouse(int button, bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    switch (button) {
    case 1:
        in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    case 2:
        in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    case 3:
        in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        in.mi.mouseData = XBUTTON1;
        break;
    case 4:
        in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        in.mi.mouseData = XBUTTON2;
        break;
    default:
        in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    }
    in.mi.dwExtraInfo = kInjectedExtra;
    SendInput(1, &in, sizeof(INPUT));
}

void SendKey(WORD vk, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    WORD scan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwExtraInfo = kInjectedExtra;
    if (scan == 0) {
        in.ki.wVk = vk;
        in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    } else {
        // Scan codes are what Raw Input / DirectInput games actually read.
        in.ki.wScan = scan;
        in.ki.dwFlags = KEYEVENTF_SCANCODE;
        if (!down) {
            in.ki.dwFlags |= KEYEVENTF_KEYUP;
        }
        if (IsExtendedVk(vk)) {
            in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
    }
    SendInput(1, &in, sizeof(INPUT));
}

void MoveCursor(int x, int y) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 1 || vh <= 1) {
        return;
    }
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = static_cast<LONG>(((x - vx) * 65535.0) / (vw - 1));
    in.mi.dy = static_cast<LONG>(((y - vy) * 65535.0) / (vh - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof(INPUT));
    SetCursorPos(x, y);
}

bool CursorOverWindow(HWND target) {
    if (!target || !IsWindow(target)) {
        return false;
    }
    POINT pt;
    if (!GetCursorPos(&pt)) {
        return false;
    }
    HWND under = WindowFromPoint(pt);
    if (!under) {
        return false;
    }
    HWND rootUnder = GetAncestor(under, GA_ROOT);
    HWND rootTarget = GetAncestor(target, GA_ROOT);
    if (!rootUnder) {
        rootUnder = under;
    }
    if (!rootTarget) {
        rootTarget = target;
    }
    return rootUnder == rootTarget;
}

bool InputAllowed(const Config& cfg) {
    if (!cfg.lockWindow) {
        return true;
    }
    return CursorOverWindow(cfg.lockHwnd);
}

void Inject(const Config& cfg, bool down, bool inject, const RunControl& ctl) {
    int64_t now = QpcNow();
    if (ctl.onEvent) {
        ctl.onEvent(ctl.ctx, down, now);
    }
    if (!inject) {
        return;
    }
    if (down && cfg.fixedPos) {
        MoveCursor(cfg.fixedX, cfg.fixedY);
    }
    if (cfg.action == ActionKind::Key) {
        if (cfg.actionVk == VK_XBUTTON1) {
            SendMouse(3, down);
        } else if (cfg.actionVk == VK_XBUTTON2) {
            SendMouse(4, down);
        } else {
            SendKey(cfg.actionVk, down);
        }
    } else {
        SendMouse(cfg.mouseButton, down);
    }
}

struct PeriodGuard {
    bool on = false;
    PeriodGuard() {
        if (timeBeginPeriod(1) == TIMERR_NOERROR) {
            on = true;
        }
    }
    ~PeriodGuard() {
        if (on) {
            timeEndPeriod(1);
        }
    }
};

int ClicksPerBurst(const Config& cfg) {
    return cfg.clickType == ClickType::Double ? 2 : 1;
}

}  // namespace

void EngineInject(const Config& cfg, bool down, const RunControl& ctl) {
    Inject(cfg, down, ctl.inject, ctl);
}

int EngineWaitMs(double ms, const RunControl& ctl) {
    if (g_freq <= 0) {
        EngineInit();
    }
    if (ms < 0) {
        ms = 0;
    }
    HANDLE timer = CreateHiResTimer();
    int64_t ticks = static_cast<int64_t>((ms * static_cast<double>(g_freq)) / 1000.0);
    int code = WaitUntil(timer, QpcNow() + ticks, ctl);
    if (timer) {
        CloseHandle(timer);
    }
    return code;
}

bool EngineInputAllowed(const Config& cfg) {
    return InputAllowed(cfg);
}

void EngineInit() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_freq = freq.QuadPart;
}

int64_t QpcNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

double QpcToMs(int64_t ticks) {
    if (g_freq <= 0) {
        return 0;
    }
    return (static_cast<double>(ticks) * 1000.0) / static_cast<double>(g_freq);
}

int64_t IntervalMsFromParts(int hours, int minutes, int seconds, int millis) {
    if (hours < 0) hours = 0;
    if (minutes < 0) minutes = 0;
    if (seconds < 0) seconds = 0;
    if (millis < 0) millis = 0;
    return ((static_cast<int64_t>(hours) * 60 + minutes) * 60 + seconds) * 1000 + millis;
}

void SplitInterval(int64_t intervalMs, int& hours, int& minutes, int& seconds, int& millis) {
    if (intervalMs < 0) {
        intervalMs = 0;
    }
    millis = static_cast<int>(intervalMs % 1000);
    int64_t totalSeconds = intervalMs / 1000;
    seconds = static_cast<int>(totalSeconds % 60);
    int64_t totalMinutes = totalSeconds / 60;
    minutes = static_cast<int>(totalMinutes % 60);
    hours = static_cast<int>(totalMinutes / 60);
}

int64_t EffectivePeriodMs(const Config& cfg) {
    int64_t hold = cfg.holdMs < 1 ? 1 : cfg.holdMs;
    if (cfg.clickType == ClickType::Hold || cfg.clickType == ClickType::TimedHold ||
        cfg.clickType == ClickType::Script) {
        return hold;
    }
    int clicks = ClicksPerBurst(cfg);
    int64_t burst = clicks * hold + static_cast<int64_t>(clicks - 1) * kDoubleGapMs;
    // 1 ms gap so the button is actually up before the next down.
    int64_t minPeriod = burst + 1;
    if (cfg.intervalMs > minPeriod) {
        return cfg.intervalMs;
    }
    return minPeriod;
}

bool ActionConflictsWithHotkeys(const Config& cfg) {
    if (cfg.action != ActionKind::Key) {
        return false;
    }
    return cfg.actionVk == cfg.hotkeyVk || cfg.actionVk == cfg.panicVk;
}

const wchar_t* MouseButtonName(int button) {
    switch (button) {
    case 1: return L"Right";
    case 2: return L"Middle";
    case 3: return L"Mouse4";
    case 4: return L"Mouse5";
    default: return L"Left";
    }
}

void KeyName(WORD vk, wchar_t* out, int outChars) {
    if (!out || outChars <= 0) {
        return;
    }
    out[0] = 0;
    if (vk == VK_XBUTTON1) {
        wcscpy_s(out, outChars, L"Mouse4");
        return;
    }
    if (vk == VK_XBUTTON2) {
        wcscpy_s(out, outChars, L"Mouse5");
        return;
    }
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (scan != 0) {
        LONG lp = static_cast<LONG>(scan << 16);
        if (IsExtendedVk(vk)) {
            lp |= 1 << 24;
        }
        if (GetKeyNameTextW(lp, out, outChars) > 0 && out[0] != 0) {
            return;
        }
    }
    swprintf_s(out, outChars, L"VK%u", static_cast<unsigned>(vk));
}

RunStats RunSession(const Config& cfg, const RunControl& ctl) {
    RunStats stats;
    if (g_freq <= 0) {
        EngineInit();
    }

    HANDLE thread = GetCurrentThread();
    int previousPriority = GetThreadPriority(thread);
    SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
    PeriodGuard period;
    HANDLE timer = CreateHiResTimer();
    const int64_t sessionStart = QpcNow();

    auto finish = [&](StopReason reason) {
        stats.reason = reason;
        double elapsed = QpcToMs(QpcNow() - sessionStart);
        if (elapsed < 0) {
            elapsed = 0;
        }
        stats.elapsedMs = static_cast<int64_t>(elapsed + 0.5);
        if (timer) {
            CloseHandle(timer);
        }
        SetThreadPriority(thread, previousPriority);
        return stats;
    };

    auto stopped = [&]() -> StopReason {
        if (!ctl.poll) {
            return StopReason::Done;
        }
        int code = ctl.poll(ctl.ctx);
        if (code == 2) {
            return StopReason::Panic;
        }
        if (code != 0) {
            return StopReason::Stopped;
        }
        return StopReason::Done;
    };

    StopReason early = stopped();
    if (early != StopReason::Done) {
        return finish(early);
    }

    const bool timedHold = cfg.clickType == ClickType::TimedHold;
    const bool holding = cfg.clickType == ClickType::Hold || timedHold;
    if (ctl.onStatus) {
        ctl.onStatus(ctl.ctx, 0, 0, holding);
    }

    if (holding) {
        bool down = false;
        bool started = false;
        int64_t holdDeadline = 0;
        int64_t lastStatus = QpcNow();
        const int64_t timedMs = cfg.timedHoldMs < 1 ? 1 : cfg.timedHoldMs;
        while (true) {
            StopReason why = stopped();
            if (why != StopReason::Done) {
                if (down) {
                    Inject(cfg, false, ctl.inject, ctl);
                    stats.clicks = 1;
                }
                return finish(why);
            }
            if (timedHold && started && QpcNow() >= holdDeadline) {
                if (down) {
                    Inject(cfg, false, ctl.inject, ctl);
                    stats.clicks = 1;
                }
                return finish(StopReason::Done);
            }
            bool allowed = InputAllowed(cfg);
            if (allowed && !down) {
                Inject(cfg, true, ctl.inject, ctl);
                down = true;
                stats.clicks = 1;
                if (timedHold && !started) {
                    started = true;
                    holdDeadline = QpcNow() + (timedMs * g_freq) / 1000;
                }
            } else if (!allowed && down) {
                Inject(cfg, false, ctl.inject, ctl);
                down = false;
            }
            int64_t now = QpcNow();
            if (ctl.onStatus && now - lastStatus >= g_freq / 5) {
                lastStatus = now;
                ctl.onStatus(ctl.ctx, stats.clicks, 0, true);
            }
            SleepQpc(timer, g_freq / 50);
        }
    }

    const int64_t periodMs = EffectivePeriodMs(cfg);
    const int64_t holdMs = cfg.holdMs < 1 ? 1 : cfg.holdMs;
    const int64_t periodTicks = (periodMs * g_freq) / 1000;
    const int64_t holdTicks = (holdMs * g_freq) / 1000;
    const int64_t gapTicks = (kDoubleGapMs * g_freq) / 1000;
    const int clicksNeeded = ClicksPerBurst(cfg);
    const int64_t origin = QpcNow();

    bool buttonDown = false;
    int clicksInBurst = 0;
    int burstsDone = 0;
    int64_t deadline = origin;
    int64_t lastStatus = origin;
    enum class Phase { NeedDown, NeedUp } phase = Phase::NeedDown;

    auto release = [&]() {
        if (buttonDown) {
            Inject(cfg, false, ctl.inject, ctl);
            buttonDown = false;
        }
    };

    while (true) {
        if (QpcNow() < deadline) {
            if (phase == Phase::NeedDown && clicksInBurst == 0 &&
                cfg.repeatMode == RepeatMode::Count && burstsDone >= cfg.repeatCount) {
                release();
                return finish(StopReason::Done);
            }
            int code = WaitUntil(timer, deadline, ctl);
            if (code != 0) {
                release();
                return finish(code == 2 ? StopReason::Panic : StopReason::Stopped);
            }
            // Deadline is due. Inject before another poll so the hold stays exact.
        }

        if (phase == Phase::NeedDown && clicksInBurst == 0 &&
            cfg.repeatMode == RepeatMode::Count && burstsDone >= cfg.repeatCount) {
            release();
            return finish(StopReason::Done);
        }

        if (phase == Phase::NeedDown && !InputAllowed(cfg)) {
            release();
            clicksInBurst = 0;
            deadline = QpcNow() + (g_freq / 50);
            continue;
        }

        if (phase == Phase::NeedDown) {
            Inject(cfg, true, ctl.inject, ctl);
            buttonDown = true;
            deadline = QpcNow() + holdTicks;
            phase = Phase::NeedUp;
            continue;
        }

        Inject(cfg, false, ctl.inject, ctl);
        buttonDown = false;
        stats.clicks += 1;
        clicksInBurst += 1;
        if (clicksInBurst < clicksNeeded) {
            deadline = QpcNow() + gapTicks;
            phase = Phase::NeedDown;
        } else {
            clicksInBurst = 0;
            burstsDone += 1;
            deadline = origin + static_cast<int64_t>(burstsDone) * periodTicks;
            int64_t now = QpcNow();
            if (deadline <= now && periodTicks > 0) {
                int64_t steps = (now - origin) / periodTicks + 1;
                deadline = origin + steps * periodTicks;
            }
            phase = Phase::NeedDown;
            if (ctl.onStatus && now - lastStatus >= g_freq / 10) {
                lastStatus = now;
                double elapsed = QpcToMs(now - origin) / 1000.0;
                double cps = elapsed > 0 ? static_cast<double>(stats.clicks) / elapsed : 0;
                ctl.onStatus(ctl.ctx, stats.clicks, cps, false);
            }
        }
    }
}

int RunSelfTest(const wchar_t* logPath) {
    EngineInit();

    Config logic = {};
    logic.intervalMs = 10;
    logic.holdMs = 20;
    logic.clickType = ClickType::Single;
    const bool periodOk = EffectivePeriodMs(logic) == 21;

    Config cfg = {};
    cfg.action = ActionKind::Mouse;
    cfg.mouseButton = 0;
    cfg.clickType = ClickType::Single;
    cfg.repeatMode = RepeatMode::Count;
    cfg.repeatCount = 60;
    cfg.intervalMs = 50;
    cfg.holdMs = 20;
    cfg.hotkeyMode = HotkeyMode::Toggle;

    struct Sample {
        int64_t t;
        bool down;
    };
    Sample samples[128];
    int sampleCount = 0;

    INPUT probe{};
    probe.type = INPUT_MOUSE;
    probe.mi.dwFlags = MOUSEEVENTF_MOVE;
    UINT sent = SendInput(1, &probe, sizeof(INPUT));

    struct Recorder {
        Sample* samples;
        int* count;
        int cap;
    };
    Recorder rec{samples, &sampleCount, 128};

    RunControl ctl{};
    ctl.poll = [](void*) -> int { return 0; };
    ctl.onStatus = nullptr;
    ctl.onEvent = [](void* ctx, bool down, int64_t qpc) {
        auto* r = static_cast<Recorder*>(ctx);
        if (*r->count < r->cap) {
            r->samples[*r->count] = Sample{qpc, down};
            *r->count += 1;
        }
    };
    ctl.ctx = &rec;
    ctl.inject = false;

    RunStats stats = RunSession(cfg, ctl);

    int downs = 0;
    int ups = 0;
    int pairs = 0;
    int unpaired = 0;
    double holdMin = 0;
    double holdMax = 0;
    double holdSum = 0;
    double intervalMin = 0;
    double intervalMax = 0;
    double intervalSum = 0;
    int intervals = 0;
    int64_t lastDown = 0;
    bool expectDown = true;
    bool holdInit = false;
    bool intervalInit = false;

    for (int i = 0; i < sampleCount; ++i) {
        if (samples[i].down) {
            downs += 1;
            if (!expectDown) {
                unpaired += 1;
            }
            if (lastDown != 0) {
                double gap = QpcToMs(samples[i].t - lastDown);
                if (!intervalInit) {
                    intervalMin = intervalMax = gap;
                    intervalInit = true;
                } else {
                    if (gap < intervalMin) intervalMin = gap;
                    if (gap > intervalMax) intervalMax = gap;
                }
                intervalSum += gap;
                intervals += 1;
            }
            lastDown = samples[i].t;
            expectDown = false;
        } else {
            ups += 1;
            if (expectDown) {
                unpaired += 1;
            } else if (i > 0 && samples[i - 1].down) {
                double held = QpcToMs(samples[i].t - samples[i - 1].t);
                if (!holdInit) {
                    holdMin = holdMax = held;
                    holdInit = true;
                } else {
                    if (held < holdMin) holdMin = held;
                    if (held > holdMax) holdMax = held;
                }
                holdSum += held;
                pairs += 1;
            }
            expectDown = true;
        }
    }

    const double holdAvg = pairs > 0 ? holdSum / pairs : 0;
    const double intervalAvg = intervals > 0 ? intervalSum / intervals : 0;
    double maxHoldError = 0;
    double maxIntervalError = 0;
    if (holdInit) {
        double errMin = holdMin - 20.0;
        if (errMin < 0) errMin = -errMin;
        double errMax = holdMax - 20.0;
        if (errMax < 0) errMax = -errMax;
        maxHoldError = errMin > errMax ? errMin : errMax;
    }
    if (intervalInit) {
        double errMin = intervalMin - 50.0;
        if (errMin < 0) errMin = -errMin;
        double errMax = intervalMax - 50.0;
        if (errMax < 0) errMax = -errMax;
        maxIntervalError = errMin > errMax ? errMin : errMax;
    }

    const bool pass = periodOk && sent == 1 && stats.clicks == 60 && pairs == 60 &&
                      downs == 60 && ups == 60 && unpaired == 0 && intervals == 59 &&
                      maxHoldError <= 2.0 && maxIntervalError <= 2.0;

    FILE* file = nullptr;
    if (logPath) {
        _wfopen_s(&file, logPath, L"w");
    }
    if (file) {
        fprintf(file, "period_logic=%s\n", periodOk ? "ok" : "fail");
        fprintf(file, "sendinput=%u\n", sent);
        fprintf(file, "clicks=%llu\n", stats.clicks);
        fprintf(file, "downs=%d\nups=%d\npairs=%d\nunpaired=%d\nintervals=%d\n",
                downs, ups, pairs, unpaired, intervals);
        fprintf(file, "hold_ms min=%.3f avg=%.3f max=%.3f error=%.3f\n",
                holdMin, holdAvg, holdMax, maxHoldError);
        fprintf(file, "interval_ms min=%.3f avg=%.3f max=%.3f error=%.3f\n",
                intervalMin, intervalAvg, intervalMax, maxIntervalError);
        fprintf(file, "result=%s\n", pass ? "PASS" : "FAIL");
        fclose(file);
    }
    return pass && file ? 0 : 1;
}
