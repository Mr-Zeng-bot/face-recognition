#ifndef FACEPIPELINE_H
#define FACEPIPELINE_H

// ============================================================================
// facepipeline.h —— 人脸识别流水线（要求 1~8 的编排层）
// 说明：
//   * 串联：图像预处理 -> 人脸质量判断 -> SCRFD 检测 -> 5点对齐 -> ArcFace 特征
//           -> 数据库 1:N 比对 -> 结果绘制 -> 日志入库。
//   * 本类对象会被 moveToThread 到独立工作线程，避免阻塞 UI。
//     因此其中的 FaceDatabase 也在工作线程内创建/连接（QtSql 连接必须与使用它的
//     线程一致，这点很关键）。
//   * 取消注册/比对的重入由内部 busy 标志控制，UI 侧收到 busyChanged 后丢帧。
// ============================================================================

#include <QObject>
#include <QImage>
#include <QStringList>
#include <QVector>
#include <QElapsedTimer>
#include <QHash>
#include <opencv2/core.hpp>

#include "imagedata.h"
#include "facedetector.h"
#include "faceextractor.h"

class FaceDatabase;
class QTimer;

class FacePipeline : public QObject
{
    Q_OBJECT
public:
    explicit FacePipeline(QObject *parent = nullptr);
    ~FacePipeline() override;

    // 在工作线程中调用：加载模型、连接数据库
    bool init(QString &error);

public slots:
    // 处理一帧（在工作线程执行）。image 为相机原始帧。
    void processFrame(const QImage &image);

    // 人脸注册：人员必须已存在于 MySQL 且启用，从当前帧取最大的人脸提取特征
    void registerFace(const QImage &image, const QString &employeeNo);

    // 按工号读取 MySQL 中的人员姓名/部门
    void lookupEmployee(const QString &employeeNo);

    // 检查外部 MySQL 是否修改了人员或模板，必要时刷新识别缓存
    void syncExternalChanges();

    // 刷新最近日志
    void reloadLogs(int limit);

    // 运行期开/关识别、调整阈值
    void setRecognitionEnabled(bool on);
    void setThreshold(float threshold);

signals:
    // 初始化完成（在工作线程触发）
    void initFinished(bool ok, const QString &message);
    // 一帧处理完成：display 为绘制了人脸框/关键点的图像，lines 为每张脸的文本信息，
    // state 为本帧的结构化结论（界面状态卡直接用它，不必解析 lines 里的中文）
    void frameProcessed(const QImage &display, const QStringList &lines,
                        const FaceData::RecognitionState &state);
    // 忙碌状态变化（true 表示正在处理，UI 应丢帧）
    void busyChanged(bool busy);
    // 日志查询结果
    void logsLoaded(const QVector<FaceData::LogRow> &rows);
    // 注册结果
    void registerFinished(bool ok, const QString &message);
    // 人员查询结果
    void employeeLookupFinished(bool ok, const QString &employeeNo,
                                const QString &name, const QString &department,
                                const QString &message);
    // 文本日志（状态栏）
    void logMessage(const QString &text);
    // 真正写入了一条识别日志（界面据此刷新日志表，避免每帧都去查库）
    void logWritten();

private:
    // 保存抓拍人脸图到磁盘，返回文件路径（失败返回空）
    // subDir 用于在 imageSaveDir 下再分一级子目录，区分注册图与识别抓拍图
    QString saveCapture(const cv::Mat &faceBgr, const QString &employeeNo,
                        const QString &subDir = QString());

    // 同一目标（工号 / 未识别）在冷却时间内只允许写一次日志与抓拍图，
    // 否则相机前持续有人时会以帧率级别无限写库、写图。
    bool logAllowed(const QString &key);

    FaceDetector  m_detector;
    FaceExtractor m_extractor;
    FaceDatabase *m_db = nullptr;

    bool  m_ready = false;
    bool  m_recognitionEnabled = true;
    bool  m_busy = false;
    bool  m_syncErrorActive = false;
    float m_threshold = 0.5f;
    QElapsedTimer m_elapsed;
    QHash<QString, qint64> m_lastLogTime;   // 目标 -> 上次写日志/存图的时间戳(ms)
    QTimer *m_cacheTimer = nullptr;
};

#endif // FACEPIPELINE_H
