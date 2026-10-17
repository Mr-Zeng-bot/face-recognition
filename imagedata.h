#ifndef IMAGEDATA_H
#define IMAGEDATA_H

// ============================================================================
// imagedata.h —— 各模块之间共享的数据结构定义（纯头文件，不含实现）
// 说明：
//   * 人脸检测模块、特征提取模块、数据库模块、UI 模块都只依赖本文件中的结构，
//     彼此不直接互相包含，保证模块解耦。
//   * 所有坐标均以“原图像素”为单位（左上角为原点）。
// ============================================================================

#include <QString>
#include <QRectF>
#include <QPointF>
#include <QVector>
#include <QDateTime>

namespace FaceData {

// 单张人脸的检测结果：人脸框 + 5 个关键点 + 置信度
struct FaceBox
{
    QRectF rect;                 // 人脸矩形框（原图坐标）
    float score = 0.0f;          // 检测置信度 [0,1]
    QPointF landmarks[5];        // 5 点关键点：左眼、右眼、鼻尖、左嘴角、右嘴角
};

// 5 个关键点在矢量中的固定顺序（与 FaceBox::landmarks 一致）
enum LandmarkIndex
{
    LeftEye = 0,
    RightEye = 1,
    Nose = 2,
    LeftMouth = 3,
    RightMouth = 4
};

// 人脸质量评估结果
struct QualityResult
{
    bool passed = false;         // 是否通过质量校验（通过才继续做识别）
    float blurScore = 0.0f;      // 清晰度：Laplacian 方差，越大越清晰
    float brightness = 0.0f;     // 平均亮度 [0,255]
    float faceWidth = 0.0f;      // 人脸框宽度（像素），过小人脸无法可靠识别
    QString reason;              // 未通过时的中文原因描述
};

// 单条人脸识别记录（用于日志、比对展示）
struct FaceRecord
{
    QString employeeNo;          // 工号
    QString name;                // 姓名（可为空）
    QVector<float> feature;      // 512 维特征向量（已 L2 归一化）
    QString modelVersion;        // 特征所用模型版本标识
};

// 识别/比对结果
struct MatchResult
{
    bool matched = false;        // 是否匹配成功（相似度 >= 阈值）
    QString employeeNo;          // 匹配到的工号
    QString name;                // 匹配到的姓名
    float similarity = 0.0f;     // 最高余弦相似度
};

// 单帧识别结论：供界面状态卡直接展示，避免界面去解析中文文本
struct RecognitionState
{
    // 一帧最终呈现的状态。多张人脸时取优先级最高的那张：
    // Matched > NotMatched > RecognitionOff > ExtractFailed > LowQuality > NoFace
    enum Status
    {
        NoFace = 0,       // 未检测到人脸
        LowQuality,       // 检测到人脸但质量不合格
        ExtractFailed,    // 质量通过但特征提取失败
        RecognitionOff,   // 识别未开启（只做检测与质量判断）
        Matched,          // 与库中某人比对命中
        NotMatched        // 比对未命中
    };

    Status  status     = NoFace;
    int     faceCount  = 0;      // 本帧检测到的人脸数
    QString employeeNo;          // 命中的工号（未命中为空）
    QString name;                // 命中的姓名
    float   similarity = 0.0f;   // 命中时为该人相似度；未命中时为库中最高相似度
    QString detail;              // 补充说明（质量不合格原因、提取失败原因等）
};

// 数据库中的一条识别日志（用于界面日志表格展示）
struct LogRow
{
    qint64 id = 0;               // 自增主键
    QString employeeNo;          // 工号
    QString name;                // 姓名
    float similarity = 0.0f;     // 相似度
    QString result;              // 识别结果文本（匹配/未匹配/低质量）
    QString imagePath;           // 抓拍图片路径
    QString captureTime;         // 抓拍时间
};

} // namespace FaceData

#endif // IMAGEDATA_H
