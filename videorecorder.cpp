#include "videorecorder.h"
#include "imagestorage.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>
#include <opencv2/videoio.hpp>
#include <cmath>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

VideoRecorder::VideoRecorder(QObject *parent) : QThread(parent) {}
VideoRecorder::~VideoRecorder() { stopRecording(); }

bool VideoRecorder::startRecording(const QString &fileName, QSize size, double fps)
{
    if (isRunning() || size.width() <= 0 || size.height() <= 0 ||
        !std::isfinite(fps) || fps <= 0 || fileName.isEmpty()) return false;
    // FFmpeg MJPEG/mp4v expects even dimensions: do not silently crop raw frames.
    if (size.width() % 2 || size.height() % 2) {
        emit recordingError(QStringLiteral("录像宽高需要为偶数，请调整相机 ROI 后再录制"));
        return false;
    }
    {
        QMutexLocker lock(&m_mutex);
        m_fileName = fileName;
        m_size = size;
        m_fps = fps;
        m_frames.clear();
        m_dropped = 0;
        m_stop = false;
        m_accepting = true;
    }
    start();
    return true;
}

void VideoRecorder::enqueueFrame(const QImage &image)
{
    QMutexLocker lock(&m_mutex);
    if (!m_accepting || image.isNull()) return;
    const qint64 budget = 128 * 1024 * 1024;
    const qint64 bytes = qMax<qint64>(1, image.sizeInBytes());
    const int limit = static_cast<int>(qMax<qint64>(1, qMin<qint64>(8, budget / bytes)));
    if (m_frames.size() >= limit) {
        ++m_dropped;
        return;
    }
    m_frames.enqueue(image); // Image from SDK signal already owns a deep copy.
    m_available.wakeOne();
}

void VideoRecorder::stopRecording()
{
    {
        QMutexLocker lock(&m_mutex);
        m_accepting = false;
        m_stop = true;
        m_available.wakeAll();
    }
    wait();
}

void VideoRecorder::run()
{
    QString error;
    qint64 count = 0;
    // Older OpenCV FFmpeg builds cannot reliably write non-ASCII filenames.
    // Encode into a temporary ASCII/short path, then atomically copy via Qt.
    QTemporaryDir temp(QDir::tempPath() + QStringLiteral("/qt-camera-XXXXXX"));
    QString directory = QDir::toNativeSeparators(temp.path());
#ifdef Q_OS_WIN
    wchar_t shortPath[32768];
    const DWORD length = GetShortPathNameW(
        reinterpret_cast<LPCWSTR>(directory.utf16()), shortPath, 32768);
    if (length > 0 && length < 32768)
        directory = QString::fromWCharArray(shortPath, static_cast<int>(length));
#endif
    const bool mp4 = QFileInfo(m_fileName).suffix().compare("mp4", Qt::CaseInsensitive) == 0;
    const QString temporaryFile = directory + (mp4 ? "/capture.mp4" : "/capture.avi");
    cv::VideoWriter writer;
    try {
        if (!temp.isValid() || directory.toLatin1() != directory.toUtf8()) {
            error = QStringLiteral("无法创建 ASCII 录像临时目录，请将 TEMP 配置为可写的英文路径");
        } else {
            const int codec = mp4 ? cv::VideoWriter::fourcc('m','p','4','v')
                                  : cv::VideoWriter::fourcc('M','J','P','G');
            if (!writer.open(temporaryFile.toStdString(), cv::CAP_FFMPEG, codec,
                             m_fps, cv::Size(m_size.width(), m_size.height()), true)) {
                error = QStringLiteral("无法打开视频编码器，请检查 OpenCV/FFmpeg DLL；建议先试 AVI (MJPG)");
            }
        }
        if (error.isEmpty()) {
            for (;;) {
                QImage image;
                {
                    QMutexLocker lock(&m_mutex);
                    while (m_frames.isEmpty() && !m_stop) m_available.wait(&m_mutex);
                    if (m_frames.isEmpty() && m_stop) break;
                    image = m_frames.dequeue();
                }
                if (image.size() != m_size) {
                    error = QStringLiteral("录像期间图像尺寸发生变化，已停止录制");
                    break;
                }
                writer.write(ImageStorage::toBgr(image));
                ++count;
            }
        }
        writer.release(); // Write the AVI/MP4 index before copying.
    } catch (const cv::Exception &e) {
        error = QStringLiteral("视频编码失败: ") + QString::fromLocal8Bit(e.what());
    } catch (const std::exception &e) {
        error = QStringLiteral("视频保存失败: ") + QString::fromLocal8Bit(e.what());
    }
    // Stop accepting even if codec initialization/encoding failed.
    qint64 dropped;
    {
        QMutexLocker lock(&m_mutex);
        m_accepting = false;
        m_frames.clear();
        dropped = m_dropped;
    }
    if (error.isEmpty() && count == 0) error = QStringLiteral("没有收到视频帧，未生成视频文件");
    if (error.isEmpty()) {
        QFile source(temporaryFile);
        QSaveFile target(m_fileName);
        if (!source.open(QIODevice::ReadOnly)) error = source.errorString();
        else if (!target.open(QIODevice::WriteOnly)) error = target.errorString();
        else {
            while (!source.atEnd()) {
                const QByteArray chunk = source.read(1024 * 1024);
                if (chunk.isEmpty() || target.write(chunk) != chunk.size()) {
                    error = QStringLiteral("复制录像文件失败: ") + source.errorString() + target.errorString();
                    break;
                }
            }
            if (error.isEmpty() && !target.commit()) error = target.errorString();
        }
    }
    if (!error.isEmpty()) emit recordingError(error);
    else emit recordingSaved(m_fileName, count, dropped);
}
