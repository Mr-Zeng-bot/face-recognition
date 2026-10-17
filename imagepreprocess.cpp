#include "imagepreprocess.h"
#include "appconfig.h"

#include <opencv2/imgproc.hpp>
#include <QRect>
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// 图像预处理模块实现
// -------------------------------------------------------------------

namespace ImagePreprocess {

cv::Mat toBgrMat(const QImage &image)
{
    if (image.isNull()) return {};
    // 统一转成 RGB888 后再转 BGR，兼容灰度/彩色/带 padding 的 QImage。
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    const cv::Mat view(rgb.height(), rgb.width(), CV_8UC3,
                       const_cast<uchar *>(rgb.constBits()), rgb.bytesPerLine());
    cv::Mat bgr;
    cv::cvtColor(view, bgr, cv::COLOR_RGB2BGR);
    return bgr; // cvtColor 已经产生新内存，可安全脱离临时 QImage 生命周期
}

QImage fromBgrMat(const cv::Mat &bgr)
{
    if (bgr.empty()) return {};
    cv::Mat rgb;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    // cv::Mat 的 step 可能不等于 width*3，QImage 需要按 step 构造后再 copy。
    const QImage view(rgb.data, rgb.cols, rgb.rows,
                      static_cast<int>(rgb.step), QImage::Format_RGB888);
    return view.copy();
}

cv::Mat letterbox(const cv::Mat &src, int inputSize, float &scale, int &padX, int &padY)
{
    scale = 1.0f;
    padX = 0;
    padY = 0;
    if (src.empty()) return {};

    // 取较小缩放比例，保证整张图都能放进 inputSize x inputSize 画布。
    scale = std::min(static_cast<float>(inputSize) / src.cols,
                     static_cast<float>(inputSize) / src.rows);
    const int newW = static_cast<int>(std::round(src.cols * scale));
    const int newH = static_cast<int>(std::round(src.rows * scale));

    cv::Mat resized;
    cv::resize(src, resized, cv::Size(newW, newH));

    // 右侧/底部补 0（黑色），使画布为正方形。
    padX = (inputSize - newW) / 2;
    padY = (inputSize - newH) / 2;
    cv::Mat canvas(inputSize, inputSize, CV_8UC3, cv::Scalar(0, 0, 0));
    resized.copyTo(canvas(cv::Rect(padX, padY, newW, newH)));
    return canvas;
}

float laplacianVariance(const cv::Mat &bgr)
{
    if (bgr.empty()) return 0.0f;
    cv::Mat gray;
    if (bgr.channels() == 3) cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    else gray = bgr;

    cv::Mat lap;
    cv::Laplacian(gray, lap, CV_64F);
    cv::Scalar mean, stddev;
    cv::meanStdDev(lap, mean, stddev);
    // 方差 = 标准差^2，方差越大表示边缘越丰富、越清晰。
    return static_cast<float>(stddev[0] * stddev[0]);
}

float meanBrightness(const cv::Mat &bgr)
{
    if (bgr.empty()) return 0.0f;
    cv::Mat gray;
    if (bgr.channels() == 3) cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    else gray = bgr;
    return static_cast<float>(cv::mean(gray)[0]);
}

FaceData::QualityResult evaluateQuality(const cv::Mat &bgr, const QRectF &faceRect)
{
    const AppConfig &cfg = AppConfig::instance();
    FaceData::QualityResult result;

    if (bgr.empty()) {
        result.reason = QStringLiteral("图像为空");
        return result;
    }

    // 把人脸框裁剪出来，只对人脸区域做质量判断，避免背景干扰。
    const cv::Rect roi = cv::Rect(static_cast<int>(faceRect.x()),
                                  static_cast<int>(faceRect.y()),
                                  static_cast<int>(faceRect.width()),
                                  static_cast<int>(faceRect.height()))
                         & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (roi.width <= 0 || roi.height <= 0) {
        result.reason = QStringLiteral("人脸区域越界");
        return result;
    }
    const cv::Mat faceRoi = bgr(roi);

    result.faceWidth  = static_cast<float>(roi.width);
    result.blurScore  = laplacianVariance(faceRoi);
    result.brightness = meanBrightness(faceRoi);

    // 1) 人脸过小
    if (result.faceWidth < cfg.minFaceWidth) {
        result.reason = QStringLiteral("人脸过小（宽 %1 < %2）")
                            .arg(result.faceWidth).arg(cfg.minFaceWidth);
        return result;
    }
    // 2) 模糊
    if (result.blurScore < cfg.minBlurScore) {
        result.reason = QStringLiteral("图像模糊（清晰度 %1 < %2）")
                            .arg(result.blurScore, 0, 'f', 1).arg(cfg.minBlurScore);
        return result;
    }
    // 3) 过暗
    if (result.brightness < cfg.minBrightness) {
        result.reason = QStringLiteral("图像过暗（亮度 %1 < %2）")
                            .arg(result.brightness, 0, 'f', 1).arg(cfg.minBrightness);
        return result;
    }
    // 4) 过曝
    if (result.brightness > cfg.maxBrightness) {
        result.reason = QStringLiteral("图像过曝（亮度 %1 > %2）")
                            .arg(result.brightness, 0, 'f', 1).arg(cfg.maxBrightness);
        return result;
    }

    result.passed = true;
    return result;
}

void drawFaceInfo(cv::Mat &bgr,
                  const FaceData::FaceBox &face,
                  const QString &label,
                  const QVector<QPointF> &landmarks,
                  bool matched)
{
    if (bgr.empty()) return;

    // 识别成功用绿色，未识别 / 质量不合格用红色，便于现场一眼区分。
    // （不能用 label 是否为空来判断：所有分支都会填 label。）
    const cv::Scalar color = matched ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255);

    const cv::Rect box(static_cast<int>(face.rect.x()), static_cast<int>(face.rect.y()),
                       static_cast<int>(face.rect.width()), static_cast<int>(face.rect.height()));
    cv::rectangle(bgr, box, color, 2);

    // 画 5 个关键点
    for (const QPointF &pt : landmarks) {
        cv::circle(bgr, cv::Point(static_cast<int>(pt.x()), static_cast<int>(pt.y())),
                   3, cv::Scalar(255, 0, 0), -1);
    }
    // 关键点为空时，退回使用检测结果内置的关键点
    if (landmarks.isEmpty()) {
        for (int i = 0; i < 5; ++i) {
            cv::circle(bgr, cv::Point(static_cast<int>(face.landmarks[i].x()),
                                      static_cast<int>(face.landmarks[i].y())),
                       3, cv::Scalar(255, 0, 0), -1);
        }
    }

    // 在框顶绘制文字标签（中文需要 OpenCV 不支持，故标签用英文/数字，详情见 UI 状态栏）
    if (!label.isEmpty()) {
        int baseline = 0;
        const cv::Size textSize = cv::getTextSize(label.toStdString(), cv::FONT_HERSHEY_SIMPLEX,
                                                  0.6, 2, &baseline);
        const int textY = std::max(box.y - 5, textSize.height + 5);
        cv::rectangle(bgr,
                      cv::Point(box.x, textY - textSize.height - 4),
                      cv::Point(box.x + textSize.width + 6, textY + 4),
                      color, -1);
        cv::putText(bgr, label.toStdString(), cv::Point(box.x + 3, textY),
                    cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 0), 2);
    }
}

} // namespace ImagePreprocess
