#include <winsock2.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <cwchar>

#ifdef _MSC_VER
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

#ifndef IP_SUCCESS
#define IP_SUCCESS 0
#endif

#define COLOR_DEFAULT (FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_BLUE)
#define COLOR_GREEN   (FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define COLOR_RED     (FOREGROUND_RED | FOREGROUND_INTENSITY)
#define COLOR_YELLOW  (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define COLOR_GREEN_DIM FOREGROUND_GREEN
#define COLOR_RED_DIM   FOREGROUND_RED

constexpr char kDefaultMatrixAlphabet[] = "ｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓﾔﾕﾖﾗﾘﾙﾚﾛﾜﾝ0123456789";
constexpr int kDashboardWidth = 107;

struct TargetState {
    std::string ip;
    std::string alias;
    int alertThresholdSec = 30;
    bool soundEnabled = true;
    std::string status = "WAITING...";
    long lastRtt = 0;
    int consecutiveFails = 0;
    std::string lastChangeTime = "--:--:--";
    bool lastPingSucceeded = true;
    
    // Поля для відстеження тривалості падіння
    bool isOutageLogged = false;
    std::chrono::system_clock::time_point outageStartTime;
    std::string outageStartTimeStr;
};

struct TargetConfig {
    std::string ip;
    std::string alias;
    int alertThresholdSec = 30;
};

struct Config {
    int timeoutMs = 1000;
    int intervalMs = 1000;
    int defaultThresholdSec = 30;
    bool matrixEnabled = false;
    int rainStepMs = 100;
    std::vector<TargetConfig> targets;
};

std::mutex g_dataMutex;
std::mutex g_audioMutex;
std::mutex g_logMutex;
std::atomic<bool> g_shouldExit{false};
std::vector<TargetState> g_targets;
bool g_matrixEnabled = false;
int g_rainStepMs = 100;
std::wstring g_matrixGlyphs;

std::string GetCurrentTimeStr();
std::string GetCurrentDateTimeStr();
std::string FormatDuration(int durationSec);
std::string FormatTargetName(const std::string& ip, const std::string& alias);
void LogOutageEvent(const std::string& ip, const std::string& alias, int durationSec, const std::string& startTimeStr, const std::string& endTimeStr);
std::wstring Utf8ToWide(const std::string& text);

WORD GetMatrixColor(const TargetState& target, bool bright) {
    if (target.status == "DROPPING...") {
        return COLOR_YELLOW;
    }

    if (target.lastPingSucceeded) {
        return bright ? COLOR_GREEN : COLOR_GREEN_DIM;
    }

    return bright ? COLOR_RED : COLOR_RED_DIM;
}

void FlushActiveOutages() {
    std::vector<std::tuple<std::string, std::string, int, std::string, std::string>> entries;

    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        auto now = std::chrono::system_clock::now();

        for (auto& t : g_targets) {
            if (!t.isOutageLogged) continue;

            int durationSec = (int)std::chrono::duration_cast<std::chrono::seconds>(now - t.outageStartTime).count();
            if (durationSec < 0) durationSec = 0;

            entries.emplace_back(t.ip, t.alias, durationSec, t.outageStartTimeStr, GetCurrentTimeStr());
            t.isOutageLogged = false;
            t.status = "OFFLINE";
        }
    }

    for (const auto& e : entries) {
        LogOutageEvent(std::get<0>(e), std::get<1>(e), std::get<2>(e), std::get<3>(e), std::get<4>(e));
    }
}

BOOL WINAPI ConsoleHandler(DWORD ctrlType) {
    switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_shouldExit = true;
            FlushActiveOutages();
            return TRUE;
        default:
            return FALSE;
    }
}

Config LoadConfig() {
    Config cfg;
    std::ifstream file("appsettings.txt");

    if (file.is_open()) {
        std::string line;
        if (std::getline(file, line)) {
            std::istringstream first(line);
            int timeout = 0;
            int interval = 0;
            if (first >> timeout >> interval) {
                cfg.timeoutMs = timeout;
                cfg.intervalMs = interval;
                std::string mode;
                if (first >> mode && mode == "matrix") {
                    cfg.matrixEnabled = true;
                    int rainStepMs = 0;
                    if (first >> rainStepMs) {
                        cfg.rainStepMs = std::clamp(rainStepMs, 25, 1000);
                    }
                }
            }
        }

        while (std::getline(file, line)) {
            if (line.empty()) continue;

            std::istringstream iss(line);
            std::string ip;
            int threshold = cfg.defaultThresholdSec;
            std::string alias;

            if (iss >> ip >> threshold) {
                if (ip.find('.') != std::string::npos || ip.find(':') != std::string::npos) {
                    std::getline(iss >> std::ws, alias);
                    TargetConfig target;
                    target.ip = ip;
                    target.alias = alias;
                    target.alertThresholdSec = std::max(1, threshold);
                    cfg.targets.push_back(target);
                }
            }
        }
    }

    if (cfg.targets.empty()) {
        TargetConfig defaultTarget;
        defaultTarget.ip = "8.8.8.8";
        defaultTarget.alertThresholdSec = cfg.defaultThresholdSec;
        cfg.targets.push_back(defaultTarget);
    }

    for (auto& target : cfg.targets) {
        if (target.alertThresholdSec < 1) {
            target.alertThresholdSec = 1;
        }
    }

    return cfg;
}

std::string GetCurrentTimeStr() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%H:%M:%S");
    return ss.str();
}

std::string GetCurrentDateTimeStr() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

std::string FormatDuration(int durationSec) {
    durationSec = std::max(0, durationSec);
    const int hours = durationSec / 3600;
    const int minutes = (durationSec % 3600) / 60;
    const int seconds = durationSec % 60;

    std::ostringstream duration;
    if (hours > 0) duration << hours << "г. ";
    if (hours > 0 || minutes > 0) duration << minutes << "хв. ";
    duration << seconds << "сек.";
    return duration.str();
}

std::string FormatTargetName(const std::string& ip, const std::string& alias) {
    return alias.empty() ? ip : alias + " (" + ip + ")";
}

// Запис у ЄДИНИЙ чистий лог-файл
void LogOutageEvent(const std::string& ip, const std::string& alias, int durationSec, const std::string& startTimeStr, const std::string& endTimeStr) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::ofstream logFile("network_outages.log", std::ios::app);
    if (logFile.is_open()) {
        logFile << "[" << GetCurrentDateTimeStr() << "] " 
                << FormatTargetName(ip, alias) << " - був відсутній зв'язок " << FormatDuration(durationSec) << " "
                << "(з " << startTimeStr << " до " << endTimeStr << ")" << std::endl;
        logFile.flush();
    }
}

void PlayAlertSound(int targetIndex) {
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        if (!g_targets[targetIndex].soundEnabled) {
            return;
        }
    }

    std::lock_guard<std::mutex> lock(g_audioMutex);
    int baseFrequency = 1000 + (targetIndex * 400);
    int beepCount = targetIndex + 1;
    for (int i = 0; i < beepCount; ++i) {
        Beep(baseFrequency, 150);
        if (i < beepCount - 1) Sleep(50);
    }
}

void MoveCursorToTop() {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    COORD coord = { 0, 0 };
    SetConsoleCursorPosition(hConsole, coord);
}

bool ClearConsoleAfterResize() {
    static short previousWidth = 0;
    static short previousHeight = 0;

    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO consoleInfo;
    if (!GetConsoleScreenBufferInfo(hConsole, &consoleInfo)) {
        return false;
    }

    const short width = consoleInfo.srWindow.Right - consoleInfo.srWindow.Left + 1;
    const short height = consoleInfo.srWindow.Bottom - consoleInfo.srWindow.Top + 1;
    if (width == previousWidth && height == previousHeight) {
        return false;
    }

    previousWidth = width;
    previousHeight = height;

    const DWORD cellCount = static_cast<DWORD>(consoleInfo.dwSize.X) * consoleInfo.dwSize.Y;
    const COORD origin = { 0, 0 };
    DWORD cellsWritten = 0;
    FillConsoleOutputCharacterW(hConsole, L' ', cellCount, origin, &cellsWritten);
    FillConsoleOutputAttribute(hConsole, COLOR_DEFAULT, cellCount, origin, &cellsWritten);
    return true;
}

void SetColor(WORD color) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, color);
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return L"";
    }

    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring wideText(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wideText.data(), length);
    wideText.pop_back();
    return wideText;
}

bool ToggleSoundAtRow(short row, short column) {
    if (row < 5 || column < 101 || column > 104) {
        return false;
    }

    const size_t targetIndex = static_cast<size_t>(row - 5);
    std::lock_guard<std::mutex> lock(g_dataMutex);
    if (targetIndex < g_targets.size()) {
        const bool shouldPlayAlert = !g_targets[targetIndex].soundEnabled &&
                                     g_targets[targetIndex].status == "OUTAGE!";
        g_targets[targetIndex].soundEnabled = !g_targets[targetIndex].soundEnabled;
        return shouldPlayAlert;
    }

    return false;
}

void ProcessConsoleInput(HANDLE hInput) {
    DWORD eventCount = 0;
    if (!GetNumberOfConsoleInputEvents(hInput, &eventCount) || eventCount == 0) {
        return;
    }

    std::vector<INPUT_RECORD> events(eventCount);
    DWORD eventsRead = 0;
    if (!ReadConsoleInputA(hInput, events.data(), eventCount, &eventsRead)) {
        return;
    }

    for (DWORD i = 0; i < eventsRead; ++i) {
        const INPUT_RECORD& event = events[i];
        if (event.EventType == MOUSE_EVENT &&
            event.Event.MouseEvent.dwEventFlags == 0 &&
            (event.Event.MouseEvent.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0) {
            const bool shouldPlayAlert = ToggleSoundAtRow(
                event.Event.MouseEvent.dwMousePosition.Y,
                event.Event.MouseEvent.dwMousePosition.X
            );
            if (shouldPlayAlert) {
                PlayAlertSound(static_cast<int>(event.Event.MouseEvent.dwMousePosition.Y - 5));
            }
        }
    }
}

void RenderDashboard() {
    static auto lastTableRender = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    const bool wasResized = ClearConsoleAfterResize();
    const bool shouldRenderTable = !g_matrixEnabled ||
        wasResized ||
        now - lastTableRender >= std::chrono::milliseconds(250);

    if (shouldRenderTable) {
        lastTableRender = now;
        MoveCursorToTop();

        SetColor(COLOR_DEFAULT);
        printf("=======================================================================\n");
        printf("                     MULTI-TARGET NETWORK MONITOR                      \n");
        printf("=======================================================================\n");
        printf(" #  | State | %-20s | %-20s | Status       | RTT     | Fails | Alert | Sound | Last Update\n", "IP Address", "Alias");
        printf("----+-------+----------------------+----------------------+--------------+---------+-------+-------+-------+-----------\n");
        
        std::lock_guard<std::mutex> lock(g_dataMutex);
        for (size_t i = 0; i < g_targets.size(); ++i) {
            const auto& t = g_targets[i];
            const std::string alias = t.alias.empty() ? "-" : t.alias;
            
            char rttStr[16];
            if (t.lastRtt >= 0) {
                snprintf(rttStr, sizeof(rttStr), "%ld ms", t.lastRtt);
            } else {
                snprintf(rttStr, sizeof(rttStr), "N/A");
            }

        SetColor(COLOR_DEFAULT);
        printf(" %-2zu |  ", i + 1);

        if (t.status == "ONLINE") {
            SetColor(COLOR_GREEN);
            printf("[O]");
            SetColor(COLOR_DEFAULT);
            printf("  | %-20s | %-20s | ", t.ip.c_str(), alias.c_str());
            SetColor(COLOR_GREEN);
            printf("%-12s", t.status.c_str());
        } 
        else if (t.status == "OUTAGE!") {
            SetColor(COLOR_RED);
            printf("[O]");
            SetColor(COLOR_DEFAULT);
            printf("  | %-20s | %-20s | ", t.ip.c_str(), alias.c_str());
            SetColor(COLOR_RED);
            printf("%-12s", t.status.c_str());
        } 
        else if (t.status == "DROPPING...") {
            SetColor(COLOR_YELLOW);
            printf("[O]");
            SetColor(COLOR_DEFAULT);
            printf("  | %-20s | %-20s | ", t.ip.c_str(), alias.c_str());
            SetColor(COLOR_YELLOW);
            printf("%-12s", t.status.c_str());
        } 
        else {
            SetColor(COLOR_DEFAULT);
            printf("[?]");
            printf("  | %-20s | %-20s | %-12s", t.ip.c_str(), alias.c_str(), t.status.c_str());
        }

        SetColor(COLOR_DEFAULT);
        printf(" | %-7s | %-5d | %-5d | ", rttStr, t.consecutiveFails, t.alertThresholdSec);
        SetColor(t.soundEnabled ? COLOR_GREEN : COLOR_RED);
        printf(t.soundEnabled ? "[ON ]" : "[OFF]");
        SetColor(COLOR_DEFAULT);
            printf(" | %s\n", t.lastChangeTime.c_str());
        }

        SetColor(COLOR_DEFAULT);
        printf("=======================================================================\n");
        printf(" Click [ON ]/[OFF] for sound, or press Ctrl+C to stop monitor.\n");
    }

    if (!g_matrixEnabled) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_dataMutex);

    CONSOLE_SCREEN_BUFFER_INFO consoleInfo;
    int consoleWidth = 120;
    int consoleHeight = 30;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &consoleInfo)) {
        consoleWidth = consoleInfo.srWindow.Right - consoleInfo.srWindow.Left + 1;
        consoleHeight = consoleInfo.srWindow.Bottom - consoleInfo.srWindow.Top + 1;
    }

    const int matrixWidth = std::min(consoleWidth, kDashboardWidth);
    const int tableHeight = static_cast<int>(g_targets.size()) + 7;
    const int matrixHeight = std::max(1, consoleHeight - tableHeight);
    const int glyphRows = matrixHeight - 1;
    const int maxRainLength = std::max(6, std::min(24, glyphRows * 2 / 3));
    const int laneWidth = std::max(1, matrixWidth / static_cast<int>(g_targets.size()));
    const size_t glyphCount = g_matrixGlyphs.size();
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

    struct RainStream {
        int headRow;
        int length;
        int cadence;
        int phase;
    };
    static int renderedWidth = 0;
    static int renderedHeight = 0;
    static std::vector<WORD> renderedColors;
    static std::vector<RainStream> rainStreams;
    static unsigned int rainTick = 0;
    static auto lastMatrixStep = std::chrono::steady_clock::time_point{};
    static unsigned int randomState = 0x9E3779B9u;

    std::vector<WORD> targetColors;
    for (const auto& target : g_targets) {
        targetColors.push_back(GetMatrixColor(target, false));
    }

    const bool needsGeometryRender = renderedWidth != consoleWidth ||
        renderedHeight != matrixHeight ||
        rainStreams.size() != static_cast<size_t>(matrixWidth);

    const auto writeCell = [&](int column, int glyphRow, wchar_t glyph, WORD color) {
        if (glyphRow < 0 || glyphRow >= glyphRows) {
            return;
        }

        CHAR_INFO cell = {};
        cell.Char.UnicodeChar = glyph;
        cell.Attributes = color;
        COORD updateSize = { 1, 1 };
        COORD updateOrigin = { 0, 0 };
        SMALL_RECT updateRect = { static_cast<SHORT>(column), static_cast<SHORT>(tableHeight + glyphRow + 1), static_cast<SHORT>(column), static_cast<SHORT>(tableHeight + glyphRow + 1) };
        WriteConsoleOutputW(hConsole, &cell, updateSize, updateOrigin, &updateRect);
    };

    if (needsGeometryRender) {
        std::vector<CHAR_INFO> matrixBuffer(static_cast<size_t>(consoleWidth) * matrixHeight);
        for (auto& cell : matrixBuffer) {
            cell.Char.UnicodeChar = L' ';
            cell.Attributes = COLOR_DEFAULT;
        }

        for (size_t i = 0; i < g_targets.size(); ++i) {
            const auto& target = g_targets[i];
            const std::wstring label = Utf8ToWide(target.alias.empty() ? target.ip : target.alias);
            const int labelStart = static_cast<int>(i) * laneWidth;
            const int labelLength = std::min({ static_cast<int>(label.size()), laneWidth, matrixWidth - labelStart });
            for (int character = 0; character < labelLength; ++character) {
                CHAR_INFO& cell = matrixBuffer[labelStart + character];
                cell.Char.UnicodeChar = label[character];
                cell.Attributes = GetMatrixColor(target, true);
            }
        }

        COORD bufferSize = { static_cast<SHORT>(consoleWidth), static_cast<SHORT>(matrixHeight) };
        COORD bufferOrigin = { 0, 0 };
        SMALL_RECT matrixRect = { 0, static_cast<SHORT>(tableHeight), static_cast<SHORT>(consoleWidth - 1), static_cast<SHORT>(consoleHeight - 1) };
        WriteConsoleOutputW(hConsole, matrixBuffer.data(), bufferSize, bufferOrigin, &matrixRect);

        renderedWidth = consoleWidth;
        renderedHeight = matrixHeight;
        renderedColors = targetColors;
        rainStreams.clear();
        for (int column = 0; column < matrixWidth; ++column) {
            const int length = 6 + (randomState % std::max(1, maxRainLength - 5));
            randomState = randomState * 1664525u + 1013904223u;
            const int startRow = -static_cast<int>(randomState % std::max(1, glyphRows + length));
            rainStreams.push_back({ startRow, length, 2 + static_cast<int>(randomState % 4), static_cast<int>(randomState % 5) });
            randomState = randomState * 1664525u + 1013904223u;
        }
        rainTick = 0;
        lastMatrixStep = now;
        return;
    }

    if (renderedColors != targetColors) {
        DWORD cellsWritten = 0;
        for (size_t i = 0; i < g_targets.size(); ++i) {
            const int labelStart = static_cast<int>(i) * laneWidth;
            FillConsoleOutputAttribute(hConsole, GetMatrixColor(g_targets[i], true),
                static_cast<DWORD>(std::min(laneWidth, matrixWidth - labelStart)),
                { static_cast<SHORT>(labelStart), static_cast<SHORT>(tableHeight) }, &cellsWritten);
        }

        for (int column = 0; column < matrixWidth; ++column) {
            const RainStream& stream = rainStreams[column];
            const size_t targetIndex = std::min(g_targets.size() - 1, static_cast<size_t>(column / laneWidth));
            const int firstRow = std::max(0, stream.headRow - stream.length + 1);
            const int lastRow = std::min(glyphRows - 1, stream.headRow + 1);
            if (firstRow <= lastRow) {
                FillConsoleOutputAttribute(hConsole, targetColors[targetIndex],
                    static_cast<DWORD>(lastRow - firstRow + 1),
                    { static_cast<SHORT>(column), static_cast<SHORT>(tableHeight + firstRow + 1) }, &cellsWritten);
            }
        }
        renderedColors = targetColors;
    }

    if (now - lastMatrixStep < std::chrono::milliseconds(g_rainStepMs)) {
        return;
    }

    if (glyphRows < 1) {
        return;
    }

    const auto nextRandom = []() {
        randomState ^= randomState << 13;
        randomState ^= randomState >> 17;
        randomState ^= randomState << 5;
        return randomState;
    };

    ++rainTick;
    for (int column = 0; column < matrixWidth; ++column) {
        RainStream& stream = rainStreams[column];
        if ((rainTick + stream.phase) % stream.cadence != 0) {
            continue;
        }

        const size_t targetIndex = std::min(g_targets.size() - 1, static_cast<size_t>(column / laneWidth));
        const int tailRow = stream.headRow - stream.length;
        writeCell(column, tailRow, L' ', COLOR_DEFAULT);
        writeCell(column, stream.headRow, g_matrixGlyphs[nextRandom() % glyphCount], targetColors[targetIndex]);

        ++stream.headRow;
        writeCell(
            column,
            stream.headRow,
            g_matrixGlyphs[nextRandom() % glyphCount],
            FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY
        );

        if (stream.headRow - stream.length >= glyphRows) {
            stream.length = 6 + static_cast<int>(nextRandom() % std::max(1, maxRainLength - 5));
            stream.cadence = 2 + static_cast<int>(nextRandom() % 4);
            stream.phase = static_cast<int>(nextRandom() % stream.cadence);
            stream.headRow = -static_cast<int>(nextRandom() % std::max(1, glyphRows / 2));
        }
    }

    lastMatrixStep = now;

}

void PingWorker(size_t index, int timeoutMs, int intervalMs) {
    std::string ip;
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        ip = g_targets[index].ip;
    }

    HANDLE hIcmpFile = IcmpCreateFile();
    if (hIcmpFile == INVALID_HANDLE_VALUE) return;

    unsigned long ipaddr = inet_addr(ip.c_str());
    char SendData[] = "PingData";
    DWORD ReplySize = sizeof(ICMP_ECHO_REPLY) + sizeof(SendData);
    VOID* ReplyBuffer = malloc(ReplySize);

    while (!g_shouldExit) {
        DWORD dwRetVal = IcmpSendEcho(
            hIcmpFile, ipaddr, SendData, (WORD)sizeof(SendData),
            NULL, ReplyBuffer, ReplySize, timeoutMs
        );

        std::string timeNow = GetCurrentTimeStr();
        bool isSuccess = false;
        long rtt = -1;

        if (dwRetVal != 0) {
            PICMP_ECHO_REPLY pEchoReply = (PICMP_ECHO_REPLY)ReplyBuffer;
            if (pEchoReply->Status == IP_SUCCESS) {
                isSuccess = true;
                rtt = pEchoReply->RoundTripTime;
            }
        }

        bool triggerSound = false;
        bool shouldLogOutage = false;
        std::string outageIp;
        std::string outageAlias;
        int outageDurationSec = 0;
        std::string outageStartTimeStr;

        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            auto& t = g_targets[index];
            t.lastChangeTime = timeNow;
            t.lastPingSucceeded = isSuccess;

            if (isSuccess) {
                if (t.isOutageLogged) {
                    auto now = std::chrono::system_clock::now();
                    outageDurationSec = std::max(0, static_cast<int>(
                        std::chrono::duration_cast<std::chrono::seconds>(now - t.outageStartTime).count()
                    ));
                    outageIp = t.ip;
                    outageAlias = t.alias;
                    outageStartTimeStr = t.outageStartTimeStr;
                    t.isOutageLogged = false;
                    shouldLogOutage = true;
                }

                t.status = "ONLINE";
                t.lastRtt = rtt;
                t.consecutiveFails = 0;
            } else {
                t.consecutiveFails++;
                t.lastRtt = -1;

                const auto now = std::chrono::system_clock::now();

                // Фіксуємо точний час початку першого фейлу
                if (t.consecutiveFails == 1) {
                    t.outageStartTime = now;
                    t.outageStartTimeStr = timeNow;
                }

                const int outageDurationSec = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::seconds>(now - t.outageStartTime).count()
                );

                // Перевищено індивідуальний поріг тривалості недоступності для поточного IP
                if (outageDurationSec >= t.alertThresholdSec) {
                    t.status = "OUTAGE!";
                    t.isOutageLogged = true; // Позначаємо, що після відновлення потрібно записати підсумок у лог
                    triggerSound = true;
                } else {
                    t.status = "DROPPING...";
                }
            }
        }

        if (shouldLogOutage) {
            LogOutageEvent(outageIp, outageAlias, outageDurationSec, outageStartTimeStr, timeNow);
        }

        if (triggerSound) {
            PlayAlertSound((int)index);
        }

        Sleep(intervalMs);
    }

    free(ReplyBuffer);
    IcmpCloseHandle(hIcmpFile);
}

int main() {
    SetConsoleTitleA("Network Loss Dashboard");

    SetConsoleOutputCP(CP_UTF8);

    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hInput = GetStdHandle(STD_INPUT_HANDLE);
    DWORD originalInputMode = 0;
    GetConsoleMode(hInput, &originalInputMode);
    SetConsoleMode(
        hInput,
        (originalInputMode & ~ENABLE_QUICK_EDIT_MODE) |
        ENABLE_EXTENDED_FLAGS |
        ENABLE_MOUSE_INPUT
    );

    CONSOLE_CURSOR_INFO cursorInfo;
    GetConsoleCursorInfo(hConsole, &cursorInfo);
    cursorInfo.bVisible = FALSE;
    SetConsoleCursorInfo(hConsole, &cursorInfo);

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    Config cfg = LoadConfig();
    g_matrixEnabled = cfg.matrixEnabled;
    g_rainStepMs = cfg.rainStepMs;
    g_matrixGlyphs = Utf8ToWide(kDefaultMatrixAlphabet);
    for (size_t i = 0; i < cfg.targets.size(); ++i) {
        TargetState st;
        st.ip = cfg.targets[i].ip;
        st.alias = cfg.targets[i].alias;
        st.alertThresholdSec = cfg.targets[i].alertThresholdSec;
        g_targets.push_back(st);
    }

    std::vector<std::thread> threads;
    for (size_t i = 0; i < cfg.targets.size(); ++i) {
        threads.emplace_back(PingWorker, i, cfg.timeoutMs, cfg.intervalMs);
    }

    while (!g_shouldExit) {
        ProcessConsoleInput(hInput);
        RenderDashboard();
        Sleep(50);
    }

    FlushActiveOutages();

    for (auto& th : threads) {
        if (th.joinable()) {
            th.join();
        }
    }

    SetConsoleMode(hInput, originalInputMode);

    return 0;
}