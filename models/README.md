# models 目录说明（人脸识别模型）

本目录存放 ncnn 格式的两个人脸识别模型，程序启动时按 `config.ini` 中的路径加载：

> 当前项目的部署模型固定为下表中的 SCRFD-500M 与 ArcFace MobileFaceNet。
> 跨机器部署时应原样复制 `models/`，不要替换模型文件；更换模型会使已入库特征失效，
> 需要重新采集所有人员模板。

| 文件 | 作用 | 输入 | 输出 |
|---|---|---|---|
| `scrfd_500m.param` / `scrfd_500m.bin` | SCRFD 人脸检测 | 640×640 RGB | 人脸框 + 5 关键点 |
| `arcface_mbf.param` / `arcface_mbf.bin` | ArcFace 特征提取 | 112×112 RGB | 512 维特征 |

当前目录中的文件由 InsightFace 官方模型转换而来：

- 原始模型：`buffalo_sc.zip` 里的 `det_500m.onnx`（SCRFD-500M）与 `w600k_mbf.onnx`（MobileFaceNet 主干）
- 转换工具：`pnnx`（ncnn 官方新一代转换工具，`pip install pnnx`）
- 检测模型已把输入尺寸固定为 **640×640**，所以 `config.ini` 的 `Detector.inputSize` 必须保持 640

---

## 一、重新转换（可复现步骤）

```powershell
# 0) 准备工具与原始 onnx
pip install onnx numpy pnnx

# 1) 下载 InsightFace 模型（约 15MB）
curl.exe -L -o buffalo_sc.zip https://github.com/deepinsight/insightface/releases/download/v0.7/buffalo_sc.zip
Expand-Archive buffalo_sc.zip -DestinationPath .

# 2) 检测模型：先把输入尺寸写死成 640。否则 pnnx 会生成 ncnn 不支持的
#    pnnx.Expression 层（原因见 convert\fix_input_shape.py 顶部说明）
python convert\fix_input_shape.py det_500m.onnx det_500m_static.onnx 640

# 3) 转成 ncnn（输出为 xxx.ncnn.param / xxx.ncnn.bin）
pnnx det_500m_static.onnx
pnnx w600k_mbf.onnx

# 4) 改成工程需要的文件名
Move-Item det_500m_static.ncnn.param scrfd_500m.param  -Force
Move-Item det_500m_static.ncnn.bin   scrfd_500m.bin    -Force
Move-Item w600k_mbf.ncnn.param       arcface_mbf.param -Force
Move-Item w600k_mbf.ncnn.bin         arcface_mbf.bin   -Force
```

构建后 `deploy-runtime.ps1` 会自动把本目录的文件拷到输出目录的 `models/` 下。

---

## 二、输出布局说明（换模型时注意）

SCRFD 官方 onnx 末尾是 `Transpose + Reshape`，转换后输出是**扁平**布局：

- `score`：`w=1`、`h=格子数×anchor`
- `bbox` ：`w=4`、`h=格子数×anchor`
- `kps`  ：`w=10`、`h=格子数×anchor`

`facedetector.cpp` 同时支持这种扁平布局和「3 维特征图」布局（`w/h` 为格子数、
`c` 为 anchor 通道），并按元素总数反推 stride 与 anchor 数。因此常见的 SCRFD
转换产物可以直接使用；若换成分辨率或 anchor 数异常的模型，程序会给出
「SCRFD 输出尺寸异常」提示。

---

## 三、排坑

1. **输入尺寸**：检测模型按 640×640 转换，`config.ini` 的 `Detector.inputSize` 必须
   同样是 640，否则框会错位或直接报输出尺寸异常。
2. **归一化**：SCRFD 用 `(x-127.5)/128`，ArcFace 用 `(x-127.5)/127.5`，RGB 顺序。
   相似度普遍偏低时优先检查这里与训练时是否一致。
3. **加载时报 `layer pnnx.Expression not exists`**：检测模型没有固定输入尺寸就转换了，
   按上面第 2) 步重做。
4. **ArcFace 维度不是 512**：换成其他识别模型时，需同步改 `config.ini` 的
   `Extractor.featureDim`；已入库特征因维度不同会被跳过比对。

---

## 四、不接相机也能自检

写一个控制台工程直接调用 `FaceDetector` / `FaceAligner` / `FaceExtractor`（不需要数据库），
对一张已知人脸照片跑检测与特征提取，检查框位置、关键点、特征维度与模长是否为 1.0。
开发时用过的自检工程放在 `build/smoketest/`（临时目录，可删）。
