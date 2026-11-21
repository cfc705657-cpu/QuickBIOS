# QuickBIOS —— 5 键 UEFI 一键入口（Qt 6 / MSVC2022）

## 覆盖即用的 5 个文件
| 文件 | 作用 |
|---|---|
| `mainwindow.ui` | **严格按你的 Designer 截图**：188×340、QVBoxLayout、lx(QLineEdit,"在这里输入") + motherboard / shutdown / bios / boot / safe，objectName 一字不差 |
| `mainwindow.h` | 只有 Qt 类型，**零 Windows 头** → moc 干净 |
| `mainwindow.cpp` | 真 UEFI 逻辑：GetFirmwareType / OsIndications 位掩码 / BootOrder+Boot#### 解析 / BootNext 兜底 / bcdedit 一次性安全模式+RunOnce 自愈 |
| `main.cpp` | `ShellExecuteExW(runas)` 自提权，**透传 argv**，UAC 被拒 → 只读模式（不 return 1 自杀） |
| `CMakeLists.txt` | AUTOMOC 含 .h 治 LNK2001；`CMAKE_MT` 自动定位治 LNK1327；`/MANIFEST:NO` 单发治 LNK4075 |
| `quickbios.manifest` | requireAdministrator + Win8/10/11 supportedOS + ComCtl v6（**不进源列表**，只喂 mt.exe） |

## 编译（Qt Creator 直接开 CMakeLists.txt 即可）
1. 路径纯英文无空格：`E:\Dev\QuickBIOS`（`E:\C++\...` 里的 `+` 虽合法，但中文区域代码页下 mt.exe 仍会抽风，建议换掉）
2. Kit 选 `Desktop Qt 6.11.1 MSVC2022 64bit` → 构建
3. 第一次运行弹 UAC → **是**

## 五个按钮干什么
| 按钮 | UEFI bit0/bit1 全支持 | 只有 bit0（华硕/技嘉常见） | Legacy/CSM | 拒了 UAC |
|---|---|---|---|---|
| 检测主板启动类型 | ✅ 重测并刷新全部策略 | ✅ | ✅ | ✅（唯一可用） |
| 关机（windows8专用） | ✅ `shutdown /s /hybrid` 快速关机 | ✅ | ✅ | 🔒 |
| 进入bios | ✅ 写 bit0 精准直达 | ✅ 可写时精准 / 否则 `/fw` | 🔒 + 日志给 mbr2gpt & Pico 两条路 | 🔒 |
| 进入启动项 | ✅ 写 bit1 = 软 F12 | ✅ **自动降级 BootNext=USB**（一次性，不动 BootOrder） | 🔒 | 🔒 |
| 进入安全模式 | ✅ 一次性 safeboot + `HKLM\...\RunOnce` 自愈 | ✅ | ✅ | 🔒 |

`lx` 编辑框（你截图里那个"在这里输入"）显示**一行最要紧的结论**，例如：
`UEFI | BIOS:OK Boot:BootNext | SanDisk 3.2Gen1`
详细证据链走状态栏 + 调试输出。

## 历史 9 个编译/链接错误 → 本版断根点
| 报错 | 根因 | 这里怎么断的 |
|---|---|---|
| C2061 `PGUID` 未声明 | 头文件里混进了 windows.h | `.h` 零 Windows 类型，GUID 硬编码 16 字节 |
| C3861 `NtSetFirmwareEnvironmentVariable` | ntdll **不导出**它，用户态永远链不上 | 删；改 `GetProcAddress` 挑 `...ExW` / `...W` |
| C2660 `SetFirmwareEnvironmentVariableW` 不接受 5 参 | Win8 SDK 里它是 **4 参**，Win10 才有 6 参 Ex 版 | 两套 typedef 分开声明，运行时二选一，编译期不挑 SDK |
| C3861 `CLSIDFromString` | 属 `<objbase.h>`+ole32，常漏 | **根本不用**：字符串 GUID 直接传，结构体 GUID 硬编码 |
| C1083 `QTextCodec` | Qt6 已移出 QtCore | 删 include；全程 QString(UTF-16) |
| C2039 `QDateTime::currentTime` | 没有这成员 | `currentDateTime().toString("HH:mm:ss")` |
| C2672 `QString::arg` 无匹配 | C 字面量上调 `.arg()` | 一律 `QString("[%1] %2").arg(a, b)` |
| **LNK2001 `metaObject/qt_metacast/qt_metacall`** → LNK1120 | `mainwindow.h` 没进 `add_executable` 源列表 → AUTOMOC 不扫 → moc 没生成 | 源列表显式列 `.h`，且 `AUTOMOC ON` 放在 `add_executable` **之前** + `CACHE FORCE` 防 kit 覆盖 |
| **LNK4075 + LNK1327（mt.exe 乱码）** | 同时给 `/MANIFEST:NO` 和 `/MANIFESTFILE` 自相矛盾；Qt Creator 的 PATH 没 VS 工具链目录，裸写 `mt.exe` 找不到 | 链接器只发 `/MANIFEST:NO`；`find_program(CMAKE_MT mt.exe ...)` 从 **编译器目录**反查 Hostx64/x64；POST_BUILD 用绝对路径嵌；`/utf-8`；manifest 独立文件且不入源列表 |

## 运行后秒退 / "terminated abnormally"？
已根治：`main.cpp` 不再 `return 1`。
- 有 UAC → `ShellExecuteExW(runas)` 交棒后 `return 0`
- 拒 UAC → 窗口照常起来，五键上锁，lx 显示"只读模式：右键 exe 以管理员运行"
- **透传 `arguments().mid(1)`** 是隐藏关键：丢掉 Qt Creator 注入的 `--platformpluginpath`，提权后的新进程找不到 `platforms/qwindows.dll`，会二次秒崩

## 自检 moc 真的跑了
```bat
dir /s /b build | findstr moc_mainwindow
```
能看到 `..._autogen\moc_Debug\moc_mainwindow.cpp` 才算过。看不到 → 别往下走，99% 是 `.h` 又被踢出源列表。

## 首次跑完把这 4 行发我
```
权限：管理员 ✓
API：Get=OK SetEx=OK Set=OK
OsInd 0x0000000000000001 b0:Y b3:N 写:OK
BootNext 候选: SanDisk 3.2Gen1
```
- `SetEx=缺 Set=OK` → Win8/8.1 或旧 SDK，已自动走 4 参路径，不用改
- `b3:N` + 有 USB 候选 → Boot 键已经等价软 F12，收工日用
- `b3:N` + 无 USB 候选 → 插 U 盘重测；还认不出告我主板型号，补 Device Path 深度解析（判 `USB_CLASS`/`USB_WWID` 节点，不靠名字猜）
- `Legacy/CSM` 红标 → 给你 `mbr2gpt /validate /allowFullOS` → `/convert` 核对清单 + Pico 伪装键盘 CircuitPython 固件（上电 200ms 内 2ms 间隔狂发 F1/F12）
