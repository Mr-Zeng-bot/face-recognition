#ifndef CAMERAMANAGER_H
#define CAMERAMANAGER_H
#include <QObject>
#include <QString>
#include <QPair>
#include <QVector>
#include <QImage>
#include <QMutex>
#include <QtGlobal>
#include "MvCameraControl.h"
// 相机信息结构体
struct CameraInfo {
    QString serialNumber;
    QString modelName;
    QString ipAddress;
    int index;
};
class CameraManager : public QObject
{
    Q_OBJECT
public:
    explicit CameraManager(QObject *parent = nullptr);
    ~CameraManager();
    // 获取当前连接状态
    bool isConnected() const { return m_isConnected; }
    bool isGrabbing() const { return m_isGrabbing; }
public slots:
    // 枚举设备
    void enumerateDevices();
    // 连接相机 (根据索引)
    void connectCamera(int index);
    // 断开相机
    void disconnectCamera();
    // 开始采集
    void startGrabbing();
    // 停止采集
    void stopGrabbing();
    // 软触发一次 (触发模式)
    void softTrigger();
    // -------- 参数设置 --------
    // 返回值: true=设置成功, false=设置失败（内部会先把值钳制到相机允许范围）
    bool setExposureTime(float value);
    bool setGain(float value);
    bool setFrameRate(float value);
    bool setSCPD(qint64 value);
    bool setPixelFormat(int format);
    bool setTriggerMode(bool on);  // true=触发模式, false=连续模式
    // -------- 参数获取 --------
    float getExposureTime();
    float getGain();
    float getFrameRate();
    qint64 getSCPD();
    float getResultingFrameRate();
    int getPixelFormat();
    bool getTriggerMode();
    // -------- 参数范围获取（用于连接后同步UI控件范围） --------
    bool getExposureRange(float& min, float& max);
    bool getGainRange(float& min, float& max);
    bool getFrameRateRange(float& min, float& max);
    bool getSCPDRange(qint64& min, qint64& max, qint64& increment);
    // 获取像素格式列表（返回预置列表，实际使用时建议在连接后动态获取）
    QVector<QPair<QString, int>> getSupportedPixelFormats();
signals:
    // 设备枚举完成信号
    void devicesEnumerated(const QVector<CameraInfo>& deviceList);
    // 相机连接成功
    void cameraConnected(const QString& message);
    // 相机断开
    void cameraDisconnected(const QString& message);
    // 采集开始
    void grabbingStarted();
    // 采集停止
    void grabbingStopped();
    // 新图像到达 (用于显示)
    void imageReady(const QImage& image);
    // 错误信息
    void errorOccurred(const QString& error);
    // 参数更新完成
    void parametersUpdated();
    // 像素格式列表准备好
    void pixelFormatsReady(const QVector<QPair<QString, int>>& formats);
    // SDK 回调实际收到的帧率（不是相机节点的理论值）
private:
    // 静态回调函数 (SDK要求静态)
    static void __stdcall imageCallback(unsigned char* pData, MV_FRAME_OUT_INFO_EX* pFrameInfo, void* pUser);
    // 处理图像数据 (在回调中调用)
    void processImage(unsigned char* pData, MV_FRAME_OUT_INFO_EX* pFrameInfo);
    // 获取像素格式列表的内部实现
    QVector<QPair<QString, int>> fetchPixelFormatsInternal();
    // GigE 相机连接后按海康官方示例配置最佳网包大小
    void configureGigETransport(unsigned int transportLayerType);
private:
    void* m_deviceHandle;           // 设备句柄
    bool m_isConnected;             // 连接状态
    bool m_isGrabbing;              // 采集状态
    QVector<CameraInfo> m_deviceList; // 设备列表
    QMutex m_mutex;                 // 互斥锁
};
#endif // CAMERAMANAGER_H


