#ifndef FACEDETECTOR_H
#define FACEDETECTOR_H

// ============================================================================
// facedetector.h —— 人脸检测模块（要求 3：独立的人脸检测模块）
// 说明：
//   * 使用 ncnn 部署 SCRFD 模型（纯 C++，无 Python 依赖），输出人脸框 + 5 个关键点。
//   * 不依赖固定的输出 blob 名字：加载后通过 Net::output_names()/output_indexes()
//     动态取输出，再按输出通道数（score / bbox / kps）自动归类，兼容不同转换脚本
//     产生的模型。
// ============================================================================

#include <QVector>
#include <QString>
#include <vector>
#include <opencv2/core.hpp>
#include <ncnn/net.h>
#include "imagedata.h"

class FaceDetector
{
public:
    FaceDetector() = default;

    // 载入参数(.param)与权重(.bin)
    //   inputSize    : 网络输入边长（SCRFD-500M 常用 640，需为 32 的整数倍）
    //   scoreThresh  : 检测置信度阈值
    //   nmsThresh    : NMS IoU 阈值
    //   numThreads   : CPU 推理线程数
    //   useVulkan    : 是否使用 Vulkan GPU 加速
    bool load(const QString &paramPath, const QString &binPath, int inputSize,
              float scoreThresh, float nmsThresh,
              int numThreads, bool useVulkan, QString &error);

    bool isLoaded() const { return m_loaded; }

    // 对单张 BGR 图做人脸检测
    QVector<FaceData::FaceBox> detect(const cv::Mat &bgr, QString &error);

private:
    // 单尺度输出解码所需的数据
    struct StrideOutputs
    {
        int stride = 0;          // 8 / 16 / 32
        int numAnchor = 1;       // 每格 anchor 数
        const ncnn::Mat *score = nullptr;
        const ncnn::Mat *bbox = nullptr;
        const ncnn::Mat *kps = nullptr;
        // 输出布局：
        //   flat = false -> 3 维特征图 (w×h = 格子数, c = anchor 或 anchor*4/anchor*10)
        //   flat = true  -> 扁平输出 (w = 每格通道数, h = 格子数*anchor)，SCRFD 官方
        //                   onnx 导出并用 pnnx/onnx2ncnn 转换后即为此形态
        bool flat = false;
        int gridW = 0;           // 特征图宽（格子数），仅 flat 布局使用
        int gridH = 0;           // 特征图高（格子数），仅 flat 布局使用
    };

    // 从一次前向的所有输出里，按空间尺寸与通道数归类到 3 个 stride 组
    bool classifyOutputs(const std::vector<ncnn::Mat> &outs,
                         QVector<StrideOutputs> &groups, QString &error);

    ncnn::Net m_net;
    bool  m_loaded = false;
    int   m_inputSize = 640;
    float m_scoreThresh = 0.5f;
    float m_nmsThresh = 0.4f;
    QString m_inputName;
};

#endif // FACEDETECTOR_H
