#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "appconfig.h"
#include "facepipeline.h"
#include "localcamera.h"

#include <QMessageBox>
#include <QFileDialog>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLineEdit>
#include <QTableWidgetItem>
#include <QMetaObject>

namespace {
// 本机摄像头探测放到后台线程执行：MSMF 打开失败时单个索引可能就要等 1~3 秒，
// 留在主线程会让界面冻结好几秒（实测约 7 秒）。
class LocalCameraProbeThread : public QThread
{
public:
    explicit LocalCameraProbeThread(QObject *parent) : QThread(parent) {}

    // 结果在 run() 里写入，调用方在 QThread::finished 之后（即 run 已返回）读取
    QVector<int> detectedIndexes() const { return m_indexes; }

protected:
    void run() override { m_indexes = LocalCamera::probeAvailableIndexes(); }

private:
    QVector<int> m_indexes;
};
}   // namespace

// ---------------------------------------------------------------------------
// 主窗口实现
// ---------------------------------------------------------------------------

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_cameraManager(new CameraManager(this))
    , m_videoRecorder(new VideoRecorder(this))
{
    ui->setupUi(this);
    ui->labelDisplay->installEventFilter(this);

    // ---- 相机信号连接 ----
    connect(m_cameraManager, &CameraManager::devicesEnumerated, this, &MainWindow::updateDeviceList);
    connect(m_cameraManager, &CameraManager::cameraConnected, this, &MainWindow::showCameraConnected);
    connect(m_cameraManager, &CameraManager::cameraDisconnected, this, &MainWindow::showCameraDisconnected);
    connect(m_cameraManager, &CameraManager::grabbingStarted, this, &MainWindow::showGrabbingStarted);
    connect(m_cameraManager, &CameraManager::grabbingStopped, this, &MainWindow::showGrabbingStopped);
    connect(m_cameraManager, &CameraManager::errorOccurred, this, &MainWindow::showError);
    connect(m_cameraManager, &CameraManager::imageReady, this, &MainWindow::onCameraFrame);
    connect(m_cameraManager, &CameraManager::parametersUpdated, this, &MainWindow::updateParametersDisplay);
    connect(m_cameraManager, &CameraManager::pixelFormatsReady, this, &MainWindow::updatePixelFormats);

    // 录像线程从相机回调线程直接收帧（保持原项目行为）
    connect(m_cameraManager, &CameraManager::imageReady,
            m_videoRecorder, &VideoRecorder::enqueueFrame, Qt::DirectConnection);

    // ---- 本机 UVC 摄像头（没有工业相机时的替代视频源） ----
    m_localCamera = new LocalCamera(this);
    connect(m_localCamera, &LocalCamera::cameraConnected, this, &MainWindow::showCameraConnected);
    connect(m_localCamera, &LocalCamera::cameraDisconnected, this, &MainWindow::showCameraDisconnected);
    connect(m_localCamera, &LocalCamera::grabbingStarted, this, &MainWindow::showGrabbingStarted);
    connect(m_localCamera, &LocalCamera::grabbingStopped, this, &MainWindow::showGrabbingStopped);
    connect(m_localCamera, &LocalCamera::errorOccurred, this, &MainWindow::showError);
    connect(m_localCamera, &LocalCamera::imageReady, this, &MainWindow::onCameraFrame);
    // 与工业相机一致：录像线程在采集线程里直接收帧
    connect(m_localCamera, &LocalCamera::imageReady,
            m_videoRecorder, &VideoRecorder::enqueueFrame, Qt::DirectConnection);

    connect(m_videoRecorder, &VideoRecorder::recordingSaved, this,
            [this](const QString &file, qint64 frames, qint64 dropped) {
                showStatus(QStringLiteral("视频已保存: %1（%2 帧，丢帧 %3）")
                               .arg(file).arg(frames).arg(dropped), 8000);
            });
    connect(m_videoRecorder, &VideoRecorder::recordingError, this, &MainWindow::showError);

    // ---- 人脸识别流水线：放到独立线程 ----
    m_faceThread = new QThread(this);
    m_pipeline = new FacePipeline();
    m_pipeline->moveToThread(m_faceThread);

    connect(m_faceThread, &QThread::started, m_pipeline, [this]() {
        QString error;
        m_pipeline->init(error);   // init 内部会 emit initFinished 反馈结果
    });
    connect(m_pipeline, &FacePipeline::initFinished, this, &MainWindow::onPipelineInit);
    connect(m_pipeline, &FacePipeline::frameProcessed, this, &MainWindow::onFrameProcessed);
    connect(m_pipeline, &FacePipeline::busyChanged, this, &MainWindow::onPipelineBusy);
    connect(m_pipeline, &FacePipeline::logsLoaded, this, &MainWindow::onLogsLoaded);
    connect(m_pipeline, &FacePipeline::registerFinished, this, &MainWindow::onRegisterFinished);
    connect(m_pipeline, &FacePipeline::employeeLookupFinished,
            this, &MainWindow::onEmployeeLookupFinished);
    connect(m_pipeline, &FacePipeline::logMessage, this, &MainWindow::onPipelineMessage);
    // 只有真正写入了日志才刷新日志表：原来每帧识别完都查一次库，既浪费又让表格一直闪
    connect(m_pipeline, &FacePipeline::logWritten, this, [this]() {
        QMetaObject::invokeMethod(m_pipeline, "reloadLogs", Qt::QueuedConnection, Q_ARG(int, 50));
    });
    // 线程退出时由流水线在自己的线程里析构（FaceDatabase / QSqlDatabase 随之在正确
    // 线程关闭），配合析构函数中的 quit + wait 使用，避免"运行中析构"崩溃。
    connect(m_faceThread, &QThread::finished, m_pipeline, &QObject::deleteLater);
    m_faceThread->start();

    const AppConfig &cfg = AppConfig::instance();

    // 人员主数据由 MySQL 管理，界面上的姓名只读并随工号自动带出。
    ui->editEmployeeName->setReadOnly(true);
    ui->editEmployeeName->setPlaceholderText(QStringLiteral("按工号自动读取"));
    connect(ui->editEmployeeNo, &QLineEdit::editingFinished,
            this, &MainWindow::lookupEmployeeFromDatabase);
    connect(ui->editEmployeeNo, &QLineEdit::textEdited, this, [this]() {
        ui->editEmployeeName->clear();
    });

    // 阈值控件初值
    ui->spinThreshold->setValue(cfg.matchThreshold);

    // 抓拍目录放到顶栏显示（原先是塞在窗口标题里），避免用户找不到图存哪里。
    // imageSaveDir 在 AppConfig::load 里已按 config.ini 所在目录解析为绝对路径。
    const QString captureDir = QDir(cfg.imageSaveDir).absolutePath();
    ui->labelCaptureDir->setText(QStringLiteral("抓拍目录  %1").arg(captureDir));

    // 全局样式。文字只有两档：主文字 #1F2328、次要文字 #4B5563；
    // 浅灰只允许出现在边框上，绝不做文字色——浅灰字落在浅底上会糊成一片看不清。
    // 每条规则、每个控件状态都显式声明 color，避免继承到低对比度的默认色。
    setStyleSheet(QStringLiteral(R"(
QMainWindow, QWidget#centralwidget { background: #F2F4F7; }
QFrame#frameTopBar, QFrame#frameControlBar {
    background: #FFFFFF; border: 1px solid #D0D5DD; border-radius: 6px;
}
QLabel { color: #1F2328; }
QLabel#labelTitle { font-size: 14px; font-weight: 600; }
QLabel#labelCaptureDir { color: #4B5563; font-size: 12px; }
QGroupBox { background: #FFFFFF; border: 1px solid #D0D5DD; border-radius: 8px;
            margin-top: 12px; font-weight: 600; }
QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left;
                   left: 10px; padding: 0 4px; color: #1F2328; }
QPushButton { background: #F2F4F7; color: #1F2328; border: 1px solid #C9CED6;
              border-radius: 5px; padding: 3px 10px; }
QPushButton:hover { background: #E7EAEE; }
QPushButton:disabled { background: #F7F8FA; color: #9CA3AF; border-color: #E3E6EA; }
QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { background: #FFFFFF; color: #1F2328;
              border: 1px solid #C9CED6; border-radius: 5px; padding: 3px 6px; }
QComboBox QAbstractItemView { background: #FFFFFF; color: #1F2328; }
QCheckBox, QRadioButton { color: #1F2328; }
QCheckBox:disabled, QRadioButton:disabled { color: #9CA3AF; }
QTableWidget { background: #FFFFFF; color: #1F2328; border: 1px solid #D0D5DD;
               border-radius: 6px; gridline-color: #E7EAEE; }
QTableWidget::item { color: #1F2328; }
QHeaderView::section { background: #F2F4F7; color: #4B5563; border: none;
               border-bottom: 1px solid #D0D5DD; padding: 5px 8px; font-weight: 600; }
QListWidget { background: #FFFFFF; color: #1F2328; border: 1px solid #D0D5DD; border-radius: 6px; }
QListWidget::item { color: #1F2328; }
QStatusBar { color: #4B5563; }
)"));

    // 状态卡各行的常驻字体；颜色由 showRecognitionState / showFeedback 按结论覆盖
    ui->labelResultState->setStyleSheet(
        QStringLiteral("font-size: 26px; font-weight: 700; color: #4B5563;"));
    ui->labelResultIdentity->setStyleSheet(
        QStringLiteral("font-size: 15px; font-weight: 600; color: #1F2328;"));
    ui->labelResultSimilarity->setStyleSheet(
        QStringLiteral("font-size: 13px; color: #1F2328; font-family: Consolas, monospace;"));
    ui->labelResultDetail->setStyleSheet(QStringLiteral("font-size: 12px; color: #4B5563;"));
    ui->labelFaceInfoTitle->setStyleSheet(
        QStringLiteral("font-size: 12px; font-weight: 600; color: #4B5563;"));
    ui->labelOpFeedback->setStyleSheet(QStringLiteral(
        "background: #F2F4F7; color: #4B5563; border-radius: 6px; padding: 8px 10px; font-size: 12px;"));

    m_frameThrottle.start();
    updateUIState();
    // 启动时自动查找工业相机（本机摄像头会短暂打开设备，只在用户点“查找相机”时探测）
    enumerateDevices(false);
    // 拉取一次日志
    QMetaObject::invokeMethod(m_pipeline, "reloadLogs", Qt::QueuedConnection, Q_ARG(int, 50));
}

MainWindow::~MainWindow()
{
    m_videoRecorder->stopRecording();
    if (m_localCamera) m_localCamera->closeDevice();
    m_cameraManager->disconnectCamera();
    // 结束人脸线程。quit() 后线程退出事件循环并发出 finished，
    // 流水线通过 deleteLater 在它自己的线程里析构，这里只负责等待线程真正结束。
    if (m_faceThread) {
        m_faceThread->quit();
        if (m_faceThread->wait(5000)) {
            m_pipeline = nullptr;   // 已由 deleteLater 回收
        } else {
            // 兜底：流水线卡在数据库/磁盘 IO 上超过 5 秒时，绝不能析构仍在运行的对象。
            // 把线程与流水线从本窗口的父子关系中摘出，交由进程退出时回收（宁可泄漏也不崩溃）。
            qWarning() << "[main] 人脸流水线线程未能在超时内退出，跳过析构以避免崩溃";
            m_faceThread->setParent(nullptr);
            m_pipeline->setParent(nullptr);
            m_pipeline = nullptr;
            m_faceThread = nullptr;
        }
    }
    // 本机摄像头探测线程是 this 的子对象，若在运行中被 QObject 析构，会在未结束的线程上
    // 调用 ~QThread 导致崩溃，所以先等它结束；超时则摘出父子关系，宁可泄漏也不崩溃。
    if (m_probeThread) {
        if (!m_probeThread->wait(5000)) {
            qWarning() << "[main] 摄像头探测线程未能在超时内退出，摘出父子关系以避免崩溃";
            m_probeThread->setParent(nullptr);
        }
        m_probeThread = nullptr;
    }
    delete ui;
}

// ---------------- UI 状态 ----------------
void MainWindow::updateUIState()
{
    const bool connected = m_usingLocalCamera ? m_localCamera->isConnected()
                                              : m_cameraManager->isConnected();
    ui->btnOpenCamera->setEnabled(!connected);
    ui->btnCloseCamera->setEnabled(connected);
    ui->btnStartGrab->setEnabled(connected && !m_isGrabbing);
    ui->btnStopGrab->setEnabled(connected && m_isGrabbing);
    // 本地摄像头是连续采集，没有软触发，也没有工业相机那组参数
    ui->btnSoftTrigger->setEnabled(!m_usingLocalCamera && connected && m_isGrabbing);
    ui->groupBoxParams->setEnabled(!m_usingLocalCamera && connected && !m_videoRecording);
    ui->radioContinuous->setEnabled(!m_usingLocalCamera);
    ui->radioTrigger->setEnabled(!m_usingLocalCamera);
    if (m_usingLocalCamera) ui->radioContinuous->setChecked(true);
    // 保存按钮直接按状态启用（原先靠 groupBoxSave 整组禁用，重构后已没有这个分组）
    ui->btnSaveJPG->setEnabled(connected && m_isGrabbing);
    ui->btnSaveVideo->setEnabled(connected && m_isGrabbing);
    ui->btnSaveVideo->setText(m_videoRecording ? QStringLiteral("停止录像")
                                               : QStringLiteral("保存视频"));

    // 顶栏连接状态胶囊：一眼看出「有没有连上、在不在采集」
    QString connText;
    QString connStyle;
    if (connected) {
        connText = QStringLiteral("● 已连接 · %1 · %2")
                       .arg(m_usingLocalCamera ? QStringLiteral("本机摄像头")
                                               : QStringLiteral("工业相机"),
                            m_isGrabbing ? QStringLiteral("采集中") : QStringLiteral("未采集"));
        connStyle = QStringLiteral("background:#DCFCE7;color:#166534;");
    } else {
        connText = QStringLiteral("● 未连接");
        connStyle = QStringLiteral("background:#E7EAEE;color:#4B5563;");
    }
    ui->labelConnStatus->setText(connText);
    ui->labelConnStatus->setStyleSheet(connStyle +
        QStringLiteral("border-radius:10px;padding:3px 12px;font-weight:600;"));
}

void MainWindow::showStatus(const QString &text, int ms)
{
    ui->statusBar->showMessage(text, ms);
}

// 右侧「操作反馈」条：反馈会一直留在界面上，比只闪一下状态栏更容易核对
void MainWindow::showFeedback(const QString &text, bool ok)
{
    ui->labelOpFeedback->setText(text.isEmpty() ? QStringLiteral("就绪") : text);
    ui->labelOpFeedback->setStyleSheet(ok
        ? QStringLiteral("background:#DCFCE7;color:#166534;border-left:4px solid #16A34A;"
                         "border-radius:6px;padding:8px 10px;font-size:12px;")
        : QStringLiteral("background:#FEE2E2;color:#991B1B;border-left:4px solid #DC2626;"
                         "border-radius:6px;padding:8px 10px;font-size:12px;"));
}

// ---------------- 相机控制 ----------------
void MainWindow::enumerateDevices(bool includeLocalCameras)
{
    ui->comboDevices->clear();
    ui->comboDevices->addItem(QStringLiteral("正在查找..."));
    if (includeLocalCameras) {
        // 本机摄像头探测会短暂打开设备且可能耗时数秒，放到后台线程；
        // 工业相机枚举本身是异步的，两边结果回来时各自刷新下拉框
        startLocalCameraProbe();
    } else {
        m_localCameraIndexes.clear();
    }
    m_cameraManager->enumerateDevices();
}

// 后台线程探测本机摄像头，结果回来后刷新下拉框
void MainWindow::startLocalCameraProbe()
{
    if (m_probeThread) return;   // 上一次探测还在进行，等它出结果即可

    auto *probe = new LocalCameraProbeThread(this);
    m_probeThread = probe;
    connect(probe, &QThread::finished, this, [this, probe]() {
        m_localCameraIndexes = probe->detectedIndexes();
        if (m_probeThread == probe) m_probeThread = nullptr;
        rebuildDeviceCombo();
        probe->deleteLater();
    });
    probe->start();
}

void MainWindow::on_btnEnumCamera_clicked()
{
    enumerateDevices(true);
}

bool MainWindow::selectedSourceIsLocal() const
{
    return ui->comboDevices->currentData().toInt() == static_cast<int>(SourceKind::LocalCamera);
}

int MainWindow::selectedDeviceIndex() const
{
    const QVariant data = ui->comboDevices->currentData(Qt::UserRole + 1);
    return data.isValid() ? data.toInt() : -1;
}

void MainWindow::on_btnOpenCamera_clicked()
{
    const int index = ui->comboDevices->currentIndex();
    const int deviceIndex = selectedDeviceIndex();
    if (index < 0 || deviceIndex < 0) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先查找相机并选择设备"));
        return;
    }
    if (selectedSourceIsLocal()) {
        m_usingLocalCamera = true;
        m_localCamera->openDevice(deviceIndex);
    } else {
        m_usingLocalCamera = false;
        m_cameraManager->connectCamera(deviceIndex);
    }
}

void MainWindow::on_btnCloseCamera_clicked()
{
    stopVideoRecording();
    if (m_usingLocalCamera) m_localCamera->closeDevice();
    else m_cameraManager->disconnectCamera();
    m_isGrabbing = false;
    updateUIState();
}

void MainWindow::on_btnStartGrab_clicked()
{
    m_showingLocalImage = false;
    if (m_usingLocalCamera) m_localCamera->startGrabbing();
    else m_cameraManager->startGrabbing();
}

void MainWindow::on_btnStopGrab_clicked()
{
    if (m_usingLocalCamera) m_localCamera->stopGrabbing();
    else m_cameraManager->stopGrabbing();
}

void MainWindow::on_btnSoftTrigger_clicked()
{
    if (m_usingLocalCamera) {
        showStatus(QStringLiteral("本地摄像头为连续采集，无需软触发"), 3000);
        return;
    }
    m_cameraManager->softTrigger();
}

// ---------------- 相机信号 ----------------
void MainWindow::updateDeviceList(const QVector<CameraInfo> &deviceList)
{
    m_mvsDeviceList = deviceList;   // 缓存：本机摄像头探测完成后还要用它重建下拉框
    rebuildDeviceCombo();
}

// 用缓存的工业相机列表 + 当前已知的本机摄像头索引重建下拉框
void MainWindow::rebuildDeviceCombo()
{
    ui->comboDevices->clear();
    for (int i = 0; i < m_mvsDeviceList.size(); ++i) {
        const CameraInfo &info = m_mvsDeviceList[i];
        ui->comboDevices->addItem(QStringLiteral("[工业相机] %1 (%2) - %3")
                                      .arg(info.modelName, info.serialNumber, info.ipAddress),
                                  QVariant(static_cast<int>(SourceKind::MvsCamera)));
        ui->comboDevices->setItemData(ui->comboDevices->count() - 1,
                                      QVariant(i), Qt::UserRole + 1);
    }
    for (int index : m_localCameraIndexes) {
        ui->comboDevices->addItem(QStringLiteral("[本机摄像头] USB 摄像头 %1").arg(index),
                                  QVariant(static_cast<int>(SourceKind::LocalCamera)));
        ui->comboDevices->setItemData(ui->comboDevices->count() - 1,
                                      QVariant(index), Qt::UserRole + 1);
    }
    if (ui->comboDevices->count() == 0) {
        ui->comboDevices->addItem(QStringLiteral("未发现工业相机（点“查找相机”可搜索本机摄像头）"));
    } else {
        ui->comboDevices->setCurrentIndex(0);
    }
}

void MainWindow::showCameraConnected(const QString &message)
{
    showStatus(message);
    updateUIState();
    updateParameterRanges();
    updateParametersDisplay();
}

void MainWindow::showCameraDisconnected(const QString &message)
{
    stopVideoRecording();
    m_rawImage = QImage();
    m_displayImage = QImage();
    m_showingLocalImage = false;
    m_usingLocalCamera = false;
    ui->labelDisplay->clear();
    ui->comboPixelFormat->clear();
    ui->listFaceInfo->clear();
    showRecognitionState(FaceData::RecognitionState());   // 断开后状态卡复位为「未检测到人脸」
    m_isGrabbing = false;
    updateUIState();
    showStatus(message);
}

void MainWindow::showGrabbingStarted()
{
    m_isGrabbing = true;
    showStatus(QStringLiteral("采集已开始"), 2000);
    updateUIState();
}

void MainWindow::showGrabbingStopped()
{
    m_isGrabbing = false;
    stopVideoRecording();
    showStatus(QStringLiteral("采集已停止"), 2000);
    updateUIState();
}

void MainWindow::showError(const QString &error)
{
    // 状态栏 + 右侧反馈条双重提示；不弹窗，避免参数设置连续失败时阻塞界面
    showStatus(QStringLiteral("错误: ") + error, 5000);
    showFeedback(QStringLiteral("错误：") + error, false);
    qWarning().noquote() << error;
}

void MainWindow::onCameraFrame(const QImage &image)
{
    if (image.isNull() || m_showingLocalImage) return;

    m_rawImage = image;

    // 立即显示原图（帧率高时比等待识别结果更流畅）
    m_displayImage = image;
    updateDisplayedPixmap();

    // 节流：流水线忙或距上次送入不足 200ms 时丢帧，避免任务堆积
    if (m_pipelineBusy || m_frameThrottle.elapsed() < 200) return;
    m_frameThrottle.restart();
    QMetaObject::invokeMethod(m_pipeline, "processFrame", Qt::QueuedConnection,
                              Q_ARG(QImage, image));
}

// ---------------- 人脸流水线信号 ----------------
void MainWindow::onPipelineInit(bool ok, const QString &message)
{
    showStatus(message, 5000);
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("人脸识别初始化失败"), message);
        ui->checkRecognition->setChecked(false);
        ui->checkRecognition->setEnabled(false);
        ui->btnRegisterFace->setEnabled(false);
    }
}

// 把一帧的结论渲染到右侧状态卡。
// 颜色只是辅助，结论文字本身就能看懂，避免只靠颜色区分。
void MainWindow::showRecognitionState(const FaceData::RecognitionState &state)
{
    using RS = FaceData::RecognitionState;

    QString title;        // 识别成功与否
    QString titleColor;
    QString inDb;         // 库中存在与否（独立于识别结论的一层信息）
    QString identity;
    QString similarity;
    QString detail = state.detail;

    switch (state.status) {
    case RS::Matched:
        title      = QStringLiteral("识别成功");
        titleColor = QStringLiteral("#16A34A");
        inDb       = QStringLiteral("库中已有");
        identity   = QStringLiteral("%1 · %2")
                         .arg(state.employeeNo,
                              state.name.isEmpty() ? QStringLiteral("未登记姓名") : state.name);
        similarity = QStringLiteral("相似度 %1").arg(QString::number(state.similarity, 'f', 3));
        break;
    case RS::NotMatched:
        title      = QStringLiteral("未识别");
        titleColor = QStringLiteral("#DC2626");
        inDb       = QStringLiteral("查无此人");
        identity   = QStringLiteral("未匹配到库中任何人员");
        similarity = QStringLiteral("库中最高相似度 %1")
                         .arg(QString::number(state.similarity, 'f', 3));
        break;
    case RS::RecognitionOff:
        title      = QStringLiteral("识别已关闭");
        titleColor = QStringLiteral("#4B5563");
        inDb       = QStringLiteral("库状态未知");
        identity   = QStringLiteral("仅做人脸检测，未参与比对");
        break;
    case RS::ExtractFailed:
        title      = QStringLiteral("特征提取失败");
        titleColor = QStringLiteral("#DC2626");
        inDb       = QStringLiteral("库状态未知");
        identity   = QStringLiteral("质量通过，但特征未提取出来");
        break;
    case RS::LowQuality:
        title      = QStringLiteral("质量不合格");
        titleColor = QStringLiteral("#D97706");
        inDb       = QStringLiteral("已丢弃");
        identity   = QStringLiteral("该人脸未参与识别");
        break;
    case RS::NoFace:
    default:
        title      = QStringLiteral("未检测到人脸");
        titleColor = QStringLiteral("#4B5563");
        break;
    }

    // 一帧里有多张人脸时补充说明，免得用户以为漏了人
    if (state.faceCount > 1 && detail.isEmpty())
        detail = QStringLiteral("本帧共检测到 %1 张人脸").arg(state.faceCount);

    ui->labelResultState->setText(title);
    ui->labelResultState->setStyleSheet(
        QStringLiteral("font-size:26px;font-weight:700;color:%1;").arg(titleColor));

    ui->labelResultInDb->setText(inDb);
    ui->labelResultInDb->setStyleSheet(inDb.isEmpty() ? QString()
        : (state.status == RS::Matched
               ? QStringLiteral("border-radius:10px;padding:3px 12px;font-weight:600;"
                                "background:#DCFCE7;color:#166534;")
               : QStringLiteral("border-radius:10px;padding:3px 12px;font-weight:600;"
                                "background:#F1F2F4;color:#374151;")));

    ui->labelResultIdentity->setText(identity);
    ui->labelResultSimilarity->setText(similarity);
    ui->labelResultDetail->setText(detail);
}

void MainWindow::onFrameProcessed(const QImage &display, const QStringList &lines,
                                  const FaceData::RecognitionState &state)
{
    if (display.isNull()) return;
    m_displayImage = display;
    updateDisplayedPixmap();

    showRecognitionState(state);

    ui->listFaceInfo->clear();
    for (const QString &line : lines)
        ui->listFaceInfo->addItem(line);
}

void MainWindow::onPipelineBusy(bool busy)
{
    m_pipelineBusy = busy;
}

void MainWindow::onLogsLoaded(const QVector<FaceData::LogRow> &rows)
{
    ui->tableLogs->setRowCount(rows.size());
    for (int r = 0; r < rows.size(); ++r) {
        const FaceData::LogRow &row = rows[r];
        ui->tableLogs->setItem(r, 0, new QTableWidgetItem(row.captureTime));
        ui->tableLogs->setItem(r, 1, new QTableWidgetItem(row.employeeNo));
        ui->tableLogs->setItem(r, 2, new QTableWidgetItem(row.name));
        ui->tableLogs->setItem(r, 3, new QTableWidgetItem(QString::number(row.similarity, 'f', 3)));
        ui->tableLogs->setItem(r, 4, new QTableWidgetItem(row.result));
        ui->tableLogs->setItem(r, 5, new QTableWidgetItem(row.imagePath));
    }
}

void MainWindow::onRegisterFinished(bool ok, const QString &message)
{
    showStatus(message, 6000);
    showFeedback(message, ok);   // 反馈条保留结果，方便核对注册的是谁
    if (!ok)
        QMessageBox::warning(this, QStringLiteral("注册失败"), message);
    QMetaObject::invokeMethod(m_pipeline, "reloadLogs", Qt::QueuedConnection, Q_ARG(int, 50));
}

void MainWindow::onEmployeeLookupFinished(bool ok, const QString &employeeNo,
                                          const QString &name,
                                          const QString &department,
                                          const QString &message)
{
    if (employeeNo != ui->editEmployeeNo->text().trimmed())
        return;

    if (!ok) {
        ui->editEmployeeName->clear();
        showFeedback(message, false);
        return;
    }

    ui->editEmployeeName->setText(name);
    ui->editEmployeeName->setToolTip(department);
    showFeedback(message, true);
}

void MainWindow::onPipelineMessage(const QString &text)
{
    showStatus(text, 4000);
    qDebug().noquote() << "[FacePipeline]" << text;
}

// ---------------- 人脸识别面板 ----------------
void MainWindow::on_btnRegisterFace_clicked()
{
    const QString employeeNo = ui->editEmployeeNo->text().trimmed();
    if (employeeNo.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先填写工号"));
        return;
    }
    if (m_rawImage.isNull()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("没有可用图像，请先采集或打开图片"));
        return;
    }
    QMetaObject::invokeMethod(m_pipeline, "registerFace", Qt::QueuedConnection,
                              Q_ARG(QImage, m_rawImage),
                              Q_ARG(QString, employeeNo));
    showStatus(QStringLiteral("正在注册..."), 2000);
}

void MainWindow::lookupEmployeeFromDatabase()
{
    const QString employeeNo = ui->editEmployeeNo->text().trimmed();
    if (employeeNo.isEmpty()) {
        ui->editEmployeeName->clear();
        return;
    }
    QMetaObject::invokeMethod(m_pipeline, "lookupEmployee", Qt::QueuedConnection,
                              Q_ARG(QString, employeeNo));
}

void MainWindow::on_checkRecognition_toggled(bool checked)
{
    QMetaObject::invokeMethod(m_pipeline, "setRecognitionEnabled", Qt::QueuedConnection,
                              Q_ARG(bool, checked));
    showStatus(checked ? QStringLiteral("已开启人脸识别") : QStringLiteral("已关闭人脸识别"), 2000);
}

void MainWindow::on_spinThreshold_valueChanged(double value)
{
    QMetaObject::invokeMethod(m_pipeline, "setThreshold", Qt::QueuedConnection,
                              Q_ARG(float, static_cast<float>(value)));
}

void MainWindow::on_btnRefreshLogs_clicked()
{
    QMetaObject::invokeMethod(m_pipeline, "reloadLogs", Qt::QueuedConnection, Q_ARG(int, 50));
}

void MainWindow::on_btnOpenLocalImage_clicked()
{
    const QString fileName = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开本地图片"), QDir::homePath(),
        QStringLiteral("图像文件 (*.bmp *.jpg *.jpeg *.png *.tif *.tiff)"));
    if (fileName.isEmpty()) return;

    const QImage image(fileName);
    if (image.isNull()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("无法打开所选图片"));
        return;
    }
    if (m_isGrabbing) {
        if (m_usingLocalCamera) m_localCamera->stopGrabbing();
        else m_cameraManager->stopGrabbing();
    }

    m_showingLocalImage = true;
    m_rawImage = image.convertToFormat(QImage::Format_RGB888);
    m_displayImage = m_rawImage;
    updateDisplayedPixmap();
    showStatus(QStringLiteral("已加载本地图片，可点击“注册人脸”进行注册"), 4000);

    // 立即对本地图片跑一次识别，便于无相机时验证
    QMetaObject::invokeMethod(m_pipeline, "processFrame", Qt::QueuedConnection,
                              Q_ARG(QImage, m_rawImage));
}

// ---------------- 显示与事件 ----------------
void MainWindow::updateDisplayedPixmap()
{
    if (m_displayImage.isNull()) {
        ui->labelDisplay->clear();
        return;
    }
    QPixmap pixmap = QPixmap::fromImage(m_displayImage);
    const QSize size = ui->labelDisplay->contentsRect().size();
    if (size.width() > 0 && size.height() > 0)
        pixmap = pixmap.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    ui->labelDisplay->setPixmap(pixmap);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->labelDisplay && event->type() == QEvent::Resize)
        updateDisplayedPixmap();
    return QMainWindow::eventFilter(watched, event);
}

// ---------------- 保存 ----------------
void MainWindow::on_btnSaveJPG_clicked()  { saveImage(); }

void MainWindow::saveImage()
{
    if (m_displayImage.isNull()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("没有图像可保存"));
        return;
    }
    QString fileName = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存图像"),
        QDir::homePath() + QStringLiteral("/image.jpg"),
        QStringLiteral("JPEG 文件 (*.jpg *.jpeg)"));
    if (fileName.isEmpty()) return;
    if (QFileInfo(fileName).suffix().isEmpty()) fileName += QStringLiteral(".jpg");

    if (!m_displayImage.save(fileName, "JPG"))
        QMessageBox::critical(this, QStringLiteral("错误"),
                              QStringLiteral("保存失败: 图像无法写入"));
    else
        showStatus(QStringLiteral("图像已保存: ") + fileName, 3000);
}

void MainWindow::on_btnSaveVideo_clicked()
{
    if (m_videoRecording) { stopVideoRecording(); return; }
    if (!m_isGrabbing || m_rawImage.isNull()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先开始采集"));
        return;
    }
    if (!m_usingLocalCamera && m_cameraManager->getTriggerMode()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("录像需切换到连续采集模式"));
        return;
    }
    QString selectedFilter;
    QString file = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存视频"), QDir::homePath() + QStringLiteral("/video.avi"),
        QStringLiteral("AVI 视频 (*.avi);;MP4 视频 (*.mp4)"), &selectedFilter);
    if (file.isEmpty()) return;
    if (QFileInfo(file).suffix().isEmpty())
        file += selectedFilter.contains(QStringLiteral("mp4")) ? QStringLiteral(".mp4") : QStringLiteral(".avi");
    const QString suffix = QFileInfo(file).suffix().toLower();
    if (suffix != QStringLiteral("avi") && suffix != QStringLiteral("mp4")) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("扩展名必须为 .avi 或 .mp4"));
        return;
    }
    double fps = 0.0;
    if (m_usingLocalCamera) {
        fps = m_localCamera->frameRate();
    } else {
        fps = m_cameraManager->getResultingFrameRate();
        if (fps <= 0) fps = m_cameraManager->getFrameRate();
    }
    if (fps <= 0) fps = 25.0;
    if (!m_videoRecorder->startRecording(file, m_rawImage.size(), fps)) {
        showError(QStringLiteral("无法开始录像"));
        return;
    }
    m_videoRecording = true;
    updateUIState();
    showStatus(QStringLiteral("开始录像，编码帧率 %1 fps").arg(fps, 0, 'f', 2), 5000);
}

void MainWindow::stopVideoRecording()
{
    if (!m_videoRecording && !m_videoRecorder->isRunning()) return;
    showStatus(QStringLiteral("正在结束录像..."), 3000);
    m_videoRecorder->stopRecording();
    m_videoRecording = false;
    updateUIState();
}

// ---------------- 参数 ----------------
void MainWindow::on_btnGetParams_clicked()
{
    updateParametersDisplay();
}

void MainWindow::on_btnSetParams_clicked()
{
    if (m_usingLocalCamera) {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("本地摄像头不支持曝光/增益/像素格式等相机参数"));
        return;
    }
    if (!m_cameraManager->isConnected()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先连接相机"));
        return;
    }
    if (m_videoRecording) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先停止录像再改参数"));
        return;
    }

    // 先快照所有输入，避免 SDK 回读信号覆盖用户输入
    const float exposure = static_cast<float>(ui->spinExposure->value());
    const float gain = static_cast<float>(ui->spinGain->value());
    const float fps = static_cast<float>(ui->spinFrameRate->value());
    const qint64 scpd = ui->spinSCPD->value();
    const QVariant pixelData = ui->comboPixelFormat->currentData();
    const bool trigger = ui->radioTrigger->isChecked();
    const bool wasGrabbing = m_isGrabbing;

    QStringList failed;
    if (wasGrabbing) m_cameraManager->stopGrabbing();

    if (!m_cameraManager->setTriggerMode(trigger)) failed << QStringLiteral("触发模式");
    if (pixelData.isValid() && pixelData.toInt() != 0 &&
        !m_cameraManager->setPixelFormat(pixelData.toInt())) failed << QStringLiteral("像素格式");
    if (!m_cameraManager->setExposureTime(exposure)) failed << QStringLiteral("曝光");
    if (!m_cameraManager->setGain(gain)) failed << QStringLiteral("增益");
    if (!m_cameraManager->setFrameRate(fps)) failed << QStringLiteral("帧率");
    if (ui->spinSCPD->isEnabled() && !m_cameraManager->setSCPD(scpd)) failed << QStringLiteral("SCPD");

    if (wasGrabbing) m_cameraManager->startGrabbing();

    updateParameterRanges();
    updateParametersDisplay();
    if (failed.isEmpty()) showStatus(QStringLiteral("参数设置完成"), 5000);
    else showStatus(QStringLiteral("以下设置失败: ") + failed.join(QStringLiteral("、")), 8000);
}

void MainWindow::updateParameterRanges()
{
    if (m_usingLocalCamera) {
        // 本地摄像头没有这些工业相机参数，只把不适用的控件关掉
        ui->spinSCPD->setEnabled(false);
        return;
    }
    float min = 0, max = 0;
    if (m_cameraManager->getExposureRange(min, max)) ui->spinExposure->setRange(min, max);
    if (m_cameraManager->getGainRange(min, max)) ui->spinGain->setRange(min, max);
    if (m_cameraManager->getFrameRateRange(min, max)) ui->spinFrameRate->setRange(min, max);

    qint64 sMin = 0, sMax = 0, sInc = 1;
    if (m_cameraManager->getSCPDRange(sMin, sMax, sInc)) {
        ui->spinSCPD->setRange(static_cast<int>(qMax<qint64>(0, sMin)),
                               static_cast<int>(qMin<qint64>(2147483647, sMax)));
        ui->spinSCPD->setSingleStep(static_cast<int>(qMax<qint64>(1, sInc)));
        ui->spinSCPD->setEnabled(true);
    } else {
        ui->spinSCPD->setEnabled(false);
    }
}

void MainWindow::updateParametersDisplay()
{
    if (m_usingLocalCamera) return;
    if (!m_cameraManager->isConnected()) return;
    const float exposure = m_cameraManager->getExposureTime();
    const float gain = m_cameraManager->getGain();
    const float fps = m_cameraManager->getFrameRate();
    const qint64 scpd = m_cameraManager->getSCPD();
    if (exposure >= 0) ui->spinExposure->setValue(exposure);
    if (gain >= 0) ui->spinGain->setValue(gain);
    if (fps >= 0) ui->spinFrameRate->setValue(fps);
    if (scpd >= 0 && ui->spinSCPD->isEnabled()) ui->spinSCPD->setValue(static_cast<int>(scpd));

    const int fmt = m_cameraManager->getPixelFormat();
    const int idx = ui->comboPixelFormat->findData(QVariant(fmt));
    ui->comboPixelFormat->setCurrentIndex(idx != -1 ? idx : 0);

    const bool triggerMode = m_cameraManager->getTriggerMode();
    ui->radioTrigger->setChecked(triggerMode);
    ui->radioContinuous->setChecked(!triggerMode);
}

void MainWindow::updatePixelFormats(const QVector<QPair<QString, int>> &formats)
{
    ui->comboPixelFormat->clear();
    for (const auto &pair : formats)
        ui->comboPixelFormat->addItem(pair.first, QVariant(pair.second));
    const int current = m_cameraManager->getPixelFormat();
    const int idx = ui->comboPixelFormat->findData(QVariant(current));
    if (idx != -1) ui->comboPixelFormat->setCurrentIndex(idx);
}
