# Scene Stage S00 Gate — 基线冻结、FG 隔离与前置缺陷

## Result

- Gate version: `S00-1.2`
- Plan version: `2.3+user-s00-blockers-2026-07-31.2`
- Status: `InProgress`
- Evaluation: `NotEvaluated`
- Input baseline: `664b4308fa5879ba83ad1523262cc1bc3df6f7ac`
- Result commit: `Pending`
- S00-1.2 delta: C adds deterministic alpha/LabPBR emission controls plus hidden-source/final-positive emitter diagnostics; D runs offsets 0/1/environment-ready once each after the user waived the 20-repeat stress, and its preview-versus-Still brightness hardware comparison is also `WaivedByUser`/non-blocking while the code fix and CPU regression remain; B keeps its product fix and CPU/build regressions while its subsequent runtime/hardware image matrix is `WaivedByUser` and non-blocking. All waivers were authorized on 2026-08-01 and must never be reported as `Passed`.
- Next action: final S00 bugfix handoff and stop；不执行设备关机（S01–S18 deferred by latest user instruction）

本文件先于产品修复建立并冻结 S00 的机器契约。后续结果只能填入证据，不能静默放宽阈值；若确需改变契约，必须提升 gate version、说明原因并保留旧结果。

`S00-1.1` 只扩展了 `S00-1.0` 已要求的固定旋转输入：新增项目自有的 60 Hz/每帧 3° Bedrock animation fixture 及其 manifest hash。全部原阈值和四个 geometry fixture 保持不变；`S00-1.0` 仍由 gate-definition commit `8656251` 保存。

## Scope

S00 冻结当前 RR、SR、Still、LabPBR、World/Sky 基线，保持 FG 默认 Off，隔离历史 PRE-PERF/PRE-DC 证据，冻结第三方参考与许可证，并关闭五个用户确认的前置缺陷：

1. S00-A：PT 已开启并成功 Present 后，热导入大量 Cube 的 Bedrock 模型会卡死。
2. S00-B：RR+FG 与 SR+FG 下旋转物体出现左右两份残影。
3. S00-C：仅 PT 下隐藏 Group/Bone 后 Cube 仍可见。
4. S00-D：切换用户所述特殊背景后静帧渲染偶发无法开始/完成；成功输出还可能丢失 Bedrock 角色与 World/Sky、只剩地形和黑背景，且同场景 Still 明显比实时预览更亮。
5. S00-E：静帧设置点击下方尺寸/采样控件时，焦点/输入错误进入文件名框。

审查范围遵守用户边界：只处理崩溃、挂死、设备/数据故障、明确错误结果、构建/测试/运行失败和有证据的严重巨大性能问题；不为风格、纯重构、微小 UI/性能偏好扩展 S00。原 v2.3 明确功能与 gate 不受此边界削减。

## Versioned Inputs

- Machine thresholds: `assets/scene_tests/s00/s00_gate_spec.json`
- Fixture authority: `assets/scene_tests/s00/fixture_manifest.json`
- Fixture generator: `tools/scene_tests/generate_s00_bedrock_fixtures.py`
- Fixture source/license note: `assets/scene_tests/s00/SOURCES.md`
- Reference/license authority: `docs/open_source_reference_ledger.json`
- FG result: `docs/scene_stage_S00_fg_revalidation.md`
- Recovery state: `docs/long_term_scene_plan_manifest.json`

Generated fixtures are written below `artifacts/scene_stage_S00/generated_fixtures/`; user-private models are local reproducers only and must never be staged or published.

S00-B 的固定运动输入为
`bedrock_s00_rotation_3deg_per_60hz.animation.json`，由同一 generator v4 生成；runtime hook 必须按 render frame/60s 采样，不能依赖 wall-clock autoplay。

## Fixed Commands

Run from the main worktree root only. The registered historical detached worktree under `.tmp/` is out of scope.

```powershell
py tools\scene_tests\generate_s00_bedrock_fixtures.py `
  --output-dir artifacts\scene_stage_S00\generated_fixtures `
  --verify-manifest assets\scene_tests\s00\fixture_manifest.json

cmake --preset s00-gate

cmake --build --preset s00-gate-release --parallel 8

ctest --preset s00-gate-release

.\out\build\s00-gate\Release\xpbd_cli.exe --help
```

Hardware runner contract（在 S00 实现中按本定义建立）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File tools\scene_tests\run_s00_hardware_gate.ps1 `
  -App .\out\build\s00-gate\Release\xpbd_baker_app.exe `
  -Spec assets\scene_tests\s00\s00_gate_spec.json `
  -EvidenceRoot artifacts\scene_stage_S00
```

每条命令期望退出码 `0`。外部 runner 必须按精确 PID 执行墙钟 watchdog；`XPBD_UNATTENDED_FRAMES` 不能代替超时。

## Build and CPU Tests

必需 targets：

- `xpbd_baker_app`
- `xpbd_cli`
- `xpbd_viewport_regression_tests`
- `xpbd_app_session_regression_tests`
- `xpbd_check_spirv`

CTest 必须发现并通过：

- `xpbd_viewport_regression`
- `xpbd_app_session_regression`

每测试上限 120 秒；0 tests、非零退出、SPIR-V 校验失败或编译 warning 以外的 build error 均失败。当前旧 `LastTestsFailed.log` 与现有二进制不绑定，不能作为通过或失败结论；最终 result commit 必须重新运行并保存完整日志。

Gate-definition validation（非最终 result evidence）：2026-07-31 在独立 build dir 完成 configure，Release 五个 targets 与 SPIR-V 通过；CTest 2/2（1.29 秒）和 CLI help exit 0。产品修复后必须在最终 commit 重新运行。

## Runtime Gate

### S00-A — PT hot import

- 启动 Vulkan，开启 PT，先成功 Present 30 帧，再触发真实 `AppSession::loadModel` 导入。
- 依次运行 8-Cube、1,280-Cube、2,624-Cube 与 144-bone/10,368-Cube 高密度自有 fixture；至少覆盖 Raw/FG Off 与 Raw/FG On。最后一项匹配用户本机 reproducer 的骨骼/Cube 计数分布，但不复制其内容。
- primary/extended/user-scale 导入分别不超过 120/240/300 秒；导入期间 Present heartbeat 最大间隔 5 秒。
- 必须看到 begin、commit、RT-ready、post-import-present、clean-exit 事件；导入后再 Present 至少 30 帧。
- cancellation/failure 在 10 秒内确认，旧场景及 generation 保持不变；任何 source hash 改变、device lost、无限 fence wait、进程强杀才结束均失败。
- UI 线程响应性与 GPU AS build 分开记录；不得用关闭 PT/FG、减少 gate Cube 数或只做 startup import 通过。

### S00-B — RR/SR + FG rotating ghost

The user waived the subsequent automated runtime/hardware rotation image matrix on 2026-08-01. Its modes and image thresholds remain below as the versioned historical definition, but the matrix is non-blocking and must be reported as `WaivedByUser`, never `Passed`. The root-cause product fix, unified Release build, and pure CPU regressions remain required.

- 固定 seed `20260731`、相机、角色、角速度 3°/真实帧；预热 30 个真实帧并捕获 48 个 present frames。
- 矩阵：RR、SR Quality、Raw+FG、RR+FG、SR+FG；记录 color/depth/motion AOV、current/previous transform、frame index、Streamline tags/options/reset reason。
- 合成帧只能有一个时空连续主体轮廓：次要连通域面积比 ≤ 0.02，左右 side-lobe energy 比 ≤ 0.05，与期望插值 pose 的主轮廓 IoU ≥ 0.90。
- 启动、稳定旋转、停止、重新启动和显式 history reset 都要覆盖；不得清空合法 motion 或关闭 FG 规避。

### S00-C — PT Group/Bone visibility

Portable C coverage loads `bedrock_s00_alpha_emissive.png` with its LabPBR sidecar `bedrock_s00_alpha_emissive_s.png`. A hidden source-emissive subtree must report at least one `rt_hidden_source_emitter_triangle_count`, while `rt_hidden_positive_weight_triangle_count` must remain zero; these are read-only diagnostics and do not change sampling weights.

- Raster 是正确 control；PT 必须在 Raw、RR、SR 与三个 FG 组合中动态 hide/unhide。
- small fixture 基线 8 Cube：隐藏 `branch_000_depth_00` 后解析可见 4 Cube；只隐藏叶子 `branch_000_depth_01` 后可见 6；unhide 恢复 8。
- hidden rigid instance mask 必须 `0`、visible `0xFF`；hide/show 的 BLAS build/refit delta 必须 `0`，packed primitive identity 与总 BLAS range 保持稳定，只允许 TLAS update。
- 同一真实模型需记录 `DownBody`+后代失败与 `UpperBody` 正常的组内对照；portable fixture 同时覆盖 opaque rigid group 与 alpha-tested/blended control，修复后两者必须使用同一 visibility 语义，不能依赖 any-hit 是否执行。
- hidden emissive positive-weight count 必须 `0`；CPU picking、RT ID/depth/color AOV 不得命中隐藏子树；history reset 只发生于声明的 visibility transition。
- target ROI mean absolute delta ≥ 0.05，control ROI ≤ 0.02，unhide 对 baseline PSNR ≥ 35 dB。

### S00-D — Still lifecycle, snapshot content and display consistency

Per the user's 2026-08-01 waiver, the former 20-repeat stress is removed. Queue offsets 0, 1, and environment-cache-ready run exactly once each (three total runs). Lifecycle, content, sky, cancel, and restart correctness remain mandatory; the display-transfer code fix and CPU regression remain required, but no preview-versus-Still brightness hardware comparison is required.

The preview-versus-Still brightness hardware comparison is separately `WaivedByUser` and non-blocking. The display-transfer product fix, unified Release build, and pure CPU regressions remain required; the retained image thresholds below document the historical comparison definition and are not a `Passed` claim.

- 使用同一冻结相机、Bedrock 角色、preview surface 与非 Off World/Sky，先保存实时 PT control，再渲染 Raw 256×256×32 spp 的 PNG/EXR。
- 另在特殊背景切换后同帧立即 queue、隔 1 个 Present queue、environment cache ready 后 queue 三种时序中各运行 1 次 Raw 64×64×4 spp 短任务（共 3 次）；每次记录 World/environment generation、resolved source、cache ready 与 descriptor identity，并按 job id 看到 queue、begin、progress、readback、save、complete。输出必须能解码且 checksum 非空；0 次允许静默停滞、跳过 save 或借主 Present loop 存活误判成功，且不能要求用户手动等待。
- snapshot 必须记录并校验 model/pose/visibility/material/world/environment/camera generations、resolved instance/cube/emitter count、environment source、transparent flag、Film/Background Exposure、tone map 与 display transfer。
- 角色 mask 占图 ≥ 0.002；opaque 输出的 sky ROI 非黑像素比例 ≥ 0.50；实时与 Still 角色 silhouette IoU ≥ 0.90；PNG 与 EXR 经同一显示变换后 PSNR ≥ 38 dB。
- 同分辨率、同采样、同冻结 snapshot 的实时 PT control 与 Still Raw 必须走相同曝光/tone-map/display-transfer；非背景/非高光稳定 ROI 的中位亮度差 ≤ 0.10 EV、显示空间 mean absolute delta ≤ 0.02、PSNR ≥ 35 dB。不得使用针对 Still 的隐藏曝光补偿通过。
- opaque World/Sky 不得变黑或 alpha=0；transparent 模式只改变背景 alpha，不得删除角色。取消、重启、连续第二次渲染均需通过。
- 只输出地形、使用错误 camera、模型/环境 generation 丢失或文件虽写成但内容错误均失败。

### S00-E — Still settings input routing

- 在 zh-CN/en-US 与 100%/150%/200% DPI 下依次点击 filename、width、height、target samples、samples per submit、format、transparent background。
- pointer-hit widget 必须等于 active widget；键盘、方向键、拖动、Tab、Escape 与 IME 只能改变目标设置。
- 数值控件 active 时 filename byte-for-byte 不变；开始 Still 后 job snapshot 值必须等于 UI 显示值。
- 通过不得依赖禁用 filename、隐藏数值输入或额外点击空白处。

### Existing baseline and FG lifecycle

- 保存最终 result commit 的 Still、RR、SR 基准，不用 pre-664b430 历史图冒充。
- FG 默认 Off；Raw+FG、SR+FG、RR+FG 冒烟；On/Off 循环；真实 Resize、Minimize/restore、F11、Dialog；proxy application present fence count 必须 0；强制 FG 错误后回退 Native 并继续 Present。
- FG 生命周期失败可保持默认 Off 并单独记录，不阻断场景开发；但 S00-B 的实际双残影是渲染正确性阻断，不能用该豁免跳过。

## Failure Rules

以下任一情况立即停止 S00 实现叠加、保存日志/fixture、manifest 标 `Failed`，且不提交失败产品代码：

- build、CTest、SPIR-V、CLI smoke 非零退出；
- crash、hang、device lost、validation error、assert、exception；
- source Bedrock checksum 改变或失败/取消后场景残留；
- RR/SR 输入契约、Motion AOV、Still/preview 内容或 visibility/emitter 结果错误；
- 图像/时限阈值未达到；
- required evidence 缺失或不能绑定 result commit。

所有日志额外扫描 `s00_gate_spec.json` 的 forbidden patterns。失败不能删除用户文件或历史 PRE-PERF/PRE-DC 证据。

## Evidence and Naming

所有运行证据写入：

```text
artifacts/scene_stage_S00/<case>/<UTC timestamp>/
```

每个 case 最低产物：`command.json`、`environment.json`、`process.json`、原始 app log、runner log、metrics.json、artifact_sha256.json；图像 case 另含 lossless color/depth/motion、ROI mask、分析 JSON 和 contact sheet。文件必须记录 result commit、dirty state、GPU/driver/Windows/Vulkan/Streamline、命令、退出码、开始/结束 UTC。

用户原始证据：

| File | Bytes | SHA-256 | Distribution |
|---|---:|---|---|
| `user_pt_hidden_group_still_visible.png` | 368370 | `FF5D06090CA1AB9A97F4F2A4561125529C8E610D05C6D18968FC780FDFEF4C49` | local diagnostic only |
| `user_still_preview_character_sky_missing.png` | 1296169 | `2EF60AA14C947CC77C81F268E00446285CA3EBBDFE674227A46F0A2E7CFB5CC0` | local diagnostic only |
| `user_still_output_render006_character_sky_missing.png` | 8296178 | `683172F95F1891E37822F0E0BB008E13002428B757BEE296008216B13F1AEFA1` | local diagnostic only |
| `user_still_settings_focus_misroute.png` | 43172 | `002B144082119FDF03E3A3237C3B282EF287AF889452972810A0E9DFC1B1D73C` | local diagnostic only |
| `user_pt_downbody_hidden_fails_upperbody_control.png` | 419533 | `683E1208D5A33587E908791D4A382EF139B7D8385BA4E908E977658B85F6EB1B` | local diagnostic only |
| `user_still_brightness_brighter.png` | 1607539 | `796E32D641A7D285CC29CB86928F551FAF5873289777C93F2DDB78182FC921AF` | local diagnostic only |
| `user_preview_brightness_reference.png` | 1717084 | `75F3CBBA0FA9A5C45D6D9D2F574950ACA7BF4957A30FD6AA66C377A55B976ED8` | local diagnostic only |

本机 S00-A reproducer（不复制、不提交）：`C:\Users\shiromizu\Desktop\144圆.geo.json`，1,405,360 bytes，144 bones / 10,368 cubes，SHA-256 `0A5FB02E96DB5404035CD8F7120DD2DC58A6C2E9032EA677C92819FE5C19A2BD`。

## Reference and License Gate

- 每个 adopted reference 必须在 `open_source_reference_ledger.json` 固定 repository、commit/tag、retrieval date、SPDX、LICENSE checksum、scope、copied files/checksums、local modifications、notices 与 stage gates。
- GPL/AGPL（Blockbench、Blender 等）只做公开行为/格式/架构参考，不复制源码、UI、图标或测试资产。
- MIT/BSD/zlib/Apache-2.0 文件仍需逐文件审查、保留许可证/版权并进入 package notices。
- 不运行时下载，不跟随 upstream latest。OBJ/glTF/tangent adopted 组合在 S00 result 前必须唯一确定。

## Known Limits

- 当前文件是预实现 gate 定义，所有 runtime result 均 Pending。
- PRE-PERF/PRE-DC 与旧 FG/Still 日志只作历史输入，不能关闭任何当前 result-commit gate。
- 用户私有模型无仓库许可证，只能本机辅助复现；portable verdict 只依赖自有 fixture。

## Result Checklist

| Gate | Result | Evidence |
|---|---|---|
| Fixture determinism | Pending | `fixture_manifest.json` + run log |
| Clean configure/build/SPIR-V | Pending | build artifacts |
| CTest + CLI smoke | Pending | test artifacts |
| S00-A PT hot import | Pending | runtime artifacts |
| S00-B rotating RR/SR+FG | WaivedByUser | Product fix + CPU/build regressions retained; subsequent runtime/hardware image matrix non-blocking |
| S00-C PT visibility/emitter | Pending | count/AOV/image artifacts |
| S00-D Still lifecycle/character/sky | Pending | lifecycle/PNG/EXR/snapshot artifacts |
| S00-D preview↔Still brightness hardware comparison | WaivedByUser | Display-transfer code fix + CPU/build regressions retained; no hardware comparison claimed Passed |
| S00-E Still UI focus | Pending | focus trace/config artifacts |
| Still/RR/SR baseline | Pending | baseline artifacts |
| FG lifecycle revalidation | Pending | `scene_stage_S00_fg_revalidation.md` |
| Reference/license candidate freeze | Passed | `open_source_reference_ledger.json`; integration probes/notices deferred with S01–S05 |
| Working tree/manifest consistency | Pending | final Git evidence |

## Manual Review

机器阈值通过后，里程碑人工项只确认未豁免的 hide/unhide 视觉语义、Still 构图/天空与 UI 焦点行为。B 旋转画面矩阵和 D 亮度实机对照已由用户豁免，不在人工项中伪写为 Passed；人工观察也不能覆盖其他机器失败。

## Next

先实现并关闭 S00-A–E，再在最终干净 result commit 运行全套命令、填写本文件与 FG 复核、提交结果并更新长期 manifest。按用户 2026-08-01 最新指令，本次 S00 `Passed` 后不进入 S01；写完最终交付后停止，不执行或安排设备关机，S01–S18 保持 Deferred/NotStarted。
