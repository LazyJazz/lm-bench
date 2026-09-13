# Sparkium CPU 渲染器实现任务

## 任务目标

在 LongMarch 工程中实现新的命令行 demo `sparkium_cpu_cli`。它应在 CPU 上加载并渲染现有 Sparkium JSON 场景，且渲染语义和收敛结果应与 `sparkium_cli` 一致。

动态场景和光栅化管线不在本任务范围内。

## 功能要求

1. 新增 CMake 构建目标 `demo_sparkium_cpu_cli`，生成可直接运行的 `demo_sparkium_cpu_cli`。
2. CPU 渲染路径不得依赖 GPU 光线追踪执行。可以通过 vcpkg 引入适合的 CPU 求交库。
3. CPU CLI 应读取与 `sparkium_cli` 相同的 JSON 场景格式，并支持场景文件目录相对路径和全局路径形式的素材引用。
4. 支持评测场景使用的相机、网格、球体、变换、纹理、材质和光源：
   - Lambertian 漫反射；
   - 理想镜面反射；
   - Principled BSDF；
   - 金属度、粗糙度、法线及各向异性纹理；
   - GGX 微表面反射和玻璃折射；
   - 点光源、面光源、双面发光材质及多次反弹。
5. 纹理坐标方向、双线性采样、颜色空间、曝光、clamp 和输出编码应与原渲染路径保持一致。
6. 光源采样、BSDF 采样、PDF、MIS 和俄罗斯轮盘赌应保持无偏，并与原渲染路径采用相同的物理含义。
7. 输出图片尺寸必须遵循场景中的 film 配置，保存为不透明 RGBA PNG。
8. `sparkium_cpu_cli` 使用 `--spp N` 指定每像素样本数，并准确渲染 `N` spp。
9. 构建产物所需的非系统运行时依赖应随目标部署，或采用静态链接，使可执行文件能从生成目录直接启动。

## 命令行接口

```text
demo_sparkium_cpu_cli <scene.json> [--output image.png] [--spp N]
```

其中：

- `scene.json` 为待渲染的 Sparkium JSON 场景；
- `--output` 或 `-o` 指定输出 PNG；
- `--spp` 必须为正整数，默认值为 1。

## 构建要求

评测会采用 Release 配置，通过 vcpkg 准备依赖，并单独构建：

```text
demo_sparkium_cpu_cli
```

实现应能在只有 2 个 CPU 核心的 Linux 环境中完成构建和渲染，不应假定存在支持硬件光线追踪的 GPU。
