<div align="center">

<h1>QT_Face</h1>

<p><strong>Windows 工业相机人脸识别上位机</strong></p>

<img alt="Windows" src="https://img.shields.io/badge/Windows-10%2F11%20x64-0078D4?logo=windows&logoColor=white">
<img alt="Qt" src="https://img.shields.io/badge/Qt-6.12.0-41CD52?logo=qt&logoColor=white">
<img alt="OpenCV" src="https://img.shields.io/badge/OpenCV-5.0.0-5C3EE8?logo=opencv&logoColor=white">
<img alt="ncnn" src="https://img.shields.io/badge/ncnn-20260526-00A3E0">
<img alt="MySQL" src="https://img.shields.io/badge/MySQL-8.0-4479A1?logo=mysql&logoColor=white">
<img alt="C++" src="https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white">
<img alt="License" src="https://img.shields.io/badge/License-MIT-yellow.svg">

</div>

## 目录

- [项目简介](#项目简介)
- [验证状态](#验证状态)
- [功能特性](#功能特性)
- [识别流程](#识别流程)
- [技术栈与环境要求](#技术栈与环境要求)
- [工具包与下载地址](#工具包与下载地址)
- [目录结构](#目录结构)
- [快速开始](#快速开始)
- [数据库设计](#数据库设计)
- [项目文档](#项目文档)
- [问题交流](#问题交流)
- [说明与注意](#说明与注意)
- [第三方组件](#第三方组件)
- [许可证](#许可证)

---

## 项目简介

基于 **Qt6 + OpenCV + ncnn + MySQL** 的 Windows 桌面端人脸采集/识别上位机，面向工业相机（海康 MVS）
场景：实时采集 → 人脸检测 → 对齐 → 特征提取 → 1:N 比对 → 日志入库。

> A Qt6 desktop face-recognition station for industrial cameras (Hikvision MVS), powered by
> OpenCV, ncnn (SCRFD + ArcFace) and MySQL.

## 验证状态

- 海康 MVS 工业相机：已实测跑通设备枚举、连接、采集和识别链路。
- 本机 UVC 摄像头：已实测采集、识别和正常停止。
- 人脸注册、1:N 识别、日志写入和抓拍保存：已验证。
- MySQL 纯 DML 运行账号、DPAPI 口令保护和数据库迁移：已验证。
- 更换相机型号、固件、MVS 版本或部署主机后，仍建议按现场环境复验相机参数。

## 功能特性

**采集**

- 海康 MVS 工业相机：设备枚举、打开/关闭采集、软触发、曝光/增益/帧率/像素格式控制。
- 本机 UVC 摄像头作为替代视频源（没有工业相机时也能完整跑通识别链路）。
- 本地图片导入：直接载入一张照片做注册或识别，无需相机。

**识别链路**

- OpenCV 5.0 负责 QImage 与 `cv::Mat` 转换、letterbox 缩放、亮度/清晰度判断、
  5 点相似变换对齐，以及 UVC 采集和视频编码。
- SCRFD-500M（ncnn）检测人脸框与 5 个关键点。
- 5 点相似变换对齐到 112×112。
- ArcFace MobileFaceNet（ncnn）提取 512 维特征并做 L2 归一化。
- 人脸质量门限：模糊（Laplacian 方差）、过暗、过曝、人脸过小 —— 不合格直接丢弃，
  不参与识别与入库，界面以红色 `LowQuality` 标注。
- 与库内**启用**人员的全部模板做余弦相似度 1:N 比对，阈值可配。

**人员与模板**

- 人员主数据由 MySQL 管理员维护，**程序只读人员、只追加人脸特征**，不会创建/覆盖人员。
- `is_active` 控制启用/停用；停用人员不能注册、也不参与比对。
- 同一个人最多保留 **5 张模板**（覆盖正面/左右侧脸/抬头低头等姿态），超限自动淘汰最旧的一张。
- 删除人员时，外键 `ON DELETE CASCADE` 会连带删除其全部模板（`face_log` 历史保留）。

**运行期**

- 改库免重启：`face_employee` / `face_feature` 的增删改由 6 个触发器递增缓存版本号，
  程序按 `cacheSyncIntervalMs`（默认 1000ms）轮询，约 1 秒内自动生效。
- 识别日志写入 `face_log`（工号/相似度/时间/抓拍图路径）；同一目标 3 秒内只写一条日志、
  只存一张抓拍图，避免对着镜头就无限刷库刷图。
- 视频录制、抓拍图片保存。
- 处理在独立工作线程执行，处理期间 UI 丢帧，不阻塞界面。

## 识别流程

```mermaid
flowchart LR
    A["海康 MVS / UVC / 本地图片"] --> B["OpenCV 预处理与质量检查"]
    B --> C["SCRFD-500M 人脸检测"]
    C --> D["5 点对齐 112x112"]
    D --> E["ArcFace 512 维特征"]
    E --> F["MySQL 1:N 余弦相似度"]
    F --> G["界面结果 / face_log / 抓拍图"]
```

---

## 技术栈与环境要求

| 组件 | 版本 / 说明 |
|---|---|
| 操作系统 | Windows 10/11 x64 |
| Qt | 6.12.0 `msvc2022_64`（需 Core / Gui / Widgets / **Sql**） |
| 编译器 | MSVC x64（Visual Studio 2022 及以上） |
| OpenCV | 5.0.0 x64（world 版，vc16） |
| ncnn | 20260526 windows-vs2022-shared x64（已随仓库提供于 `thirdparty/`） |
| 海康 MVS SDK | 需**自行安装**（默认 `C:\Program Files (x86)\MVS`） |
| MySQL | 8.0.x + Qt 的 `QMYSQL` 驱动 + 运行期 `libmysql.dll` |

部署时使用的模型固定为 `models/scrfd_500m.*` 与 `models/arcface_mbf.*`；模型产物
不随公开仓库提交，需要按 [`models/README.md`](models/README.md) 自行下载和转换。
更换模型会使已入库特征失效，需要重新采集全部人员模板。

## 工具包与下载地址

以下均为官方发布页；下载版本请以现场环境与发布页说明为准。

| 工具 / 组件 | 用途 | 官方获取地址 |
|---|---|---|
| Visual Studio 2022 Build Tools | MSVC x64 编译器和 Windows SDK | https://visualstudio.microsoft.com/visual-cpp-build-tools/ |
| Git for Windows | 克隆仓库、版本管理 | https://git-scm.com/download/win |
| Qt 6.12.0 Open Source | Widgets、Sql、qmake | https://www.qt.io/download-open-source |
| Qt 6.12.0 归档目录 | 指定版本和离线包 | https://download.qt.io/archive/qt/6.12/6.12.0/ |
| OpenCV 5.0.0 | 图像预处理、对齐、采集和编码 | https://opencv.org/releases/ |
| ncnn | SCRFD / ArcFace 推理引擎 | https://github.com/Tencent/ncnn/releases |
| jom | Qt/MSVC 并行构建工具 | https://download.qt.io/official_releases/jom/ |
| MySQL Installer 8.0 | MySQL Server、客户端库、Workbench | https://dev.mysql.com/downloads/installer/ |
| Qt QMYSQL 驱动说明 | Qt 与 MySQL 驱动配置 | https://doc.qt.io/qt-6/sql-driver.html#qmysql |
| 海康机器人 MVS SDK | 工业相机 SDK 和运行时 | https://www.hikrobotics.com/cn/machinevision/service/download |
| Microsoft VC++ 运行库 | 目标机运行依赖 | https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist |
| Python for Windows | 模型转换工具运行环境 | https://www.python.org/downloads/windows/ |
| pnnx | PyTorch/ONNX 转 ncnn | https://github.com/pnnx/pnnx/releases |
| InsightFace buffalo_sc | SCRFD 与 MobileFaceNet 原始模型 | https://github.com/deepinsight/insightface/releases/tag/v0.7 |

说明：

- 仓库已包含 ncnn x64 头文件、库和运行时，无需再次下载 ncnn 才能编译；官方链接用于升级或重新获取。
- Qt 安装包通常不包含 `QMYSQL` 驱动，需要按本文档第五节单独编译。
- 海康 MVS SDK 为专有 SDK，不随仓库分发，需要安装后通过 `MVS_SDK` 指向本机路径。
- InsightFace 模型仅限非商业研究用途，模型产物不随仓库提交。

---

## 目录结构

```text
QT_Face/
├─ QT_Face.pro            qmake 工程（OpenCV / ncnn / QtSql / MVS 配置 + 构建后自动部署）
├─ opencv.pri, ncnn.pri   第三方依赖配置
├─ deploy-runtime.ps1     构建后部署脚本（Qt/ncnn/MVS/OpenCV 运行时 + 模型 + 配置）
├─ config.ini.example      运行时配置模板（复制为 config.ini 后使用）
├─ config.ini             运行时配置（不入库，需自行创建，见下方“配置”）
│
├─ main.cpp               程序入口
├─ mainwindow.*           主界面与业务编排
├─ cameraManager.*        ① 海康 MVS 采集
├─ localcamera.*          本机 UVC 摄像头采集
├─ imagepreprocess.*      ② 预处理 + ⑤ 人脸质量判断
├─ facedetector.*         ③ SCRFD 检测（ncnn）
├─ facealigner.*          ④ 5 点对齐裁剪
├─ faceextractor.*        ⑤ ArcFace 特征提取（ncnn）
├─ facedatabase.*         ⑥ MySQL 模块（特征入库 / 1:N 比对 / 日志）
├─ facepipeline.*         流水线编排（独立线程）
├─ imagestorage.* / videorecorder.*   图像转换 / 视频录制
├─ appconfig.*            配置读取（含 DPAPI 口令解密）
├─ imagedata.h            共享数据结构
│
├─ include/, lib/         海康 MVS SDK 头文件与导入库（不入库，直接使用已安装的 SDK）
├─ thirdparty/ncnn-...    ncnn 预编译库
├─ models/                模型说明与转换脚本（模型产物不入库）
├─ sql/                   建表与迁移脚本
├─ docs/                  编译部署说明、数据库运行账号说明
└─ tools/                 DPAPI 口令设置脚本
```

---

## 快速开始

### 1. 依赖准备

安装 Qt 6.12.0（MSVC 2022 64bit）、OpenCV 5.0.0、海康 MVS SDK、MySQL 8.0。
OpenCV 与 MVS 的路径可在 [`opencv.pri`](opencv.pri)、[`QT_Face.pro`](QT_Face.pro) 中修改，
也可在 qmake 命令行覆盖：

```powershell
qmake QT_Face.pro OPENCV_ROOT=D:/path/to/opencv MVS_SDK="C:/Program Files (x86)/MVS/Development"
```

Qt6 官方安装包默认不含 `QMYSQL` 驱动，需自行编译后放入 Qt 的
`plugins/sqldrivers/`，并保证运行目录有 `libmysql.dll`，详见
[编译与部署说明 §五](docs/编译与部署说明.md)。

### 2. 初始化数据库（管理员执行）

```bat
:: 新库
cmd /c "mysql -u root -p < sql\schema.sql"
:: 旧库升级（先备份）
cmd /c "mysqldump -u root -p face_db > face_db_backup.sql"
cmd /c "mysql -u root -p < sql\migrate_personnel_source.sql"
```

迁移脚本会建表、加 `is_active`/外键、创建 `face_cache_revision` 与 6 个缓存触发器，
并写入 `face_schema_meta.schema_version = 1`。**程序自身不执行任何 DDL**，
未迁移时会在启动阶段明确拒绝运行。

### 3. 创建 config.ini

程序读取 **exe 同目录** 的 `config.ini`，该文件不入版本库。仓库提供了完整模板
[`config.ini.example`](config.ini.example)，复制一份改名即可：

```powershell
Copy-Item config.ini.example config.ini
```

至少要按现场改 `[Database]` 段：

```ini
[Database]
host=127.0.0.1
port=3306
name=face_db
user=face_app
password=
passwordDpapi=
driver=QMYSQL
cacheSyncIntervalMs=1000
```

其余模型路径、识别阈值、质量门限、抓拍目录等在模板中已给出默认值，按需调整。

运行账号建议使用**仅 DML** 的专用账号（不要用 root）：

```sql
CREATE USER 'face_app'@'localhost' IDENTIFIED BY '替换成高强度密码';
GRANT SELECT                       ON face_db.face_employee       TO 'face_app'@'localhost';
GRANT SELECT, INSERT, DELETE       ON face_db.face_feature        TO 'face_app'@'localhost';
GRANT SELECT, INSERT               ON face_db.face_log            TO 'face_app'@'localhost';
GRANT SELECT                       ON face_db.face_cache_revision TO 'face_app'@'localhost';
GRANT SELECT                       ON face_db.face_schema_meta    TO 'face_app'@'localhost';
```

数据库口令可用 Windows DPAPI 加密后写入 `passwordDpapi`（`password` 留空），
由**实际启动程序的 Windows 用户**执行：

```powershell
.\tools\set-db-password-dpapi.ps1 -ConfigPath .\config.ini
```

> 注意：DPAPI 密文与当前 Windows 用户绑定，复制到别的机器/用户下无法解密，
> 它防的是“配置文件被拷走复用”，并不改变程序读取口令的流程。

### 4. 构建与部署

```powershell
mkdir build; cd build
qmake ..\QT_Face.pro OPENCV_ROOT=D:/path/to/opencv   # 占位路径，换成你的 OpenCV 根目录
jom            # 或 nmake
```

链接完成后 `deploy-runtime.ps1` 会自动把 Qt / ncnn / MVS / OpenCV 运行时、模型与
`config.ini` 拷到输出目录（若源码里还没有 `config.ini`，会自动用 `config.ini.example`
生成一份，此时需在输出目录中补齐数据库账号与口令）。把输出目录整体复制到目标机即可运行。

---

## 数据库设计

| 表 | 用途 | 写入方 |
|---|---|---|
| `face_employee` | 人员主数据（工号/姓名/部门/`is_active`） | **MySQL 管理员** |
| `face_feature` | 人脸特征（一人最多 5 张模板，外键级联删除） | 程序 |
| `face_log` | 识别日志（工号/姓名/相似度/结果/抓拍图路径） | 程序 |
| `face_cache_revision` | 缓存版本号（触发器递增，程序轮询） | 触发器 |
| `face_schema_meta` | 结构版本（`schema_version >= 1` 才允许启动） | 迁移脚本 |

---

## 项目文档

- [编译与部署说明](docs/编译与部署说明.md) —— 环境、编译、部署、注册流程、排坑清单
- [数据库运行账号与 DPAPI 实施说明](docs/DB_RUNTIME_ACCOUNT_AND_DPAPI.md) —— 权限边界、DPAPI、验证记录
- [风险登记与排障手册](docs/RISK_REGISTER.md) —— 已知风险、常见故障检查与恢复步骤
- [第三方组件与许可证声明](THIRD_PARTY_NOTICES.md) —— ncnn、OpenCV、Qt、InsightFace 等
- [models/README.md](models/README.md) —— 模型来源、重新转换步骤、输出布局与排坑
- [sql/schema.sql](sql/schema.sql) / [sql/migrate_personnel_source.sql](sql/migrate_personnel_source.sql) —— 建库与迁移

---

## 问题交流

如果遇到编译、部署、摄像头接入、人脸识别或数据库相关问题，可以发送邮件到：

```text
zwj8475364@gmail.com
```

也可以直接在 GitHub Issues 中提问。提交问题时建议附上：

- 操作系统和程序版本；
- 摄像头型号或使用的是本机 UVC 摄像头；
- 完整的错误提示和日志；
- MySQL 版本及 `face_schema_meta.schema_version`；
- 已执行的排查步骤。

---

## 说明与注意

- **模型产物 `.param`、`.bin`、`.onnx` 等均不入库**。来源为 InsightFace 官方模型
  （`buffalo_sc.zip` 的 `det_500m.onnx` / `w600k_mbf.onnx`）经 `pnnx` 转换。
  InsightFace 训练数据及由其训练的模型仅限非商业研究使用；获取、转换和许可说明见
  [`models/README.md`](models/README.md)。
- **海康 MVS SDK** 为用户自备的专有 SDK，路径通过 `MVS_SDK` 指定。
- 运行期产生的 `captures/`（抓拍图）与数据库内容含**人脸等个人信息**，
  已在 `.gitignore` 中排除，请勿提交到版本库。

## 第三方组件

| 组件 | 许可 |
|---|---|
| ncnn | BSD-3-Clause，许可文本见 `thirdparty/ncnn-20260526-windows-vs2022-shared/LICENSE.txt` |
| OpenCV | Apache-2.0 |
| Qt | LGPL-3.0 / GPL（按 Qt 官方条款使用） |

本仓库以动态链接方式使用 Qt（LGPL-3.0），不涉及对 Qt 本身的静态链接或修改。

---

## 许可证

本项目基于 [MIT License](LICENSE) 开源。

版权声明见 [LICENSE](LICENSE)：`Copyright (c) 2026 Mr-Zeng-bot`。
第三方组件（ncnn / OpenCV / Qt）及海康 MVS SDK、InsightFace 模型各自遵循其上游
许可，不在本许可范围内。完整说明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
