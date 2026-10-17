# OpenCV 5.0.0 official Windows binaries: MSVC x64 only.
!win32: error(This project currently requires Windows.)
!msvc: error(Select a Qt MSVC 2022 64-bit kit. The installed OpenCV binaries are not compatible with MinGW.)
!equals(QT_ARCH, x86_64): error(OpenCV requires a 64-bit Qt kit.)

# OpenCV 安装根目录：占位值，编译前必须指向本机实际路径，二选一：
#   1) qmake 命令行覆盖：qmake QT_Face.pro OPENCV_ROOT=D:/path/to/opencv
#   2) Qt Creator 构建配置里填“附加 qmake 参数”：OPENCV_ROOT=D:/path/to/opencv
isEmpty(OPENCV_ROOT): OPENCV_ROOT = D:/path/to/opencv
OPENCV_INCLUDE_DIR = $$OPENCV_ROOT/build/include
OPENCV_LIB_DIR = $$OPENCV_ROOT/build/x64/vc16/lib
OPENCV_BIN_DIR = $$OPENCV_ROOT/build/x64/vc16/bin
OPENCV_VERSION = 500
CONFIG(debug, debug|release): OPENCV_SUFFIX = d
OPENCV_LIBRARY = opencv_world$${OPENCV_VERSION}$${OPENCV_SUFFIX}
!exists($$OPENCV_INCLUDE_DIR/opencv2/opencv.hpp): error(OpenCV headers missing: $$OPENCV_INCLUDE_DIR  --  请把 OPENCV_ROOT 指向本机 OpenCV 根目录)
!exists($$OPENCV_LIB_DIR/$${OPENCV_LIBRARY}.lib): error(OpenCV library missing: $$OPENCV_LIBRARY)
!exists($$OPENCV_BIN_DIR/$${OPENCV_LIBRARY}.dll): error(OpenCV runtime missing: $$OPENCV_LIBRARY)
INCLUDEPATH += $$OPENCV_INCLUDE_DIR
LIBS += $$quote($$OPENCV_LIB_DIR/$${OPENCV_LIBRARY}.lib)
QMAKE_CXXFLAGS += /utf-8
message(OpenCV: $$OPENCV_LIBRARY MSVC x64)
