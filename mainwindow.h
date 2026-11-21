#ifndef MAINWINDOW_H
#define MAINWINDOW_H

// ─────────────────────────────────────────────────────────────────────────────
// 注意：本头文件【绝不】include <windows.h> / <winternl.h> / <objbase.h>
// 原因： windows.h 的 min/max 宏与 NTSTATUS 宏会污染 moc 生成的 moc_mainwindow.cpp，
//       历史上正是这里连锁引爆 C2061(PGUID) / LNK2001(metaObject) / C3861。
//       所有 Windows 类型一律关在 mainwindow.cpp 里，头文件只用 Qt / 标准类型。
// ─────────────────────────────────────────────────────────────────────────────

#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QVector>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class QLineEdit;
class QPushButton;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    /// UAC 被用户点"否"时进入只读模式：只检测，不动系统
    void setReadOnlyMode(bool on);

private slots:
    // 槽名严格按 mainwindow.ui 里的 objectName 自动连接 (QMetaObject::connectSlotsByName)
    void on_motherboard_clicked();
    void on_shutdown_clicked();
    void on_bios_clicked();
    void on_boot_clicked();
    void on_safe_clicked();

private:
    struct BootEntry {
        QString name;      // Boot0000
        QString desc;      // "Windows Boot Manager" / "UEFI: SanDisk 3.2Gen1"
        bool    usb = false;
    };

    struct Probe {
        bool    apiOk      = false;   // GetFirmwareType 可用
        int     fwType     = 0;       // 0 Unknown / 1 Legacy / 2 UEFI
        bool    bit0FwUi   = false;   // OsIndicationsSupported bit0 → BIOS Setup
        bool    bit1Boot   = false;   // OsIndicationsSupported bit1 → Boot Menu(F12)
        bool    writable   = false;   // OsIndications 可写
        QString supportedHex;         // 0x0000...0003
        QString err;                  // 最近一次失败原因
        QString board;
        QString product;
        QString bios;
        QString secureBoot;
        QStringList   bootOrder;
        QVector<BootEntry> entries;
        QString usbPick;              // 挑出来的 U 盘 Boot####
    };

    // ── UI ──────────────────────────────────────────
    void log(const QString &msg);          // 写上方 lx 编辑框 + 状态栏
    void logRule(const QString &title);
    void applyButtonPolicy();
    void setLx(const QString &oneLine);
    static bool ask(QWidget *w, const QString &title, const QString &body);

    // ── 固件层（实现在 .cpp，含 windows.h）────────────
    Probe probeAll();
    bool  readSupported(quint64 &out, QString *err) const;
    bool  writeIndications(quint64 bits, QString *err) const;
    bool  readBootNext(quint16 &out) const;
    bool  writeBootNext(quint16 idx) const;
    static bool isAdmin();
    static QString winErr(unsigned long code);
    static QString hex64(quint64 v);

    // ── 动作 ────────────────────────────────────────
    bool rebootViaOsIndication(quint64 bit, const QString &label);
    bool rebootViaBootNext();
    bool setSafeBoot(bool enable, QString *err) const;
    bool clearSafeBoot(QString *err) const;
    bool safeBootActive() const;
    void doShutdown();
    void machineReboot();

    Ui::MainWindow *ui = nullptr;

    Probe  m_p;
    bool   m_readOnly = false;
    bool   m_safeOn   = false;
};

#endif // MAINWINDOW_H
