# Sparkium CPU 渲染评测方法

## 配置与编译流程

评测环境以工作区内的 LongMarch 工程为被测对象，采用 Release 配置，通过 vcpkg 提供依赖，并使用 Ninja 构建 `demo_sparkium_cpu_cli`：

```bash
cmake -S . -B build-release \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DVCPKG_PATH=<vcpkg-root> \
  -DVCPKG_TARGET_TRIPLET=x64-linux

cmake --build build-release \
  --target demo_sparkium_cpu_cli \
  -j 2
```

构建并确认目标存在后，评测程序依次调用 CPU CLI 渲染各测试项目。候选图统一使用 32 spp；参考图由 `sparkium_cli` 使用 4096 spp 生成。32 spp 的配置用于将完整评测控制在适合 2 个 CPU 核心的计算量级。

## 测试项目

| 测试项目 | 检查内容 |
| --- | --- |
| 面光源 | 面光源采样、可见性、直接照明及软阴影 |
| Cornell Box | 漫反射、多次反弹和颜色串扰 |
| 点光源 | 点光源距离衰减和阴影判断 |
| 镜面材质 | 理想镜面反射及后续光线路径 |
| 纹理材质 | 纹理坐标、双线性采样、法线贴图、Principled BSDF、各向异性 GGX 和玻璃折射 |

每个项目独立渲染和评分。任何项目低于阈值都会使整个评测失败，平均分不参与通过判断。

## 图像预处理

候选图和参考图转换为取值范围 `[0, 1]` 的 RGB 浮点数组。所有输出均为不透明图片，因此不读取或评价 alpha 通道。尺寸不一致时，该项目直接记为 0 分。

sRGB 按以下分段公式转换为线性 RGB：

```text
C_linear = C_srgb / 12.92                              C_srgb <= 0.04045
C_linear = ((C_srgb + 0.055) / 1.055) ^ 2.4           其他情况
```

## 单项指标

每张图计算三个互补指标。

1. **线性 RGB RMSE**衡量绝对辐射值差异：

   ```text
   rmse = sqrt(mean((candidate_linear - reference_linear)^2))
   rmse_score = max(0, 1 - rmse / 0.20)
   ```

2. **亮度 SSIM**衡量结构、轮廓和局部对比度。亮度在线性 RGB 上按 Rec.709 权重计算：

   ```text
   Y = 0.2126 R + 0.7152 G + 0.0722 B
   ```

   SSIM 使用高斯权重，`sigma=1.5`、`data_range=1.0`，结果限制到 `[0, 1]`。

3. **CIEDE2000 色差**衡量感知颜色差异。显示 sRGB 转换到 CIELAB 后，逐像素计算 CIEDE2000 色差并取平均：

   ```text
   color_score = exp(-mean_delta_e_00 / 20)
   ```

## 综合评分

单张图片的综合分数为：

```text
image_score = 0.55 * SSIM
            + 0.30 * rmse_score
            + 0.15 * color_score
```

每个测试项目的通过阈值为 `0.80`。该阈值使用与 GPU 路径对齐的 CPU 实现在 32 spp 下校准，以容纳低采样率固有的蒙特卡洛噪声。

最终结果遵循以下规则：

```text
resolved = 所有 image_score >= 0.80
score = min(image_score)
```

结果说明同时记录每个项目的综合分数、SSIM、线性 RGB RMSE 和平均 CIEDE2000 色差，便于判断回归来自结构、亮度还是颜色偏差。
