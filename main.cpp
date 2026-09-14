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
#include <algorithm>
#include <cstdlib>

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
    bool speedtestEnabled = false;
    int rainStepMs = 100;
    std::vector<TargetConfig> targets;
};

std::mutex g_dataMutex;
std::mutex g_audioMutex;
std::mutex g_logMutex;
std::atomic<bool> g_shouldExit{false};
std::vector<TargetState> g_targets;
bool g_matrixEnabled = false;
bool g_speedtestEnabled = false;
int g_rainStepMs = 100;
std::wstring g_matrixGlyphs;

struct SpeedtestResult {
    bool hasRun = false;
    bool isRunning = false;
    bool success = false;
    double downloadMbps = 0.0;
    double uploadMbps = 0.0;
    double pingMs = 0.0;
    std::string progressText;
    std::string serverName;
    std::string isp;
    std::string timestampStr;
    std::string errorMessage;
};

std::mutex g_speedtestMutex;
SpeedtestResult g_speedtest;

std::string GetCurrentTimeStr();
std::string GetCurrentDateTimeStr();
std::string FormatDuration(int durationSec);
std::string FormatTargetName(const std::string& ip, const std::string& alias);
void LogOutageEvent(const std::string& ip, const std::string& alias, int durationSec, const std::string& startTimeStr, const std::string& endTimeStr);
std::wstring Utf8ToWide(const std::string& text);

WORD GetSpeedColor(double mbps) {
    if (mbps < 50.0) {
        return COLOR_RED;
    }
    if (mbps < 100.0) {
        return COLOR_YELLOW;
    }
    return COLOR_GREEN;
}

int GetEffectiveRainStepMs() {
    std::lock_guard<std::mutex> lock(g_speedtestMutex);
    if (!g_speedtest.hasRun || !g_speedtest.success) {
        return g_rainStepMs;
    }

    const double downloadSpeed = g_speedtest.downloadMbps;
    if (downloadSpeed >= 100.0) {
        return 40;    // Швидкий дощ (зелена швидкість >= 100 Mbps)
    } else if (downloadSpeed >= 50.0) {
        return 250;   // Помірна швидкість дощу (жовта швидкість 50..100 Mbps)
    } else {
        return 500;   // Дуже повільний дощ (червона швидкість < 60 Mbps)
    }
}

void LogSpeedtestEvent(const SpeedtestResult& res) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::ofstream logFile("speedtest.log", std::ios::app);
    if (logFile.is_open()) {
        if (res.success) {
            logFile << "[" << res.timestampStr << "] "
                    << "Download: " << std::fixed << std::setprecision(2) << res.downloadMbps << " Mbps, "
                    << "Upload: " << std::fixed << std::setprecision(2) << res.uploadMbps << " Mbps, "
                    << "Ping: " << std::fixed << std::setprecision(1) << res.pingMs << " ms";
            if (!res.serverName.empty()) {
                logFile << " (" << res.serverName << ")";
            }
            logFile << std::endl;
        } else {
            logFile << "[" << res.timestampStr << "] Speedtest failed: " << res.errorMessage << std::endl;
        }
        logFile.flush();
    }
}

std::string FindSpeedtestExecutable() {
    const std::vector<std::string> candidates = {
        "speedtest.exe",
        ".\\speedtest.exe",
        "tools\\speedtest.exe",
        "tools/speedtest.exe",
        ".\\tools\\speedtest.exe"
    };

    for (const auto& path : candidates) {
        DWORD attrib = GetFileAttributesA(path.c_str());
        if (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY)) {
            return path;
        }
    }
    return "speedtest.exe";
}

static double ExtractJsonNumber(const std::string& json, size_t sectionStart, const std::string& key);

void UpdateSpeedtestProgress(const std::string& line) {
    const size_t typePos = line.find("\"type\":\"");
    if (typePos == std::string::npos) {
        return;
    }

    const size_t typeStart = typePos + 8;
    const size_t typeEnd = line.find('"', typeStart);
    if (typeEnd == std::string::npos) {
        return;
    }

    const std::string type = line.substr(typeStart, typeEnd - typeStart);
    const double progressValue = ExtractJsonNumber(line, 0, "\"progress\"");
    std::string progress;
    if (progressValue >= 0.0) {
        const int percent = std::clamp(static_cast<int>(progressValue * 100.0 + 0.5), 0, 100);
        const int filled = percent / 10;
        progress = " [" + std::string(filled, '#') + std::string(10 - filled, '-') + "] " + std::to_string(percent) + "%";
    }

    std::string message;
    if (type == "testStart") {
        message = "Connecting to Ookla server...";
    } else if (type == "ping") {
        message = "Measuring latency...";
    } else if (type == "download" || type == "upload") {
        const double bandwidth = ExtractJsonNumber(line, 0, "\"bandwidth\"");
        if (bandwidth >= 0.0) {
            std::ostringstream speed;
            speed << std::fixed << std::setprecision(2) << (bandwidth * 8.0) / 1000000.0;
            message = (type == "download" ? "Download: " : "Upload: ") + speed.str() + " Mbps";
        } else {
            message = type == "download" ? "Download: measuring..." : "Upload: measuring...";
        }
    } else if (type == "testEnd") {
        message = "Finishing measurement...";
    }

    if (!message.empty()) {
        std::lock_guard<std::mutex> lock(g_speedtestMutex);
        g_speedtest.progressText = message + progress;
        const double bandwidth = ExtractJsonNumber(line, 0, "\"bandwidth\"");
        if (bandwidth >= 0.0 && type == "download") {
            g_speedtest.downloadMbps = (bandwidth * 8.0) / 1000000.0;
        } else if (bandwidth >= 0.0 && type == "upload") {
            g_speedtest.uploadMbps = (bandwidth * 8.0) / 1000000.0;
        }
    }
}

bool RunSpeedtestProcess(const std::string& exePath, std::string& outJson, std::string& outErr) {
    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE hChildStdOutRead = NULL;
    HANDLE hChildStdOutWrite = NULL;

    if (!CreatePipe(&hChildStdOutRead, &hChildStdOutWrite, &saAttr, 0)) {
        outErr = "Failed to create pipe";
        return false;
    }
    SetHandleInformation(hChildStdOutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(STARTUPINFOA));
    si.cb = sizeof(STARTUPINFOA);
    si.hStdError = hChildStdOutWrite;
    si.hStdOutput = hChildStdOutWrite;
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));

    std::string cmd = "\"" + exePath + "\" --accept-license --accept-gdpr -f json";
    std::vector<char> cmdBuffer(cmd.begin(), cmd.end());
    cmdBuffer.push_back('\0');

    BOOL success = CreateProcessA(
        NULL,
        cmdBuffer.data(),
        NULL,
        NULL,
        TRUE,
        CREATE_NO_WINDOW,
        NULL,
        NULL,
        &si,
        &pi
    );

    CloseHandle(hChildStdOutWrite);

    if (!success) {
        CloseHandle(hChildStdOutRead);
        outErr = "Failed to launch speedtest executable (" + exePath + ")";
        return false;
    }

    std::string output;
    std::string pendingLine;
    char buffer[1024];
    DWORD bytesRead = 0;
    while (ReadFile(hChildStdOutRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        output.append(buffer, bytesRead);
        pendingLine.append(buffer, bytesRead);
        size_t newlinePos = 0;
        while ((newlinePos = pendingLine.find('\n')) != std::string::npos) {
            UpdateSpeedtestProgress(pendingLine.substr(0, newlinePos));
            pendingLine.erase(0, newlinePos + 1);
        }
    }

    if (!pendingLine.empty()) {
        UpdateSpeedtestProgress(pendingLine);
    }

    CloseHandle(hChildStdOutRead);

    DWORD waitResult = WAIT_TIMEOUT;
    while (!g_shouldExit) {
        waitResult = WaitForSingleObject(pi.hProcess, 500);
        if (waitResult != WAIT_TIMEOUT) {
            break;
        }
    }

    if (g_shouldExit && waitResult == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
    }

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    outJson = output;
    return (exitCode == 0);
}

static double ExtractJsonNumber(const std::string& json, size_t sectionStart, const std::string& key) {
    size_t keyPos = json.find(key, sectionStart);
    if (keyPos == std::string::npos) return -1.0;
    size_t colonPos = json.find(':', keyPos);
    if (colonPos == std::string::npos) return -1.0;
    size_t numStart = json.find_first_of("0123456789.-", colonPos);
    if (numStart == std::string::npos) return -1.0;
    char* endPtr = nullptr;
    double val = std::strtod(json.c_str() + numStart, &endPtr);
    return val;
}

static std::string ExtractJsonString(const std::string& json, size_t sectionStart, const std::string& key) {
    size_t keyPos = json.find(key, sectionStart);
    if (keyPos == std::string::npos) return "";
    size_t colonPos = json.find(':', keyPos);
    if (colonPos == std::string::npos) return "";
    size_t quoteStart = json.find('"', colonPos);
    if (quoteStart == std::string::npos) return "";
    size_t quoteEnd = json.find('"', quoteStart + 1);
    if (quoteEnd == std::string::npos) return "";
    return json.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
}

bool ParseSpeedtestJson(const std::string& jsonStr, SpeedtestResult& res) {
    size_t dlPos = jsonStr.rfind("\"download\"");
    size_t ulPos = jsonStr.rfind("\"upload\"");
    size_t pingPos = jsonStr.rfind("\"ping\"");

    if (dlPos == std::string::npos || ulPos == std::string::npos) {
        return false;
    }

    double dlBandwidth = ExtractJsonNumber(jsonStr, dlPos, "\"bandwidth\"");
    double ulBandwidth = ExtractJsonNumber(jsonStr, ulPos, "\"bandwidth\"");

    if (dlBandwidth < 0 || ulBandwidth < 0) {
        return false;
    }

    // Convert bytes per second to Mbps (1 byte = 8 bits, 1 Mbps = 1,000,000 bps)
    res.downloadMbps = (dlBandwidth * 8.0) / 1000000.0;
    res.uploadMbps = (ulBandwidth * 8.0) / 1000000.0;

    if (pingPos != std::string::npos) {
        double latency = ExtractJsonNumber(jsonStr, pingPos, "\"latency\"");
        if (latency >= 0) {
            res.pingMs = latency;
        }
    }

    size_t serverPos = jsonStr.find("\"server\"");
    if (serverPos != std::string::npos) {
        std::string serverName = ExtractJsonString(jsonStr, serverPos, "\"name\"");
        std::string location = ExtractJsonString(jsonStr, serverPos, "\"location\"");
        if (!serverName.empty()) {
            res.serverName = serverName;
            if (!location.empty() && location != serverName) {
                res.serverName += " - " + location;
            }
        }
    }

    std::string isp = ExtractJsonString(jsonStr, 0, "\"isp\"");
    if (!isp.empty()) {
        res.isp = isp;
        if (res.serverName.empty()) {
            res.serverName = isp;
        }
    }

    res.success = true;
    return true;
}

void SpeedtestWorker(bool enabled) {
    if (!enabled) {
        return;
    }

    while (!g_shouldExit) {
        auto now = std::chrono::system_clock::now();
        auto nowTimeT = std::chrono::system_clock::to_time_t(now);
        std::tm localTm = *std::localtime(&nowTimeT);
        localTm.tm_min = 0;
        localTm.tm_sec = 0;
        localTm.tm_hour = ((localTm.tm_hour / 4) + 1) * 4;
        const std::time_t nextRunTimeT = std::mktime(&localTm);
        const auto nextRunPoint = std::chrono::system_clock::from_time_t(nextRunTimeT);

        while (!g_shouldExit && std::chrono::system_clock::now() < nextRunPoint) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        if (g_shouldExit) {
            break;
        }

        {
            std::lock_guard<std::mutex> lock(g_speedtestMutex);
            g_speedtest.isRunning = true;
            g_speedtest.progressText = "Connecting to Ookla server...";
        }

        std::string exePath = FindSpeedtestExecutable();
        std::string outJson;
        std::string outErr;
        SpeedtestResult result;
        const bool procOk = RunSpeedtestProcess(exePath, outJson, outErr);
        const bool resultParsed = procOk && ParseSpeedtestJson(outJson, result);

        std::string timeNow = GetCurrentDateTimeStr();
        result.timestampStr = timeNow;
        result.isRunning = false;
        result.hasRun = true;

        if (resultParsed) {
            result.success = true;
        } else {
            result.success = false;
            if (!outErr.empty()) {
                result.errorMessage = outErr;
            } else {
                std::string msg = ExtractJsonString(outJson, 0, "\"message\"");
                if (!msg.empty()) {
                    if (msg.find("Configuration") != std::string::npos || msg.find("unreachable") != std::string::npos) {
                        result.errorMessage = "No connection to Ookla servers (Network unreachable)";
                    } else {
                        result.errorMessage = msg;
                    }
                } else if (outJson.find("Network is unreachable") != std::string::npos || outJson.find("Couldn't connect") != std::string::npos) {
                    result.errorMessage = "No connection to Ookla servers (Network unreachable)";
                } else {
                    result.errorMessage = "Speedtest error / no server connection";
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_speedtestMutex);
            g_speedtest = result;
        }

        LogSpeedtestEvent(result);
    }
}

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
                std::string option;
                while (first >> option) {
                    if (option == "matrix") {
                        cfg.matrixEnabled = true;
                    } else if (option == "speedtest") {
                        cfg.speedtestEnabled = true;
                    } else if (!option.empty() &&
                               std::all_of(option.begin(), option.end(), [](unsigned char character) {
                                   return std::isdigit(character) != 0;
                               })) {
                        cfg.rainStepMs = std::clamp(std::stoi(option), 25, 1000);
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
        if (g_speedtestEnabled) {
            printf("-----------------------------------------------------------------------\n");
            std::lock_guard<std::mutex> lock(g_speedtestMutex);
            printf(" Speedtest : ");
            if (g_speedtest.isRunning && !g_speedtest.hasRun) {
                SetColor(COLOR_YELLOW);
                printf("[TESTING...] %s", g_speedtest.progressText.empty() ? "Measurement in progress..." : g_speedtest.progressText.c_str());
                SetColor(COLOR_DEFAULT);
            } else if (g_speedtest.isRunning && g_speedtest.hasRun) {
                const WORD overallColor = GetSpeedColor(g_speedtest.downloadMbps);
                SetColor(overallColor);
                if (g_speedtest.downloadMbps > 0.0 || g_speedtest.uploadMbps > 0.0) {
                    printf("DL: %.2f Mbps | UL: %.2f Mbps", g_speedtest.downloadMbps, g_speedtest.uploadMbps);
                }
                SetColor(COLOR_DEFAULT);
                printf(" | ");
                SetColor(COLOR_YELLOW);
                printf("[MEASURING] %s", g_speedtest.progressText.empty() ? "Measurement in progress..." : g_speedtest.progressText.c_str());
                SetColor(COLOR_DEFAULT);
            } else if (g_speedtest.hasRun && g_speedtest.success) {
                const WORD overallColor = GetSpeedColor(g_speedtest.downloadMbps);
                SetColor(overallColor);
                printf("DL: %.2f Mbps | UL: %.2f Mbps | Ping: %.1f ms",
                       g_speedtest.downloadMbps, g_speedtest.uploadMbps, g_speedtest.pingMs);
                if (!g_speedtest.serverName.empty()) {
                    std::string sName = g_speedtest.serverName;
                    if (sName.length() > 18) sName = sName.substr(0, 15) + "...";
                    printf(" (%s)", sName.c_str());
                }
                printf(" [%s]", g_speedtest.timestampStr.c_str());
                SetColor(COLOR_DEFAULT);
            } else if (g_speedtest.hasRun && !g_speedtest.success) {
                SetColor(COLOR_RED);
                std::string eMsg = g_speedtest.errorMessage;
                if (eMsg.length() > 38) eMsg = eMsg.substr(0, 35) + "...";
                printf("[FAILED] %s", eMsg.c_str());
                if (!g_speedtest.timestampStr.empty()) {
                    printf(" [%s]", g_speedtest.timestampStr.c_str());
                }
                SetColor(COLOR_DEFAULT);
            } else {
                printf("Waiting for scheduled measurement...");
            }
            printf("\n");
            printf("=======================================================================\n");
        }
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
    const int tableHeight = static_cast<int>(g_targets.size()) + (g_speedtestEnabled ? 10 : 7);
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

    const int currentRainStepMs = GetEffectiveRainStepMs();
    if (now - lastMatrixStep < std::chrono::milliseconds(currentRainStepMs)) {
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
    g_speedtestEnabled = cfg.speedtestEnabled;
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

    std::thread speedtestThread(SpeedtestWorker, cfg.speedtestEnabled);

    while (!g_shouldExit) {
        ProcessConsoleInput(hInput);
        RenderDashboard();
        Sleep(10);
    }

    FlushActiveOutages();

    for (auto& th : threads) {
        if (th.joinable()) {
            th.join();
        }
    }

    if (speedtestThread.joinable()) {
        speedtestThread.join();
    }

    SetConsoleMode(hInput, originalInputMode);

    return 0;
}