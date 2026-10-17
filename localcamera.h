#ifndef LOCALCAMERA_H
#define LOCALCAMERA_H

// ============================================================================
// localcamera.h —— 本机 UVC 摄像头采集（没有工业相机时的替代视频源）
// 说明：
//   * 用 OpenCV VideoCapture 打开电脑自带/外接 USB 摄像头。Windows 下固定使用 MSMF
//     后端：本工程用的 OpenCV 5.0 不再提供 DSHOW 后端插件，指定它会直接报
//     "VIDEOIO(DSHOW): backend is generally available but can't be used to capture by index"；
//     用 CAP_ANY 还会先撞上不支持的 FFMPEG / OBSENSOR 后端，白白浪费时间。
//   * 只实现主窗口需要的「打开/关闭 + 连续采集」，曝光/增益/像素格式等工业相机参数对
//     UVC 不适用；主窗口在使用本地摄像头时会禁用那一组控件。
//   * 采集跑在独立线程里，通过 imageReady 信号把帧交给界面与录像线程，
//     与 CameraManager 的行为保持一致。
// ============================================================================

#include <QObject>
#include <QImage>
#include <QString>
#include <QVector>

namespace cv { class VideoCapture; }

class LocalCamera : public QObject
{
    Q_OBJECT
public:
    explicit LocalCamera(QObject *parent = nullptr);
    ~LocalCamera() override;

    // 依次尝试打开前 maxProbe 个索引，返回真正能读出帧的索引（设备被占用时探测不到）
    static QVector<int> probeAvailableIndexes(int maxProbe = 3);

    bool isConnected() const { return m_isConnected; }
    bool isGrabbing() const { return m_isGrabbing; }
    int  deviceIndex() const { return m_deviceIndex; }
    double frameRate() const { return m_frameRate; }

public slots:
    void openDevice(int index);
    void closeDevice();
    void startGrabbing();
    void stopGrabbing();

signals:
    void cameraConnected(const QString &message);
    void cameraDisconnected(const QString &message);
    void grabbingStarted();
    void grabbingStopped();
    void imageReady(const QImage &image);
    void errorOccurred(const QString &error);

private:
    class CaptureThread;      // 采集循环（独立线程）

    cv::VideoCapture *m_video = nullptr;
    CaptureThread *m_thread = nullptr;
    bool m_isConnected = false;
    bool m_isGrabbing = false;
    int  m_deviceIndex = -1;
    double m_frameRate = 25.0;   // 打开时从驱动读到的帧率，用于录像
};

#endif // LOCALCAMERA_H
