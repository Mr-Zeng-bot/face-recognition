#ifndef FACEALIGNER_H
#define FACEALIGNER_H

// ============================================================================
// facealigner.h —— 人脸对齐模块（要求 4）
// 说明：
//   * 用检测到的 5 个关键点，通过相似变换（等比 + 旋转 + 平移）把任意姿态的人脸
//     校正到标准的 112x112 正脸，供 ArcFace 提取特征使用。
//   * 参考点采用 ArcFace 官方定义在 112x112 画布上的 5 点坐标。
// ============================================================================

#include <QPointF>
#include <opencv2/core.hpp>

namespace FaceAligner {

// 对齐输出尺寸（ArcFace 标准输入）
const int kAlignedSize = 112;

// 5 点参考模板（左眼、右眼、鼻尖、左嘴角、右嘴角），112x112 坐标系
const float *referencePoints();

// 根据 5 点关键点对齐并裁剪人脸。
//   bgr        : 原图（BGR）
//   landmarks  : 5 个关键点（原图坐标，顺序与 imagedata.h 中一致）
//   aligned    : 输出 112x112 的 BGR 人脸图
// 返回是否成功。
bool align(const cv::Mat &bgr, const QPointF landmarks[5], cv::Mat &aligned);

} // namespace FaceAligner

#endif // FACEALIGNER_H
