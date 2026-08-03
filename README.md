# XPBD Baker（C++）

Minecraft 基岩版实体物理动画烘焙工具的 C++ 移植。仓库里的 Java 工程仍是功能对照；本目录是可独立构建的 C++23 版本。

## 致谢 / Credits

| 角色     | 名字                                          |
|----------|-----------------------------------------------|
| C++ 移植 | 卡门线                                        |
| 源java仓库 | https://github.com/ranjitmutum/xpbd_baker     |

感谢 Nuklear、SDL、Bullet、stb、spdlog 等开源项目作者。第三方授权全文在构建产物旁的 **`notices/`** 目录（见下文）。

English: [README.en.md](README.en.md)

## 环境

- Windows x64（主目标）
- CMake 3.21+
- MSVC 2022 或同等 C++23 工具链
- [vcpkg](https://vcpkg.io/)，并设置 `VCPKG_ROOT`

从源码构建 DLSS SR/RR/FG 与 Reflex 时，需要自行取得官方 NVIDIA
Streamline 2.12.0 SDK，并在配置 CMake 时传入
`-DXPBD_STREAMLINE_SDK_ROOT=<SDK目录>`。SDK 头文件、DLL 与许可证不会提交到
本仓库；可运行包只携带经签名校验的生产 DLL 和对应许可证。

## 快速开始

```powershell
cmake --preset vscode-windows-app
cmake --build --preset vscode-windows-app-release

# Vulkan（支持光栅与 RT）
.\out\build\vscode-windows-app\Release\xpbd_baker_app.exe -vk

# OpenGL 3.3 光栅回退
.\out\build\vscode-windows-app\Release\xpbd_baker_app.exe -gl
```

无界面烘焙示例：

```powershell
cmake --build out\build\vscode-windows-app --config Release --target xpbd_cli
.\out\build\vscode-windows-app\Release\xpbd_cli.exe bake --model model.geo.json --anim idle.animation.json --out idle.baked.json --bones mid,tip
```

## 图形后端

应用只保留 **Vulkan** 与 **OpenGL 3.3**。Vulkan 提供光栅与硬件 RT；
OpenGL 是光栅回退。DX11 与 Metal 后端已移除。

| 启动参数 / 环境变量         | 说明                              |
|-----------------------------|-----------------------------------|
| `-vk` / `XPBD_GFX=vulkan`   | 强制 Vulkan                       |
| `-gl` / `XPBD_GFX=opengl`   | 强制 OpenGL 3.3                   |
| `auto`（默认）              | 先尝试 Vulkan，失败后回退 OpenGL  |
| `XPBD_VULKAN_VALIDATION=1`  | 请求 Khronos Validation；不可用时记录并安全降级 |
| `XPBD_VULKAN_DIAGNOSTICS=1` | 输出 Vulkan 层、扩展与等待诊断    |
| `XPBD_ANIMATION=<path>`      | 启动时加载动画（无人值守验证用） |
| `XPBD_AUTOPLAY=1`            | 模型和动画加载成功后自动播放     |
| `XPBD_SHOW_BONES=0`          | 关闭骨骼覆盖层（无人值守 RT 验证用） |
| `XPBD_RT_DEBUG=<mode>`       | RT Pipeline 调试视图：`instance`/`primitive`/`cube`/`face`/`material`/`normal` |
| `XPBD_PT_SPP=<1..64>`        | 每个 frame slot 每帧追加的路径追踪样本数 |
| `XPBD_PT_MAX_SAMPLES=<n>`    | 每个独立历史的最大样本数；`0` 表示不限 |
| `XPBD_PT_BOUNCES=<1..64>`    | 最大路径反弹总数 |
| `XPBD_PT_DIFFUSE_BOUNCES=<0..16>` | 最大漫反射反弹数 |
| `XPBD_PT_GLOSSY_BOUNCES=<0..16>` | 最大光泽/GGX 反弹数 |
| `XPBD_PT_TRANSMISSION_BOUNCES=<0..32>` | 最大透射/折射反弹数 |
| `XPBD_PT_TRANSPARENT_BOUNCES=<0..64>` | 最大透明反弹数（Phase 7 完整使用） |
| `XPBD_PT_RR=<0|1>`           | 是否启用 Russian roulette |
| `XPBD_PT_RR_START=<1..Bounces>` | 开始 Russian roulette 的反弹数 |
| `XPBD_PT_SEED=<uint32>`      | 固定的确定性采样 seed |
| `XPBD_PT_ENVIRONMENT=<0..16>` | 临时解析 clear-sky 强度；默认 `0`（Sky Rendering Off） |
| `XPBD_PT_CAPTURE=<png>`      | 无人值守诊断：达到非零有限 `XPBD_PT_MAX_SAMPLES` 后写出一张 PT PNG；不是静帧渲染工作流 |

在支持的 NVIDIA 显卡上，可在「选项」中启用「高级预览光照（NVIDIA）」。
模型、地面、水面与预览场景共用同一个支持 alpha 的 BVH；不透明、裁切与半透明
材质分别采用正确的可见性和阴影规则。

DLSS 帧生成默认关闭，只在用户手动开启后加载。Vulkan 帧生成会关闭应用自身的
VSync，并且只在交换链可使用 `VK_PRESENT_MODE_IMMEDIATE_KHR` 时启用。G-SYNC/
VRR 仍由 NVIDIA 驱动和显示器控制；F11 无边框模式需要在 NVIDIA 控制面板中为
“窗口和全屏模式”启用 G-SYNC。程序本身不会替用户修改驱动设置。

为避免第三方覆盖层插入 Vulkan 调用链导致驱动不稳定，程序默认只在自身进程内禁用
GamePP 与 RTSS 的 Vulkan 隐式层；这不会修改系统设置或卸载相关软件。确需启用这些
覆盖层时，可设置 `XPBD_VULKAN_ALLOW_THIRD_PARTY_LAYERS=1`。

视口是 GPU 网格预览。工具栏可导入 PNG/JPEG 贴图。右侧「选项」含显示骨骼、MCBE 坐标系、界面语言。

相机：

- 左键拖动：环绕
- 右键拖动：地面前后左右平移
- 中键 / Shift+右键：高度
- 滚轮：缩放；Shift+滚轮：高度

## 界面语言

启动时按系统显示语言自动选择：

- English
- 简体中文（默认）

可在右侧「选项 / Options」里手动切换。

## Molang 与只烘焙所选骨骼

导出只会用数值关键帧替换已选择物理骨骼的 `position` 和 `rotation`。未选择骨骼的全部原始通道，以及已选择骨骼的 `scale`，都会按源动画原样保留。

开始烘焙前，GUI 会检查源动画和生效中的过渡目标：

- 未选择的祖先/碰撞依赖若含 position/rotation Molang，确认页会说明它只在本次物理计算中按 `0` 采样，导出的原 Molang 不变。
- 已选择骨骼若含 position/rotation Molang，确认页会明确列出将被数值烘焙关键帧覆盖的动画、骨骼和通道。
- 每次确认只对紧接着的一次烘焙有效；取消或完成后恢复安全默认。
- Molang scale 或非单位 scale 无法由采样器可靠处理，会阻止烘焙，不能通过确认绕过。

## CLI 参数（bake）

| 参数                      | 含义                 |
|---------------------------|----------------------|
| `--model`                 | 几何 JSON            |
| `--anim`                  | 动画 JSON            |
| `--out`                   | 烘焙输出             |
| `--bones a,b`             | 物理骨骼（默认全部） |
| `--mode xpbd\|bullet`     | 求解器               |
| `--loop auto\|once\|loop` | 循环策略             |
| `--velocity path`         | 速度缓存             |
| `--dt`                    | 步长（默认 1/60）    |
| `--assume-molang-zero`    | 明确允许本次无界面烘焙将不支持的 position/rotation Molang 按 0 采样（不能绕过 scale 检查） |
