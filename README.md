# USB Monitor - Windows USB 设备监控程序

## 项目简介

这是一个 Windows 平台的 USB 设备监控程序，能够自动识别并处理两种类型的 U 盘，同时监控下载文件夹的文件变化：

- **U 盘 A**：通过文件名模糊匹配（Levenshtein 相似度算法）识别，将整个 U 盘内容复制到本地
- **U 盘 B**：通过标识文件 `USBB_IDENTIFIER.txt` 识别，将本地文件同步回 U 盘
- **下载文件夹**：实时监控用户下载文件夹，仅复制匹配的文件到本地保存目录

## 功能特性

### 核心功能
- 📁 **智能 U 盘识别**：自动区分 U 盘 A 和 U 盘 B，执行不同的同步策略
- 📥 **下载文件夹监控**：实时监测下载文件夹，自动提取匹配的重要文件
- 🔍 **模糊文件匹配**：使用 Levenshtein 相似度算法，支持文件名模糊搜索
- 💾 **智能文件复制**：比较源文件和目标文件大小，只复制较大的版本
- ⏱️ **防重复处理**：3 秒间隔机制，避免重复处理同一设备
- 📊 **进度跟踪**：详细的文件复制进度和统计信息
- 📝 **日志记录**：完整的操作日志，便于问题排查

### 系统功能
- 🖥️ **系统托盘运行**：后台静默运行，不干扰用户操作
- 🚀 **开机自启动**：自动注册到系统启动项
- 📂 **自定义保存目录**：支持配置文件保存路径（默认为程序根目录）

## 项目结构

```
/workspace
├── include/           # 头文件目录
│   └── usb_monitor.h  # USBMonitor 类定义和配置常量
├── src/               # 源代码目录
│   ├── main.cpp       # 程序入口和窗口过程
│   └── usb_monitor.cpp # USBMonitor 类实现
├── config/            # 配置文件目录（可选）
└── README.md          # 项目说明文档
```

## 编译说明

### 环境要求
- Windows 10/11
- Visual Studio 2019 或更高版本
- 或 MinGW-w64 with Windows SDK
- C++17 标准支持

### 使用 Visual Studio 编译
```bash
cl /EHsc /I include src/main.cpp src/usb_monitor.cpp /link user32.lib shell32.lib advapi32.lib setupapi.lib /OUT:USBMonitor.exe
```

### 使用 MinGW 编译
```bash
g++ -std=c++17 -I include src/main.cpp src/usb_monitor.cpp -o USBMonitor.exe -lsetupapi -lshell32 -luser32 -ladvapi32
```

## 使用方法

### 基本使用
1. 编译生成 `USBMonitor.exe`
2. 双击运行程序（首次运行建议以管理员身份运行以设置开机启动）
3. 程序会自动在系统托盘中运行
4. 插入 U 盘后自动检测并处理：
   - **U 盘 A**：如果包含匹配文件（如"选三 U8 答案版分课时打印.doc"、"微点"、"选二 U4 答案版"），则复制全部内容到本地保存目录
   - **U 盘 B**：如果包含 `USBB_IDENTIFIER.txt` 文件，则将本地文件同步到 U 盘
5. 下载文件夹中的匹配文件会自动复制到保存目录

### 自定义保存目录
程序默认将文件保存到程序运行的根目录下。如需修改保存路径：

**方法一：配置文件**
在程序同目录下创建 `config.ini` 文件，内容如下：
```ini
[Settings]
SavePath=D:\MyUSBFiles
```

**方法二：命令行参数**
```bash
USBMonitor.exe --save-path "D:\MyUSBFiles"
```

如果不指定保存目录，程序将使用当前工作目录作为文件保存位置。

### 日志查看
- 日志文件保存在程序根目录下，文件名格式为 `YYYY-MM-DD.log`
- 接收的文件保存在配置的保存目录中（默认为程序根目录）

## 配置说明

可在 `include/usb_monitor.h` 中修改以下配置：

```cpp
// U 盘 A 的搜索模式（支持模糊匹配）
static constexpr const char* USB_A_SEARCH_PATTERNS[] = {
    "选三 U8 答案版分课时打印.doc",
    "微点",
    "选二 U4 答案版"
};

// U 盘 B 的标识文件
static constexpr const char* USB_B_IDENTIFIER_FILE = "USBB_IDENTIFIER.txt";

// 防重复处理间隔（毫秒）
static constexpr DWORD MIN_PROCESS_INTERVAL_MS = 3000;

// 文件名相似度阈值（0.0-1.0）
static constexpr double SIMILARITY_THRESHOLD = 0.3;
```

## 技术细节

### 字符串相似度算法
使用 Levenshtein 距离计算两个字符串的编辑距离，从而判断文件名是否匹配。算法支持中文和混合字符，相似度阈值可配置。

### 设备通知机制
采用三层降级策略注册设备通知：
1. 首先尝试注册所有设备变化通知（`DBT_DEVTYP_DEVICEINTERFACE`）
2. 失败后尝试注册卷设备通知（`DBT_DEVTYP_VOLUME`）
3. 最后尝试最简单的注册方式

### 文件复制策略
- 跳过系统文件、隐藏文件、临时文件
- 比较源文件和目标文件大小，只复制较大的版本
- 递归复制整个目录结构
- 保持原有目录层级

### 下载文件夹监控
- 使用 Windows Shell API 获取当前用户的下载文件夹路径
- 定期扫描下载文件夹（每 5 秒）
- 仅复制匹配的文件，不复制整个文件夹

## 注意事项

⚠️ **仅限 Windows 平台**：本程序使用 Windows API，无法在其他操作系统上运行

⚠️ **需要管理员权限**：设置开机启动时需要访问注册表，建议首次运行时右键选择"以管理员身份运行"

⚠️ **杀毒软件可能误报**：由于程序监控 USB 设备并自动复制文件，某些杀毒软件可能会误报，请添加信任或将程序加入白名单

⚠️ **U 盘文件系统**：支持 FAT32、exFAT、NTFS 等常见文件系统

⚠️ **网络驱动器**：不支持网络驱动器和映射驱动器

## 常见问题

### Q: 程序运行后看不到界面？
A: 程序设计为系统托盘应用，运行后会在任务栏右下角显示图标，不会显示主窗口。

### Q: 如何确认程序是否在运行？
A: 查看任务栏右下角的系统托盘区域，应该能看到 USBMonitor 的图标。也可以在任务管理器中查看进程。

### Q: 文件没有自动复制？
A: 请检查：
1. 日志文件中是否有错误信息
2. U 盘是否包含匹配的标识文件或文件名
3. 目标目录是否有写入权限
4. 杀毒软件是否阻止了程序运行

### Q: 如何关闭程序？
A: 右键点击系统托盘图标，选择"退出"即可关闭程序。

## 许可证

本项目仅供学习参考使用。
