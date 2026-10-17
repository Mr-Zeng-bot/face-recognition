# 第三方组件与许可证声明

本项目源码以 MIT License 发布，但以下第三方组件、模型和 SDK 各自遵循其上游许可证。
本文件不改变任何上游许可证条款。

## ncnn

- 项目：https://github.com/Tencent/ncnn
- 本地版本目录：`thirdparty/ncnn-20260526-windows-vs2022-shared/`
- 仓库内分发内容：x64 头文件、导入库和运行时二进制
- 许可证：BSD 3-Clause
- 完整许可证：
  `thirdparty/ncnn-20260526-windows-vs2022-shared/LICENSE.txt`

该许可证文件来自 ncnn 官方仓库，并包含 ncnn 所集成的其他第三方组件许可说明。

## OpenCV

- 项目：https://opencv.org/
- 使用方式：动态链接 OpenCV 5.0.0 x64
- 许可证：Apache License 2.0
- 许可证文本：https://github.com/opencv/opencv/blob/5.x/LICENSE

源代码仓库不包含 OpenCV 二进制。构建和运行目录中的 OpenCV DLL 由本机安装的
OpenCV 部署，使用时需遵守其许可证。

## Qt

- 项目：https://www.qt.io/
- 使用方式：动态链接 Qt 6.12.0
- 许可证：按 Qt 官方条款，LGPL-3.0 或 GPL
- 许可证说明：https://www.qt.io/licensing/open-source-lgpl-obligations

本项目以动态链接方式使用 Qt，不修改 Qt 本身。发布二进制时应保留动态链接，
并满足 Qt 对应许可证的分发义务。

## InsightFace 模型

- 项目：https://github.com/deepinsight/insightface
- 使用来源：`buffalo_sc.zip` 中的 `det_500m.onnx` 与 `w600k_mbf.onnx`
- 使用形式：通过 pnnx 转换为 ncnn `.param` / `.bin`
- 许可证政策：InsightFace 代码为 MIT，但训练数据及由此训练的模型仅限非商业研究

本项目**不随 Git 仓库分发**任何 InsightFace `.onnx`、`.param` 或 `.bin` 模型产物。
用户必须在本地自行下载、转换并遵守 InsightFace 上游许可证：

```text
https://github.com/deepinsight/insightface
```

因此，仓库中的 MIT License 仅适用于本项目自有源码、文档和转换脚本，不适用于用户
自行获取的 InsightFace 模型产物。商业使用需要另行取得合适的模型授权。

## 海康 MVS SDK

- 厂商：杭州海康机器人股份有限公司
- 使用方式：用户本机安装的海康 MVS SDK
- 许可证：专有软件，以厂商许可为准

本仓库不包含 MVS SDK 头文件、导入库或运行时 DLL。`include/`、`lib/` 和 MVS
运行时目录均被 `.gitignore` 排除，用户需自行安装。

## MySQL 客户端库

- 项目：https://www.mysql.com/
- 使用方式：QMYSQL 驱动依赖本机 `libmysql.dll`
- 许可证：以 Oracle MySQL 官方条款为准

源代码仓库不包含 MySQL 客户端库。部署脚本可能从本机 MySQL 安装目录复制运行时文件，
发布二进制时需要遵守对应 MySQL 许可。

## pnnx

- 项目：https://github.com/pnnx/pnnx
- 使用方式：用户本地转换模型时安装
- 许可证：以 pnnx 上游许可证为准

仓库不包含 pnnx 二进制。
