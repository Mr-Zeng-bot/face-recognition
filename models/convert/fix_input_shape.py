# -*- coding: utf-8 -*-
"""
把检测模型（SCRFD）的动态输入形状固定为 1x3xNxN。

为什么需要这一步：
  SCRFD 官方导出的 onnx 输入是动态尺寸 [1,3,?,?]，转换工具（pnnx）无法折叠
  图里的 shape/size 相关算子，会生成 ncnn 不支持的 pnnx.Expression 层，模型
  加载时报 "layer pnnx.Expression not exists or registered"。把输入尺寸写死
  后这些算子会变成常量，转换结果就是干净的 ncnn 模型。

用法：
  python fix_input_shape.py det_500m.onnx det_500m_static.onnx 640

注意：尺寸必须与 config.ini 中 [Detector] inputSize 一致。
"""
import sys

import onnx


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    src, dst = sys.argv[1], sys.argv[2]
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 640

    model = onnx.load(src)
    for inp in model.graph.input:
        shape = inp.type.tensor_type.shape
        dims = [d.dim_value or d.dim_param for d in shape.dim]
        print("input:", inp.name, dims, "->", [1, 3, size, size])
        del shape.dim[:]
        for value in (1, 3, size, size):
            shape.dim.add().dim_value = value

    onnx.checker.check_model(model)
    onnx.save(model, dst)
    print("saved:", dst)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
