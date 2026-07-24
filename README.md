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

# 库 + 测试 + CLI
.\cpp\build.bat

# 桌面程序
.\cpp\run_app.bat
.\cpp\run_app.bat -gl
.\cpp\run_app.bat -vk
.\cpp\run_app.bat -d3d
```

无界面烘焙示例：

```powershell
.\cpp\run_cli.bat bake --model model.geo.json --anim idle.animation.json --out idle.baked.json --bones mid,tip
```

## 图形后端

| 启动参数 / 环境变量       | 后端                    |
|---------------------------|-------------------------|
| `-gl` / `XPBD_GFX=opengl` | OpenGL 3.3              |
| `-vk` / `XPBD_GFX=vulkan` | Vulkan（FIFO 垂直同步） |
| `-d3d` / `XPBD_GFX=dx11`  | Direct3D 11             |
| `auto`                    | Vulkan → DX11 → OpenGL  |

视口是 GPU 网格预览。工具栏可导入 PNG/JPEG 贴图。右侧「选项」含显示骨骼、MCBE 坐标系、界面语言。

相机：

- 左键拖动：环绕
- 右键拖动：地面前后左右平移
- 中键 / Shift+右键：高度
- 滚轮：缩放；Shift+滚轮：高度

## 界面语言

启动时按系统显示语言自动选择：

- English（默认）
- 简体中文
- 繁體中文（香港）
- 繁體中文（台灣）

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

