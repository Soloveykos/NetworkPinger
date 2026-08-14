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

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

#ifndef IP_SUCCESS
#define IP_SUCCESS 0
#endif

#define COLOR_DEFAULT (FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_BLUE)
#define COLOR_GREEN   (FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define COLOR_RED     (FOREGROUND_RED | FOREGROUND_INTENSITY)
#define COLOR_YELLOW  (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY)

struct TargetState {
    std::string ip;
    int targetIndex = 0;
    int alertThresholdSec = 30;
    bool soundEnabled = true;
    std::string status = "WAITING...";
    long lastRtt = 0;
    int consecutiveFails = 0;
    std::string lastChangeTime = "--:--:--";
    
    // Поля для відстеження тривалості падіння
    bool isOutageLogged = false;
    std::chrono::system_clock::time_point outageStartTime;
    std::string outageStartTimeStr;
};

struct TargetConfig {
    std::string ip;
    int alertThresholdSec = 30;
};

struct Config {
    int timeoutMs = 1000;
    int intervalMs = 1000;
    int defaultThresholdSec = 30;
    std::vector<TargetConfig> targets;
};

std::mutex g_dataMutex;
std::mutex g_audioMutex;
std::mutex g_logMutex;
std::atomic<bool> g_shouldExit{false};
std::vector<TargetState> g_targets;

std::string GetCurrentTimeStr();
std::string GetCurrentDateTimeStr();
void LogOutageEvent(const std::string& ip, int durationSec, const std::string& startTimeStr, const std::string& endTimeStr);

void FlushActiveOutages() {
    std::vector<std::tuple<std::string, int, std::string, std::string>> entries;

    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        auto now = std::chrono::system_clock::now();

        for (auto& t : g_targets) {
            if (!t.isOutageLogged) continue;

            int durationSec = (int)std::chrono::duration_cast<std::chrono::seconds>(now - t.outageStartTime).count();
            if (durationSec < 0) durationSec = 0;

            entries.emplace_back(t.ip, durationSec, t.outageStartTimeStr, GetCurrentTimeStr());
            t.isOutageLogged = false;
            t.status = "OFFLINE";
        }
    }

    for (const auto& e : entries) {
        LogOutageEvent(std::get<0>(e), std::get<1>(e), std::get<2>(e), std::get<3>(e));
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
            }
        }

        while (std::getline(file, line)) {
            if (line.empty()) continue;

            std::istringstream iss(line);
            std::string ip;
            int threshold = cfg.defaultThresholdSec;

            if (iss >> ip >> threshold) {
                if (ip.find('.') != std::string::npos || ip.find(':') != std::string::npos) {
                    TargetConfig target;
                    target.ip = ip;
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

// Запис у ЄДИНИЙ чистий лог-файл
void LogOutageEvent(const std::string& ip, int durationSec, const std::string& startTimeStr, const std::string& endTimeStr) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::ofstream logFile("network_outages.log", std::ios::app);
    if (logFile.is_open()) {
        logFile << "[" << GetCurrentDateTimeStr() << "] " 
                << ip << " - був відсутній зв'язок " << durationSec << " сек "
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

void SetColor(WORD color) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, color);
}

bool ToggleSoundAtRow(short row, short column) {
    if (row < 5 || column < 77 || column > 83) {
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
    MoveCursorToTop();

    SetColor(COLOR_DEFAULT);
    printf("=======================================================================\n");
    printf("                     MULTI-TARGET NETWORK MONITOR                      \n");
    printf("=======================================================================\n");
    printf(" #  | State | %-20s | Status       | RTT     | Fails | Alert | Sound | Last Update\n", "IP Address");
    printf("----+-------+----------------------+--------------+---------+-------+-------+-------+-----------\n");

    std::lock_guard<std::mutex> lock(g_dataMutex);
    for (size_t i = 0; i < g_targets.size(); ++i) {
        const auto& t = g_targets[i];
        
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
            printf("  | %-20s | ", t.ip.c_str());
            SetColor(COLOR_GREEN);
            printf("%-12s", t.status.c_str());
        } 
        else if (t.status == "OUTAGE!") {
            SetColor(COLOR_RED);
            printf("[O]");
            SetColor(COLOR_DEFAULT);
            printf("  | %-20s | ", t.ip.c_str());
            SetColor(COLOR_RED);
            printf("%-12s", t.status.c_str());
        } 
        else if (t.status == "DROPPING...") {
            SetColor(COLOR_YELLOW);
            printf("[O]");
            SetColor(COLOR_DEFAULT);
            printf("  | %-20s | ", t.ip.c_str());
            SetColor(COLOR_YELLOW);
            printf("%-12s", t.status.c_str());
        } 
        else {
            SetColor(COLOR_DEFAULT);
            printf("[?]");
            printf("  | %-20s | %-12s", t.ip.c_str(), t.status.c_str());
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

void PingWorker(size_t index, int timeoutMs, int intervalMs) {
    std::string ip;
    int alertThresholdSec = 30;
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        ip = g_targets[index].ip;
        alertThresholdSec = g_targets[index].alertThresholdSec;
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

        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            auto& t = g_targets[index];
            t.lastChangeTime = timeNow;

            if (isSuccess) {
                std::string outageIp;
                int outageDurationSec = 0;
                std::string outageStartTimeStr;

                if (t.isOutageLogged) {
                    auto now = std::chrono::system_clock::now();
                    outageDurationSec = (int)std::chrono::duration_cast<std::chrono::seconds>(now - t.outageStartTime).count();
                    outageIp = t.ip;
                    outageStartTimeStr = t.outageStartTimeStr;
                    t.isOutageLogged = false;
                }

                t.status = "ONLINE";
                t.lastRtt = rtt;
                t.consecutiveFails = 0;

                if (!outageIp.empty()) {
                    std::string endTimeStr = timeNow;
                    std::lock_guard<std::mutex> logLock(g_logMutex);
                    std::ofstream logFile("network_outages.log", std::ios::app);
                    if (logFile.is_open()) {
                        logFile << "[" << GetCurrentDateTimeStr() << "] "
                                << outageIp << " - був відсутній зв'язок " << outageDurationSec << " сек "
                                << "(з " << outageStartTimeStr << " до " << endTimeStr << ")" << std::endl;
                        logFile.flush();
                    }
                }
            } else {
                t.consecutiveFails++;
                t.lastRtt = -1;

                // Фіксуємо точний час початку першого фейлу
                if (t.consecutiveFails == 1) {
                    t.outageStartTime = std::chrono::system_clock::now();
                    t.outageStartTimeStr = timeNow;
                }

                // Перевищено індивідуальний поріг для поточного IP
                if (t.consecutiveFails >= t.alertThresholdSec) {
                    t.status = "OUTAGE!";
                    t.isOutageLogged = true; // Позначаємо, що після відновлення потрібно записати підсумок у лог
                    triggerSound = true;
                } else {
                    t.status = "DROPPING...";
                }
            }
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
    for (size_t i = 0; i < cfg.targets.size(); ++i) {
        TargetState st;
        st.ip = cfg.targets[i].ip;
        st.targetIndex = (int)i;
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
        Sleep(250);
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