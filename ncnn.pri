# ============================================================================
# ncnn.pri —— ncnn 依赖配置
#   使用官方 Windows VS2022 x64 预编译库（thirdparty 目录，已随工程提供）。
#   如需更换版本，只需修改 NCNN_ROOT；也可在 qmake 命令行覆盖：
#       qmake NCNN_ROOT=D:/path/to/ncnn/x64
# ============================================================================

isEmpty(NCNN_ROOT): NCNN_ROOT = $$PWD/thirdparty/ncnn-20260526-windows-vs2022-shared/x64

NCNN_INCLUDE_DIR = $$NCNN_ROOT/include
NCNN_LIB_DIR     = $$NCNN_ROOT/lib
NCNN_BIN_DIR     = $$NCNN_ROOT/bin

!exists($$NCNN_INCLUDE_DIR/ncnn/net.h): error(ncnn headers not found: $$NCNN_INCLUDE_DIR)
!exists($$NCNN_LIB_DIR/ncnn.lib):       error(ncnn library not found: $$NCNN_LIB_DIR)
!exists($$NCNN_BIN_DIR/ncnn.dll):       error(ncnn runtime not found: $$NCNN_BIN_DIR)

INCLUDEPATH += $$quote($$NCNN_INCLUDE_DIR)
DEPENDPATH  += $$quote($$NCNN_INCLUDE_DIR)

# ncnn 头文件自身通过 ncnn/platform.h 决定 NCNN_VULKAN / NCNN_STRING 等开关，
# 无需在工程里额外定义；NOMINMAX 与官方 CMake 配置保持一致。
DEFINES += NOMINMAX

LIBS += $$quote($$NCNN_LIB_DIR/ncnn.lib)

message(ncnn: $$NCNN_LIB_DIR/ncnn.lib)
