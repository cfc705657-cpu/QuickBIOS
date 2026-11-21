// main.cpp —— QuickBIOS 入口
// 两件关键事：
//   1) 未提权时用 ShellExecuteExW(runas) 重启自己（UAC），旧进程干净交棒 return 0。
//      ★ 绝不 return 1 自杀：那正是 Qt Creator 里 "terminated abnormally" 的由来。
//   2) 提权重启必须【透传 argv】：Qt Creator 会注入 --platformpluginpath=...
//      丢掉它，提权后的新进程加载不到 platforms/qwindows.dll，会二次秒崩。
//
#include "mainwindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFont>
#include <QString>
#include <QStringList>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#endif

static bool runningAdmin()
{
#ifdef Q_OS_WIN
    BOOL ok = FALSE; HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION ev{}; DWORD cb = sizeof(ev);
        if (GetTokenInformation(tok, TokenElevation, &ev, sizeof(ev), &cb)) ok = ev.TokenIsElevated;
        CloseHandle(tok);
    }
    return ok;
#else
    return true;
#endif
}

// 返回 true = 已把接力棒交出去，本进程应当 return 0
static bool relaunchElevated(const QStringList &extraArgs)
{
#ifdef Q_OS_WIN
    wchar_t exe[MAX_PATH * 2]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH * 2)) return false;

    const QString params = extraArgs.join(QLatin1Char(' '));
    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.lpVerb       = L"runas";                 // ← 弹 UAC
    sei.lpFile       = exe;
    sei.lpParameters = params.isEmpty() ? nullptr : (LPCWSTR)params.utf16();
    sei.nShow        = SW_SHOWNORMAL;
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;

    if (ShellExecuteExW(&sei)) {
        if (sei.hProcess) CloseHandle(sei.hProcess);
        return true;
    }
    // 用户点了"否" → ERROR_CANCELLED(1223)：不自杀，交给 MainWindow 进只读模式
#endif
    Q_UNUSED(extraArgs)
    return false;
}

int main(int argc, char *argv[])
{
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication a(argc, argv);
    a.setApplicationName("QuickBIOS");
    a.setOrganizationName("QuickBIOS");

    QFont f = a.font();
    f.setFamily("Microsoft YaHei");
    f.setPointSize(10);
    a.setFont(f);

    bool admin = runningAdmin();

    if (!admin) {
        // 透传 Qt Creator / shell 给的全部参数（含 --platformpluginpath）
        if (relaunchElevated(QCoreApplication::arguments().mid(1)))
            return 0;                     // 干净交棒 ✔ 不再 terminated abnormally
        admin = runningAdmin();           // 用户拒了 UAC
    }

    MainWindow w;
    w.setReadOnlyMode(!admin);            // 拒绝提权 → 只读诊断，窗口照样出来
    w.show();
    return a.exec();
}
