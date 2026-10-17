#include "cameraManager.h"
#include <QDebug>
#include <QThread>
CameraManager::CameraManager(QObject *parent)
    : QObject(parent)
    , m_deviceHandle(nullptr)
    , m_isConnected(false)
    , m_isGrabbing(false)
{
    // 初始化SDK
    int nRet = MV_CC_Initialize();
    if (nRet != MV_OK) {
        emit errorOccurred(QString("SDK初始化失败，错误码: %1").arg(nRet));
    } else {
        qDebug() << "MVS SDK初始化成功";
    }
}
CameraManager::~CameraManager()
{
    // 停止采集
    if (m_isGrabbing) {
        stopGrabbing();
    }
    // 断开连接
    if (m_isConnected) {
        disconnectCamera();
    }
    // 反初始化SDK
    MV_CC_Finalize();
    qDebug() << "MVS SDK反初始化完成";
}
// ---------- 设备枚举 ----------
void CameraManager::enumerateDevices()
{
    m_deviceList.clear();
    MV_CC_DEVICE_INFO_LIST stDeviceList;
    memset(&stDeviceList, 0, sizeof(MV_CC_DEVICE_INFO_LIST));
    int nRet = MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &stDeviceList);
    if (nRet != MV_OK) {
        emit errorOccurred(QString("枚举设备失败，错误码: %1").arg(nRet));
        emit devicesEnumerated(m_deviceList);
        return;
    }
    if (stDeviceList.nDeviceNum == 0) {
        emit errorOccurred("未检测到任何相机设备");
        emit devicesEnumerated(m_deviceList);
        return;
    }
    for (unsigned int i = 0; i < stDeviceList.nDeviceNum; i++) {
        MV_CC_DEVICE_INFO* pDeviceInfo = stDeviceList.pDeviceInfo[i];
        CameraInfo info;
        info.index = i;
        if (pDeviceInfo->nTLayerType == MV_GIGE_DEVICE) {
            info.serialNumber = QString::fromLatin1(
                reinterpret_cast<const char*>(pDeviceInfo->SpecialInfo.stGigEInfo.chSerialNumber)
                );
            info.modelName = QString::fromLatin1(
                reinterpret_cast<const char*>(pDeviceInfo->SpecialInfo.stGigEInfo.chModelName)
                );
            unsigned int ip = pDeviceInfo->SpecialInfo.stGigEInfo.nCurrentIp;
            info.ipAddress = QString("%1.%2.%3.%4")
                                 .arg((ip >> 24) & 0xFF)
                                 .arg((ip >> 16) & 0xFF)
                                 .arg((ip >> 8) & 0xFF)
                                 .arg(ip & 0xFF);
        } else if (pDeviceInfo->nTLayerType == MV_USB_DEVICE) {
            info.serialNumber = QString::fromLatin1(
                reinterpret_cast<const char*>(pDeviceInfo->SpecialInfo.stUsb3VInfo.chSerialNumber)
                );
            info.modelName = QString::fromLatin1(
                reinterpret_cast<const char*>(pDeviceInfo->SpecialInfo.stUsb3VInfo.chModelName)
                );
            info.ipAddress = "USB";
        }
        m_deviceList.append(info);
        qDebug() << "发现设备:" << info.modelName << "序列号:" << info.serialNumber;
    }
    emit devicesEnumerated(m_deviceList);
}
// ---------- 连接相机 ----------
void CameraManager::connectCamera(int index)
{
    if (index < 0 || index >= m_deviceList.size()) {
        emit errorOccurred("无效的设备索引");
        return;
    }
    if (m_isConnected) {
        disconnectCamera();
    }
    // 重新枚举获取最新设备列表
    MV_CC_DEVICE_INFO_LIST stDeviceList;
    memset(&stDeviceList, 0, sizeof(MV_CC_DEVICE_INFO_LIST));
    int nRet = MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &stDeviceList);
    if (nRet != MV_OK || static_cast<unsigned int>(index) >= stDeviceList.nDeviceNum) {
        emit errorOccurred("枚举设备失败，无法连接");
        return;
    }
    nRet = MV_CC_CreateHandle(&m_deviceHandle, stDeviceList.pDeviceInfo[index]);
    if (nRet != MV_OK) {
        emit errorOccurred(QString("创建设备句柄失败，错误码: %1").arg(nRet));
        m_deviceHandle = nullptr;
        return;
    }
    nRet = MV_CC_OpenDevice(m_deviceHandle);
    if (nRet != MV_OK) {
        emit errorOccurred(QString("打开设备失败，错误码: %1").arg(nRet));
        MV_CC_DestroyHandle(m_deviceHandle);
        m_deviceHandle = nullptr;
        return;
    }
    m_isConnected = true;
    configureGigETransport(stDeviceList.pDeviceInfo[index]->nTLayerType);
    QString msg = QString("成功连接到相机: %1").arg(m_deviceList[index].modelName);
    // Notify the UI only AFTER configuring default parameter modes.
    qDebug() << msg;
    // 设置默认触发模式为连续模式
    nRet = MV_CC_SetEnumValue(m_deviceHandle, "TriggerMode", 0);
    qDebug() << "设置默认连续模式，结果:" << (nRet == MV_OK ? "成功" : QString("失败 0x%1").arg(nRet, 0, 16));
    // 关闭自动增益
    nRet = MV_CC_SetEnumValueByString(m_deviceHandle, "GainAuto", "Off");
    if (nRet != MV_OK) nRet = MV_CC_SetEnumValue(m_deviceHandle, "GainAuto", 0);
    qDebug() << "关闭自动增益，结果:" << (nRet == MV_OK ? "成功" : QString("失败 0x%1").arg(nRet, 0, 16));
    // 使能帧率控制
    nRet = MV_CC_SetBoolValue(m_deviceHandle, "AcquisitionFrameRateEnable", true);
    qDebug() << "使能帧率控制，结果:" << (nRet == MV_OK ? "成功" : QString("失败 0x%1").arg(nRet, 0, 16));
    emit cameraConnected(msg);
    // 获取像素格式列表并发送
    QVector<QPair<QString, int>> formats = getSupportedPixelFormats();
    emit pixelFormatsReady(formats);
}
// ---------- GigE 传输优化 ----------
void CameraManager::configureGigETransport(unsigned int transportLayerType)
{
    if (transportLayerType != MV_GIGE_DEVICE || m_deviceHandle == nullptr) return;

    // 海康官方 BasicDemo 在打开 GigE 相机后执行同样的步骤。
    // 未设置最佳包大小时，大分辨率图像可能被大量分包，吞吐量会明显下降。
    const int packetSize = MV_CC_GetOptimalPacketSize(m_deviceHandle);
    if (packetSize <= 0) {
        qWarning() << "探测 GigE 最佳包大小失败:" << packetSize;
        return;
    }

    const int result = MV_CC_SetIntValueEx(m_deviceHandle, "GevSCPSPacketSize", packetSize);
    if (result == MV_OK) {
        qDebug() << "GigE 最佳包大小已设置:" << packetSize;
    } else {
        // 部分网卡未启用巨型帧时，最优值可能不可写；保留相机原值继续运行。
        qWarning() << "设置 GigE 包大小失败，错误码:"
                   << QString("0x%1").arg(static_cast<unsigned int>(result), 8, 16, QLatin1Char('0'));
    }

    // SCPD 由用户通过参数面板设置。连接和重新连接时只读取当前值，
    // 避免传输初始化覆盖用户在本程序或 MVS 中设置的数据包间隔。
    MVCC_INTVALUE_EX packetDelay = {};
    const int delayReadResult = MV_CC_GetIntValueEx(m_deviceHandle, "GevSCPD", &packetDelay);
    if (delayReadResult == MV_OK) {
        qDebug() << "GigE 包间隔 GevSCPD 当前值:" << packetDelay.nCurValue
                 << "（保留当前设置）"
                 << "范围:" << packetDelay.nMin << "-" << packetDelay.nMax;
    } else {
        qWarning() << "读取 GigE 包间隔失败，错误码:"
                   << QString("0x%1").arg(static_cast<unsigned int>(delayReadResult),
                                           8, 16, QLatin1Char('0'));
    }
}

// ---------- 断开相机 ----------
void CameraManager::disconnectCamera()
{
    if (m_deviceHandle != nullptr) {
        if (m_isGrabbing) {
            stopGrabbing();
        }
        MV_CC_CloseDevice(m_deviceHandle);
        MV_CC_DestroyHandle(m_deviceHandle);
        m_deviceHandle = nullptr;
        m_isConnected = false;
        emit cameraDisconnected("相机已断开连接");
        qDebug() << "相机已断开";
    }
}
// ---------- 开始采集 ----------
void CameraManager::startGrabbing()
{
    if (!m_isConnected || m_deviceHandle == nullptr) {
        emit errorOccurred("相机未连接，无法开始采集");
        return;
    }
    if (m_isGrabbing) {
        emit errorOccurred("采集已在进行中");
        return;
    }
    // 注册图像回调函数
    int nRet = MV_CC_RegisterImageCallBackEx(m_deviceHandle, imageCallback, this);
    if (nRet != MV_OK) {
        emit errorOccurred(QString("注册图像回调失败，错误码: %1").arg(nRet));
        return;
    }
    nRet = MV_CC_StartGrabbing(m_deviceHandle);
    if (nRet != MV_OK) {
        emit errorOccurred(QString("开始采集失败，错误码: %1").arg(nRet));
        return;
    }
    m_isGrabbing = true;
    emit grabbingStarted();
    qDebug() << "开始采集";
}
// ---------- 停止采集 ----------
void CameraManager::stopGrabbing()
{
    if (!m_isGrabbing || m_deviceHandle == nullptr) {
        return;
    }
    const int result = MV_CC_StopGrabbing(m_deviceHandle);
    if (result != MV_OK) {
        emit errorOccurred(QString("停止采集失败: 0x%1").arg(static_cast<unsigned int>(result), 8, 16, QLatin1Char('0')));
        return;
    }
    m_isGrabbing = false;
    emit grabbingStopped();
    qDebug() << "停止采集";
}
// ---------- 软触发 ----------
void CameraManager::softTrigger()
{
    if (!m_isConnected || m_deviceHandle == nullptr) {
        emit errorOccurred("相机未连接");
        return;
    }
    // 软触发前必须确认：触发模式=On 且触发源=Software，否则 TriggerSoftware 节点不可访问(0x80000106)
    MVCC_ENUMVALUE stMode = {0};
    if (MV_CC_GetEnumValue(m_deviceHandle, "TriggerMode", &stMode) != MV_OK || stMode.nCurValue != 1) {
        emit errorOccurred("软触发失败：当前不是触发模式，请先选择「触发模式」并点「参数设置」");
        return;
    }
    MVCC_ENUMVALUE stSource = {0};
    if (MV_CC_GetEnumValue(m_deviceHandle, "TriggerSource", &stSource) == MV_OK) {
        qDebug() << "当前触发源枚举值:" << stSource.nCurValue;
    }
    int nRet = MV_CC_SetCommandValue(m_deviceHandle, "TriggerSoftware");
    if (nRet != MV_OK) {
        emit errorOccurred(QString("软触发失败，错误码: %1(0x%2)，请确认已选触发模式并点过「参数设置」")
                               .arg(nRet).arg(nRet, 0, 16));
    } else {
        qDebug() << "软触发成功";
    }
}
// ---------- 静态回调函数 ----------
void __stdcall CameraManager::imageCallback(unsigned char* pData, MV_FRAME_OUT_INFO_EX* pFrameInfo, void* pUser)
{
    CameraManager* pManager = static_cast<CameraManager*>(pUser);
    if (pManager) {
        pManager->processImage(pData, pFrameInfo);
    }
}
// ---------- 处理图像数据 ----------
void CameraManager::processImage(unsigned char* pData, MV_FRAME_OUT_INFO_EX* pFrameInfo)
{
    if (pData == nullptr || pFrameInfo == nullptr) {
        return;
    }
    QMutexLocker locker(&m_mutex);
    int width = pFrameInfo->nWidth;
    int height = pFrameInfo->nHeight;
    int pixelFormat = pFrameInfo->enPixelType;
    QImage image;
    // 判断像素格式并转换为QImage
    if (pixelFormat == PixelType_Gvsp_Mono8) {
        image = QImage(pData, width, height, width, QImage::Format_Grayscale8);
    } else if (pixelFormat == PixelType_Gvsp_RGB8_Packed) {
        image = QImage(pData, width, height, width * 3, QImage::Format_RGB888);
    } else {
        QByteArray rgb(width * height * 3, Qt::Uninitialized);
        MV_CC_PIXEL_CONVERT_PARAM_EX conversion = {};
        conversion.nWidth = width;
        conversion.nHeight = height;
        conversion.enSrcPixelType = pFrameInfo->enPixelType;
        conversion.pSrcData = pData;
        conversion.nSrcDataLen = pFrameInfo->nFrameLen;
        conversion.enDstPixelType = PixelType_Gvsp_RGB8_Packed;
        conversion.pDstBuffer = reinterpret_cast<unsigned char *>(rgb.data());
        conversion.nDstBufferSize = static_cast<unsigned int>(rgb.size());
        const int result = MV_CC_ConvertPixelTypeEx(m_deviceHandle, &conversion);
        if (result != MV_OK) {
            qWarning() << "Pixel conversion failed:" << Qt::hex << result;
            return;
        }
        image = QImage(reinterpret_cast<const uchar *>(rgb.constData()), width, height,
                       width * 3, QImage::Format_RGB888).copy();
    }
    if (!image.isNull()) {
        emit imageReady(image.copy());
    }
}
// ---------- 参数设置 ----------
bool CameraManager::setExposureTime(float value)
{
    if (!m_isConnected || m_deviceHandle == nullptr) {
        emit errorOccurred("相机未连接");
        return false;
    }
    // 关闭自动曝光（必须成功，否则 ExposureTime 节点不可写，会报 0x80000106）
    int nRetAuto = MV_CC_SetEnumValueByString(m_deviceHandle, "ExposureAuto", "Off");
    if (nRetAuto != MV_OK) {
        nRetAuto = MV_CC_SetEnumValue(m_deviceHandle, "ExposureAuto", 0);
    }
    if (nRetAuto != MV_OK) {
        emit errorOccurred(QString("关闭自动曝光失败，曝光节点不可写，错误码: %1(0x%2)")
                               .arg(nRetAuto).arg(nRetAuto, 0, 16));
        return false;
    }
    // 获取曝光范围并钳制到相机允许范围，防止超范围导致设置失败
    MVCC_FLOATVALUE stRange = {0};
    int nRet = MV_CC_GetFloatValue(m_deviceHandle, "ExposureTime", &stRange);
    if (nRet == MV_OK) {
        if (value < stRange.fMin) value = stRange.fMin;
        if (value > stRange.fMax) value = stRange.fMax;
        qDebug() << "曝光范围:" << stRange.fMin << "-" << stRange.fMax << "实际设置:" << value;
    }
    nRet = MV_CC_SetFloatValue(m_deviceHandle, "ExposureTime", value);
    if (nRet != MV_OK) {
        emit errorOccurred(QString("设置曝光时间失败，错误码: %1").arg(nRet));
        return false;
    }
    emit parametersUpdated();
    return true;
}
bool CameraManager::setGain(float value)
{
    if (!m_isConnected || m_deviceHandle == nullptr) {
        emit errorOccurred("相机未连接");
        return false;
    }
    // 关闭自动增益（必须成功，否则 Gain 节点不可写，会报 0x80000106）
    int nRetAuto = MV_CC_SetEnumValueByString(m_deviceHandle, "GainAuto", "Off");
    if (nRetAuto != MV_OK) {
        nRetAuto = MV_CC_SetEnumValue(m_deviceHandle, "GainAuto", 0);
    }
    if (nRetAuto != MV_OK) {
        emit errorOccurred(QString("关闭自动增益失败，增益节点不可写，错误码: %1(0x%2)")
                               .arg(nRetAuto).arg(nRetAuto, 0, 16));
        return false;
    }
    // 获取范围并钳制到相机允许范围
    MVCC_FLOATVALUE stRange = {0};
    int nRet = MV_CC_GetFloatValue(m_deviceHandle, "Gain", &stRange);
    if (nRet == MV_OK) {
        if (value < stRange.fMin) value = stRange.fMin;
        if (value > stRange.fMax) value = stRange.fMax;
        qDebug() << "增益范围:" << stRange.fMin << "-" << stRange.fMax << "实际设置:" << value;
    } else {
        qDebug() << "读取增益范围失败，错误码:" << nRet;
    }
    nRet = MV_CC_SetFloatValue(m_deviceHandle, "Gain", value);
    if (nRet != MV_OK) {
        emit errorOccurred(QString("设置增益失败，错误码: %1").arg(nRet));
        return false;
    }
    emit parametersUpdated();
    return true;
}
bool CameraManager::setFrameRate(float value)
{
    if (!m_isConnected || m_deviceHandle == nullptr) {
        emit errorOccurred("相机未连接");
        return false;
    }
    // 使能帧率控制（必须成功，否则 AcquisitionFrameRate 节点不可写，会报 0x80000106）
    // AcquisitionFrameRateEnable is a BOOLEAN GenICam node, not an enum.
    bool enabled = false;
    const int readEnable = MV_CC_GetBoolValue(m_deviceHandle, "AcquisitionFrameRateEnable", &enabled);
    if (readEnable == MV_OK && !enabled) {
        const int result = MV_CC_SetBoolValue(m_deviceHandle, "AcquisitionFrameRateEnable", true);
        if (result != MV_OK) {
            emit errorOccurred(QString("使能帧率控制失败: 0x%1")
                .arg(static_cast<unsigned int>(result), 8, 16, QLatin1Char('0')));
            return false;
        }
    } else if (readEnable != MV_OK) {
        // Some models expose AcquisitionFrameRate without an enable node.
        qDebug() << "No readable AcquisitionFrameRateEnable node; try frame rate directly";
    }    // 获取帧率范围并钳制到相机允许范围
    MVCC_FLOATVALUE stRange = {0};
    int nRet = MV_CC_GetFloatValue(m_deviceHandle, "AcquisitionFrameRate", &stRange);
    if (nRet == MV_OK) {
        if (value < stRange.fMin) value = stRange.fMin;
        if (value > stRange.fMax) value = stRange.fMax;
        qDebug() << "帧率范围:" << stRange.fMin << "-" << stRange.fMax << "实际设置:" << value;
    } else {
        qDebug() << "读取帧率范围失败，错误码:" << nRet;
    }
    nRet = MV_CC_SetFloatValue(m_deviceHandle, "AcquisitionFrameRate", value);
    if (nRet != MV_OK) {
        // 帧率设置受带宽/节点支持影响，可能失败；作为单个参数失败提示
        emit errorOccurred(QString("设置帧率失败（可能相机不支持或带宽不足），错误码: %1(0x%2)")
                               .arg(nRet).arg(nRet, 0, 16));
        return false;
    }
    emit parametersUpdated();
    return true;
}
bool CameraManager::setSCPD(qint64 value)
{
    if (!m_isConnected || m_deviceHandle == nullptr) {
        emit errorOccurred("相机未连接");
        return false;
    }

    MVCC_INTVALUE_EX range = {};
    const int readResult =
        MV_CC_GetIntValueEx(m_deviceHandle, "GevSCPD", &range);
    if (readResult != MV_OK) {
        emit errorOccurred(QString("读取 SCPD 范围失败，错误码: %1(0x%2)")
            .arg(readResult)
            .arg(readResult, 0, 16));
        return false;
    }

    value = qBound<qint64>(
        static_cast<qint64>(range.nMin),
        value,
        static_cast<qint64>(range.nMax));
    if (range.nInc > 1 && value > range.nMin) {
        value = static_cast<qint64>(range.nMin)
              + ((value - static_cast<qint64>(range.nMin))
                 / static_cast<qint64>(range.nInc))
                * static_cast<qint64>(range.nInc);
    }

    const int result = MV_CC_SetIntValueEx(
        m_deviceHandle, "GevSCPD", static_cast<int64_t>(value));
    if (result != MV_OK) {
        emit errorOccurred(QString("设置 SCPD 失败，错误码: %1(0x%2)")
            .arg(result)
            .arg(result, 0, 16));
        return false;
    }

    emit parametersUpdated();
    return true;
}
bool CameraManager::setPixelFormat(int format)
{
    if (!m_isConnected || m_deviceHandle == nullptr) {
        emit errorOccurred("相机未连接");
        return false;
    }
    int nRet = MV_CC_SetEnumValue(m_deviceHandle, "PixelFormat", format);
    if (nRet != MV_OK) {
        emit errorOccurred(QString("设置像素格式失败，错误码: %1(0x%2)").arg(nRet).arg(nRet,0,16));
        return false;
    }
    emit parametersUpdated();
    return true;
}
bool CameraManager::setTriggerMode(bool on)
{
    if (!m_isConnected || m_deviceHandle == nullptr) {
        emit errorOccurred("相机未连接");
        return false;
    }
    int nRet;
    if (on) {
        // 触发模式必须同时把触发源设为 Software，否则"软触发"命令不生效。
        // 不同相机 Software 的枚举值不同(常见0或7)，用字符串接口最可靠。
        nRet = MV_CC_SetEnumValueByString(m_deviceHandle, "TriggerSource", "Software");
        if (nRet != MV_OK) {
            // 兼容不支持字符串接口的旧 SDK
            nRet = MV_CC_SetEnumValue(m_deviceHandle, "TriggerSource", 7); // MV_TRIGGER_SOURCE_SOFTWARE
        }
        if (nRet != MV_OK) {
            qDebug() << "设置触发源(TriggerSource=Software)失败，错误码:" << nRet;
        }
        nRet = MV_CC_SetEnumValueByString(m_deviceHandle, "TriggerMode", "On");
        if (nRet != MV_OK) {
            nRet = MV_CC_SetEnumValue(m_deviceHandle, "TriggerMode", 1);
        }
    } else {
        nRet = MV_CC_SetEnumValueByString(m_deviceHandle, "TriggerMode", "Off");
        if (nRet != MV_OK) {
            nRet = MV_CC_SetEnumValue(m_deviceHandle, "TriggerMode", 0);
        }
    }
    if (nRet != MV_OK) {
        emit errorOccurred(QString("设置触发模式失败，错误码: %1(0x%2)").arg(nRet).arg(nRet, 0, 16));
        return false;
    }
    emit parametersUpdated();
    return true;
}
// ---------- 参数获取 ----------
float CameraManager::getExposureTime()
{
    if (!m_isConnected || m_deviceHandle == nullptr) return -1;
    MVCC_FLOATVALUE stFloatValue;
    memset(&stFloatValue, 0, sizeof(MVCC_FLOATVALUE));
    int nRet = MV_CC_GetFloatValue(m_deviceHandle, "ExposureTime", &stFloatValue);
    if (nRet == MV_OK) {
        return stFloatValue.fCurValue;
    }
    return -1;
}
float CameraManager::getGain()
{
    if (!m_isConnected || m_deviceHandle == nullptr) return -1;
    MVCC_FLOATVALUE stFloatValue;
    memset(&stFloatValue, 0, sizeof(MVCC_FLOATVALUE));
    int nRet = MV_CC_GetFloatValue(m_deviceHandle, "Gain", &stFloatValue);
    if (nRet == MV_OK) {
        return stFloatValue.fCurValue;
    }
    return -1;
}
float CameraManager::getFrameRate()
{
    if (!m_isConnected || m_deviceHandle == nullptr) return -1;
    MVCC_FLOATVALUE stFloatValue;
    memset(&stFloatValue, 0, sizeof(MVCC_FLOATVALUE));
    int nRet = MV_CC_GetFloatValue(m_deviceHandle, "AcquisitionFrameRate", &stFloatValue);
    if (nRet == MV_OK) {
        return stFloatValue.fCurValue;
    }
    return -1;
}
qint64 CameraManager::getSCPD()
{
    if (!m_isConnected || m_deviceHandle == nullptr) return -1;

    MVCC_INTVALUE_EX value = {};
    if (MV_CC_GetIntValueEx(
            m_deviceHandle, "GevSCPD", &value) != MV_OK) {
        return -1;
    }
    return static_cast<qint64>(value.nCurValue);
}
float CameraManager::getResultingFrameRate()
{
    if (!m_isConnected || !m_deviceHandle) return -1;
    MVCC_FLOATVALUE value = {};
    if (MV_CC_GetFloatValue(m_deviceHandle, "ResultingFrameRate", &value) == MV_OK)
        return value.fCurValue;
    if (MV_CC_GetFloatValue(m_deviceHandle, "ResultingFrameRateAbs", &value) == MV_OK)
        return value.fCurValue;
    return -1;
}
int CameraManager::getPixelFormat()
{
    if (!m_isConnected || m_deviceHandle == nullptr) return -1;
    MVCC_ENUMVALUE stEnumValue;
    memset(&stEnumValue, 0, sizeof(MVCC_ENUMVALUE));
    int nRet = MV_CC_GetEnumValue(m_deviceHandle, "PixelFormat", &stEnumValue);
    if (nRet == MV_OK) {
        return stEnumValue.nCurValue;
    }
    return -1;
}
bool CameraManager::getTriggerMode()
{
    if (!m_isConnected || m_deviceHandle == nullptr) return false;
    MVCC_ENUMVALUE stEnumValue;
    memset(&stEnumValue, 0, sizeof(MVCC_ENUMVALUE));
    int nRet = MV_CC_GetEnumValue(m_deviceHandle, "TriggerMode", &stEnumValue);
    if (nRet == MV_OK) {
        return stEnumValue.nCurValue == 1;
    }
    return false;
}
// ---------- 参数范围获取 ----------
bool CameraManager::getExposureRange(float& min, float& max)
{
    if (!m_isConnected || m_deviceHandle == nullptr) return false;
    MVCC_FLOATVALUE stRange = {0};
    if (MV_CC_GetFloatValue(m_deviceHandle, "ExposureTime", &stRange) != MV_OK) return false;
    min = stRange.fMin;
    max = stRange.fMax;
    return true;
}
bool CameraManager::getGainRange(float& min, float& max)
{
    if (!m_isConnected || m_deviceHandle == nullptr) return false;
    MVCC_FLOATVALUE stRange = {0};
    if (MV_CC_GetFloatValue(m_deviceHandle, "Gain", &stRange) != MV_OK) return false;
    min = stRange.fMin;
    max = stRange.fMax;
    return true;
}
bool CameraManager::getFrameRateRange(float& min, float& max)
{
    if (!m_isConnected || m_deviceHandle == nullptr) return false;
    MVCC_FLOATVALUE stRange = {0};
    if (MV_CC_GetFloatValue(m_deviceHandle, "AcquisitionFrameRate", &stRange) != MV_OK) return false;
    min = stRange.fMin;
    max = stRange.fMax;
    return true;
}
bool CameraManager::getSCPDRange(
    qint64& min, qint64& max, qint64& increment)
{
    if (!m_isConnected || m_deviceHandle == nullptr) return false;

    MVCC_INTVALUE_EX value = {};
    if (MV_CC_GetIntValueEx(
            m_deviceHandle, "GevSCPD", &value) != MV_OK) {
        return false;
    }
    min = static_cast<qint64>(value.nMin);
    max = static_cast<qint64>(value.nMax);
    increment = static_cast<qint64>(value.nInc);
    return true;
}
// ---------- 获取像素格式列表 ----------
QVector<QPair<QString, int>> CameraManager::getSupportedPixelFormats()
{
    return fetchPixelFormatsInternal();
}
QVector<QPair<QString, int>> CameraManager::fetchPixelFormatsInternal()
{
    QVector<QPair<QString, int>> formatList;
    // 注意：部分旧版 MVS SDK 不提供 MV_CC_GetEnumEntryCount 等动态枚举接口，
    // 这里使用预置常用格式列表。若相机不支持某格式，设置时会返回错误码，可在状态栏看到。
    // MV-CS050-10GC(彩色) 常见支持: Mono8 / BayerRG8 / BayerRG10 / BayerRG12 等
    formatList.append(qMakePair(QString("Mono8"), PixelType_Gvsp_Mono8));
    formatList.append(qMakePair(QString("BayerRG8"), PixelType_Gvsp_BayerRG8));
    formatList.append(qMakePair(QString("BayerGB8"), PixelType_Gvsp_BayerGB8));
    formatList.append(qMakePair(QString("BayerBG8"), PixelType_Gvsp_BayerBG8));
    formatList.append(qMakePair(QString("BayerGR8"), PixelType_Gvsp_BayerGR8));
    formatList.append(qMakePair(QString("RGB8"), PixelType_Gvsp_RGB8_Packed));
    return formatList;
}





