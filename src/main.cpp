/**
 * @file main.cpp
 * @brief USB 监控程序入口
 * 
 * Windows USB 设备监控程序，自动识别并处理两种类型的 U 盘：
 * - U 盘 A：通过文件名模糊匹配识别，将整个 U 盘内容复制到本地
 * - U 盘 B：通过标识文件识别，将本地文件同步回 U 盘
 */

#include "usb_monitor.h"
#include <iostream>

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "       USB Monitor Starting..." << std::endl;
    std::cout << "========================================" << std::endl;

    USBMonitor monitor;

    if (!monitor.Initialize()) {
        std::cerr << "Failed to initialize USB Monitor!" << std::endl;
        return 1;
    }

    std::cout << "USB Monitor initialized successfully." << std::endl;
    std::cout << "Running in system tray. Press Ctrl+C to exit." << std::endl;

    monitor.Run();

    std::cout << "USB Monitor exited." << std::endl;
    return 0;
}
