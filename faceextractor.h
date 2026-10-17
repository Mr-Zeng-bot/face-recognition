#ifndef FACEEXTRACTOR_H
#define FACEEXTRACTOR_H

// ============================================================================
// faceextractor.h —— 人脸特征提取模块（要求 3 / 5）
// 说明：
//   * 使用 ncnn 部署 ArcFace（纯 C++），输入 112x112 对齐人脸，输出 512 维特征。
//   * 输出做 L2 归一化，后续用余弦相似度比对（归一化后等价于点积）。
// ============================================================================

#include <QVector>
#include <QString>
#include <opencv2/core.hpp>
#include <ncnn/net.h>

class FaceExtractor
{
public:
    FaceExtractor() = default;

    // 载入模型
    //   featureDim : 期望特征维度（ArcFace-MBF 为 512）
    bool load(const QString &paramPath, const QString &binPath, int featureDim,
              int numThreads, bool useVulkan, QString &error);

    bool isLoaded() const { return m_loaded; }
    int  featureDim() const { return m_dim; }

    // 提取特征。alignedBgr 必须是 112x112 的对齐人脸（BGR）。
    // 输出 feature 为 L2 归一化后的向量。
    bool extract(const cv::Mat &alignedBgr, QVector<float> &feature, QString &error);

private:
    ncnn::Net m_net;
    bool    m_loaded = false;
    int     m_dim = 512;
    QString m_inputName;
    QString m_outputName;
};

#endif // FACEEXTRACTOR_H
