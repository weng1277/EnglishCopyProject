#include "usb_monitor.h"
#include <setupapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <initguid.h>
#include <fstream>
#include <ctime>
#include <iostream>
#include <algorithm>
#include <cctype>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

// 定义卷设备接口 GUID
DEFINE_GUID(GUID_DEVINTERFACE_VOLUME, 0x53f5630dL, 0xb6bf, 0x11d0, 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b);

// 全局监控器指针
static USBMonitor* g_pMonitor = nullptr;

// ==================== 构造和析构 ====================

USBMonitor::USBMonitor() 
    : hwnd_(nullptr)
    , hDevNotify_(nullptr)
    , totalFilesToCopy_(0)
    , copiedFilesCount_(0)
    , totalDirectoriesToCreate_(0)
    , createdDirectoriesCount_(0) 
{
    g_pMonitor = this;
    memset(&trayIconData_, 0, sizeof(trayIconData_));

    try {
    // 获取程序根目录
    char programPath[MAX_PATH];
    GetModuleFileNameA(NULL, programPath, MAX_PATH);
    std::string programDir = programPath;
    size_t lastSlash = programDir.find_last_of("");
    if (lastSlash != std::string::npos) {
        programDir = programDir.substr(0, lastSlash);
    }

    // 加载配置文件（可能包含自定义保存目录）
    LoadConfig();

    // 如果 saveDir_ 为空，则使用程序根目录作为默认保存目录
    if (saveDir_.empty()) {
        saveDir_ = programDir;
    }

    char appDataPath[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appDataPath) == S_OK) {
        std::string baseDir = std::string(appDataPath) + "\\USBMonitor";
        CreateDirectoryA(baseDir.c_str(), NULL);
    }
    
    // 日志文件保存在程序根目录
    logDir_ = programDir;
    
    // 确保保存目录存在
    CreateDirectoryA(saveDir_.c_str(), NULL);

    // 获取下载文件夹路径
    char downloadsPath[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, downloadsPath) == S_OK) {
        downloadsDir_ = std::string(downloadsPath) + "\Downloads";
    } catch (...) {
    char programPath[MAX_PATH];
    GetModuleFileNameA(NULL, programPath, MAX_PATH);
    std::string programDir = programPath;
    size_t lastSlash = programDir.find_last_of("");
    if (lastSlash != std::string::npos) {
        programDir = programDir.substr(0, lastSlash);
    }
    
    logDir_ = programDir;
    saveDir_ = programDir;
    downloadsDir_ = programDir + "\\Downloads";
    }
}

USBMonitor::~USBMonitor() {
    Cleanup();
    g_pMonitor = nullptr;
}

// ==================== 初始化方法 ====================

bool USBMonitor::Initialize() {
    WriteLog("正在启动 USB 监控程序...");

    // 设置开机启动
    CheckAndSetAutoStart();

    // 注册窗口类
    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = TEXT("USBMonitorClass");

    if (!RegisterClassEx(&wc)) {
        DWORD error = GetLastError();
        WriteLog("注册窗口类失败，错误代码：" + std::to_string(error));
        return false;
    }
    WriteLog("窗口类注册成功");

    // 创建消息窗口
    hwnd_ = CreateWindowEx(
            0,
            TEXT("USBMonitorClass"),
            TEXT("USB Monitor"),
            0,
            0,
            0,
            0,
            0,
            HWND_MESSAGE,
            NULL,
            GetModuleHandle(NULL),
            NULL
    );

    if (!hwnd_) {
        DWORD error = GetLastError();
        WriteLog("创建窗口失败，错误代码：" + std::to_string(error));
        return false;
    }
    WriteLog("消息窗口创建成功");

    // 创建托盘图标
    if (CreateTrayIcon()) {
        WriteLog("托盘图标创建成功");
    } else {
        WriteLog("托盘图标创建失败，但继续运行");
    }

    // 注册设备通知（三种方式尝试）
    WriteLog("开始注册设备通知...");

    // 方法 1: 注册所有设备变化通知
    hDevNotify_ = RegisterDeviceNotification(
            hwnd_,
            NULL,
            DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES
    );

    if (!hDevNotify_) {
        DWORD error = GetLastError();
        WriteLog("方法 1 注册失败，错误代码：" + std::to_string(error) + "，尝试方法 2");

        // 方法 2: 注册卷设备通知
        DEV_BROADCAST_DEVICEINTERFACE dbdi = {0};
        dbdi.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
        dbdi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        dbdi.dbcc_classguid = GUID_DEVINTERFACE_VOLUME;

        hDevNotify_ = RegisterDeviceNotification(
                hwnd_,
                &dbdi,
                DEVICE_NOTIFY_WINDOW_HANDLE
        );

        if (!hDevNotify_) {
            error = GetLastError();
            WriteLog("方法 2 也失败，错误代码：" + std::to_string(error) + "，尝试方法 3");

            // 方法 3: 最简单的注册方式
            DEV_BROADCAST_HDR dbh = {0};
            dbh.dbch_size = sizeof(DEV_BROADCAST_HDR);
            dbh.dbch_devicetype = DBT_DEVTYP_DEVICEINTERFACE;

            hDevNotify_ = RegisterDeviceNotification(
                    hwnd_,
                    &dbh,
                    DEVICE_NOTIFY_WINDOW_HANDLE
            );

            if (!hDevNotify_) {
                error = GetLastError();
                WriteLog("所有设备通知注册方法都失败，错误代码：" + std::to_string(error));
                WriteLog("警告：设备通知可能无法正常工作，但程序继续运行");
            } else {
                WriteLog("方法 3 设备通知注册成功");
            }
        } else {
            WriteLog("方法 2 设备通知注册成功");
        }
    } else {
        WriteLog("方法 1 设备通知注册成功");
    }

    WriteLog("USB 监控程序初始化完成");

    // 启动后立即扫描一次现有驱动器
    WriteLog("启动后立即扫描现有驱动器...");
    ScanAllDrives();

    return true;
}

void USBMonitor::Run() {
    WriteLog("进入消息循环");

    MSG msg;
    int messageCount = 0;

    while (true) {
        try {
            BOOL result = GetMessage(&msg, NULL, 0, 0);

            if (result == 0) {
                WriteLog("收到退出消息");
                break;
            } else if (result == -1) {
                WriteLog("GetMessage 失败");
                break;
            } else {
                messageCount++;
                if (messageCount <= 10 || messageCount % 100 == 0) {
                    WriteLog("处理消息 #" + std::to_string(messageCount) + ": " + std::to_string(msg.message));
                }

                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
        catch (const std::exception& e) {
            WriteLog("消息循环中发生异常：" + std::string(e.what()));
            Sleep(100);
        }
        catch (...) {
            WriteLog("消息循环中发生未知异常，继续运行");
            Sleep(100);
        }
    }

    WriteLog("消息循环结束");
}

// ==================== 静态窗口过程 ====================

LRESULT CALLBACK USBMonitor::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    try {
        if (g_pMonitor) {
            return g_pMonitor->HandleMessage(hwnd, uMsg, wParam, lParam);
        }
    }
    catch (const std::exception& e) {
        if (g_pMonitor) {
            g_pMonitor->WriteLog("WindowProc 中发生异常：" + std::string(e.what()));
        }
    }
    catch (...) {
        if (g_pMonitor) {
            g_pMonitor->WriteLog("WindowProc 中发生未知异常，消息：" + std::to_string(uMsg));
        }
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// ==================== 字符串处理方法 ====================

std::string USBMonitor::NormalizeString(const std::string& str) {
    std::string normalized;

    for (char c : str) {
        char lowerC = std::tolower(static_cast<unsigned char>(c));
        if (std::isalnum(static_cast<unsigned char>(lowerC)) || (lowerC & 0x80)) {
            normalized += lowerC;
        }
    }

    return normalized;
}

double USBMonitor::CalculateStringSimilarity(const std::string& str1, const std::string& str2) {
    if (str1.empty() && str2.empty()) {
        return 1.0;
    }

    if (str1.empty() || str2.empty()) {
        return 0.0;
    }

    std::string normalized1 = NormalizeString(str1);
    std::string normalized2 = NormalizeString(str2);

    int distance = LevenshteinDistance(normalized1, normalized2);
    int maxLen = std::max(normalized1.length(), normalized2.length());

    if (maxLen == 0) {
        return 1.0;
    }

    return 1.0 - static_cast<double>(distance) / maxLen;
}

int USBMonitor::LevenshteinDistance(const std::string& str1, const std::string& str2) {
    int len1 = static_cast<int>(str1.length());
    int len2 = static_cast<int>(str2.length());

    std::vector<std::vector<int>> dp(len1 + 1, std::vector<int>(len2 + 1));

    for (int i = 0; i <= len1; i++) {
        dp[i][0] = i;
    }
    for (int j = 0; j <= len2; j++) {
        dp[0][j] = j;
    }

    for (int i = 1; i <= len1; i++) {
        for (int j = 1; j <= len2; j++) {
            if (str1[i-1] == str2[j-1]) {
                dp[i][j] = dp[i-1][j-1];
            } else {
                dp[i][j] = 1 + std::min({
                    dp[i-1][j],
                    dp[i][j-1],
                    dp[i-1][j-1]
                });
            }
        }
    }

    return dp[len1][len2];
}

// ==================== 日志方法 ====================

void USBMonitor::WriteLog(const std::string& message) {
    try {
        time_t now = time(0);
        struct tm timeinfo;
        localtime_s(&timeinfo, &now);

        char timeStr[32];
        strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);

        char dateStr[32];
        strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &timeinfo);

        std::string logFile = logDir_ + "" + std::string(dateStr) + ".log";

        std::ofstream file(logFile, std::ios::app);
        if (file.is_open()) {
            file << "[" << timeStr << "] " << message << std::endl;
            file.close();
        }

        std::cout << "[" << timeStr << "] " << message << std::endl;
    }
    catch (...) {
        std::cout << "日志异常：" << message << std::endl;
    }
}

// ==================== 系统设置方法 ====================

void USBMonitor::CheckAndSetAutoStart() {
    try {
        HKEY hKey;
        const char* subKey = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
        const char* valueName = "USBMonitor";

        LONG result = RegOpenKeyExA(HKEY_CURRENT_USER, subKey, 0, KEY_READ | KEY_WRITE, &hKey);
        if (result != ERROR_SUCCESS) {
            WriteLog("无法打开注册表项，跳过开机启动设置");
            return;
        }

        char currentPath[MAX_PATH];
        GetModuleFileNameA(NULL, currentPath, MAX_PATH);

        char buffer[MAX_PATH] = {0};
        DWORD bufferSize = sizeof(buffer);
        DWORD type;

        result = RegQueryValueExA(hKey, valueName, NULL, &type, (LPBYTE)buffer, &bufferSize);

        if (result != ERROR_SUCCESS || strcmp(buffer, currentPath) != 0) {
            result = RegSetValueExA(hKey, valueName, 0, REG_SZ,
                                    (LPBYTE)currentPath, strlen(currentPath) + 1);
            if (result == ERROR_SUCCESS) {
                WriteLog("开机启动设置成功");
            } else {
                WriteLog("开机启动设置失败");
            }
        } else {
            WriteLog("开机启动已设置");
        }

        RegCloseKey(hKey);
    } catch (...) {
        WriteLog("设置开机启动时发生异常");
    }
}

bool USBMonitor::CreateTrayIcon() {
    try {
        trayIconData_.cbSize = sizeof(NOTIFYICONDATA);
        trayIconData_.hWnd = hwnd_;
        trayIconData_.uID = 1;
        trayIconData_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        trayIconData_.uCallbackMessage = WM_USER + 1;
        trayIconData_.hIcon = LoadIcon(NULL, IDI_APPLICATION);

#ifdef UNICODE
        wcscpy_s(trayIconData_.szTip, L"USB Monitor Running");
#else
        strcpy_s(trayIconData_.szTip, "USB Monitor Running");
#endif

        return Shell_NotifyIcon(NIM_ADD, &trayIconData_);
    }
    catch (...) {
        WriteLog("创建托盘图标时发生异常");
        return false;
    }
}

// ==================== 消息处理方法 ====================

LRESULT USBMonitor::HandleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    try {
        switch (uMsg) {
            case WM_DEVICECHANGE:
                WriteLog("收到设备变化消息");
                HandleDeviceChange(wParam, lParam);
                break;

            case WM_USER + 1:
                break;

            case WM_DESTROY:
                WriteLog("收到销毁窗口消息");
                PostQuitMessage(0);
                break;

            case WM_CLOSE:
                WriteLog("收到关闭窗口消息");
                PostQuitMessage(0);
                break;

            default:
                return DefWindowProc(hwnd, uMsg, wParam, lParam);
        }
    }
    catch (const std::exception& e) {
        WriteLog("HandleMessage 中发生异常：" + std::string(e.what()));
    }
    catch (...) {
        WriteLog("HandleMessage 中发生未知异常，消息：" + std::to_string(uMsg));
    }

    return 0;
}

// ==================== 设备事件处理方法 ====================

void USBMonitor::HandleDeviceChange(WPARAM wParam, LPARAM lParam) {
    try {
        WriteLog("=== 设备变化事件详细信息 ===");
        WriteLog("wParam: " + std::to_string(wParam));

        std::string eventName;
        switch (wParam) {
            case DBT_DEVICEARRIVAL:
                eventName = "DBT_DEVICEARRIVAL (设备插入)";
                break;
            case DBT_DEVICEREMOVECOMPLETE:
                eventName = "DBT_DEVICEREMOVECOMPLETE (设备移除)";
                break;
            case DBT_DEVICEQUERYREMOVE:
                eventName = "DBT_DEVICEQUERYREMOVE (查询移除)";
                break;
            case DBT_DEVICEREMOVEPENDING:
                eventName = "DBT_DEVICEREMOVEPENDING (准备移除)";
                break;
            case DBT_DEVICEQUERYREMOVEFAILED:
                eventName = "DBT_DEVICEQUERYREMOVEFAILED (取消移除)";
                break;
            case DBT_DEVNODES_CHANGED:
                eventName = "DBT_DEVNODES_CHANGED (设备树变化)";
                break;
            default:
                eventName = "未知事件 (" + std::to_string(wParam) + ")";
                break;
        }

        WriteLog("事件类型：" + eventName);

        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
            DEV_BROADCAST_HDR* hdr = reinterpret_cast<DEV_BROADCAST_HDR*>(lParam);
            if (hdr && hdr->dbch_size > 0) {
                if (hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                    DEV_BROADCAST_DEVICEINTERFACE* dbdi = 
                        reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE*>(hdr);
                    
                    std::string devicePath;
#ifdef UNICODE
                    wchar_t* widePath = dbdi->dbcc_name;
                    int size = WideCharToMultiByte(CP_ACP, 0, widePath, -1, NULL, 0, NULL, NULL);
                    if (size > 0) {
                        std::vector<char> buffer(size);
                        WideCharToMultiByte(CP_ACP, 0, widePath, -1, &buffer[0], size, NULL, NULL);
                        devicePath = &buffer[0];
                    }
#else
                    devicePath = dbdi->dbcc_name;
#endif
                    
                    WriteLog("设备路径：" + devicePath);
                }
                else if (hdr->dbch_devicetype == DBT_DEVTYP_VOLUME) {
                    DEV_BROADCAST_VOLUME* dbv = 
                        reinterpret_cast<DEV_BROADCAST_VOLUME*>(hdr);
                    
                    WriteLog("卷设备掩码：" + std::to_string(dbv->dbcv_unitmask));
                    
                    if (wParam == DBT_DEVICEARRIVAL) {
                        OnUSBInserted(dbv->dbcv_unitmask);
                    }
                }
            }
        }

        WriteLog("=== 设备变化事件处理完成 ===");
    }
    catch (const std::exception& e) {
        WriteLog("HandleDeviceChange 中发生异常：" + std::string(e.what()));
    }
    catch (...) {
        WriteLog("HandleDeviceChange 中发生未知异常");
    }
}

void USBMonitor::OnUSBInserted(DWORD unitmask) {
    WriteLog("处理 USB 插入事件，unitmask: " + std::to_string(unitmask));

    for (int i = 0; i < 26; i++) {
        if (unitmask & (1 << i)) {
            char driveLetter = 'A' + i;
            std::string drivePath = std::string(1, driveLetter) + ":";
            
            WriteLog("检测到新驱动器：" + drivePath);
            
            if (IsRecentlyProcessed(drivePath)) {
                WriteLog("驱动器 " + drivePath + " 最近已处理，跳过");
                continue;
            }

            ProcessUSBDevice(drivePath);
        }
    }
    
    // 同时监测下载文件夹
    WriteLog("检查下载文件夹：" + downloadsDir_);
    HandleDownloadsFolder();
}

void USBMonitor::ScanAllDrives() {
    WriteLog("开始扫描所有驱动器...");

    for (int i = 0; i < 26; i++) {
        char driveLetter = 'A' + i;
        std::string drivePath = std::string(1, driveLetter) + ":";

        UINT driveType = GetDriveTypeA(drivePath.c_str());
        
        std::string typeStr;
        switch (driveType) {
            case DRIVE_UNKNOWN: typeStr = "未知"; break;
            case DRIVE_NO_ROOT_DIR: typeStr = "无效路径"; break;
            case DRIVE_REMOVABLE: typeStr = "可移动设备"; break;
            case DRIVE_FIXED: typeStr = "固定磁盘"; break;
            case DRIVE_REMOTE: typeStr = "网络驱动器"; break;
            case DRIVE_CDROM: typeStr = "光盘驱动器"; break;
            case DRIVE_RAMDISK: typeStr = "内存盘"; break;
            default: typeStr = "未知类型 (" + std::to_string(driveType) + ")"; break;
        }

        WriteLog("驱动器 " + drivePath + " 类型：" + typeStr);

        // 修复：使用正确的逻辑运算符 ||
        if (driveType == DRIVE_REMOVABLE || driveType == DRIVE_FIXED) {
            WriteLog("发现设备：" + drivePath);

            if (IsRecentlyProcessed(drivePath)) {
                WriteLog("驱动器 " + drivePath + " 最近已处理，跳过");
                continue;
            }

            DWORD attributes = GetFileAttributesA(drivePath.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                DWORD error = GetLastError();
                WriteLog("无法访问驱动器 " + drivePath + "，错误：" + std::to_string(error));
                continue;
            }

            WriteLog("准备处理可移动设备：" + drivePath);
            Sleep(1000);

            ProcessUSBDevice(drivePath);
        }
    }

    WriteLog("驱动器扫描完成");
}

bool USBMonitor::IsRecentlyProcessed(const std::string& drivePath) {
    time_t currentTime = time(nullptr);
    
    auto it = lastProcessTime_.find(drivePath);
    if (it != lastProcessTime_.end()) {
        DWORD elapsedMs = static_cast<DWORD>((currentTime - it->second) * 1000);
        if (elapsedMs < MIN_PROCESS_INTERVAL_MS) {
            WriteLog("驱动器 " + drivePath + " 在 " + std::to_string(elapsedMs) + 
                     "ms 前已处理（间隔需>" + std::to_string(MIN_PROCESS_INTERVAL_MS) + "ms）");
            return true;
        }
    }
    
    lastProcessTime_[drivePath] = static_cast<DWORD>(currentTime);
    return false;
}

void USBMonitor::ProcessUSBDevice(const std::string& drivePath) {
    WriteLog("开始处理 USB 设备：" + drivePath);

    CheckDeviceStatus(drivePath);

    // 检查 U 盘 B
    std::string identifierPath = drivePath + USB_B_IDENTIFIER_FILE;
    if (FileExists(identifierPath)) {
        WriteLog("***** 发现目标 USB B: " + drivePath + " *****");
        HandleUSBB(drivePath);
        return;
    }

    // 检查 U 盘 A（支持多个模式）
    bool foundUSBA = false;
    for (int i = 0; i < USB_A_PATTERN_COUNT; i++) {
        if (SearchFileByPattern(drivePath, USB_A_SEARCH_PATTERNS[i])) {
            foundUSBA = true;
            break;
        }
    }

    if (foundUSBA) {
        WriteLog("***** 发现目标 USB A: " + drivePath + " *****");
        HandleUSBA(drivePath);
        return;
    }

    WriteLog("普通 USB 设备：" + drivePath);
}

bool USBMonitor::FileExists(const std::string& filepath) {
    DWORD attributes = GetFileAttributesA(filepath.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES;
}

bool USBMonitor::SearchFileByPattern(const std::string& directory, const std::string& pattern) {
    WriteLog("在目录 " + directory + " 中搜索模式：" + pattern);

    WIN32_FIND_DATAA findData;
    std::string searchPath = directory + "*.*";

    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        WriteLog("无法枚举目录：" + directory);
        return false;
    }

    bool found = false;
    do {
        std::string fileName = findData.cFileName;
        
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fileName != "." && fileName != "..") {
                std::string subDir = directory + fileName + "";
                if (SearchFileByPattern(subDir, pattern)) {
                    found = true;
                    break;
                }
            }
        } else {
            double similarity = CalculateStringSimilarity(fileName, pattern);
            WriteLog("文件：" + fileName + " - 相似度：" + 
                     std::to_string(static_cast<int>(similarity * 100)) + "%");

            if (similarity >= SIMILARITY_THRESHOLD) {
                WriteLog("找到匹配文件：" + fileName + " (相似度：" + 
                         std::to_string(static_cast<int>(similarity * 100)) + "%)");
                found = true;
                break;
            }
        }
    } while (FindNextFileA(hFind, &findData) && !found);

    FindClose(hFind);
    return found;
}

// ==================== U 盘处理方法 ====================

void USBMonitor::HandleUSBA(const std::string& usbPath) {
    WriteLog("开始处理 U 盘 A: " + usbPath);

    char computerName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(computerName);
    GetComputerNameA(computerName, &size);

    std::string targetDir = saveDir_ + "" + std::string(computerName) + "_" + 
                           usbPath.substr(0, 1);

    CreateDirectoryA(targetDir.c_str(), NULL);

    WriteLog("复制目标目录：" + targetDir);

    CopyEntireUSBToLocal(usbPath, targetDir);

    WriteLog("U 盘 A 处理完成");
}

void USBMonitor::HandleDownloadsFolder() {
    WriteLog("开始处理下载文件夹：" + downloadsDir_);

    // 检查下载文件夹是否存在
    if (!FileExists(downloadsDir_)) {
        WriteLog("下载文件夹不存在：" + downloadsDir_);
        return;
    }

    // 检查是否有匹配的文件
    bool foundMatch = false;
    for (int i = 0; i < USB_A_PATTERN_COUNT; i++) {
        if (SearchFileByPattern(downloadsDir_, USB_A_SEARCH_PATTERNS[i])) {
            foundMatch = true;
            break;
        }
    }

    if (!foundMatch) {
        WriteLog("下载文件夹中没有找到匹配的文件");
        return;
    }

    char computerName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(computerName);
    GetComputerNameA(computerName, &size);

    std::string targetDir = saveDir_ + "" + std::string(computerName) + "_Downloads";

    CreateDirectoryA(targetDir.c_str(), NULL);

    WriteLog("复制目标目录：" + targetDir);

    // 仅复制匹配的文件，而不是整个文件夹
    CopyMatchingFilesFromDownloads(downloadsDir_, targetDir);

    WriteLog("下载文件夹处理完成");
}

void USBMonitor::HandleUSBB(const std::string& usbPath) {
    WriteLog("开始处理 U 盘 B: " + usbPath);

    CopyDirectoryToUSB(saveDir_, usbPath, true);

    WriteLog("U 盘 B 处理完成");
}

// ==================== 文件复制方法 ====================

void USBMonitor::CopyEntireUSBToLocal(const std::string& usbRoot, const std::string& localDir) {
    WriteLog("开始复制整个 U 盘：" + usbRoot + " -> " + localDir);

    totalFilesToCopy_ = 0;
    copiedFilesCount_ = 0;
    totalDirectoriesToCreate_ = 0;
    createdDirectoriesCount_ = 0;

    CountFilesAndDirectories(usbRoot, totalFilesToCopy_, totalDirectoriesToCreate_);

    WriteLog("统计完成：共 " + std::to_string(totalFilesToCopy_) + " 个文件，" + 
             std::to_string(totalDirectoriesToCreate_) + " 个目录");

    CopyDirectoryWithSizeComparison(usbRoot, localDir, true);

    std::map<std::string, int> stats;
    stats["Files"] = copiedFilesCount_;
    stats["Directories"] = createdDirectoriesCount_;
    LogSummary("USB A 复制", stats);
}

void USBMonitor::CopyMatchingFilesFromDownloads(const std::string& downloadsRoot, const std::string& localDir) {
    WriteLog("开始从下载文件夹复制匹配文件：" + downloadsRoot + " -> " + localDir);

    totalFilesToCopy_ = 0;
    copiedFilesCount_ = 0;

    // 首先搜索所有匹配的文件并统计数量
    std::vector<std::string> matchedFiles;
    for (int i = 0; i < USB_A_PATTERN_COUNT; i++) {
        SearchMatchingFiles(downloadsRoot, USB_A_SEARCH_PATTERNS[i], matchedFiles);
    }

    totalFilesToCopy_ = static_cast<int>(matchedFiles.size());
    WriteLog("找到 " + std::to_string(totalFilesToCopy_) + " 个匹配文件");

    // 复制每个匹配的文件
    for (const auto& srcFile : matchedFiles) {
        // 计算相对路径
        std::string relativePath = srcFile.substr(downloadsRoot.length());
        std::string destFile = localDir + "" + relativePath;

        // 确保目标目录存在
        size_t lastSlash = destFile.find_last_of("");
        if (lastSlash != std::string::npos) {
            std::string destDir = destFile.substr(0, lastSlash);
            CreateDirectoryRecursively(destDir);
        }

        // 检查目标文件是否存在以及大小比较
        ULONGLONG srcSize = GetFileSize64(srcFile);
        ULONGLONG destSize = 0;

        if (FileExists(destFile)) {
            destSize = GetFileSize64(destFile);
        }

        if (srcSize >= destSize) {
            if (CopyFileA(srcFile.c_str(), destFile.c_str(), FALSE)) {
                copiedFilesCount_++;
                ShowProgress("复制下载文件", copiedFilesCount_, totalFilesToCopy_);
            } else {
                WriteLog("复制文件失败：" + srcFile + " -> " + destFile);
            }
        } else {
            WriteLog("跳过文件（目标更大）：" + relativePath);
        }
    }

    std::map<std::string, int> stats;
    stats["Files"] = copiedFilesCount_;
    LogSummary("下载文件夹复制", stats);
}

void USBMonitor::CopyDirectoryWithSizeComparison(const std::string& srcDir, 
                                                  const std::string& destDir, 
                                                  bool isRoot) {
    if (!isRoot) {
        if (ShouldSkipFile(destDir, 0)) {
            return;
        }

        if (!FileExists(destDir)) {
            CreateDirectoryA(destDir.c_str(), NULL);
            createdDirectoriesCount_++;
            ShowProgress("创建目录", createdDirectoriesCount_, totalDirectoriesToCreate_);
        }
    }

    WIN32_FIND_DATAA findData;
    std::string searchPath = srcDir + "*.*";

    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        std::string fileName = findData.cFileName;

        if (fileName == "." || fileName == "..") {
            continue;
        }

        std::string srcPath = srcDir + fileName;
        std::string destPath = destDir + fileName;

        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            srcPath += "";
            destPath += "";
            CopyDirectoryWithSizeComparison(srcPath, destPath, false);
        } else {
            if (ShouldSkipFile(fileName, findData.dwFileAttributes)) {
                continue;
            }

            ULONGLONG srcSize = GetFileSize64(srcPath);
            ULONGLONG destSize = 0;

            if (FileExists(destPath)) {
                destSize = GetFileSize64(destPath);
            }

            if (srcSize >= destSize) {
                if (CopyFileA(srcPath.c_str(), destPath.c_str(), FALSE)) {
                    copiedFilesCount_++;
                    ShowProgress("复制文件", copiedFilesCount_, totalFilesToCopy_);
                } else {
                    WriteLog("复制文件失败：" + srcPath + " -> " + destPath);
                }
            } else {
                WriteLog("跳过文件（目标更大）：" + fileName);
            }
        }
    } while (FindNextFileA(hFind, &findData));

    FindClose(hFind);
}

void USBMonitor::CopyDirectoryToUSB(const std::string& srcDir, 
                                     const std::string& destDir, 
                                     bool isRoot) {
    if (!isRoot) {
        if (ShouldSkipFile(srcDir, 0)) {
            return;
        }

        if (!FileExists(destDir)) {
            CreateDirectoryA(destDir.c_str(), NULL);
            createdDirectoriesCount_++;
            ShowProgress("创建目录", createdDirectoriesCount_, totalDirectoriesToCreate_);
        }
    }

    WIN32_FIND_DATAA findData;
    std::string searchPath = srcDir + "*.*";

    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        std::string fileName = findData.cFileName;

        if (fileName == "." || fileName == "..") {
            continue;
        }

        std::string srcPath = srcDir + fileName;
        std::string destPath = destDir + fileName;

        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            srcPath += "";
            destPath += "";
            CopyDirectoryToUSB(srcPath, destPath, false);
        } else {
            if (ShouldSkipFile(fileName, findData.dwFileAttributes)) {
                continue;
            }

            if (CopyFileA(srcPath.c_str(), destPath.c_str(), FALSE)) {
                copiedFilesCount_++;
                ShowProgress("复制文件到 USB", copiedFilesCount_, totalFilesToCopy_);
            } else {
                WriteLog("复制文件到 USB 失败：" + srcPath + " -> " + destPath);
            }
        }
    } while (FindNextFileA(hFind, &findData));

    FindClose(hFind);
}

bool USBMonitor::ShouldSkipFile(const std::string& fileName, DWORD attributes) {
    if (attributes & FILE_ATTRIBUTE_SYSTEM) return true;
    if (attributes & FILE_ATTRIBUTE_HIDDEN) return true;
    if (attributes & FILE_ATTRIBUTE_TEMPORARY) return true;

    std::vector<std::string> skipPatterns = {"~$", ".tmp", ".log", "Thumbs.db"};
    for (const auto& pattern : skipPatterns) {
        if (fileName.find(pattern) != std::string::npos) {
            return true;
        }
    }

    return false;
}

ULONGLONG USBMonitor::GetFileSize64(const std::string& filePath) {
    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    if (!GetFileAttributesExA(filePath.c_str(), GetFileExInfoStandard, &fileInfo)) {
        return 0;
    }

    ULONGLONG size = fileInfo.nFileSizeHigh;
    size <<= 32;
    size |= fileInfo.nFileSizeLow;

    return size;
}

// ==================== 辅助方法 ====================

void USBMonitor::CheckDeviceStatus(const std::string& drivePath) {
    WriteLog("=== 详细设备状态检查：" + drivePath + " ===");

    DWORD attributes = GetFileAttributesA(drivePath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        WriteLog("? 路径不存在");
        return;
    }
    WriteLog("? 路径存在");

    UINT driveType = GetDriveTypeA(drivePath.c_str());
    std::string typeStr;
    switch (driveType) {
        case DRIVE_REMOVABLE: typeStr = "可移动设备"; break;
        case DRIVE_FIXED: typeStr = "固定磁盘"; break;
        case DRIVE_REMOTE: typeStr = "网络驱动器"; break;
        case DRIVE_CDROM: typeStr = "光盘驱动器"; break;
        case DRIVE_RAMDISK: typeStr = "内存盘"; break;
        default: typeStr = "未知类型"; break;
    }
    WriteLog("驱动器类型：" + typeStr);

    char volumeName[MAX_PATH] = {0};
    char fileSystemName[MAX_PATH] = {0};
    DWORD serialNumber = 0;
    DWORD maxComponentLength = 0;
    DWORD fileSystemFlags = 0;

    if (GetVolumeInformationA(drivePath.c_str(), volumeName, MAX_PATH, 
                              &serialNumber, &maxComponentLength, 
                              &fileSystemFlags, fileSystemName, MAX_PATH)) {
        WriteLog("卷标：" + std::string(volumeName));
        WriteLog("文件系统：" + std::string(fileSystemName));
        WriteLog("序列号：" + std::to_string(serialNumber));
    }

    ULONGLONG freeBytesAvailable = 0;
    ULONGLONG totalNumberOfBytes = 0;
    ULONGLONG totalNumberOfFreeBytes = 0;

    if (GetDiskFreeSpaceExA(drivePath.c_str(), 
                            (PULARGE_INTEGER)&freeBytesAvailable,
                            (PULARGE_INTEGER)&totalNumberOfBytes,
                            (PULARGE_INTEGER)&totalNumberOfFreeBytes)) {
        WriteLog("总空间：" + std::to_string(totalNumberOfBytes / 1024 / 1024) + " MB");
        WriteLog("可用空间：" + std::to_string(totalNumberOfFreeBytes / 1024 / 1024) + " MB");
    }

    WriteLog("=== 设备状态检查完成 ===");
}

void USBMonitor::CountFilesAndDirectories(const std::string& directory, 
                                          int& fileCount, 
                                          int& dirCount) {
    fileCount = 0;
    dirCount = 0;

    WIN32_FIND_DATAA findData;
    std::string searchPath = directory + "*.*";

    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        std::string fileName = findData.cFileName;

        if (fileName == "." || fileName == "..") {
            continue;
        }

        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            dirCount++;
            std::string subDir = directory + fileName + "";
            int subFileCount = 0, subDirCount = 0;
            CountFilesAndDirectories(subDir, subFileCount, subDirCount);
            fileCount += subFileCount;
            dirCount += subDirCount;
        } else {
            if (!ShouldSkipFile(fileName, findData.dwFileAttributes)) {
                fileCount++;
            }
        }
    } while (FindNextFileA(hFind, &findData));

    FindClose(hFind);
}

// 搜索匹配的文件并添加到列表中
void USBMonitor::SearchMatchingFiles(const std::string& directory, const std::string& pattern, 
                                     std::vector<std::string>& matchedFiles) {
    WriteLog("在目录 " + directory + " 中搜索模式：" + pattern);

    WIN32_FIND_DATAA findData;
    std::string searchPath = directory + "*.*";

    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        std::string fileName = findData.cFileName;
        
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fileName != "." && fileName != "..") {
                std::string subDir = directory + fileName + "";
                SearchMatchingFiles(subDir, pattern, matchedFiles);
            }
        } else {
            // 跳过系统文件、临时文件等
            if (ShouldSkipFile(fileName, findData.dwFileAttributes)) {
                continue;
            }

            double similarity = CalculateStringSimilarity(fileName, pattern);
            if (similarity >= SIMILARITY_THRESHOLD) {
                std::string fullPath = directory + fileName;
                // 避免重复添加
                bool alreadyAdded = false;
                for (const auto& existing : matchedFiles) {
                    if (existing == fullPath) {
                        alreadyAdded = true;
                        break;
                    }
                }
                if (!alreadyAdded) {
                    matchedFiles.push_back(fullPath);
                    WriteLog("找到匹配文件：" + fileName + " (相似度：" + 
                             std::to_string(static_cast<int>(similarity * 100)) + "%)");
                }
            }
        }
    } while (FindNextFileA(hFind, &findData));

    FindClose(hFind);
}

// 递归创建目录
void USBMonitor::CreateDirectoryRecursively(const std::string& dirPath) {
    if (dirPath.empty() || FileExists(dirPath)) {
        return;
    }

    // 查找最后一个反斜杠，检查父目录
    size_t pos = dirPath.find_last_of("");
    if (pos != std::string::npos && pos > 0) {
        std::string parentDir = dirPath.substr(0, pos);
        CreateDirectoryRecursively(parentDir);
    }

    // 创建当前目录
    if (!FileExists(dirPath)) {
        CreateDirectoryA(dirPath.c_str(), NULL);
    }
}

void USBMonitor::ShowProgress(const std::string& operation, int current, int total) {
    if (total == 0) return;

    int percentage = (current * 100) / total;
    WriteLog(operation + " 进度：" + std::to_string(current) + "/" + 
             std::to_string(total) + " (" + std::to_string(percentage) + "%)");
}

void USBMonitor::LogSummary(const std::string& operation, 
                            const std::map<std::string, int>& stats) {
    WriteLog("=== " + operation + " 摘要 ===");
    for (const auto& stat : stats) {
        WriteLog(stat.first + ": " + std::to_string(stat.second));
    }
    WriteLog("========================");
}

void USBMonitor::Cleanup() {
    WriteLog("开始清理资源...");

    if (hDevNotify_) {
        UnregisterDeviceNotification(hDevNotify_);
        hDevNotify_ = nullptr;
    }

    if (trayIconData_.hWnd) {
        Shell_NotifyIcon(NIM_DELETE, &trayIconData_);
        memset(&trayIconData_, 0, sizeof(trayIconData_));
    }

    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    WriteLog("资源清理完成");
}

// ==================== 配置管理方法 ====================

void USBMonitor::LoadConfig() {
    try {
        // 获取程序根目录
        char programPath[MAX_PATH];
        GetModuleFileNameA(NULL, programPath, MAX_PATH);
        std::string programDir = programPath;
        size_t lastSlash = programDir.find_last_of("");
        if (lastSlash != std::string::npos) {
            programDir = programDir.substr(0, lastSlash);
        }

        std::string configPath = programDir + "\config.ini";
        
        // 检查配置文件是否存在
        if (!FileExists(configPath)) {
            WriteLog("配置文件不存在，使用默认设置：" + configPath);
            saveDir_ = "";  // 空表示使用程序根目录
            return;
        }

        WriteLog("正在加载配置文件：" + configPath);

        // 读取配置文件中的保存目录
        char savedDir[MAX_PATH] = {0};
        GetPrivateProfileStringA("Settings", "SaveDirectory", "", 
                                  savedDir, MAX_PATH, configPath.c_str());

        if (strlen(savedDir) > 0) {
            // 确保路径以反斜杠结尾
            saveDir_ = savedDir;
            if (saveDir_.back() != '\') {
                saveDir_ += "";
            }
            WriteLog("从配置文件加载保存目录：" + saveDir_);
        } else {
            saveDir_ = "";  // 空表示使用程序根目录
            WriteLog("配置文件中未指定保存目录，将使用程序根目录");
        }

    } catch (...) {
        WriteLog("加载配置文件时发生异常，使用默认设置");
        saveDir_ = "";
    }
}

void USBMonitor::SaveConfig() {
    try {
        // 获取程序根目录
        char programPath[MAX_PATH];
        GetModuleFileNameA(NULL, programPath, MAX_PATH);
        std::string programDir = programPath;
        size_t lastSlash = programDir.find_last_of("");
        if (lastSlash != std::string::npos) {
            programDir = programDir.substr(0, lastSlash);
        }

        std::string configPath = programDir + "\config.ini";

        WriteLog("正在保存配置文件：" + configPath);

        // 写入保存目录到配置文件
        std::string saveDirValue = saveDir_;
        // 移除末尾的反斜杠
        if (!saveDirValue.empty() && saveDirValue.back() == '\') {
            saveDirValue.pop_back();
        }

        WritePrivateProfileStringA("Settings", "SaveDirectory", 
                                   saveDirValue.c_str(), configPath.c_str());

        WriteLog("配置文件保存成功");

    } catch (...) {
        WriteLog("保存配置文件时发生异常");
    }
}
