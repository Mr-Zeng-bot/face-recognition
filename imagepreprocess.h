#ifndef IMAGEPREPROCESS_H
#define IMAGEPREPROCESS_H

// ============================================================================
// imagepreprocess.h —— 图像预处理模块（要求 2）与人脸质量判断（要求 5）
// 说明：
//   * 基于 OpenCV 完成：QImage -> cv::Mat 转换、尺寸缩放、letterbox 等比缩放、
//     亮度/清晰度评估。
//   * 提供人脸质量判断：模糊（Laplacian 方差过小）、过暗/过曝（平均亮度越界）、
//     人脸过小（宽高不足）时直接丢弃，不做后续识别。
// ============================================================================

#include <QImage>
#include <QRectF>
#include <QVector>
#include <opencv2/core.hpp>   // cv::Mat 前置声明不足以在头文件中使用，这里直接包含
#include "imagedata.h"

namespace ImagePreprocess {

// QImage 转 OpenCV BGR Mat（做深拷贝，保证生命周期安全）
cv::Mat toBgrMat(const QImage &image);

// OpenCV BGR Mat 转 QImage（深拷贝）
QImage fromBgrMat(const cv::Mat &bgr);

// 等比缩放（letterbox），返回缩放后的图像与相对原图的缩放比例、填充偏移。
// 用于把人脸图按模型输入尺寸送入 SCRFD，且不破坏长宽比。
cv::Mat letterbox(const cv::Mat &src, int inputSize, float &scale, int &padX, int &padY);

// 通过 Laplacian 方差评估清晰度（越模糊值越小）
float laplacianVariance(const cv::Mat &bgr);

// 计算区域平均亮度
float meanBrightness(const cv::Mat &bgr);

// 人脸质量评估：模糊 / 过暗 / 过曝 / 人脸过小 判定
FaceData::QualityResult evaluateQuality(const cv::Mat &bgr,
                                        const QRectF &faceRect);

// 在图像上绘制人脸框、关键点与文字信息（用于界面显示，返回绘制后的图像）
//   matched : 是否比对成功；true 画绿框，false（未识别 / 质量不合格）画红框
void drawFaceInfo(cv::Mat &bgr,
                  const FaceData::FaceBox &face,
                  const QString &label,
                  const QVector<QPointF> &landmarks,
                  bool matched);

} // namespace ImagePreprocess

#endif // IMAGEPREPROCESS_H
