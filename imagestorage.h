#ifndef IMAGESTORAGE_H
#define IMAGESTORAGE_H
#include <QImage>
#include <opencv2/core.hpp>
namespace ImageStorage {
// Returned Mat owns its pixels; Qt scanline padding is respected.
cv::Mat toBgr(const QImage &image);
}
#endif
