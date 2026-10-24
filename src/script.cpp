#include "engine.h"

#include <timeapi.h>

#include <cwchar>
#include <string>
#include <vector>

// One command per line. A tab under rep marks the lines that belong to it.
//
//   lef              click left once
//   lef 5s           keep clicking left for 5 seconds (uses the interval up top)
//   lef+             hold left down until lef- or stop
//   lef+ 5s          hold left for 5 seconds, then release
//   lef-             release left
//   lshift a         Shift+A once. lshift a 5s keeps doing it for 5 seconds
//   lshift 5         Shift+5 once. 5 is the top-row key, num5 is the number pad
//   wait 5s          pause. 500ms, 2m, 1h, and 0:0:5:0 are times too
//   rep              repeat the tabbed lines until stop
//   rep 4            repeat them 4 times, not for 4 seconds
//   # comment
//
// A bare number is a key. A time always has a unit, or all four h:m:s:ms fields.
// Stop, the hotkey, and panic release every button the script still has down.

namespace {

enum class Op { Click, Down, Up, Wait, Rep };

struct Tok {
    WORD vk;
    const wchar_t* name;
};

const Tok kTokens[] = {
    {VK_SPACE, L"spc"},       {VK_RETURN, L"enter"},    {VK_ESCAPE, L"esc"},
    {VK_TAB, L"tab"},         {VK_BACK, L"bksp"},       {VK_DELETE, L"del"},
    {VK_INSERT, L"insert"},   {VK_HOME, L"home"},       {VK_END, L"end"},
    {VK_PRIOR, L"pgup"},      {VK_NEXT, L"pgdn"},       {VK_UP, L"up"},
    {VK_DOWN, L"down"},       {VK_LEFT, L"aleft"},      {VK_RIGHT, L"aright"},
    {VK_LSHIFT, L"lshift"},   {VK_RSHIFT, L"rshift"},   {VK_LCONTROL, L"lctrl"},
    {VK_RCONTROL, L"rctrl"},  {VK_LMENU, L"lalt"},      {VK_RMENU, L"ralt"},
    {VK_LWIN, L"lwin"},       {VK_RWIN, L"rwin"},       {VK_APPS, L"apps"},
    {VK_CAPITAL, L"caps"},    {VK_NUMLOCK, L"numlock"}, {VK_SCROLL, L"scroll"},
    {VK_PAUSE, L"pause"},     {VK_SNAPSHOT, L"prtsc"},  {VK_OEM_1, L"semi"},
    {VK_OEM_PLUS, L"equal"},  {VK_OEM_COMMA, L"comma"}, {VK_OEM_MINUS, L"minus"},
    {VK_OEM_PERIOD, L"period"}, {VK_OEM_2, L"slash"},   {VK_OEM_3, L"grave"},
    {VK_OEM_4, L"lbrack"},    {VK_OEM_5, L"bslash"},    {VK_OEM_6, L"rbrack"},
    {VK_OEM_7, L"quote"},     {VK_MULTIPLY, L"nummul"}, {VK_ADD, L"numadd"},
    {VK_SUBTRACT, L"numsub"}, {VK_DECIMAL, L"numdec"},  {VK_DIVIDE, L"numdiv"},
    {VK_NUMPAD0, L"num0"},    {VK_NUMPAD1, L"num1"},    {VK_NUMPAD2, L"num2"},
    {VK_NUMPAD3, L"num3"},    {VK_NUMPAD4, L"num4"},    {VK_NUMPAD5, L"num5"},
    {VK_NUMPAD6, L"num6"},    {VK_NUMPAD7, L"num7"},    {VK_NUMPAD8, L"num8"},
    {VK_NUMPAD9, L"num9"},
};

struct Cmd {
    Op op = Op::Wait;
    int line = 0;
    Config button;
    std::vector<Config> mods;
    double seconds = 0;
    int times = 0;
    std::vector<Cmd> body;
};

struct Line {
    int number = 0;
    int indent = 0;
    std::wstring text;
};

struct Held {
    Config cfg;
};

struct Issue {
    int line = 0;
    int kind = 1;
    std::wstring msg;
};

bool Fail(wchar_t* error, int errorChars, int line, const wchar_t* msg) {
    if (error && errorChars > 0) {
        if (line > 0) {
            swprintf_s(error, static_cast<size_t>(errorChars), L"line %d: %s", line, msg);
        } else {
            wcscpy_s(error, static_cast<size_t>(errorChars), msg);
        }
    }
    return false;
}

// True means the caller should skip this line and keep scanning. False means stop.
bool Reject(std::vector<Issue>* issues, wchar_t* error, int errorChars, int line, const wchar_t* msg) {
    if (issues) {
        Issue item;
        item.line = line;
        item.kind = 1;
        item.msg = msg;
        issues->push_back(item);
        return true;
    }
    Fail(error, errorChars, line, msg);
    return false;
}

// A tab is one level. A leading space is still one level, so a script saved
// with spaces stays inside its rep.
int IndentOf(const std::wstring& line) {
    int n = 0;
    for (wchar_t c : line) {
        if (c == L' ' || c == L'\t') {
            n += 1;
        } else {
            break;
        }
    }
    return n;
}

std::wstring Trim(const std::wstring& in) {
    size_t begin = 0;
    while (begin < in.size() && (in[begin] == L' ' || in[begin] == L'\t' || in[begin] == L'\r')) {
        ++begin;
    }
    size_t end = in.size();
    while (end > begin && (in[end - 1] == L' ' || in[end - 1] == L'\t' || in[end - 1] == L'\r')) {
        --end;
    }
    return in.substr(begin, end - begin);
}

bool SplitLines(const wchar_t* text, std::vector<Line>& out, wchar_t* error, int errorChars) {
    int number = 1;
    const wchar_t* p = text ? text : L"";
    while (*p) {
        const wchar_t* start = p;
        while (*p && *p != L'\n') {
            ++p;
        }
        std::wstring raw(start, p);
        if (*p == L'\n') {
            ++p;
        }
        int indent = IndentOf(raw);
        std::wstring text = Trim(raw);
        size_t hash = text.find(L'#');
        if (hash != std::wstring::npos && (hash == 0 || text[hash - 1] == L' ' || text[hash - 1] == L'\t')) {
            text = Trim(text.substr(0, hash));
        }
        if (!text.empty()) {
            if (static_cast<int>(out.size()) >= 500) {
                return Fail(error, errorChars, number, L"script is longer than 500 lines");
            }
            Line line;
            line.number = number;
            line.indent = indent;
            line.text = text;
            out.push_back(line);
        }
        ++number;
    }
    return true;
}

bool Eq(const std::wstring& a, const wchar_t* b) {
    return _wcsicmp(a.c_str(), b) == 0;
}

bool ButtonFromName(const std::wstring& name, const Config& base, Config& out) {
    out = base;
    out.clickType = ClickType::Single;
    if (Eq(name, L"lef") || Eq(name, L"left") || Eq(name, L"lmb")) {
        out.action = ActionKind::Mouse;
        out.mouseButton = 0;
        return true;
    }
    if (Eq(name, L"rig") || Eq(name, L"right") || Eq(name, L"rmb")) {
        out.action = ActionKind::Mouse;
        out.mouseButton = 1;
        return true;
    }
    if (Eq(name, L"mid") || Eq(name, L"middle") || Eq(name, L"mmb")) {
        out.action = ActionKind::Mouse;
        out.mouseButton = 2;
        return true;
    }
    if (Eq(name, L"x1")) {
        out.action = ActionKind::Mouse;
        out.mouseButton = 3;
        return true;
    }
    if (Eq(name, L"x2")) {
        out.action = ActionKind::Mouse;
        out.mouseButton = 4;
        return true;
    }
    std::wstring key = name;
    if (Eq(key, L"shift")) key = L"lshift";
    if (Eq(key, L"ctrl") || Eq(key, L"control")) key = L"lctrl";
    if (Eq(key, L"alt")) key = L"lalt";
    if (Eq(key, L"ins")) key = L"insert";
    if (Eq(key, L"space")) key = L"spc";
    if (Eq(key, L"ent") || Eq(key, L"ret") || Eq(key, L"return")) key = L"enter";
    if (Eq(key, L"escape")) key = L"esc";
    if (Eq(key, L"back") || Eq(key, L"backspace")) key = L"bksp";
    if (Eq(key, L"delete")) key = L"del";
    for (const Tok& tok : kTokens) {
        if (Eq(key, tok.name)) {
            out.action = ActionKind::Key;
            out.actionVk = tok.vk;
            return true;
        }
    }
    if (key.size() == 1) {
        wchar_t c = key[0];
        if (c >= L'a' && c <= L'z') {
            c = static_cast<wchar_t>(c - L'a' + L'A');
        }
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) {
            out.action = ActionKind::Key;
            out.actionVk = static_cast<WORD>(c);
            return true;
        }
    }
    if (key.size() > 1 && (key[0] == L'f' || key[0] == L'F')) {
        wchar_t* end = nullptr;
        long n = wcstol(key.c_str() + 1, &end, 10);
        if (end && *end == 0 && n >= 1 && n <= 24) {
            out.action = ActionKind::Key;
            out.actionVk = static_cast<WORD>(VK_F1 + (n - 1));
            return true;
        }
    }
    return false;
}

void WordsOf(const std::wstring& text, std::vector<std::wstring>& words) {
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == L' ' || text[i] == L'\t')) {
            ++i;
        }
        size_t j = i;
        while (j < text.size() && text[j] != L' ' && text[j] != L'\t') {
            ++j;
        }
        if (j > i) {
            words.push_back(text.substr(i, j - i));
        }
        i = j;
    }
}

bool ParseClock(const std::wstring& token, double& seconds) {
    int parts[4] = {};
    int field = 0;
    size_t i = 0;
    while (field < 4) {
        if (i >= token.size() || token[i] < L'0' || token[i] > L'9') {
            return false;
        }
        long value = 0;
        while (i < token.size() && token[i] >= L'0' && token[i] <= L'9') {
            value = value * 10 + (token[i] - L'0');
            if (value > 1000000) {
                return false;
            }
            ++i;
        }
        parts[field++] = static_cast<int>(value);
        if (field < 4) {
            if (i >= token.size() || token[i] != L':') {
                return false;
            }
            ++i;
        }
    }
    if (i != token.size()) {
        return false;
    }
    int64_t ms = ((static_cast<int64_t>(parts[0]) * 60 + parts[1]) * 60 + parts[2]) * 1000 + parts[3];
    seconds = static_cast<double>(ms) / 1000.0;
    return true;
}

// 1 is a time, 2 is a partial clock, 0 is not a time. A bare number is not a time.
int ParseTime(const std::wstring& token, double& seconds) {
    seconds = 0;
    if (token.empty() || token.find(L',') != std::wstring::npos) {
        return 0;
    }
    if (token.find(L':') != std::wstring::npos) {
        return ParseClock(token, seconds) ? 1 : 2;
    }
    wchar_t* end = nullptr;
    double value = wcstod(token.c_str(), &end);
    if (end == token.c_str()) {
        return 0;
    }
    if (_wcsicmp(end, L"ms") == 0) {
        seconds = value / 1000.0;
    } else if (_wcsicmp(end, L"s") == 0) {
        seconds = value;
    } else if (_wcsicmp(end, L"m") == 0) {
        seconds = value * 60.0;
    } else if (_wcsicmp(end, L"h") == 0) {
        seconds = value * 3600.0;
    } else {
        return 0;
    }
    return seconds >= 0 ? 1 : 0;
}

const wchar_t* TimeMessage(const std::wstring& token) {
    if (token.find(L':') != std::wstring::npos) {
        return L"use a unit, like 90s or 1m, or all four fields, like 0:1:30:0";
    }
    return L"time needs a unit, like 5s or 500ms, or 0:0:5:0";
}

bool IsModifierName(const std::wstring& name) {
    return Eq(name, L"lshift") || Eq(name, L"rshift") || Eq(name, L"shift") ||
           Eq(name, L"lctrl") || Eq(name, L"rctrl") || Eq(name, L"ctrl") || Eq(name, L"control") ||
           Eq(name, L"lalt") || Eq(name, L"ralt") || Eq(name, L"alt") ||
           Eq(name, L"lwin") || Eq(name, L"rwin");
}

bool BareDigit(const std::wstring& token) {
    return token.size() == 1 && token[0] >= L'0' && token[0] <= L'9';
}

bool ParseBlock(const std::vector<Line>& lines, size_t& index, int minIndent, int depth,
                std::vector<Cmd>& out, const Config& base, wchar_t* error, int errorChars,
                std::vector<Issue>* issues) {
    if (depth > 8) {
        int line = index < lines.size() ? lines[index].number : 0;
        if (!Reject(issues, error, errorChars, line, L"too many nested reps")) {
            return false;
        }
        return true;
    }
    while (index < lines.size()) {
        const Line& line = lines[index];
        if (line.indent < minIndent) {
            return true;
        }
        size_t gap = line.text.find_first_of(L" \t");
        std::wstring word = gap == std::wstring::npos ? line.text : line.text.substr(0, gap);
        std::wstring rest = gap == std::wstring::npos ? L"" : Trim(line.text.substr(gap));
        if (Eq(word, L"rep")) {
            Cmd cmd;
            cmd.op = Op::Rep;
            cmd.line = line.number;
            cmd.times = -1;
            if (!rest.empty()) {
                wchar_t* end = nullptr;
                long n = wcstol(rest.c_str(), &end, 10);
                if (end == rest.c_str() || *end != 0 || n < 0 || n > 1000000000) {
                    if (!Reject(issues, error, errorChars, line.number, L"rep needs a count, or nothing")) {
                        return false;
                    }
                    ++index;
                    continue;
                }
                cmd.times = static_cast<int>(n);
            }
            int bodyIndent = line.indent + 1;
            ++index;
            if (!ParseBlock(lines, index, bodyIndent, depth + 1, cmd.body, base, error, errorChars, issues)) {
                return false;
            }
            if (cmd.body.empty()) {
                if (!Reject(issues, error, errorChars, line.number, L"rep needs a line under it")) {
                    return false;
                }
                continue;
            }
            out.push_back(std::move(cmd));
            continue;
        }
        if (Eq(word, L"wait")) {
            Cmd cmd;
            cmd.op = Op::Wait;
            cmd.line = line.number;
            int kind = ParseTime(rest, cmd.seconds);
            if (kind != 1) {
                const wchar_t* msg = TimeMessage(rest);
                wchar_t built[128];
                if (BareDigit(rest)) {
                    swprintf_s(built, L"%s is a key; time needs a unit, like 5s, or 0:0:5:0", rest.c_str());
                    msg = built;
                }
                if (!Reject(issues, error, errorChars, line.number, msg)) {
                    return false;
                }
                ++index;
                continue;
            }
            out.push_back(cmd);
            ++index;
            continue;
        }
        std::vector<std::wstring> words;
        words.push_back(word);
        if (!rest.empty()) {
            WordsOf(rest, words);
        }
        double seconds = 0;
        bool hasTime = false;
        int timeKind = ParseTime(words.back(), seconds);
        if (timeKind == 1) {
            hasTime = true;
            words.pop_back();
        } else if (timeKind == 2) {
            if (!Reject(issues, error, errorChars, line.number, TimeMessage(words.back()))) {
                return false;
            }
            ++index;
            continue;
        }
        if (words.empty()) {
            if (!Reject(issues, error, errorChars, line.number, L"unknown button or key")) {
                return false;
            }
            ++index;
            continue;
        }
        bool badToken = false;
        for (size_t i = 0; i + 1 < words.size(); ++i) {
            double ignored = 0;
            int mid = ParseTime(words[i], ignored);
            if (mid != 0) {
                const wchar_t* msg = mid == 2 ? TimeMessage(words[i]) : L"the time goes at the end";
                if (!Reject(issues, error, errorChars, line.number, msg)) {
                    return false;
                }
                badToken = true;
                break;
            }
        }
        if (badToken) {
            ++index;
            continue;
        }
        if (!hasTime && words.size() == 2 && BareDigit(words[1])) {
            std::wstring head = words[0];
            wchar_t headMark = 0;
            if (!head.empty() && (head.back() == L'+' || head.back() == L'-')) {
                headMark = head.back();
                head.pop_back();
            }
            if (headMark != 0 || !IsModifierName(head)) {
                const wchar_t* msg = TimeMessage(words[1]);
                wchar_t built[128];
                if (headMark == L'-') {
                    msg = L"a release does not take a time";
                } else if (headMark == 0) {
                    swprintf_s(built, L"%s is a key; time needs a unit, like 5s, or 0:0:5:0", words[1].c_str());
                    msg = built;
                }
                if (!Reject(issues, error, errorChars, line.number, msg)) {
                    return false;
                }
                ++index;
                continue;
            }
        }
        struct Named {
            Config cfg;
            wchar_t mark = 0;
        };
        std::vector<Named> names;
        bool namedOk = true;
        for (const std::wstring& token : words) {
            Named named;
            std::wstring name = token;
            if (!name.empty() && (name.back() == L'+' || name.back() == L'-')) {
                named.mark = name.back();
                name.pop_back();
            }
            if (name.empty() || !ButtonFromName(name, base, named.cfg)) {
                const wchar_t* msg = L"unknown button or key";
                if (!name.empty() && (name[0] == L'.' || (name[0] >= L'0' && name[0] <= L'9'))) {
                    msg = TimeMessage(token);
                }
                if (!Reject(issues, error, errorChars, line.number, msg)) {
                    return false;
                }
                namedOk = false;
                break;
            }
            names.push_back(named);
        }
        if (!namedOk) {
            ++index;
            continue;
        }
        Cmd cmd;
        cmd.line = line.number;
        cmd.seconds = hasTime ? seconds : 0;
        if (names.size() > 1) {
            bool marked = false;
            for (const Named& named : names) {
                if (named.mark != 0) {
                    marked = true;
                    break;
                }
            }
            if (marked) {
                if (!Reject(issues, error, errorChars, line.number, L"a hold or release is its own line")) {
                    return false;
                }
                ++index;
                continue;
            }
            cmd.op = Op::Click;
            cmd.button = names.back().cfg;
            for (size_t i = 0; i + 1 < names.size(); ++i) {
                cmd.mods.push_back(names[i].cfg);
            }
            if (hasTime && seconds <= 0) {
                if (!Reject(issues, error, errorChars, line.number, L"time needs to be more than 0")) {
                    return false;
                }
                ++index;
                continue;
            }
        } else if (names[0].mark == L'-') {
            if (hasTime) {
                if (!Reject(issues, error, errorChars, line.number, L"a release does not take a time")) {
                    return false;
                }
                ++index;
                continue;
            }
            cmd.op = Op::Up;
            cmd.button = names[0].cfg;
        } else if (names[0].mark == L'+') {
            cmd.op = Op::Down;
            cmd.button = names[0].cfg;
            if (hasTime && seconds <= 0) {
                if (!Reject(issues, error, errorChars, line.number, L"time needs to be more than 0")) {
                    return false;
                }
                ++index;
                continue;
            }
        } else {
            cmd.op = Op::Click;
            cmd.button = names[0].cfg;
            if (hasTime && seconds <= 0) {
                if (!Reject(issues, error, errorChars, line.number, L"time needs to be more than 0")) {
                    return false;
                }
                ++index;
                continue;
            }
        }
        out.push_back(std::move(cmd));
        ++index;
        continue;
    }
    return true;
}

bool SameButton(const Config& a, const Config& b) {
    if (a.action != b.action) {
        return false;
    }
    if (a.action == ActionKind::Key) {
        return a.actionVk == b.actionVk;
    }
    return a.mouseButton == b.mouseButton;
}

void ReleaseAll(std::vector<Held>& held, const RunControl& ctl) {
    for (size_t i = held.size(); i > 0; --i) {
        EngineInject(held[i - 1].cfg, false, ctl);
    }
    held.clear();
}

int Poll(const RunControl& ctl) {
    if (!ctl.poll) {
        return 0;
    }
    return ctl.poll(ctl.ctx);
}

struct Live {
    int64_t lastPost = 0;
    int line = -1;
    int loop = -999;
};

void Note(const RunControl& ctl, Live& live, int line, double remain, int loopLeft, unsigned long long clicks) {
    if (!ctl.onLine) {
        return;
    }
    int64_t now = QpcNow();
    bool jump = line != live.line || loopLeft != live.loop;
    if (!jump && live.lastPost != 0 && QpcToMs(now - live.lastPost) < 100) {
        return;
    }
    live.line = line;
    live.loop = loopLeft;
    live.lastPost = now;
    ctl.onLine(ctl.ctx, line, remain, loopLeft, clicks);
}

int WaitNoted(double ms, int line, int loopLeft, unsigned long long clicks, const RunControl& ctl, Live& live) {
    if (ms <= 0) {
        Note(ctl, live, line, 0, loopLeft, clicks);
        return 0;
    }
    int64_t began = QpcNow();
    for (;;) {
        double left = ms - QpcToMs(QpcNow() - began);
        if (left < 0) {
            left = 0;
        }
        Note(ctl, live, line, left / 1000.0, loopLeft, clicks);
        if (left <= 0) {
            return 0;
        }
        double slice = left > 100 ? 100.0 : left;
        int code = EngineWaitMs(slice, ctl);
        if (code != 0) {
            return code;
        }
    }
}

void Report(const RunControl& ctl, unsigned long long clicks, int64_t start) {
    if (!ctl.onStatus) {
        return;
    }
    double secs = QpcToMs(QpcNow() - start) / 1000.0;
    double cps = secs > 0.05 ? static_cast<double>(clicks) / secs : 0;
    ctl.onStatus(ctl.ctx, clicks, cps, false);
}

void Forget(std::vector<Held>& held, const Config& button) {
    for (size_t i = held.size(); i > 0; --i) {
        if (SameButton(held[i - 1].cfg, button)) {
            held.erase(held.begin() + static_cast<std::ptrdiff_t>(i - 1));
            break;
        }
    }
}

int WaitAllowed(const Config& button, const RunControl& ctl) {
    while (!EngineInputAllowed(button)) {
        int waited = EngineWaitMs(10, ctl);
        if (waited != 0) {
            return waited;
        }
    }
    return 0;
}

int PressMods(const std::vector<Config>& mods, std::vector<Held>& held, const RunControl& ctl) {
    for (size_t n = 0; n < mods.size(); ++n) {
        int ready = WaitAllowed(mods[n], ctl);
        if (ready != 0) {
            for (size_t i = n; i > 0; --i) {
                EngineInject(mods[i - 1], false, ctl);
                Forget(held, mods[i - 1]);
            }
            return ready;
        }
        EngineInject(mods[n], true, ctl);
        Held item;
        item.cfg = mods[n];
        held.push_back(item);
    }
    return 0;
}

void ReleaseMods(const std::vector<Config>& mods, std::vector<Held>& held, const RunControl& ctl) {
    for (size_t i = mods.size(); i > 0; --i) {
        EngineInject(mods[i - 1], false, ctl);
        Forget(held, mods[i - 1]);
    }
}

int Pulse(const Config& button, std::vector<Held>& held, unsigned long long& clicks, int64_t start,
          const RunControl& ctl) {
    int ready = WaitAllowed(button, ctl);
    if (ready != 0) {
        return ready;
    }
    EngineInject(button, true, ctl);
    Held item;
    item.cfg = button;
    held.push_back(item);
    ++clicks;
    Report(ctl, clicks, start);
    int hold = button.holdMs < 1 ? 1 : button.holdMs;
    int code = EngineWaitMs(hold, ctl);
    EngineInject(button, false, ctl);
    Forget(held, button);
    return code;
}

int Exec(const std::vector<Cmd>& cmds, std::vector<Held>& held, unsigned long long& clicks,
         int64_t start, const RunControl& ctl, Live& live, int loopLeft) {
    for (const Cmd& cmd : cmds) {
        int code = Poll(ctl);
        if (code != 0) {
            return code;
        }
        if (cmd.op == Op::Click) {
            int pressed = PressMods(cmd.mods, held, ctl);
            if (pressed != 0) {
                return pressed;
            }
            int result = 0;
            if (cmd.seconds <= 0) {
                Note(ctl, live, cmd.line, -1, loopLeft, clicks);
                result = Pulse(cmd.button, held, clicks, start, ctl);
            } else {
                int hold = cmd.button.holdMs < 1 ? 1 : cmd.button.holdMs;
                int64_t period = cmd.button.intervalMs;
                if (period < static_cast<int64_t>(hold) + 1) {
                    period = static_cast<int64_t>(hold) + 1;
                }
                int64_t gap = period - hold;
                int64_t began = QpcNow();
                double limit = cmd.seconds * 1000.0;
                while (QpcToMs(QpcNow() - began) < limit) {
                    double remain = (limit - QpcToMs(QpcNow() - began)) / 1000.0;
                    Note(ctl, live, cmd.line, remain, loopLeft, clicks);
                    result = Pulse(cmd.button, held, clicks, start, ctl);
                    if (result != 0) {
                        break;
                    }
                    remain = limit - QpcToMs(QpcNow() - began);
                    if (remain <= 0) {
                        break;
                    }
                    double nap = static_cast<double>(gap);
                    if (nap > remain) {
                        nap = remain;
                    }
                    result = WaitNoted(nap, cmd.line, loopLeft, clicks, ctl, live);
                    if (result != 0) {
                        break;
                    }
                }
            }
            ReleaseMods(cmd.mods, held, ctl);
            if (result != 0) {
                return result;
            }
        } else if (cmd.op == Op::Down) {
            int ready = WaitAllowed(cmd.button, ctl);
            if (ready != 0) {
                return ready;
            }
            Note(ctl, live, cmd.line, cmd.seconds > 0 ? cmd.seconds : -1, loopLeft, clicks);
            EngineInject(cmd.button, true, ctl);
            Held item;
            item.cfg = cmd.button;
            held.push_back(item);
            ++clicks;
            Report(ctl, clicks, start);
            if (cmd.seconds > 0) {
                int waited = WaitNoted(cmd.seconds * 1000.0, cmd.line, loopLeft, clicks, ctl, live);
                EngineInject(cmd.button, false, ctl);
                Forget(held, cmd.button);
                if (waited != 0) {
                    return waited;
                }
            }
        } else if (cmd.op == Op::Up) {
            Note(ctl, live, cmd.line, -1, loopLeft, clicks);
            EngineInject(cmd.button, false, ctl);
            Forget(held, cmd.button);
        } else if (cmd.op == Op::Wait) {
            int waited = WaitNoted(cmd.seconds * 1000.0, cmd.line, loopLeft, clicks, ctl, live);
            if (waited != 0) {
                return waited;
            }
        } else if (cmd.op == Op::Rep) {
            if (cmd.times < 0) {
                for (;;) {
                    int nested = Exec(cmd.body, held, clicks, start, ctl, live, -1);
                    if (nested != 0) {
                        return nested;
                    }
                }
            }
            for (int n = 0; n < cmd.times; ++n) {
                int nested = Exec(cmd.body, held, clicks, start, ctl, live, cmd.times - n);
                if (nested != 0) {
                    return nested;
                }
            }
        }
    }
    return 0;
}

struct HoldKey {
    int kind = 0;
    int id = 0;
};

HoldKey KeyOf(const Config& cfg) {
    HoldKey key;
    if (cfg.action == ActionKind::Key) {
        key.kind = 1;
        key.id = cfg.actionVk;
    } else {
        key.kind = 0;
        key.id = cfg.mouseButton;
    }
    return key;
}

bool HasHold(const std::vector<HoldKey>& held, HoldKey key) {
    for (const HoldKey& item : held) {
        if (item.kind == key.kind && item.id == key.id) {
            return true;
        }
    }
    return false;
}

void DropHold(std::vector<HoldKey>& held, HoldKey key) {
    for (size_t i = 0; i < held.size(); ++i) {
        if (held[i].kind == key.kind && held[i].id == key.id) {
            held.erase(held.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

void AddLogic(std::vector<Issue>& issues, int line, const wchar_t* msg) {
    Issue item;
    item.line = line;
    item.kind = 2;
    item.msg = msg;
    issues.push_back(item);
}

void CheckHolds(const std::vector<Cmd>& cmds, std::vector<HoldKey>& held, std::vector<Issue>& issues) {
    for (const Cmd& cmd : cmds) {
        HoldKey key = KeyOf(cmd.button);
        if (cmd.op == Op::Click || (cmd.op == Op::Down && cmd.seconds > 0)) {
            for (const Config& mod : cmd.mods) {
                HoldKey modKey = KeyOf(mod);
                if (HasHold(held, modKey)) {
                    AddLogic(issues, cmd.line, L"releases a hold");
                    DropHold(held, modKey);
                }
            }
            if (HasHold(held, key)) {
                AddLogic(issues, cmd.line, L"releases a hold");
                DropHold(held, key);
            }
        } else if (cmd.op == Op::Down) {
            if (HasHold(held, key)) {
                AddLogic(issues, cmd.line, L"already held");
            } else {
                held.push_back(key);
            }
        } else if (cmd.op == Op::Up) {
            if (!HasHold(held, key)) {
                AddLogic(issues, cmd.line, L"not held");
            } else {
                DropHold(held, key);
            }
        } else if (cmd.op == Op::Rep) {
            if (cmd.times == 0) {
                std::vector<HoldKey> copy = held;
                CheckHolds(cmd.body, copy, issues);
            } else if (cmd.times == 1) {
                CheckHolds(cmd.body, held, issues);
            } else {
                std::vector<HoldKey> before = held;
                CheckHolds(cmd.body, held, issues);
                bool extra = false;
                bool missing = false;
                for (const HoldKey& item : held) {
                    if (!HasHold(before, item)) {
                        extra = true;
                    }
                }
                for (const HoldKey& item : before) {
                    if (!HasHold(held, item)) {
                        missing = true;
                    }
                }
                if (extra) {
                    AddLogic(issues, cmd.line, L"loop leaves a button down");
                } else if (missing) {
                    AddLogic(issues, cmd.line, L"loop lets a button up");
                }
            }
        }
    }
}

}  // namespace

bool ScriptTokenForVk(int vk, wchar_t* out, int outChars) {
    if (!out || outChars <= 0) {
        return false;
    }
    out[0] = 0;
    const wchar_t* name = nullptr;
    if (vk == VK_LBUTTON) name = L"lef";
    else if (vk == VK_RBUTTON) name = L"rig";
    else if (vk == VK_MBUTTON) name = L"mid";
    else if (vk == VK_XBUTTON1) name = L"x1";
    else if (vk == VK_XBUTTON2) name = L"x2";
    if (!name) {
        for (const Tok& tok : kTokens) {
            if (tok.vk == static_cast<WORD>(vk)) {
                name = tok.name;
                break;
            }
        }
    }
    if (!name && vk >= 'A' && vk <= 'Z') {
        wchar_t letter[2] = {static_cast<wchar_t>(vk - 'A' + 'a'), 0};
        wcscpy_s(out, static_cast<size_t>(outChars), letter);
        return true;
    }
    if (!name && vk >= '0' && vk <= '9') {
        wchar_t digit[2] = {static_cast<wchar_t>(vk), 0};
        wcscpy_s(out, static_cast<size_t>(outChars), digit);
        return true;
    }
    if (!name && vk >= VK_F1 && vk <= VK_F24) {
        swprintf_s(out, static_cast<size_t>(outChars), L"f%d", vk - VK_F1 + 1);
        return true;
    }
    if (!name) {
        return false;
    }
    wcscpy_s(out, static_cast<size_t>(outChars), name);
    return true;
}

int ScriptMarks(const wchar_t* text, const Config& cfg, ScriptMark* marks, int maxMarks, wchar_t* first, int firstChars) {
    if (first && firstChars > 0) {
        first[0] = 0;
    }
    std::vector<Issue> issues;
    wchar_t splitError[160] = {};
    std::vector<Line> lines;
    if (!SplitLines(text, lines, splitError, 160)) {
        if (first && firstChars > 0 && splitError[0] != 0) {
            wcscpy_s(first, static_cast<size_t>(firstChars), splitError);
        }
        return 0;
    }
    std::vector<Cmd> cmds;
    size_t index = 0;
    ParseBlock(lines, index, 0, 0, cmds, cfg, nullptr, 0, &issues);
    std::vector<HoldKey> held;
    CheckHolds(cmds, held, issues);
    if (!marks || maxMarks <= 0) {
        return 0;
    }
    int count = 0;
    const Issue* shown = nullptr;
    for (const Issue& issue : issues) {
        if (!shown || (shown->kind != 1 && issue.kind == 1)) {
            shown = &issue;
        }
        if (count >= maxMarks) {
            continue;
        }
        bool already = false;
        for (int i = 0; i < count; ++i) {
            if (marks[i].line == issue.line) {
                if (issue.kind == 1) {
                    marks[i].kind = 1;
                }
                already = true;
                break;
            }
        }
        if (already) {
            continue;
        }
        marks[count].line = issue.line;
        marks[count].kind = issue.kind;
        ++count;
    }
    if (shown && first && firstChars > 0) {
        if (shown->line > 0) {
            swprintf_s(first, static_cast<size_t>(firstChars), L"line %d: %s", shown->line, shown->msg.c_str());
        } else {
            wcscpy_s(first, static_cast<size_t>(firstChars), shown->msg.c_str());
        }
    }
    return count;
}

RunStats RunScript(const wchar_t* text, const Config& cfg, const RunControl& ctl, wchar_t* error, int errorChars) {
    RunStats stats;
    if (error && errorChars > 0) {
        error[0] = 0;
    }
    EngineInit();
    std::vector<Line> lines;
    if (!SplitLines(text, lines, error, errorChars)) {
        return stats;
    }
    std::vector<Cmd> cmds;
    size_t index = 0;
    if (!ParseBlock(lines, index, 0, 0, cmds, cfg, error, errorChars, nullptr)) {
        return stats;
    }
    if (cmds.empty()) {
        Fail(error, errorChars, 0, L"script is empty");
        return stats;
    }

    bool timerOn = timeBeginPeriod(1) == TIMERR_NOERROR;
    int64_t start = QpcNow();
    std::vector<Held> held;
    Live live;
    int code = Exec(cmds, held, stats.clicks, start, ctl, live, -1);
    ReleaseAll(held, ctl);
    stats.elapsedMs = static_cast<int64_t>(QpcToMs(QpcNow() - start));
    if (code == 2) {
        stats.reason = StopReason::Panic;
    } else if (code == 1) {
        stats.reason = StopReason::Stopped;
    } else {
        stats.reason = StopReason::Done;
    }
    if (timerOn) {
        timeEndPeriod(1);
    }
    return stats;
}
