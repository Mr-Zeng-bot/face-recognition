#ifndef VIDEORECORDER_H
#define VIDEORECORDER_H
#include <QImage>
#include <QMutex>
#include <QQueue>
#include <QSize>
#include <QThread>
#include <QWaitCondition>

// Encoding runs off the GUI/SDK threads. The bounded queue prevents a slow
// disk/encoder from consuming unlimited memory. All public calls are thread-safe
// except startRecording, which is called by the GUI thread only.
class VideoRecorder : public QThread
{
    Q_OBJECT
public:
    explicit VideoRecorder(QObject *parent = nullptr);
    ~VideoRecorder() override;
    bool startRecording(const QString &fileName, QSize size, double fps);
    void enqueueFrame(const QImage &image);
    void stopRecording(); // Drains the queue, finalizes container, waits for disk I/O.
signals:
    void recordingSaved(const QString &fileName, qint64 frames, qint64 dropped);
    void recordingError(const QString &error);
protected:
    void run() override;
private:
    mutable QMutex m_mutex;
    QWaitCondition m_available;
    QQueue<QImage> m_frames;
    bool m_accepting = false;
    bool m_stop = false;
    QString m_fileName;
    QSize m_size;
    double m_fps = 25.0;
    qint64 m_dropped = 0;
};
#endif
