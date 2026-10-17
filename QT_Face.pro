# ============================================================================
# QT_Face.pro —— 工业相机人脸识别上位机（qmake 工程文件）
#
# 依赖：Qt6 (Widgets/Sql) + OpenCV5 + ncnn + 海康 MVS SDK
# 目标平台：Windows x64 / MSVC 2022
#
# 目录结构：
#   QT_Face/
#     cameraManager.*      相机采集模块（海康 MVS）
#     localcamera.*        本机 UVC 摄像头采集（无工业相机时验证用）
#     imagepreprocess.*    图像预处理 + 人脸质量判断
#     facedetector.*       SCRFD 人脸检测（ncnn）
#     facealigner.*        5 点对齐裁剪
#     faceextractor.*      ArcFace 特征提取（ncnn）
#     facedatabase.*       MySQL 数据库模块（QtSql）
#     facepipeline.*       流水线编排（独立线程）
#     imagestorage.*       QImage -> cv::Mat 转换（供录像编码用）
#     videorecorder.*      视频录制
#     mainwindow.*         界面
#     models/              SCRFD / ArcFace 的 ncnn 模型文件
#     sql/schema.sql       MySQL 建表语句
#     docs/                编译与部署说明
# ============================================================================

QT += core gui widgets sql

CONFIG += c++17
TARGET = QT_Face
TEMPLATE = app

# MSVC 编译：UTF-8 源码 + 多核编译
msvc {
    QMAKE_CXXFLAGS += /utf-8 /MP
    # 第三方头文件按「外部头文件」处理：它们自身的告警不再刷屏，例如
    #   ncnn/gpu.h 的 friend 声明缺导出属性 -> C4273 dll 链接不一致
    #   Qt 头文件里 compressEvent 已弃用        -> C4996
    # 自己的头文件是用 "" 包含的，不受影响，照常告警。
    QMAKE_CXXFLAGS += /external:anglebrackets /external:W0
}
DEFINES += QT_DEPRECATED_WARNINGS

# ---------------------------------------------------------------------------
# MSVC / Windows SDK 头文件与库路径自动探测
# （若 Qt Creator 所选套件已提供完整 INCLUDE/LIB 环境，可整段删除）
# ---------------------------------------------------------------------------
win32-msvc {
    CONFIG -= embed_manifest_exe

    isEmpty(MSVC_ROOT): MSVC_ROOT = C:/Program Files/Microsoft Visual Studio/18/Community
    !exists("$$MSVC_ROOT/VC") {
        isEmpty(MSVC_ROOT_2022): MSVC_ROOT_2022 = C:/Program Files/Microsoft Visual Studio/2022/Community
        exists("$$MSVC_ROOT_2022/VC"): MSVC_ROOT = $$MSVC_ROOT_2022
    }
    isEmpty(MSVC_TOOLS_VERSION) {
        MSVC_TOOLSET_DIRS = $$files("$${MSVC_ROOT}/VC/Tools/MSVC/*")
        for(toolset, MSVC_TOOLSET_DIRS) {
            exists("$${toolset}/bin/Hostx64/x64/cl.exe"): MSVC_TOOLS_VERSION = $$basename(toolset)
        }
    }
    isEmpty(MSVC_INCLUDE_DIR): MSVC_INCLUDE_DIR = $$MSVC_ROOT/VC/Tools/MSVC/$$MSVC_TOOLS_VERSION/include
    isEmpty(MSVC_LIB_DIR): MSVC_LIB_DIR = $$MSVC_ROOT/VC/Tools/MSVC/$$MSVC_TOOLS_VERSION/lib/x64

    isEmpty(WINSDK_ROOT) {
        WINSDK_ROOT = D:/Windows Kits/10
        !exists("$${WINSDK_ROOT}/Include"): WINSDK_ROOT = "C:/Program Files (x86)/Windows Kits/10"
        !exists("$${WINSDK_ROOT}/Include"): WINSDK_ROOT = "C:/Program Files/Windows Kits/10"
    }
    isEmpty(WINSDK_VERSION) {
        WINSDK_INCLUDE_DIRS = $$files("$${WINSDK_ROOT}/Include/*")
        for(sdkdir, WINSDK_INCLUDE_DIRS) {
            exists("$${sdkdir}/um/windows.h"): WINSDK_VERSION = $$basename(sdkdir)
        }
    }
    isEmpty(WINSDK_INCLUDE_ROOT): WINSDK_INCLUDE_ROOT = $$WINSDK_ROOT/Include/$$WINSDK_VERSION
    isEmpty(WINSDK_LIB_ROOT): WINSDK_LIB_ROOT = $$WINSDK_ROOT/Lib/$$WINSDK_VERSION

    # 用 /I 编译开关传入，而不是 QMAKE_INCDIR：
    # qmake 会把 QMAKE_INCDIR 中的绝对路径改写成相对路径并写进 Makefile 的依赖列表，
    # 而该相对深度与构建目录不匹配（例如影子构建下变成 "..\..\..\..\..\Windows Kits\..."，
    # 越过盘符根），jom 因此报 "dependent ... does not exist"（nmake 能容忍，jom 不能）。
    MSVC_EXTRA_INCLUDES =
    exists("$${MSVC_INCLUDE_DIR}"): MSVC_EXTRA_INCLUDES += $$shell_quote(/I$$MSVC_INCLUDE_DIR)
    exists("$${WINSDK_INCLUDE_ROOT}") {
        MSVC_EXTRA_INCLUDES += $$shell_quote(/I$$WINSDK_INCLUDE_ROOT/ucrt)
        MSVC_EXTRA_INCLUDES += $$shell_quote(/I$$WINSDK_INCLUDE_ROOT/shared)
        MSVC_EXTRA_INCLUDES += $$shell_quote(/I$$WINSDK_INCLUDE_ROOT/um)
        MSVC_EXTRA_INCLUDES += $$shell_quote(/I$$WINSDK_INCLUDE_ROOT/winrt)
        MSVC_EXTRA_INCLUDES += $$shell_quote(/I$$WINSDK_INCLUDE_ROOT/cppwinrt)
    }
    !isEmpty(MSVC_EXTRA_INCLUDES) {
        QMAKE_CFLAGS   += $$MSVC_EXTRA_INCLUDES
        QMAKE_CXXFLAGS += $$MSVC_EXTRA_INCLUDES
    }
    exists("$${MSVC_LIB_DIR}"): QMAKE_LFLAGS += "\"/LIBPATH:$$MSVC_LIB_DIR\""
    exists("$${WINSDK_LIB_ROOT}") {
        QMAKE_LFLAGS += "\"/LIBPATH:$$WINSDK_LIB_ROOT/ucrt/x64\""
        QMAKE_LFLAGS += "\"/LIBPATH:$$WINSDK_LIB_ROOT/um/x64\""
    }
}

# ---------------------------------------------------------------------------
# 第三方依赖
# ---------------------------------------------------------------------------
include(opencv.pri)   # OpenCV
include(ncnn.pri)     # ncnn

# 海康 MVS SDK（头文件/导入库/运行时）
isEmpty(MVS_SDK): MVS_SDK = C:/Program Files (x86)/MVS/Development
MVS_INCLUDE_DIR = $$PWD/include
MVS_LIB_DIR = $$MVS_SDK/Libraries/win64
!exists($${MVS_INCLUDE_DIR}/MvCameraControl.h): MVS_INCLUDE_DIR = $$MVS_SDK/Includes
!exists($${MVS_LIB_DIR}/MvCameraControl.lib): MVS_LIB_DIR = $$PWD/lib
MVS_RUNTIME_DIR = "C:/Program Files (x86)/Common Files/MVS/Runtime/Win64_x64"
!exists($${MVS_INCLUDE_DIR}/MvCameraControl.h): error(MVS headers not found: $$MVS_INCLUDE_DIR)
!exists($${MVS_LIB_DIR}/MvCameraControl.lib): error(MVS library not found: $$MVS_LIB_DIR)
!exists($${MVS_RUNTIME_DIR}/MvCameraControl.dll): error(MVS runtime not found: $$MVS_RUNTIME_DIR)
INCLUDEPATH += $$quote($$MVS_INCLUDE_DIR)
DEPENDPATH += $$quote($$MVS_INCLUDE_DIR)
LIBS += $$quote($$MVS_LIB_DIR/MvCameraControl.lib)
message(MVS SDK: $$MVS_SDK)

# ---------------------------------------------------------------------------
# 源码
# ---------------------------------------------------------------------------
SOURCES += \
    main.cpp \
    mainwindow.cpp \
    cameraManager.cpp \
    imagepreprocess.cpp \
    facedetector.cpp \
    facealigner.cpp \
    faceextractor.cpp \
    facedatabase.cpp \
    facepipeline.cpp \
    imagestorage.cpp \
    videorecorder.cpp \
    appconfig.cpp \
    localcamera.cpp

HEADERS += \
    mainwindow.h \
    cameraManager.h \
    imagepreprocess.h \
    facedetector.h \
    facealigner.h \
    faceextractor.h \
    facedatabase.h \
    facepipeline.h \
    imagestorage.h \
    videorecorder.h \
    appconfig.h \
    localcamera.h \
    imagedata.h

FORMS += \
    mainwindow.ui

DISTFILES += \
    opencv.pri \
    ncnn.pri \
    deploy-runtime.ps1 \
    config.ini \
    sql/schema.sql \
    sql/migrate_personnel_source.sql \
    sql/check_cache_triggers.sql \
    tools/set-db-password-dpapi.ps1 \
    models/README.md \
    models/convert/fix_input_shape.py \
    docs/编译与部署说明.md \
    docs/DB_RUNTIME_ACCOUNT_AND_DPAPI.md

# ---------------------------------------------------------------------------
# 构建后自动部署：Qt 运行时 + ncnn/OpenCV/MVS 运行时 + 模型文件
# ---------------------------------------------------------------------------
QT_DEPLOY_TOOL = $$[QT_INSTALL_BINS]/windeployqt.exe
CONFIG(debug, debug|release) {
    DEPLOY_SUBDIR = debug
    DEPLOY_CONFIG = debug
} else {
    DEPLOY_SUBDIR = release
    DEPLOY_CONFIG = release
}
DEPLOY_SCRIPT = $$shell_path($$PWD/deploy-runtime.ps1)
DEPLOY_DESTINATION = $$shell_path($$OUT_PWD/$$DEPLOY_SUBDIR)
QMAKE_POST_LINK += powershell.exe -NoProfile -ExecutionPolicy Bypass
QMAKE_POST_LINK += -File $$shell_quote($$DEPLOY_SCRIPT)
QMAKE_POST_LINK += -Destination $$shell_quote($$DEPLOY_DESTINATION)
QMAKE_POST_LINK += -MvsRuntime $$shell_quote($$shell_path($$MVS_RUNTIME_DIR))
QMAKE_POST_LINK += -OpenCvBin $$shell_quote($$shell_path($$OPENCV_BIN_DIR))
QMAKE_POST_LINK += -OpenCvLibrary $$shell_quote($$OPENCV_LIBRARY)
QMAKE_POST_LINK += -NcnnBin $$shell_quote($$shell_path($$NCNN_BIN_DIR))
QMAKE_POST_LINK += -ModelsDir $$shell_quote($$shell_path($$PWD/models))
QMAKE_POST_LINK += -QtDeployTool $$shell_quote($$shell_path($$QT_DEPLOY_TOOL))
QMAKE_POST_LINK += -Configuration $$DEPLOY_CONFIG
