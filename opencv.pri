# OpenCV 5.0.0 official Windows binaries: MSVC x64 only.
!win32: error(This project currently requires Windows.)
!msvc: error(Select a Qt MSVC 2022 64-bit kit. The installed OpenCV binaries are not compatible with MinGW.)
!equals(QT_ARCH, x86_64): error(OpenCV requires a 64-bit Qt kit.)

isEmpty(OPENCV_ROOT): OPENCV_ROOT = E:/OpenCV/opencv
OPENCV_INCLUDE_DIR = $$OPENCV_ROOT/build/include
OPENCV_LIB_DIR = $$OPENCV_ROOT/build/x64/vc16/lib
OPENCV_BIN_DIR = $$OPENCV_ROOT/build/x64/vc16/bin
OPENCV_VERSION = 500
CONFIG(debug, debug|release): OPENCV_SUFFIX = d
OPENCV_LIBRARY = opencv_world$${OPENCV_VERSION}$${OPENCV_SUFFIX}
!exists($$OPENCV_INCLUDE_DIR/opencv2/opencv.hpp): error(OpenCV headers missing: $$OPENCV_INCLUDE_DIR)
!exists($$OPENCV_LIB_DIR/$${OPENCV_LIBRARY}.lib): error(OpenCV library missing: $$OPENCV_LIBRARY)
!exists($$OPENCV_BIN_DIR/$${OPENCV_LIBRARY}.dll): error(OpenCV runtime missing: $$OPENCV_LIBRARY)
INCLUDEPATH += $$OPENCV_INCLUDE_DIR
LIBS += $$quote($$OPENCV_LIB_DIR/$${OPENCV_LIBRARY}.lib)
QMAKE_CXXFLAGS += /utf-8
message(OpenCV: $$OPENCV_LIBRARY MSVC x64)
