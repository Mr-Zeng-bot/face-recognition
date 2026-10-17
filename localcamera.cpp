#include "localcamera.h"
#include "imagepreprocess.h"

#include <QThread>
#include <QDebug>
#include <atomic>
#include <opencv2/videoio.hpp>

// ---------------------------------------------------------------------------
// 本地（UVC）摄像头采集实现
// ---------------------------------------------------------------------------

namespace {
const int kPreferredWidth = 1280;      // 期望分辨率，驱动不支持时自动退回默认值
const int kPreferredHeight = 720;
const int kMaxReadFailures = 30;       // 连续读帧失败多少次后判定设备掉线
}

// 采集循环：只读帧、转 QImage、发信号；停止由原子标志控制，不依赖事件循环
class LocalCamera::CaptureThread : public QThread
{
public:
    CaptureThread(LocalCamera *owner, cv::VideoCapture *video)
        : m_owner(owner), m_video(video) {}

    void requestStop() { m_stop = true; }

protected:
    void run() override
    {
        cv::Mat frame;
        int failures = 0;
        while (!m_stop) {
            if (!m_video->read(frame) || frame.empty()) {
                if (++failures >= kMaxReadFailures) {
                    emit m_owner->errorOccurred(
                        QStringLiteral("本地摄像头连续读取失败，采集已停止"));
                    break;
                }
                msleep(10);
                continue;
            }
            failures = 0;
            // fromBgrMat 内部做深拷贝，拷完就可以让驱动复用帧缓冲
            const QImage image = ImagePreprocess::fromBgrMat(frame);
            if (!image.isNull())
                emit m_owner->imageReady(image);
        }
    }

private:
    LocalCamera *m_owner = nullptr;
    cv::VideoCapture *m_video = nullptr;
    std::atomic<bool> m_stop{false};
};

LocalCamera::LocalCamera(QObject *parent) : QObject(parent) {}

LocalCamera::~LocalCamera()
{
    stopGrabbing();
    closeDevice();
}

QVector<int> LocalCamera::probeAvailableIndexes(int maxProbe)
{
    QVector<int> found;
    for (int i = 0; i < maxProbe; ++i) {
        cv::VideoCapture probe;
        // 显式指定 MSMF 后端：本工程用的 OpenCV 5.0 不带 DSHOW 插件，而 CAP_ANY 还会先
        // 撞上不支持的 FFMPEG / OBSENSOR 后端（详见 localcamera.h 顶部说明）
        bool ok = probe.open(i, cv::CAP_MSMF);
        if (ok && probe.isOpened()) {
            // 设备刚打开时前几帧常常拿不到（MSMF 需要一点时间起流），多试几次
            cv::Mat frame;
            for (int attempt = 0; attempt < 15; ++attempt) {
                if (probe.read(frame) && !frame.empty()) {
                    found.push_back(i);
                    break;
                }
                QThread::msleep(30);
            }
        }
        probe.release();
        // MSMF 释放是异步的，给驱动一点时间，避免紧接着正式打开时抢不到设备
        QThread::msleep(200);
    }
    return found;
}

void LocalCamera::openDevice(int index)
{
    if (m_isConnected) closeDevice();

    auto *video = new cv::VideoCapture();
    // 显式指定 MSMF 后端（见 probeAvailableIndexes 中的说明）；失败则稍后重试一次
    bool ok = video->open(index, cv::CAP_MSMF);
    if (!ok || !video->isOpened()) {
        QThread::msleep(300);
        ok = video->open(index, cv::CAP_MSMF);
    }
    if (!ok || !video->isOpened()) {
        video->release();
        delete video;
        emit errorOccurred(QStringLiteral("打开本地摄像头 %1 失败，可能被其他程序占用").arg(index));
        return;
    }

    video->set(cv::CAP_PROP_FRAME_WIDTH, kPreferredWidth);
    video->set(cv::CAP_PROP_FRAME_HEIGHT, kPreferredHeight);
    video->set(cv::CAP_PROP_BUFFERSIZE, 1);   // 只留最新帧，降低显示延迟（部分后端忽略）

    m_video = video;
    m_deviceIndex = index;
    m_isConnected = true;

    const double w = video->get(cv::CAP_PROP_FRAME_WIDTH);
    const double h = video->get(cv::CAP_PROP_FRAME_HEIGHT);
    const double fps = video->get(cv::CAP_PROP_FPS);
    m_frameRate = fps > 0 ? fps : 25.0;
    const QString rate = fps > 0 ? QStringLiteral("%1 fps").arg(fps, 0, 'f', 0)
                                 : QStringLiteral("帧率未知");
    emit cameraConnected(QStringLiteral("本地摄像头 %1 已打开：%2x%3 @ %4（连续采集）")
                             .arg(index)
                             .arg(w, 0, 'f', 0)
                             .arg(h, 0, 'f', 0)
                             .arg(rate));
}

void LocalCamera::closeDevice()
{
    stopGrabbing();
    // stopGrabbing 只在确认采集线程结束后才销毁它。若 m_thread 仍非空，说明线程还卡在
    // 驱动里，此时释放并销毁 VideoCapture 会让线程内部用到悬空指针，因此放弃关闭。
    if (m_thread != nullptr) {
        emit errorOccurred(QStringLiteral("本地摄像头采集线程未能退出，已放弃关闭设备，请重启程序"));
        return;
    }
    if (m_video) {
        m_video->release();
        delete m_video;
        m_video = nullptr;
    }
    const bool wasConnected = m_isConnected;
    m_isConnected = false;
    m_deviceIndex = -1;
    if (wasConnected)
        emit cameraDisconnected(QStringLiteral("本地摄像头已关闭"));
}

void LocalCamera::startGrabbing()
{
    if (!m_isConnected || m_isGrabbing || m_video == nullptr) return;
    m_thread = new CaptureThread(this, m_video);
    // 采集线程自行退出（例如摄像头掉线）时也要复位状态，避免留下野指针
    connect(m_thread, &QThread::finished, this, [this]() {
        if (m_thread == nullptr) return;   // 已经由 stopGrabbing 收尾
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
        m_isGrabbing = false;
        emit grabbingStopped();
    });
    m_isGrabbing = true;
    m_thread->start();
    emit grabbingStarted();
}

void LocalCamera::stopGrabbing()
{
    if (m_thread == nullptr) {
        m_isGrabbing = false;
        return;
    }

    // 从采集线程内部调用（例如在 imageReady 回调里停止采集）时不能等自己，
    // 转交给本对象所属线程处理
    if (QThread::currentThread() == m_thread) {
        QMetaObject::invokeMethod(this, "stopGrabbing", Qt::QueuedConnection);
        return;
    }

    m_thread->requestStop();

    // 采集线程可能卡在驱动读帧里（相机掉线、驱动异常等）。Qt 6 对仍在运行的 QThread
    // 析构会直接 qFatal 中止进程，所以必须先确认它真结束了，再销毁。
    bool finished = m_thread->wait(3000);
    if (!finished) {
        // 3 秒内不肯退出的唯一手段是强制终止（Qt 里线程无法被安全中断）
        qWarning("本地摄像头采集线程未在 3 秒内退出，执行强制终止");
        m_thread->terminate();
        finished = m_thread->wait(2000);
    }
    if (!finished) {
        // 极端情况：连强制终止都没能结束。这里保留线程对象、不销毁也不改状态，
        // 等它自己退出后由 finished 回调收尾（回调里 wait() 后才 delete）。
        // 带着「仍在运行」的状态销毁 QThread 会让整个进程被中止，比停不下来更糟。
        qWarning("本地摄像头采集线程仍未退出，跳过销毁以避免进程被中止");
        return;
    }

    delete m_thread;
    m_thread = nullptr;
    m_isGrabbing = false;
    emit grabbingStopped();
}
