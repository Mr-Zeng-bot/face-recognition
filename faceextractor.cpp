#include "faceextractor.h"
#include "facealigner.h"

#include <QDebug>
#include <cmath>

// ---------------------------------------------------------------------------
// ArcFace（ncnn）特征提取实现
// ---------------------------------------------------------------------------

bool FaceExtractor::load(const QString &paramPath, const QString &binPath, int featureDim,
                         int numThreads, bool useVulkan, QString &error)
{
    m_dim = featureDim;
    m_net.opt.num_threads = numThreads;
    m_net.opt.use_vulkan_compute = useVulkan;

    if (m_net.load_param(paramPath.toUtf8().constData()) != 0) {
        error = QStringLiteral("加载 ArcFace param 失败: ") + paramPath;
        return false;
    }
    if (m_net.load_model(binPath.toUtf8().constData()) != 0) {
        error = QStringLiteral("加载 ArcFace bin 失败: ") + binPath;
        return false;
    }

    if (!m_net.input_names().empty())
        m_inputName = QString::fromUtf8(m_net.input_names().front());
    if (!m_net.output_names().empty())
        m_outputName = QString::fromUtf8(m_net.output_names().front());

    m_loaded = true;
    qDebug() << "[FaceExtractor] ArcFace 载入成功, 维度=" << m_dim
             << "输入blob=" << m_inputName << "输出blob=" << m_outputName;
    return true;
}

bool FaceExtractor::extract(const cv::Mat &alignedBgr, QVector<float> &feature, QString &error)
{
    if (!m_loaded) {
        error = QStringLiteral("特征提取模型尚未加载");
        return false;
    }
    if (alignedBgr.empty() ||
        alignedBgr.cols != FaceAligner::kAlignedSize ||
        alignedBgr.rows != FaceAligner::kAlignedSize) {
        error = QStringLiteral("对齐人脸尺寸不是 112x112");
        return false;
    }

    // 1) 构造输入：BGR->RGB，归一化 (x - 127.5) / 127.5，范围 [-1,1]
    ncnn::Mat in = ncnn::Mat::from_pixels(alignedBgr.data, ncnn::Mat::PIXEL_BGR2RGB,
                                          alignedBgr.cols, alignedBgr.rows);
    const float meanVals[3] = {127.5f, 127.5f, 127.5f};
    const float normVals[3] = {1.0f / 127.5f, 1.0f / 127.5f, 1.0f / 127.5f};
    in.substract_mean_normalize(meanVals, normVals);

    // 2) 前向推理
    ncnn::Extractor ex = m_net.create_extractor();
    if (!m_inputName.isEmpty())
        ex.input(m_inputName.toUtf8().constData(), in);
    else
        ex.input(0, in);

    // 3) 取输出特征
    ncnn::Mat out;
    int ret = -1;
    if (!m_outputName.isEmpty())
        ret = ex.extract(m_outputName.toUtf8().constData(), out);
    if (ret != 0) {
        const std::vector<int> &idx = m_net.output_indexes();
        if (!idx.empty()) ret = ex.extract(idx.front(), out);
    }
    if (ret != 0 || out.empty()) {
        error = QStringLiteral("ArcFace 前向推理失败");
        return false;
    }

    // 4) 拉平成 float 向量
    const int total = static_cast<int>(out.total());
    if (total <= 0) {
        error = QStringLiteral("ArcFace 输出为空");
        return false;
    }
    QVector<float> raw(total);
    for (int i = 0; i < total; ++i) raw[i] = out[i];

    // 5) L2 归一化（比对用余弦相似度，需先归一化）
    double sum = 0.0;
    for (float v : raw) sum += static_cast<double>(v) * v;
    const double norm = std::sqrt(sum);
    if (norm < 1e-6) {
        error = QStringLiteral("特征向量范数异常");
        return false;
    }
    feature.resize(total);
    for (int i = 0; i < total; ++i) feature[i] = static_cast<float>(raw[i] / norm);

    // 若模型输出维度与配置不一致，给出提示（不阻断）
    if (m_dim > 0 && total != m_dim) {
        qWarning() << "[FaceExtractor] 模型实际维度" << total << "与配置" << m_dim << "不一致";
    }
    return true;
}
