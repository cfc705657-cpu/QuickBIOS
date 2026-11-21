# QuickBIOS

一个基于 Qt6 + C++ 的 Windows 轻量级工具，通过 UEFI 变量与 Windows API 实现“软按键”进入 BIOS / 启动项 / 安全模式。

## ✨ 核心功能
- **检测主板启动类型**：一行看清 UEFI/Legacy + OsIndications 状态。
- **进入 BIOS**：软 F2，写入 `OsIndications` bit0。
- **进入启动项**：软 F12；针对华硕/技嘉 bit1=0 自动降级走一次性 `BootNext`，不动 `BootOrder`。
- **进入安全模式**：`bcdedit safeboot` + RunOnce 自愈，不锁死系统。
- **快速关机**：Win8+ 快速关机支持。

## ⚠️ 使用注意事项（必读）
1. **仅支持 UEFI 启动**；Legacy/CSM 下 BIOS 与启动项两键自动上锁。
2. **必须管理员权限**，未提权时进入只读诊断模式。
3. **路径请保持纯英文**，含中文时 `mt.exe` 会报 LNK1327。
4. **未代码签名**，SmartScreen 需点「更多信息 → 仍要运行」。
5. 极老/山寨 BIOS 对 OsIndications 实现有缺陷，重启黑屏超 60 秒则拔电长按 10 秒强制恢复。
6. 安全模式键在仅蓝牙键鼠的机器上慎用（安全模式不加载蓝牙栈）。
7. 无联网、无遥测、无自启动；唯一注册表写入是撤销 safeboot 的 RunOnce。

## 🛠️ 编译与坑位（开发者看这里）
- **提权逻辑**：`main.cpp` 用 `ShellExecuteExW(runas)` 自提权，不再 `return 1` 自杀。
- **清单文件**：`quickbios.manifest` 为外部 UAC 清单，不进 `add_executable` 源列表。
- **华硕/技嘉兼容**：bit1=0 自动降级 BootNext。
- 依赖：Qt 6.x (MSVC2022 x64)、CMake。

## 📥 下载
前往 [Releases](../../releases) 下载 `QuickBIOS_v1.0_Win64_Portable.zip`。
解压 → 双击 `QuickBIOS.exe`（图标带 🛡）→ 完事。

## 📋 主板实测矩阵（欢迎补充）
| 主板/BIOS | bit0 | bit1 | 兜底 | 状态 |
|---|---|---|---|---|
| 技嘉 B650 / F22 | ✅ | ❌ | BootNext ✅ | 通过 |
| 华硕 X670 / 2301 | ✅ | ❌ | BootNext ✅ | 通过 |
| MSI B650 / A21 | ✅ | ✅ | 原生 | 通过 |
启动项功能暂不可用。
