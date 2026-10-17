#ifndef MAINWINDOW_H
#define MAINWINDOW_H

// ============================================================================
// mainwindow.h —— 主窗口（界面 + 业务编排）
// 说明：
//   * 保留原项目的相机控制、参数设置、保存/录像功能；
//   * 新增人脸识别面板（开关、阈值、注册、结果）与识别日志表；
//   * 人脸识别流水线 FacePipeline 运行在独立线程，主窗口通过信号槽与其交互，
//     保证界面不卡顿。
// ============================================================================

#include <QMainWindow>
#include <QImage>
#include <QPair>
#include <QVector>
#include <QThread>
#include <QElapsedTimer>

#include "cameraManager.h"
#include "videorecorder.h"
#include "imagedata.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class FacePipeline;
class LocalCamera;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    // ---- 相机控制 ----
    void on_btnEnumCamera_clicked();
    void on_btnOpenCamera_clicked();
    void on_btnCloseCamera_clicked();
    // ---- 采集 ----
    void on_btnStartGrab_clicked();
    void on_btnStopGrab_clicked();
    void on_btnSoftTrigger_clicked();
    // ---- 保存 ----
    void on_btnSaveJPG_clicked();
    void on_btnSaveVideo_clicked();
    void on_btnOpenLocalImage_clicked();
    // ---- 参数 ----
    void on_btnGetParams_clicked();
    void on_btnSetParams_clicked();
    // ---- 人脸识别 ----
    void on_btnRegisterFace_clicked();
    void lookupEmployeeFromDatabase();
    void on_checkRecognition_toggled(bool checked);
    void on_spinThreshold_valueChanged(double value);
    void on_btnRefreshLogs_clicked();
    // ---- CameraManager 信号 ----
    void updateDeviceList(const QVector<CameraInfo> &deviceList);
    void showCameraConnected(const QString &message);
    void showCameraDisconnected(const QString &message);
    void showGrabbingStarted();
    void showGrabbingStopped();
    void showError(const QString &error);
    void onCameraFrame(const QImage &image);
    void updateParametersDisplay();
    void updatePixelFormats(const QVector<QPair<QString, int>> &formats);
    // ---- FacePipeline 信号 ----
    void onPipelineInit(bool ok, const QString &message);
    void onFrameProcessed(const QImage &display, const QStringList &lines,
                          const FaceData::RecognitionState &state);
    void onPipelineBusy(bool busy);
    void onLogsLoaded(const QVector<FaceData::LogRow> &rows);
    void onRegisterFinished(bool ok, const QString &message);
    void onEmployeeLookupFinished(bool ok, const QString &employeeNo,
                                  const QString &name, const QString &department,
                                  const QString &message);
    void onPipelineMessage(const QString &text);

private:
    // 设备下拉框的数据项：来源类型（海康工业相机 / 本机 UVC 摄像头）
    enum class SourceKind { MvsCamera = 0, LocalCamera = 1 };
    bool selectedSourceIsLocal() const;
    int  selectedDeviceIndex() const;
    // 查找设备：includeLocalCameras=true 时同时探测本机 UVC 摄像头
    void enumerateDevices(bool includeLocalCameras);
    // 在后台线程探测本机 UVC 摄像头（MSMF 探测失败时每个索引要等 1~3 秒，不能占着主线程）
    void startLocalCameraProbe();
    // 用缓存的工业相机列表 + 已知的本机摄像头索引重建下拉框
    void rebuildDeviceCombo();

    void updateUIState();
    void saveImage();
    void updateParameterRanges();
    void updateDisplayedPixmap();
    void stopVideoRecording();
    void showStatus(const QString &text, int ms = 3000);
    // 右侧「操作反馈」条：ok=true 用成功色，false 用失败色
    void showFeedback(const QString &text, bool ok);
    // 把一帧的识别结论渲染到右侧状态卡（识别成功与否 / 库中存在与否）
    void showRecognitionState(const FaceData::RecognitionState &state);
    bool eventFilter(QObject *watched, QEvent *event) override;

    Ui::MainWindow *ui;
    CameraManager *m_cameraManager = nullptr;
    VideoRecorder *m_videoRecorder = nullptr;
    FacePipeline *m_pipeline = nullptr;
    QThread *m_faceThread = nullptr;
    LocalCamera *m_localCamera = nullptr;

    QVector<int> m_localCameraIndexes;   // 查找相机时探测到的本地摄像头索引
    bool m_usingLocalCamera = false;     // 当前打开的是本地摄像头而不是工业相机
    QVector<CameraInfo> m_mvsDeviceList; // 最近一次工业相机枚举结果，供探测线程出结果后重建下拉框
    QThread *m_probeThread = nullptr;    // 本机摄像头探测线程；非空表示正在探测，不要重复启动

    QImage m_rawImage;        // 相机原始帧 / 加载的本地图（识别的输入）
    QImage m_displayImage;    // 当前显示图（原始帧或带识别框的结果）
    bool m_isGrabbing = false;
    bool m_videoRecording = false;
    bool m_pipelineBusy = false;     // 流水线忙时丢弃新帧，避免堆积
    bool m_showingLocalImage = false;

    QElapsedTimer m_frameThrottle;   // 控制送入流水线的帧率
};

#endif // MAINWINDOW_H
