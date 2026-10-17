#include "facedetector.h"
#include "imagepreprocess.h"

#include <QDebug>
#include <QMap>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// SCRFD（ncnn）人脸检测实现
// ---------------------------------------------------------------------------

namespace {

// 计算两个框的 IoU，用于 NMS
float iou(const cv::Rect2f &a, const cv::Rect2f &b)
{
    const float x1 = std::max(a.x, b.x);
    const float y1 = std::max(a.y, b.y);
    const float x2 = std::min(a.x + a.width, b.x + b.width);
    const float y2 = std::min(a.y + a.height, b.y + b.height);
    const float interW = std::max(0.0f, x2 - x1);
    const float interH = std::max(0.0f, y2 - y1);
    const float inter = interW * interH;
    const float uni = a.width * a.height + b.width * b.height - inter;
    return uni <= 0.0f ? 0.0f : inter / uni;
}

// 按分数从高到低做 NMS
void nms(std::vector<FaceData::FaceBox> &faces, float iouThresh)
{
    std::sort(faces.begin(), faces.end(), [](const FaceData::FaceBox &a, const FaceData::FaceBox &b) {
        return a.score > b.score;
    });
    std::vector<FaceData::FaceBox> keep;
    std::vector<bool> removed(faces.size(), false);
    for (size_t i = 0; i < faces.size(); ++i) {
        if (removed[i]) continue;
        keep.push_back(faces[i]);
        const cv::Rect2f ri(static_cast<float>(faces[i].rect.x()),
                           static_cast<float>(faces[i].rect.y()),
                           static_cast<float>(faces[i].rect.width()),
                           static_cast<float>(faces[i].rect.height()));
        for (size_t j = i + 1; j < faces.size(); ++j) {
            if (removed[j]) continue;
            const cv::Rect2f rj(static_cast<float>(faces[j].rect.x()),
                               static_cast<float>(faces[j].rect.y()),
                               static_cast<float>(faces[j].rect.width()),
                               static_cast<float>(faces[j].rect.height()));
            if (iou(ri, rj) > iouThresh) removed[j] = true;
        }
    }
    faces.swap(keep);
}

} // namespace

bool FaceDetector::load(const QString &paramPath, const QString &binPath, int inputSize,
                        float scoreThresh, float nmsThresh,
                        int numThreads, bool useVulkan, QString &error)
{
    m_inputSize = inputSize;
    m_scoreThresh = scoreThresh;
    m_nmsThresh = nmsThresh;

    // ncnn 运行选项：CPU 线程数、是否启用 Vulkan。
    m_net.opt.num_threads = numThreads;
    m_net.opt.use_vulkan_compute = useVulkan;

    if (m_net.load_param(paramPath.toUtf8().constData()) != 0) {
        error = QStringLiteral("加载 SCRFD param 失败: ") + paramPath;
        return false;
    }
    if (m_net.load_model(binPath.toUtf8().constData()) != 0) {
        error = QStringLiteral("加载 SCRFD bin 失败: ") + binPath;
        return false;
    }

    // 读取输入 blob 名（不同转换脚本可能是 "input.1" / "data" / "input" 等）
    if (!m_net.input_names().empty())
        m_inputName = QString::fromUtf8(m_net.input_names().front());

    m_loaded = true;
    qDebug() << "[FaceDetector] SCRFD 载入成功, 输入尺寸=" << m_inputSize
             << "输入blob=" << m_inputName;
    return true;
}

bool FaceDetector::classifyOutputs(const std::vector<ncnn::Mat> &outs,
                                   QVector<StrideOutputs> &groups, QString &error)
{
    // 两种常见输出布局都要支持：
    //   A) 3 维特征图：w/h 为格子数、c 为 anchor（或 anchor*4 / anchor*10）
    //   B) 扁平输出：w 为每格通道数、h 为「格子数*anchor」，SCRFD 官方 onnx 导出
    //      经 pnnx / onnx2ncnn 转换后就是这种形态（w=1/4/10，h=格子数*anchor）
    QMap<int, QVector<const ncnn::Mat *>> byStride;   // 布局 A：按 stride 分组
    QMap<int, QVector<const ncnn::Mat *>> byCount;    // 布局 B：按 h（元素总数）分组
    for (const ncnn::Mat &m : outs) {
        if (m.w <= 0 || m.h <= 0) continue;
        if (m.dims == 3 && m.w == m.h) {
            const int stride = m_inputSize / m.w;
            if (stride > 0) byStride[stride].push_back(&m);
        } else if (m.dims == 2) {
            byCount[m.h].push_back(&m);
        }
    }

    for (auto it = byStride.begin(); it != byStride.end(); ++it) {
        const QVector<const ncnn::Mat *> &list = it.value();
        if (list.size() < 3) continue;

        // 通道数最小的那个一定是 score（c = anchor 数）
        const ncnn::Mat *score = nullptr;
        for (const ncnn::Mat *m : list) {
            if (score == nullptr || m->c < score->c) score = m;
        }
        const int anchor = score->c;

        StrideOutputs out;
        out.stride = it.key();
        out.numAnchor = anchor;
        out.score = score;
        for (const ncnn::Mat *m : list) {
            if (m == score) continue;
            if (m->c == anchor * 4) out.bbox = m;
            else if (m->c == anchor * 10) out.kps = m;
        }

        // 部分模型 anchor 布局不同（如 anchor=1 时 bbox=4 / kps=10）
        if (out.bbox == nullptr || out.kps == nullptr) {
            error = QStringLiteral("SCRFD 输出通道不匹配（stride %1, anchor %2），"
                                   "请确认模型转换正确").arg(out.stride).arg(anchor);
            return false;
        }
        groups.push_back(out);
    }

    // 布局 B：w = 每个 anchor 的通道数（score=1 / bbox=4 / kps=10），
    //         h = 格子数 * anchor，同一尺度三者的 h 相同。
    for (auto it = byCount.begin(); it != byCount.end(); ++it) {
        const QVector<const ncnn::Mat *> &list = it.value();
        if (list.size() < 3) continue;

        const ncnn::Mat *score = nullptr;
        const ncnn::Mat *bbox = nullptr;
        const ncnn::Mat *kps = nullptr;
        for (const ncnn::Mat *m : list) {
            if (m->w == 1 && score == nullptr) score = m;
            else if (m->w == 4) bbox = m;
            else if (m->w == 10) kps = m;
        }
        if (score == nullptr || bbox == nullptr || kps == nullptr) continue;

        // 已知元素总数，反推 (stride, anchor)：格子必须是正方形，
        // 同一条输出线上 anchor 数一致，取 anchor 最少（stride 最大）的合理组合。
        const int count = it.key();
        int bestStride = 0;
        int bestAnchor = 0;
        const int strides[] = {8, 16, 32};
        for (int stride : strides) {
            if (m_inputSize % stride != 0) continue;
            const int grid = m_inputSize / stride;
            const int cells = grid * grid;
            if (count % cells != 0) continue;
            const int anchor = count / cells;
            if (anchor < 1 || anchor > 8) continue;
            if (bestAnchor == 0 || anchor < bestAnchor) {
                bestStride = stride;
                bestAnchor = anchor;
            }
        }
        if (bestStride == 0) {
            error = QStringLiteral("SCRFD 输出尺寸异常：元素数 %1，请确认模型输入边长与 "
                                   "config.ini 的 inputSize 一致").arg(count);
            continue;
        }

        StrideOutputs out;
        out.flat = true;
        out.stride = bestStride;
        out.numAnchor = bestAnchor;
        out.gridW = m_inputSize / bestStride;
        out.gridH = out.gridW;
        out.score = score;
        out.bbox = bbox;
        out.kps = kps;
        groups.push_back(out);
    }

    if (groups.isEmpty()) {
        error = QStringLiteral("未能从 SCRFD 输出中解析出有效特征图");
        return false;
    }
    return true;
}

QVector<FaceData::FaceBox> FaceDetector::detect(const cv::Mat &bgr, QString &error)
{
    QVector<FaceData::FaceBox> result;
    if (!m_loaded) {
        error = QStringLiteral("检测模型尚未加载");
        return result;
    }
    if (bgr.empty()) {
        error = QStringLiteral("输入图像为空");
        return result;
    }

    // 1) letterbox 等比缩放到模型输入尺寸
    float scale = 1.0f;
    int padX = 0, padY = 0;
    const cv::Mat canvas = ImagePreprocess::letterbox(bgr, m_inputSize, scale, padX, padY);

    // 2) 构造 ncnn 输入：BGR->RGB，并做 (x - 127.5) / 128 归一化
    ncnn::Mat in = ncnn::Mat::from_pixels(canvas.data, ncnn::Mat::PIXEL_BGR2RGB,
                                          canvas.cols, canvas.rows);
    const float meanVals[3] = {127.5f, 127.5f, 127.5f};
    const float normVals[3] = {1.0f / 128.0f, 1.0f / 128.0f, 1.0f / 128.0f};
    in.substract_mean_normalize(meanVals, normVals);

    // 3) 前向推理
    ncnn::Extractor ex = m_net.create_extractor();
    if (!m_inputName.isEmpty())
        ex.input(m_inputName.toUtf8().constData(), in);
    else
        ex.input(0, in);

    // 4) 取出所有输出 blob（不假设名字/顺序）
    std::vector<ncnn::Mat> outs;
    const std::vector<const char *> &outNames = m_net.output_names();
    for (const char *name : outNames) {
        ncnn::Mat out;
        if (ex.extract(name, out) == 0) outs.push_back(out);
    }
    if (outs.empty()) {
        // 无输出名（极少见）：退化为按输出索引取
        const std::vector<int> &outIdx = m_net.output_indexes();
        for (int idx : outIdx) {
            ncnn::Mat out;
            if (ex.extract(idx, out) == 0) outs.push_back(out);
        }
    }

    // 5) 归类输出
    QVector<StrideOutputs> groups;
    if (!classifyOutputs(outs, groups, error)) return result;

    // 6) 解码：anchor 中心 + 距离回归 -> 人脸框 / 关键点
    std::vector<FaceData::FaceBox> candidates;
    for (const StrideOutputs &g : groups) {
        const ncnn::Mat &score = *g.score;
        const ncnn::Mat &bbox = *g.bbox;
        const ncnn::Mat &kps = *g.kps;
        const int stride = g.stride;
        const int gridW = g.flat ? g.gridW : score.w;
        const int gridH = g.flat ? g.gridH : score.h;
        const int numAnchor = g.numAnchor;

        // 扁平布局下直接按线性下标取值（ncnn::Mat 可隐式转为 float*）
        const float *scorePtr = g.flat ? static_cast<const float *>(score) : nullptr;
        const float *bboxPtr = g.flat ? static_cast<const float *>(bbox) : nullptr;
        const float *kpsPtr = g.flat ? static_cast<const float *>(kps) : nullptr;

        for (int y = 0; y < gridH; ++y) {
            for (int x = 0; x < gridW; ++x) {
                const int cell = y * gridW + x;
                for (int a = 0; a < numAnchor; ++a) {
                    // 扁平布局的元素顺序：格子序号 * anchor 数 + anchor 序号
                    const int n = cell * numAnchor + a;

                    const float s = g.flat ? scorePtr[n] : score.channel(a).row(y)[x];
                    if (s < m_scoreThresh) continue;

                    // anchor 中心（SCRFD 无 0.5 偏移）
                    const float cx = x * stride;
                    const float cy = y * stride;

                    const float dx0 = g.flat ? bboxPtr[n * 4 + 0] : bbox.channel(a * 4 + 0).row(y)[x];
                    const float dy0 = g.flat ? bboxPtr[n * 4 + 1] : bbox.channel(a * 4 + 1).row(y)[x];
                    const float dx1 = g.flat ? bboxPtr[n * 4 + 2] : bbox.channel(a * 4 + 2).row(y)[x];
                    const float dy1 = g.flat ? bboxPtr[n * 4 + 3] : bbox.channel(a * 4 + 3).row(y)[x];

                    FaceData::FaceBox fb;
                    fb.score = s;

                    // 映射回原图坐标：(值 - padding) / scale
                    auto toOrig = [&](float px, float py, QPointF &out) {
                        out = QPointF((px - padX) / scale, (py - padY) / scale);
                    };

                    QPointF p1, p2;
                    toOrig(cx - dx0 * stride, cy - dy0 * stride, p1);
                    toOrig(cx + dx1 * stride, cy + dy1 * stride, p2);
                    fb.rect = QRectF(p1, p2).normalized();

                    // 5 个关键点
                    for (int k = 0; k < 5; ++k) {
                        const float kx = g.flat ? kpsPtr[n * 10 + k * 2 + 0]
                                                : kps.channel(a * 10 + k * 2 + 0).row(y)[x];
                        const float ky = g.flat ? kpsPtr[n * 10 + k * 2 + 1]
                                                : kps.channel(a * 10 + k * 2 + 1).row(y)[x];
                        QPointF kp;
                        toOrig(cx + kx * stride, cy + ky * stride, kp);
                        fb.landmarks[k] = kp;
                    }

                    // 过滤越界/无效框
                    if (fb.rect.width() < 4 || fb.rect.height() < 4) continue;
                    candidates.push_back(fb);
                }
            }
        }
    }

    // 7) NMS 去重
    nms(candidates, m_nmsThresh);

    // 8) 裁剪到图像范围内
    for (const FaceData::FaceBox &c : candidates) {
        FaceData::FaceBox fb = c;
        const QRectF clipped = fb.rect.intersected(
            QRectF(0, 0, bgr.cols, bgr.rows));
        if (clipped.width() < 4 || clipped.height() < 4) continue;
        fb.rect = clipped;
        result.push_back(fb);
    }

    return result;
}
