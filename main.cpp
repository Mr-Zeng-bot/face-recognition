#include "mainwindow.h"
#include "appconfig.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QDebug>

// ---------------------------------------------------------------------------
// 程序入口
//   1. 载入 config.ini（数据库、模型路径、阈值、抓拍目录等）
//   2. 创建并显示主窗口
// ---------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    // 内置 UVC 摄像头通常输出 MJPEG 流，MSMF 后端默认会调用系统注册的硬件解码器 MFT。
    // 本机这台机器上的 AMD MJPEG 解码器 MFT 在非 AMD 适配器上创建 D3D11 设备失败
    // （日志中可见 Not an AMD adapter / Adapter is not connected to display），
    // 导致解码器建不起来、摄像头打不开。关闭 MSMF 硬件变换后改走软件解码，绕开该 MFT。
    // 必须在任何 cv::VideoCapture 创建之前设置，所以放在程序入口最前面。
    qputenv("OPENCV_VIDEOIO_MSMF_ENABLE_HW_TRANSFORMS", "0");

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("QT_Face"));

    // 优先读取可执行文件同目录下的 config.ini，便于部署时修改；
    // 若不存在则退回当前工作目录的 config.ini。
    const QString exeIni = QDir(QCoreApplication::applicationDirPath())
                               .filePath(QStringLiteral("config.ini"));
    if (QFileInfo::exists(exeIni)) {
        AppConfig::instance().load(exeIni);
    } else {
        AppConfig::instance().load(QStringLiteral("config.ini"));
    }
    qDebug() << "[main] 配置文件:" << AppConfig::instance().configPath();

    MainWindow w;
    w.show();
    return QApplication::exec();
}
