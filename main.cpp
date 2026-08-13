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
    std::string status = "WAITING...";
    long lastRtt = 0;
    int consecutiveFails = 0;
    std::string lastChangeTime = "--:--:--";
    
    // Поля для відстеження тривалості падіння
    bool isOutageLogged = false;
    std::chrono::system_clock::time_point outageStartTime;
    std::string outageStartTimeStr;
};

struct Config {
    int timeoutMs = 1000;
    int intervalMs = 1000;
    int minFailCount = 30; // Поріг у секундах/провалах (наприклад, 30)
    std::vector<std::string> ips;
};

std::mutex g_dataMutex;
std::mutex g_audioMutex;
std::mutex g_logMutex;
std::vector<TargetState> g_targets;

Config LoadConfig() {
    Config cfg;
    std::ifstream file("appsettings.txt");
    if (file.is_open()) {
        file >> cfg.timeoutMs >> cfg.intervalMs >> cfg.minFailCount;
        std::string ip;
        while (file >> ip) {
            if (!ip.empty()) cfg.ips.push_back(ip);
        }
    }
    if (cfg.ips.empty()) cfg.ips.push_back("8.8.8.8");
    if (cfg.minFailCount < 1) cfg.minFailCount = 1;
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

void RenderDashboard(int minFailCount, int intervalMs) {
    MoveCursorToTop();

    SetColor(COLOR_DEFAULT);
    printf("=======================================================================\n");
    printf("                     MULTI-TARGET NETWORK MONITOR                      \n");
    printf("   Log Threshold: >= %d sec outage | Single Log: network_outages.log   \n", minFailCount);
    printf("=======================================================================\n");
    printf(" #  | State | IP Address      | Status       | RTT     | Fails | Last Update\n");
    printf("----+-------+-----------------+--------------+---------+-------+-----------\n");

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
            printf("  | %-15s | ", t.ip.c_str());
            SetColor(COLOR_GREEN);
            printf("%-12s", t.status.c_str());
        } 
        else if (t.status == "OUTAGE!") {
            SetColor(COLOR_RED);
            printf("[O]");
            SetColor(COLOR_DEFAULT);
            printf("  | %-15s | ", t.ip.c_str());
            SetColor(COLOR_RED);
            printf("%-12s", t.status.c_str());
        } 
        else if (t.status == "DROPPING...") {
            SetColor(COLOR_YELLOW);
            printf("[O]");
            SetColor(COLOR_DEFAULT);
            printf("  | %-15s | ", t.ip.c_str());
            SetColor(COLOR_YELLOW);
            printf("%-12s", t.status.c_str());
        } 
        else {
            SetColor(COLOR_DEFAULT);
            printf("[?]");
            printf("  | %-15s | %-12s", t.ip.c_str(), t.status.c_str());
        }

        SetColor(COLOR_DEFAULT);
        printf(" | %-7s | %-5d | %s\n", rttStr, t.consecutiveFails, t.lastChangeTime.c_str());
    }

    SetColor(COLOR_DEFAULT);
    printf("=======================================================================\n");
    printf(" Press Ctrl+C to stop monitor.\n");
}

void PingWorker(size_t index, int timeoutMs, int intervalMs, int minFailCount) {
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

    while (true) {
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
                // Якщо зв'язок відновився після критичного падіння (котре перевищило поріг)
                if (t.isOutageLogged) {
                    auto now = std::chrono::system_clock::now();
                    int durationSec = (int)std::chrono::duration_cast<std::chrono::seconds>(now - t.outageStartTime).count();
                    
                    // Фіксуємо подію в єдиний лог
                    LogOutageEvent(t.ip, durationSec, t.outageStartTimeStr, timeNow);
                    t.isOutageLogged = false;
                }

                t.status = "ONLINE";
                t.lastRtt = rtt;
                t.consecutiveFails = 0;
            } else {
                t.consecutiveFails++;
                t.lastRtt = -1;

                // Фіксуємо точний час початку першого фейлу
                if (t.consecutiveFails == 1) {
                    t.outageStartTime = std::chrono::system_clock::now();
                    t.outageStartTimeStr = timeNow;
                }

                // Перевищено поріг (наприклад, >= 30 провалів підряд)
                if (t.consecutiveFails >= minFailCount) {
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
    CONSOLE_CURSOR_INFO cursorInfo;
    GetConsoleCursorInfo(hConsole, &cursorInfo);
    cursorInfo.bVisible = FALSE;
    SetConsoleCursorInfo(hConsole, &cursorInfo);

    Config cfg = LoadConfig();

    for (size_t i = 0; i < cfg.ips.size(); ++i) {
        TargetState st;
        st.ip = cfg.ips[i];
        st.targetIndex = (int)i;
        g_targets.push_back(st);
    }

    std::vector<std::thread> threads;
    for (size_t i = 0; i < cfg.ips.size(); ++i) {
        threads.emplace_back(PingWorker, i, cfg.timeoutMs, cfg.intervalMs, cfg.minFailCount);
    }

    while (true) {
        RenderDashboard(cfg.minFailCount, cfg.intervalMs);
        Sleep(250);
    }

    return 0;
}