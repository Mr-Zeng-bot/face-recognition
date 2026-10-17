#include "imagestorage.h"
#include <opencv2/imgproc.hpp>

cv::Mat ImageStorage::toBgr(const QImage &image)
{
    if (image.isNull()) return {};
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    const cv::Mat view(rgb.height(), rgb.width(), CV_8UC3,
                       const_cast<uchar *>(rgb.constBits()), rgb.bytesPerLine());
    cv::Mat bgr;
    cv::cvtColor(view, bgr, cv::COLOR_RGB2BGR);
    return bgr;
}
