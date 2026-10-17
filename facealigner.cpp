#include "facealigner.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/calib3d.hpp>   // estimateAffinePartial2D 位于 calib3d 模块
#include <vector>

namespace FaceAligner {

const float *referencePoints()
{
    // ArcFace / InsightFace 官方 112x112 参考点（顺序：左眼、右眼、鼻尖、左嘴角、右嘴角）
    static const float kRef[10] = {
        38.2946f, 51.6963f,   // 左眼
        73.5318f, 51.5014f,   // 右眼
        56.0252f, 71.7366f,   // 鼻尖
        41.5493f, 92.3655f,   // 左嘴角
        70.7299f, 92.2041f    // 右嘴角
    };
    return kRef;
}

bool align(const cv::Mat &bgr, const QPointF landmarks[5], cv::Mat &aligned)
{
    if (bgr.empty() || landmarks == nullptr) return false;

    // 源点：检测到的 5 个关键点
    std::vector<cv::Point2f> src;
    src.reserve(5);
    for (int i = 0; i < 5; ++i) {
        src.emplace_back(static_cast<float>(landmarks[i].x()),
                         static_cast<float>(landmarks[i].y()));
    }

    // 目标点：标准正脸模板
    const float *ref = referencePoints();
    std::vector<cv::Point2f> dst;
    dst.reserve(5);
    for (int i = 0; i < 5; ++i) {
        dst.emplace_back(ref[i * 2], ref[i * 2 + 1]);
    }

    // 相似变换（只能等比缩放 + 旋转 + 平移），比仿射变换更符合人脸对齐需求。
    cv::Mat transform = cv::estimateAffinePartial2D(src, dst);
    if (transform.empty()) return false;

    cv::warpAffine(bgr, aligned, transform,
                   cv::Size(kAlignedSize, kAlignedSize),
                   cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
    return !aligned.empty();
}

} // namespace FaceAligner
