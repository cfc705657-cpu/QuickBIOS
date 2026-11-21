#include "mainwindow.h"
#include "ui_mainwindow.h"

// ═══════════════ Windows：必须压宏，且必须排在 Qt 头之前 ═══════════════════
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX                 // 否则 min/max 宏会炸 <algorithm> 与 qglobal.h
#endif
#include <windows.h>

// 不 include <winternl.h>：它的 NTSTATUS 宏与 Qt 的 qglobal.h 冲突，
// 而我们只调 kernel32 正式导出的 Get/SetFirmwareEnvironmentVariable*W，够用。

#include <QApplication>
#include <QDateTime>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
// ⚠ 绝不要 #include <QTextCodec> —— Qt6 已把它移出 QtCore，必炸 C1083。
//   本项目全程 QString(UTF-16) + fromLocal8Bit，根本用不到编解码器。
// ⚠ 也不要 #include <winternl.h> —— NTSTATUS 宏与 qglobal.h 冲突。

#include <cstring>
#include <cwchar>
#include <vector>

// ═══════════════════ UEFI 规范常量 (Spec §3.3 / §8.2) ═══════════════════════
// EFI_GLOBAL_VARIABLE = {8BE4DF61-93CA-11D2-AA0D-00E098032B8C}
// 写成【字符串】直接喂给 Get/Set，硬编码 16 字节 GUID 结构体喂给 Ex 版。
// → 完全不需要 CLSIDFromString / <objbase.h> / ole32，C3861 从此绝迹。
static const wchar_t *kEfiGuidStr = L"{8be4df61-93ca-11d2-aa0d-00e098032b8c}";

static const GUID kEfiGlobalGuid = {
    0x8be4df61, 0x93ca, 0x11d2, { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c }
};

static const wchar_t *kVarWant = L"OsIndications";           // 写：请求固件跳转
static const wchar_t *kVarCan  = L"OsIndicationsSupported";  // 读：固件声明能力
static const wchar_t *kVarBootOrder = L"BootOrder";
static const wchar_t *kVarBootNext  = L"BootNext";
static const wchar_t *kVarBootFmt   = L"Boot%04X";

static const quint64 kBitBios = 0x0000000000000001ULL; // BOOT_TO_FW_UI
static const quint64 kBitBoot = 0x0000000000000002ULL; // BOOT_TO_FW_UI_BOOT_OPTIONS

// EFI_VARIABLE_ATTRIBUTES
static const DWORD kEfiAttrs =
    0x00000001u | // NON_VOLATILE
    0x00000002u | // BOOTSERVICE_ACCESS
    0x00000004u | // RUNTIME_ACCESS
    0x00000008u;  // EFI_VARIABLE_RUNTIME_ACCESS_BOOTSERVICE 兼容位

namespace {

// ── 动态解析固件变量 API ────────────────────────────────────────────────────
// 不用 NtSetFirmwareEnvironmentVariable（ntdll 未导出 → C3861 死路），
// 也不在编译期直接调 Ex 版（Win8/旧 SDK 无声明 → C3861）。
using GetFn  = DWORD (WINAPI *)(LPCWSTR, LPCWSTR, PVOID, DWORD, PDWORD);
using SetExFn = LONG  (WINAPI *)(LPCWSTR, GUID *, DWORD, DWORD, PVOID, DWORD); // Win10+
using SetFn  = LONG  (WINAPI *)(LPCWSTR, GUID *, DWORD, PVOID, DWORD);         // Win8

GetFn   g_get   = nullptr;
SetExFn g_setEx = nullptr;
SetFn   g_set   = nullptr;

void resolveApi()
{
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    g_get   = (GetFn)  GetProcAddress(k, "GetFirmwareEnvironmentVariableW");
    g_setEx = (SetExFn)GetProcAddress(k, "SetFirmwareEnvironmentVariableExW");
    g_set   = (SetFn)  GetProcAddress(k, "SetFirmwareEnvironmentVariableW");
}

QString winErrText(unsigned long e)
{
    wchar_t *buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e,
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&buf, 0, nullptr);
    QString s = buf ? QString::fromWCharArray(buf).trimmed() : QString();
    if (buf) LocalFree(buf);
    if (s.isEmpty()) s = QStringLiteral("未知错误");
    return QStringLiteral("%1 (0x%2)").arg(s).arg(e, 8, 16, QChar('0'));
}

QString hex64(quint64 v) { return QString("0x%1").arg(v, 16, 16, QChar('0')); }

QString ts() { return QDateTime::currentDateTime().toString("HH:mm:ss"); }

// 无黑窗跑命令
QString runCap(const QString &prog, const QStringList &args, int ms = 5000)
{
    QProcess p;
    p.setProgram(prog);
    p.setArguments(args);
    p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) {
        a->flags |= CREATE_NO_WINDOW;
        if (a->startupInfo) a->startupInfo->wShowWindow = SW_HIDE;
    });
    p.start();
    if (!p.waitForFinished(ms)) return QString();
    return QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
}

bool looksUsb(const QString &s)
{
    static const char *kw[] = {
        "usb", "udisk", "u盘", "sanDisk", "kingston", "cruzer", "dt ",
        "ventoy", "rufus", "3.2gen1", "3.0", "2.0", "flash", "thumb",
        "transcend", "adata", "silicon power", "team", "lexar", "pnv",
        "mass storage", "removable", "ts-", "ufd", "general ",
        "tsd", "netac", "biwin", "foresee", "phison", "smi "
    };
    const QString t = s.toLower();
    for (const char *k : kw)
        if (t.contains(QLatin1String(k))) return true;
    return false;
}

// 从 EFI_LOAD_OPTION 二进制里抽 UTF-16LE 的 Description
// 布局: Attributes(4) | FilePathListLength(2) | Description(UTF-16LE, NUL 结尾) | ...
QString decodeLoadOptionDesc(const QByteArray &raw)
{
    if (raw.size() < 6) return QString();
    const quint16 *p = reinterpret_cast<const quint16 *>(raw.constData() + 6);
    int maxChars = (raw.size() - 6) / 2;
    QString d;
    for (int i = 0; i < maxChars && p[i] != 0; ++i) {
        d.append(QChar(p[i]));
        if (d.size() > 96) break;
    }
    return d.trimmed();
}

} // namespace

// ═════════════════════════ 构造 / 析构 ═════════════════════════════════════
MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);                 // ← 严格按 mainwindow.ui 建控件
    resolveApi();
    setFixedSize(196, 348);

    // 上方那个 QLineEdit 就是你要的 "lx 编辑框"：显示检测结果
    ui->lx->setStyleSheet(
        "QLineEdit{background:#141518;border:1px solid #2a2c31;border-radius:4px;"
        "padding:2px 6px;font:12px 'Cascadia Mono',Consolas;color:#a5d6ff;}"
        "QLineEdit:disabled{color:#5b6472;}");

    // 五个按钮配色 + 禁用态提示
    struct Style { const char *obj; const char *css; const char *tip; };
    const Style styles[] = {
        {"motherboard", "#3b4266", "重新检测固件启动类型与位掩码"},
        {"shutdown",    "#5b2c2c", "shutdown /s /t 0 /hybrid（Win8 快速关机）"},
        {"bios",        "#1d4ed8", "写 OsIndications bit0 → 重启进 BIOS"},
        {"boot",        "#0f766e", "写 bit1；不支持则 BootNext 软 F12"},
        {"safe",        "#7c3aed", "一次性 safeboot + RunOnce 自愈"},
    };
    for (const auto &s : styles) {
        if (auto *b = findChild<QPushButton *>(s.obj)) {
            b->setToolTip(tr(s.tip));
            b->setAttribute(Qt::WA_AlwaysShowToolTips, true);
            b->setStyleSheet(QString(
                "QPushButton{border:none;border-radius:6px;color:#fff;"
                "font:bold 12px 'Microsoft YaHei';background:%1;}"
                "QPushButton:hover:!disabled{background:%1;}"
                "QPushButton:pressed:!disabled{padding-top:2px;}"
                "QPushButton:disabled{background:#24252a;color:#64676c;}")
                .arg(s.css));
            // hover 提亮交给 Qt 的 palette 过渡，避免再写一遍颜色表
            b->setCursor(Qt::PointingHandCursor);
        }
    }

    statusBar()->setStyleSheet("QStatusBar{font:11px 'Microsoft YaHei';color:#6b7280;}");

    logRule("QuickBIOS");
    log(QString("权限：%1").arg(isAdmin() ? "管理员 ✓" : "普通用户 ✗"));
    log(QString("API：Get=%1 SetEx=%2 Set=%3")
        .arg(g_get ? "OK" : "缺", g_setEx ? "OK" : "缺", g_set ? "OK" : "缺"));

    if (!isAdmin()) {
        setReadOnlyMode(true);   // 不再 return 1 自杀，改为只读诊断
    } else {
        on_motherboard_clicked();   // 启动即自动检测一次 → 配置其余四键
    }
}

MainWindow::~MainWindow() { delete ui; }

void MainWindow::setReadOnlyMode(bool on)
{
    m_readOnly = on;
    if (!on) return;
    log("⚠ 无管理员权限 → 只读模式：仅可检测，动作键已锁");
    if (auto *b = findChild<QPushButton *>("motherboard")) b->setEnabled(true);
    for (const char *n : { "shutdown", "bios", "boot", "safe" })
        if (auto *b = findChild<QPushButton *>(n)) b->setEnabled(false);
    setLx("只读模式：右键 exe 以管理员运行");
    statusBar()->showMessage("只读模式 — 右键 exe『以管理员身份运行』解锁", 8000);
}

// ═════════════════════════ 日志 ════════════════════════════════════════════
void MainWindow::setLx(const QString &oneLine)
{
    if (ui->lx) ui->lx->setText(oneLine.left(48));
}

void MainWindow::log(const QString &msg)
{
    // lx 是 QLineEdit（按你的 UI），只留最后一行最要紧的结论
    setLx(msg);
    statusBar()->showMessage(msg, 6000);
#ifndef QT_NO_DEBUG_OUTPUT
    qDebug("[QuickBIOS] %s %s", qPrintable(ts()), qPrintable(msg));
#endif
}

void MainWindow::logRule(const QString &title)
{
    log(QString("── %1 ").arg(title));
}

bool MainWindow::ask(QWidget *w, const QString &t, const QString &b)
{
    return QMessageBox::warning(w, t, b, QMessageBox::Ok | QMessageBox::Cancel,
                                QMessageBox::Cancel) == QMessageBox::Ok;
}

QString MainWindow::winErr(unsigned long e) { return winErrText(e); }
QString MainWindow::hex64(quint64 v)       { return ::hex64(v); }

bool MainWindow::isAdmin()
{
    BOOL ok = FALSE; HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION ev{}; DWORD cb = sizeof(ev);
        if (GetTokenInformation(tok, TokenElevation, &ev, sizeof(ev), &cb)) ok = ev.TokenIsElevated;
        CloseHandle(tok);
    }
    return ok;
}

// ═══════════════════ 固件变量：读 / 写 ═════════════════════════════════════
bool MainWindow::readSupported(quint64 &out, QString *err) const
{
    if (!g_get) { if (err) *err = "kernel32 无 GetFirmwareEnvironmentVariableW"; return false; }
    quint64 v = 0; DWORD need = 0;
    SetLastError(0);
    DWORD r = g_get(kVarCan, kEfiGuidStr, &v, sizeof(v), &need);
    DWORD gle = GetLastError();
    if (r == 0 || need != sizeof(v)) { if (err) *err = winErrText(gle); return false; }
    out = v;
    return true;
}

bool MainWindow::writeIndications(quint64 bits, QString *err) const
{
    GUID g = kEfiGlobalGuid;
    LONG r = -1; DWORD gle = 0;

    if (g_setEx) {                       // Win10+：可显式带 EFI Attributes
        r = g_setEx(kVarWant, &g, kEfiAttrs, sizeof(bits), &bits, sizeof(bits));
        gle = GetLastError();
        if (r == 0) return true;
    }
    if (g_set) {                         // Win8/8.1：4 参版，属性由内核推断
        r = g_set(kVarWant, &g, sizeof(bits), &bits, sizeof(bits));
        gle = GetLastError();
        if (r == 0) return true;
    }
    if (err) {
        *err = g_setEx || g_set ? winErrText(gle)
                                : QStringLiteral("kernel32 无任何 SetFirmwareEnvironmentVariable* 导出");
    }
    return false;
}

bool MainWindow::readBootNext(quint16 &out) const
{
    if (!g_get) return false;
    quint16 v = 0; DWORD need = 0;
    SetLastError(0);
    DWORD r = g_get(kVarBootNext, kEfiGuidStr, &v, sizeof(v), &need);
    if (r == 0 || need != sizeof(v)) return false;
    out = v; return true;
}

bool MainWindow::writeBootNext(quint16 idx) const
{
    GUID g = kEfiGlobalGuid;
    const DWORD attrs = 0x00000001u | 0x00000002u | 0x00000004u; // NV|BS|RT
    if (g_setEx) {
        if (g_setEx(kVarBootNext, &g, attrs, sizeof(idx), &idx, sizeof(idx)) == 0) return true;
    }
    if (g_set) {
        if (g_set(kVarBootNext, &g, sizeof(idx), &idx, sizeof(idx)) == 0) return true;
    }
    return false;
}

// ═══════════════ ① Motherboard：检测 + 位掩码 + BootOrder 解析 ═════════════
MainWindow::Probe MainWindow::probeAll()
{
    Probe p;

    // 1) 权威启动类型：GetFirmwareType (Win8+)
    using FtFn = BOOL (WINAPI *)(DWORD *);
    auto ft = (FtFn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetFirmwareType");
    DWORD t = 0;
    if (ft && ft(&t)) p.apiOk = true;
    p.fwType = (t > 2) ? 0 : (int)t;
    static const char *names[] = { "Unknown", "Legacy BIOS (CSM)", "UEFI" };
    const QString typeText = names[p.fwType];

    // 2) OsIndicationsSupported 位掩码
    quint64 sup = 0; QString e;
    if (readSupported(sup, &e)) {
        p.supportedHex = hex64(sup);
        p.bit0FwUi = (sup & kBitBios) != 0;
        p.bit1Boot = (sup & kBitBoot) != 0;
        QString we;
        p.writable = writeIndications(sup & (kBitBios | kBitBoot), &we);  // 试探写回
        if (!p.writable) p.err = we;
    } else {
        p.supportedHex = "不可读";
        p.err = e;
        if (!p.apiOk && e.contains("0x00000001", Qt::CaseInsensitive)) p.fwType = 1; // ERROR_INVALID_FUNCTION
    }

    // 3) 主板 / BIOS / SecureBoot（失败不致命）
    p.board   = runCap("powershell", {"-NoProfile","-NonInteractive","-Command",
                  "(Get-CimInstance Win32_BaseBoard).Manufacturer"});
    p.product = runCap("powershell", {"-NoProfile","-NonInteractive","-Command",
                  "(Get-CimInstance Win32_BaseBoard).Product"});
    p.bios    = runCap("powershell", {"-NoProfile","-NonInteractive","-Command",
                  "$b=Get-CimInstance Win32_BIOS; $b.Manufacturer+' '+$b.SMBIOSBIOSVersion"});
    QString sb = runCap("powershell", {"-NoProfile","-NonInteractive","-Command",
                  "try{ (Confirm-SecureBootUEFI) }catch{ 'N/A' }"});
    p.secureBoot = (sb == "True") ? "On" : (sb == "False") ? "Off" : "N/A";

    // 4) BootOrder + Boot####  →  找 U 盘（bit1=0 时的兜底）
    if (g_get && p.fwType == 2) {
        DWORD need = 0;
        std::vector<quint16> order(16, 0);
        SetLastError(0);
        DWORD r = g_get(kVarBootOrder, kEfiGuidStr, order.data(),
                        (DWORD)(order.size() * sizeof(quint16)), &need);
        if (r == 0 && GetLastError() == ERROR_MORE_DATA && need > 0) {
            order.resize(need / sizeof(quint16) + 1);
            r = g_get(kVarBootOrder, kEfiGuidStr, order.data(),
                      (DWORD)(order.size() * sizeof(quint16)), &need);
        }
        if (r != 0 && need >= 2) {
            const int n = (int)(need / sizeof(quint16));
            for (int i = 0; i < n; ++i) {
                BootEntry be; be.name = QString(kVarBootFmt).arg(order[i], 4, 16, QChar('0')).toUpper();
                wchar_t vn[16]{};
                swprintf(vn, 16, L"Boot%04X", order[i]);
                std::vector<BYTE> buf(1024, 0); DWORD got = 0;
                SetLastError(0);
                DWORD g2 = g_get(vn, kEfiGuidStr, buf.data(), (DWORD)buf.size(), &got);
                if (g2 == 0 && GetLastError() == ERROR_MORE_DATA && got) {
                    buf.assign(got, 0);
                    g2 = g_get(vn, kEfiGuidStr, buf.data(), got, &got);
                }
                if (g2 != 0 && got >= 6) {
                    be.desc = decodeLoadOptionDesc(QByteArray((const char *)buf.data(), (int)got));
                    be.usb  = looksUsb(be.desc);
                }
                if (be.desc.isEmpty()) be.desc = "(无法解析)";
                p.bootOrder << be.name + "  " + be.desc + (be.usb ? "  [USB]" : "");
                if (be.usb && p.usbPick.isEmpty()) p.usbPick = be.name;
                p.entries << be;
            }
        }
    }

    // 5) 当前 bcdedit 是否已带 safeboot
    m_safeOn = runCap("bcdedit", {"/enum"}, 4000).contains("safeboot", Qt::CaseInsensitive);
    return p;
}

void MainWindow::on_motherboard_clicked()
{
    logRule("Motherboard");
    if (m_readOnly) { setReadOnlyMode(true); return; }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_p = probeAll();
    QApplication::restoreOverrideCursor();
    const Probe &p = m_p;

    log(QString("主板 %1 %2").arg(p.board.isEmpty() ? "?" : p.board,
                                  p.product.isEmpty() ? "?" : p.product));
    log(QString("BIOS %1").arg(p.bios.isEmpty() ? "?" : p.bios));
    log(QString("启动 %1%2  SB:%3").arg(
            QString(p.fwType == 2 ? "UEFI" : p.fwType == 1 ? "Legacy/CSM" : "Unknown"),
            p.apiOk ? "" : "(API缺)", p.secureBoot));
    log(QString("OsInd %1 b0:%2 b3:%3 写:%4")
        .arg(p.supportedHex,
             p.bit0FwUi ? "Y" : "N", p.bit1Boot ? "Y" : "N",
             p.writable ? "OK" : "NG"));
    if (!p.err.isEmpty()) log("写探测: " + p.err);

    if (!p.bootOrder.isEmpty()) {
        log("BootOrder:");
        for (const QString &l : p.bootOrder) log("  " + l);
        log(p.usbPick.isEmpty() ? "BootNext 候选: 无 USB"
                                : "BootNext 候选: " + p.usbPick);
    }
    if (m_safeOn) log("注意: bcdedit 已带 safeboot，Safe 键可一键撤销");

    // 顶栏结论写进 lx 编辑框（这是你唯一常看的那一格）
    if (p.fwType == 2) {
        setLx(QString("UEFI | BIOS:%1 Boot:%2 | %3")
              .arg(p.bit0FwUi ? "OK" : "NO",
                   p.bit1Boot ? "F12" : (p.usbPick.isEmpty() ? "NO" : "BootNext"),
                   p.usbPick.isEmpty() ? QString() : p.usbPick.mid(4)));
    } else if (p.fwType == 1) {
        setLx("Legacy/CSM — BIOS/Boot 已锁");
        log("→ Legacy 下 Windows 无官方通道进 Setup/BootMenu。");
        log("  ① mbr2gpt /validate /allowFullOS → /convert（无损转 UEFI）");
        log("  ② 树莓派 Pico 伪装键盘，上电 200ms 内 2ms 间隔狂发 F1/F12");
    } else {
        setLx("启动类型未知 — 仅关机/安全模式");
    }
    applyButtonPolicy();
}

void MainWindow::applyButtonPolicy()
{
    if (m_readOnly) { setReadOnlyMode(true); return; }

    const bool uefi = (m_p.fwType == 2) && m_p.apiOk;
    auto setBtn = [&](const char *obj, bool on, const QString &tip) {
        if (auto *b = findChild<QPushButton *>(obj)) { b->setEnabled(on); b->setToolTip(tip); }
    };

    setBtn("shutdown", true, "shutdown /s /t 0 /hybrid（Win8 快速关机，开机更快）");

    setBtn("safe", true, m_safeOn ? "当前已置位 → 点一下清除，不重启"
                                  : "一次性 safeboot；进桌面由 RunOnce 自动撤销");
    if (m_safeOn) if (auto *b = findChild<QPushButton *>("safe")) b->setText("取消安全模式");

    setBtn("bios", uefi, !uefi ? "Legacy/CSM：无固件变量接口"
          : m_p.bit0FwUi ? (m_p.writable ? "写 bit0 精准直达 BIOS" : "回退 shutdown /r /t 0 /fw")
                         : "固件未声明 FW_UI，仍试 /fw；不认则普通重启");

    setBtn("boot", uefi && (m_p.bit1Boot || !m_p.usbPick.isEmpty()),
           !uefi ? "Legacy/CSM：无固件变量接口"
           : m_p.bit1Boot ? "写 bit1 → 直达 Boot Menu（等价狂按 F12）"
           : !m_p.usbPick.isEmpty() ? "bit1 不支持 → 降级 BootNext=" + m_p.usbPick.mid(4) + "（一次性，不改写 BootOrder）"
                                    : "bit1=0 且没认到 U 盘：插上 U 盘再点「检测主板启动类型」");

    statusBar()->showMessage(
        QString("UEFI=%1 bit0=%2 bit1=%3 BootNext兜底=%4")
            .arg(uefi ? "Y" : "N", m_p.bit0FwUi ? "Y" : "N",
                 m_p.bit1Boot ? "Y" : "N", m_p.usbPick.isEmpty() ? "-" : m_p.usbPick));
}

// ═══════════════════ ②③ 重启进 BIOS / Boot Menu ═════════════════════════
bool MainWindow::rebootViaOsIndication(quint64 bit, const QString &label)
{
    quint64 base = 0;
    readSupported(base, nullptr);                       // 保留其它已支持位
    const quint64 bits = (base & ~(kBitBios | kBitBoot)) | bit;

    QString err;
    if (m_p.writable && writeIndications(bits, &err)) {
        log(QString("OsIndications=%1 [%2 精准]").arg(hex64(bits), label));
        machineReboot();
        return true;
    }
    log(QString("精准写入不可用（%1）→ 回退标准通道").arg(err));
    if (bit == kBitBios) {                              // /fw 只覆盖 bit0
        if (QProcess::startDetached("shutdown", {"/r", "/t", "0", "/fw"})) {
            log("已下发 shutdown /r /fw");
            return true;
        }
    }
    return false;
}

bool MainWindow::rebootViaBootNext()
{
    // 从 "Boot0002" 抽出 0x0002
    bool ok = false;
    const quint16 idx = (quint16)m_p.usbPick.mid(4).toUInt(&ok, 16);
    if (!ok || m_p.usbPick.isEmpty()) return false;

    quint16 old = 0; const bool hadOld = readBootNext(old);
    if (!writeBootNext(idx)) {
        log("✗ BootNext 写入失败（固件锁？SecureBoot？）");
        return false;
    }
    log(QString("BootNext = %1 (%2)  ← 一次性，固件消费后自清")
        .arg(m_p.usbPick, m_p.usbPick.mid(4)));
    if (hadOld) log(QString("  原 BootNext=%1 已被覆盖（BootOrder 未动，下轮自动回硬盘）")
                        .arg(hadOld, 4, 16, QChar('0')));
    machineReboot();
    return true;
}

void MainWindow::machineReboot()
{
    // 先试 API（最快），失败退 shutdown.exe
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(),
                         TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        TOKEN_PRIVILEGES tp{}; tp.PrivilegeCount = 1;
        LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        CloseHandle(tok);
        if (ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, 0)) { log("重启指令已下发 ✔"); return; }
    }
    if (QProcess::startDetached("shutdown", {"/r", "/t", "0"})) log("回退 shutdown /r 已下发");
    else log("✗ 重启指令下发失败，请手动重启");
}

void MainWindow::on_bios_clicked()
{
    if (m_readOnly) return;
    if (m_p.fwType != 2) { log("非 UEFI，拒绝执行"); return; }
    if (!ask(this, "进入 BIOS", "即将重启并进入 UEFI / BIOS 设置。\n\n请先保存所有工作！")) return;
    logRule("BIOS");
    if (!rebootViaOsIndication(kBitBios, "BOOT_TO_FW_UI"))
        QMessageBox::warning(this, "失败", "下发失败，详见状态栏。\n兜底：重启手按 F1/F2。");
}

void MainWindow::on_boot_clicked()
{
    if (m_readOnly) return;
    if (m_p.fwType != 2) { log("非 UEFI，拒绝执行"); return; }

    logRule("Boot 菜单");
    if (m_p.bit1Boot) {
        if (!ask(this, "启动菜单", "重启进入固件 Boot Menu（等价狂按 F12）。\n\n先保存工作！")) return;
        if (rebootViaOsIndication(kBitBoot, "BOOT_TO_FW_UI_BOOT_OPTIONS")) return;
    }
    // 降级路径：BootNext 一次性跳 U 盘
    if (!m_p.usbPick.isEmpty()) {
        QString who = m_p.usbPick.mid(4);
        for (const auto &e : m_p.entries)
            if (e.name == m_p.usbPick) { who += "  " + e.desc; break; }
        if (!ask(this, "一次性从 U 盘启动",
                 "本固件未实现 bit1（软 F12 不可用）。\n\n改用 BootNext 一次性跳：\n   " +
                 who + "\n\n启动完固件自动清标志，BootOrder 不动，下次仍回硬盘。\n继续？")) return;
        if (rebootViaBootNext()) return;
    }
    log("✗ 两条路都走不通。插 U 盘后点「检测主板启动类型」重试，或手动 F12。");
    QMessageBox::information(this, "Boot 菜单不可用",
        "固件既没实现 OsIndications bit1，也没认到 USB 启动项。\n"
        "① 插上 U 盘 → 点「检测主板启动类型」→ 再试\n"
        "② 或：设置 → 恢复 → 高级启动 → 使用设备\n"
        "③ 或：BIOS 里把 U 盘提到 Boot Option #1（一次性的活）");
}

// ═══════════════════ ⑤ Safe：一次性 + RunOnce 自愈 ════════════════════════
bool MainWindow::setSafeBoot(bool enable, QString *err) const
{
    if (enable) {
        QProcess p1; p1.setProgram("bcdedit");
        p1.setArguments({"/set", "{current}", "safeboot", "minimal"});
        p1.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a){ a->flags |= CREATE_NO_WINDOW; });
        p1.start(); p1.waitForFinished(6000);
        if (p1.exitCode() != 0) {
            if (err) *err = QString::fromLocal8Bit(p1.readAllStandardError()).trimmed();
            return false;
        }
        // 自愈：进到安全模式桌面那一刻自动撤销，避免"永远卡安全模式"
        QSettings s("HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
                    QSettings::NativeFormat);
        s.setValue("QuickBIOS_ClearSafeBoot",
                   "bcdedit /deletevalue {current} safeboot");
        s.sync();
        if (s.status() != QSettings::NoError) {
            QProcess::startDetached("bcdedit", {"/deletevalue", "{current}", "safeboot"});
            if (err) *err = "safeboot 已设，但 RunOnce 自愈项写入失败 → 已回滚";
            return false;
        }
        return true;
    }
    return clearSafeBoot(err);
}

bool MainWindow::clearSafeBoot(QString *err) const
{
    // bcdedit 在"本来就没设"时返回非 0，不作为失败
    QProcess::execute("bcdedit", QStringList{"/deletevalue", "{current}", "safeboot"});
    QSettings("HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
              QSettings::NativeFormat).remove("QuickBIOS_ClearSafeBoot");
    if (err) err->clear();
    return true;
}

bool MainWindow::safeBootActive() const { return m_safeOn; }

void MainWindow::on_safe_clicked()
{
    if (m_readOnly) return;
    logRule("Safe");

    if (m_safeOn) {
        if (!ask(this, "取消安全模式", "清除 safeboot 标志，下次正常启动（不重启）。")) return;
        QString e; setSafeBoot(false, &e);
        m_safeOn = safeBootActive();
        if (!m_safeOn) {
            log("✔ 已清 safeboot + RunOnce 自愈项");
            if (auto *b = findChild<QPushButton *>("safe")) b->setText("进入安全模式");
            setLx("safeboot 已清除");
        } else log("✗ 清除失败：" + e);
        applyButtonPolicy();
        return;
    }

    if (!ask(this, "安全模式",
             "设置【一次性】安全模式并立即重启。\n"
             "进到安全模式桌面后由 RunOnce 自动撤销，不会卡住。\n\n先保存工作！")) return;

    QString e;
    if (!setSafeBoot(true, &e)) {
        log("✗ bcdedit 失败：" + e);
        QMessageBox::warning(this, "失败", "设置安全模式失败：\n" + e +
                             "\n\n请确认 exe 是『以管理员身份运行』。");
        return;
    }
    log("bcdedit /set {current} safeboot minimal ✔");
    log("HKLM\\...\\RunOnce\\QuickBIOS_ClearSafeBoot ✔（进桌面自动撤）");
    m_safeOn = true;
    if (auto *b = findChild<QPushButton *>("safe")) b->setText("取消安全模式");
    machineReboot();
}

// ═══════════════════ ④ Shutdown（Win8 专用）═══════════════════════════════
void MainWindow::doShutdown()
{
    // /hybrid = Win8 快速关机（内核会话存盘，下次开机秒进），正是给 Win8 用户找不着的那个
    if (QProcess::startDetached("shutdown", {"/s", "/t", "0", "/hybrid", "/d", "p:0:0"})) {
        log("shutdown /s /hybrid 已下发 ✔");
        return;
    }
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        TOKEN_PRIVILEGES tp{}; tp.PrivilegeCount = 1;
        LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        CloseHandle(tok);
        ExitWindowsEx(EWX_POWEROFF | EWX_FORCEIFHUNG, 0);
    }
}

void MainWindow::on_shutdown_clicked()
{
    if (m_readOnly) return;
    if (!ask(this, "关机", "立即关机？（Win8 快速关机，下次开机更快）")) return;
    logRule("Shutdown");
    doShutdown();
}
