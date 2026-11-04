# 透眉透瞳 (See-Through Brows / Eyes)

眉毛、睫毛、瞳孔被刘海挡住时，把它们重新叠回头发上面，让表情透过刘海读得出来——手绘动画的常见画法。
参考效果：UFSH《用虚幻引擎5为〈幻塔〉定制高品质动画流程风格化渲染管线》的四档对比（不透眉眼 / 仅透眉 / 半透眉眼 / 全透眉眼）。

实现是引擎里一个"逐材质叠加 pass"（`Moon_MeshPass_FaceOverlay`）：登记过的眉眼网格在光照之后再画一次，
只画被同角色头发挡住的片元，按不透明度叠到已经算好光照的头发上。从头发的角度看，等价于"把眼睛区域的头发调透明"。

## 配置一个角色

材质必须基于 **M_MoonToon**（VRM4U 导入的 MToon 实例要先转换）。

### 1. 眉眼材质：打开叠加

在眉毛 / 睫毛 / 瞳孔 / 眼睛高光材质实例上勾 **`Enable See Through Overlay`**（组 `24 - See Through Overlay`）。
这是静态开关：关着的材质完全不进 pass，零开销。

**重新生成 M_MoonToon.dsm 之后要检查这个开关**——UE 按 ExpressionGUID 匹配静态参数覆盖，生成器换了 GUID 时实例会悄悄丢掉勾选。

### 2. `Through Hair Opacity`（"半透"档的强度）

| 部位 | 建议值 |
| --- | --- |
| 眉毛 | 0.6 – 0.7 |
| 睫毛 | 0.55 – 0.65 |
| 瞳孔 | 0.35 – 0.5 |
| 眼睛高光 | 0.45 – 0.55 |
| 眼白（可选） | 0.25 – 0.35 |

不要超过 1：≥1 时整块替换，发丝梢被吃掉，看起来像"瞳孔穿过头发"。想要全透请用下面的角色档位，不要把这个值拉满。

### 3. `Through Hair Part`（这个材质算眉还是算眼）

角色档位按"眉 / 眼"两组控制。`0` = 自动（标了眉毛特性 EYEBROW 的算眉，其余算眼），`1` = 眉，`2` = 眼。
睫毛卡片如果用普通 toon 着色，自动会算成眼，一般不用改；眉毛如果没标 EYEBROW 特性，请手动设 `1`。

### 4. 角色档位（四档切换）

在角色的 **MoonToonActorComponent** 上，分类 `Toon Actor|Face Overlay`：

| `Face Overlay Mode` | 效果 | (眉, 眼) 强度 |
| --- | --- | --- |
| Authored（半透眉眼，默认） | 材质里调的 `Through Hair Opacity` | (1, 1) |
| Off（不透眉眼） | 什么都不透 | (0, 0) |
| Brows Only（仅透眉） | 眉按材质值，眼不透 | (1, 0) |
| Full（全透眉眼） | 眉眼完全压在头发上 | (2, 2) |
| Custom | 用 `Face Overlay Brow Strength` / `Eye Strength` | 0 – 2 |

强度 0–1 按比例缩小材质不透明度，1–2 推向完全不透明（推到 `saturate(4a)`，所以遮罩的软边仍然是软的）。

这几个属性都是 `Interp`，**Sequencer 可以直接按镜头 key**；也有蓝图函数 `SetFaceOverlayMode` / `SetFaceOverlayBrowStrength` / `SetFaceOverlayEyeStrength`。
数值走组件每帧上传的 ToonActorTexture 行（第 2、3 行的 `.w`），切换不创建 MID、不触发 shader 编译。
没有 MoonToonActorComponent 的角色一律按 Authored 处理。

### 5. 光照

透出来的眉眼用和延迟光照**同一套 toon 主光计算**（`ToonBxDFCore`）：主光明暗带、diffuse ramp、SDF 脸部阴影、角色光向覆盖与曝光倍数、眼睛高光，
外加 toon 环境光（天光 SH 平均 + GI 颜色/强度/去饱和/底光，和 `MoonToonComposeIndirect` 同一公式，去掉 Lumen 项——被遮住的像素拿不到自己的 GI）。
不带投射阴影：那个像素上的阴影属于前面的头发。

- `r.MoonToon.FaceOverlay.ToonLit 1`（默认）；`0` 回到旧的平光近似（天光 + 半兰伯特）。
- 只在 Substrate 存储下生效；legacy（`r.Substrate=0`）一直用平光近似，自动部位也一律算眼。
- 场景没有 Moon 主光时自动退回平光。
- `r.MoonToon.FaceOverlay.LitBlend` 仍然是"原色 ↔ 受光"的混合。

### 5.5 头发厚度感（侧面能不能看见）

透出强度随"头发离眉眼的距离"衰减：每远 `Through Hair Thickness` cm 减半（0 = 引擎默认 `r.MoonToon.FaceOverlay.Thickness`，6 cm；负数 = 不衰减）。
刘海贴着眉毛时透得明显，侧面隔着厚厚的侧发只剩淡淡的影子，两侧被不同发片盖住时强度也自然不同——这就是鸣潮那种"头发有厚度"的读感。
衰减作用在材质的透明度上、角色档位之前，所以"全透"档仍能把它推满。

想让某个角色侧面也看得清楚：把它眉眼材质的 `Through Hair Thickness` 调大（8–12）；想让侧面完全看不见：调小（2–3）。

视角淡出（`Through Hair View Fade Start/End`）默认值从 0.12/0.40 放宽到 0.02/0.15，只裁几乎完全侧对的薄片；侧面的淡出交给厚度衰减。
**`Use Front Hair Depth Gate`（旧 MF_TransparentFrontHair 移植）会按头朝向在约 45°–70° 之间把透视归零，侧面永远看不见，新角色不要开。**
眼睛的 `Through Hair Depth Threshold` 不要太小：侧发离眼睛常有 4–8 cm，阈值 3–4 cm 会把侧面直接切掉（ww_yy 用 12）。

### 5.6 俯视淡出（从上往下看不透）

从上往下看时刘海是顺着视线盖在眉眼上的，透出来像五官浮在头发上面。`Through Hair Top View Fade Start/End`（度，默认 35 / 55）：
视线高出**头部水平面** Start 度开始变淡，到 End 度完全不透；Start ≥ 90 关闭。

- "头部水平面"由脸朝前方向决定（`Head Forward From Baked Face UV` 或 `HeadForward` 参数）：上方向 = 世界 Z 去掉朝前分量。
  所以**低头也算俯视**（机位平视、角色低头时一样会淡），**左右转头 / 侧面永远不算**。没有朝向来源时退回世界 Z。
- 作用在材质透明度上（角色档位之前），超过 End 后连登记都取消，"全透"档也不会再透。
- ww_yy（冻结姿势同机位 A/B，距眼 45 cm）：30° 两者一样照常透；40° 开始变淡；50°–55° 关掉淡出时还能看到睫毛线，开了就没有。
  想让高机位也保留一点：把 End 提到 65–70；想更早收掉：Start 降到 25–30。

### 6. 发丝线条压在上面

- **内描线**（屏幕空间线）：`r.MoonToon.FaceOverlay.HairLinesOnTop 1`（默认）让叠加在内描线之前画，头发的发丝线保留在透出的眉眼上面；`0` 是旧顺序（眉眼盖住线）。
- **头发描边外壳**：在头发对应的**描边**材质实例（M_MoonToonOutline）上勾 `Write Face Overlay Occluder`，类别保持 `9`（组 `10 - See Through`）。
  眉眼不接受类别 9，所以在发丝轮廓线处会让开。**前提：头发本体也要登记成类别 8**（M_MoonToon 的 `Write Face Overlay Occluder`），
  否则发片内部最近的登记面就是描边外壳，眉眼整片都不透。

### 7. 眉眼自己的描边一起透

在眉 / 眼对应的**描边**材质实例上勾 `Enable See Through Overlay`（组 `10 - See Through`），
`Through Hair Opacity` 一般和眉眼本体设成一样，`Through Hair Part` 默认 `1`（眉），给眼睛描边改成 `2`。
描边外壳是背面绘制的，所以它的 N·V 视角淡出被关掉了。

### 8. 其他逐材质调节（组 `24 - See Through Overlay`）

| 参数 | 默认 | 用途 |
| --- | --- | --- |
| `Through Hair Tint` | 白 | 只给透出部分染色 |
| `Through Hair Depth Threshold` | 0 | 遮挡物最远距离 cm，0 = 用全局 30。长发角色远处头发误触发时降到 15–20 |
| `Through Hair Depth Fade` | 0 | 阈值前 N cm 内淡出，只在 Threshold > 0 时读 |
| `Through Hair View Fade Start` / `End` | 0.02 / 0.15 | 网格法线 N·V 淡出窗口，只裁几乎侧对的薄片；侧面强弱交给厚度（5.5） |
| `Through Hair Thickness` | 0 | 头发厚度衰减 cm，0 = 引擎默认 6，见 5.5 |
| `Through Hair Top View Fade Start` / `End` | 35 / 55 | 俯视淡出（度，相对头部水平面），见 5.6 |
| `Through Hair Occluder ID A` / `B` | -1 | 额外接受的遮挡类别（帽檐、发饰），见下 |
| `Through Hair Mask Source` / `Channel` | 0 | 脸、眉、眼共用一个材质时，用打包遮罩图在材质内部划范围 |

## 头发以外的遮挡物（帽檐、发饰、眼镜框）

遮挡物登记走专用遮挡层（#10），不再用 CustomDepth / CustomStencil：

1. 遮挡物的材质勾 `Write Face Overlay Occluder`，设 `Face Overlay Occluder Class`（1–255；8 = 默认接受的"前发"）。
2. 非 8 的类别，在眉眼材质的 `Through Hair Occluder ID A/B` 里写上同一个数字表示接受。

没有任何登记的场景走回退规则：遮挡物是皮肤 / 眼睛 / 眉毛就不透，其他（头发、帽子、未标记材质）都透。
所以**头发不需要再设特定 ShadingFeatureID**（旧文档里的 2/4/5 要求已作废）。

## 判定规则（片元必须全部满足才画）

| # | 条件 | 作用 |
| --- | --- | --- |
| 0 | 片元朝向相机 | 防止后脑勺透出另一侧的眼睛 |
| 1 | 确实被挡住，且遮挡物距离 ≤ 阈值 | 只认贴在脸上的头发 |
| 2 | 遮挡物是同一个 toon 角色 | 排除墙、别的角色 |
| 3a | 遮挡物不是另一个登记的眉眼部件 | 眉眼之间的层次交给真实深度 |
| 3b | 有登记遮挡物时：类别被接受 | 帽檐/发饰按类别放行，头发描边（9）让开 |
| 3c 回退 | 没有登记时：遮挡物不是皮肤/眼/眉 | 闭眼的眼皮、脸颊、手不透 |

之后不透明度依次乘：角色档位、深度羽化、N·V 视角淡出。

另外两条（2026-10-30 遮挡测试后补上）：

- **非 toon 遮挡物一律不透**（Substrate）：道具、墙、普通材质的武器挡在脸前，眉眼不会画到它上面。以前没有角色 ID 的角色会 0 == 0 放行，把眉眼画在任何离脸 30 cm 以内的道具上。
- **登记了头发时，头发前面有没登记的东西就不透**：自己的袖子、帽檐挡在登记过的刘海前面时，眉眼不会画上去。

## 遮挡测试结果与已知限制

在 DevTest 的 ww_yy（有角色 ID）和 SK_Lin（无角色 ID）上，用一块小板子挡在眼前测过：

| 情况 | 结果 |
| --- | --- |
| 普通道具挡在刘海前面 | 不透 ✓ |
| 别的角色 / 别的物体的 toon 头发挡在前面 | 不透 ✓（角色 ID 不同；没 ID 的角色实测也没透） |
| 自己身上的 toon 部件（袖子、帽子）挡在刘海前面，**头发没登记** | **会透**：回退规则只排除皮肤 / 眼 / 眉，其余 toon 部件都当头发 |
| 同上，**头发登记了 8** | 不透 ✓ |
| 物体**夹在刘海和眼睛之间**（眼镜框、帽檐压在刘海下面） | **会透**：每个像素只看得到最近的一层（头发），看不到头发后面的东西 |

建议：
- 要透视的角色都挂 **MoonToonActorComponent**（有角色 ID，跨角色 / 道具判定才可靠）。
- 有帽子、长袖抬手这类动作的角色，把头发登记为 8（头发材质勾 `Write Face Overlay Occluder`）。
- 眼镜这类夹层物体目前没有办法。可行的做法是加一个"阻挡层"：眼镜材质登记成阻挡类别，单独画一层最近深度，眉眼片元前方只要有阻挡层的表面就不透（和现在的遮挡登记层同一套机制，多一张深度 + ID 目标）。尚未实现。

## 全局开关

```
r.MoonToon.FaceOverlay 1                     // 总开关，也是 A/B 对照用
r.MoonToon.FaceOverlay.DepthThreshold 30     // 材质阈值为 0 时的默认值 (cm)
r.MoonToon.FaceOverlay.FadeRange 10          // 材质淡出为 0 时的默认值 (cm)
r.MoonToon.FaceOverlay.HairStencil 8         // 默认接受的遮挡类别；-1 关闭登记层判定
r.MoonToon.FaceOverlay.ToonLit 1             // toon 主光光照（Substrate）
r.MoonToon.FaceOverlay.Thickness 6           // 头发厚度衰减：每 N cm 透出强度减半；0 = 不衰减
r.MoonToon.FaceOverlay.HairLinesOnTop 1      // 内描线画在透出的眉眼上面
r.MoonToon.FaceOverlay.LitBlend 1            // 0 = 材质原色，1 = 受光
```

## 排查

| 现象 | 看哪里 |
| --- | --- |
| 重生成母材质后全部失效 | 静态开关 `Enable See Through Overlay` 丢了，逐个实例重新勾 |
| 完全没效果 | 静态开关；角色档位是 Off；看的是后脑勺；头发描边登记了 9 但头发本体没登记 8 |
| 只有眉透、眼不透（或反过来） | 角色档位是 Brows Only；或材质 `Through Hair Part` 分错组 |
| 瞳孔像"穿过头发"、发丝梢被吃掉 | `Through Hair Opacity` ≥ 1，降到 0.35–0.5；要全透用 Full 档 |
| 透出的眉眼比脸亮 / 暗 | `ToonLit` 是不是 1、是不是 Substrate；场景有没有 Moon 主光 |
| 远处几何误触发 | 降 `Through Hair Depth Threshold` 到 15 |
| 侧脸有残留 | `Through Hair View Fade Start` 提到 0.2 |

做前后对比时先把角色动画冻住（骨骼网格 `Global Anim Rate Scale` = 0），再切 `r.MoonToon.FaceOverlay 0/1`。

## 源码

| 部分 | 文件 |
| --- | --- |
| Mesh pass、绘制状态、cvar | `Engine/Source/Runtime/Renderer/Private/Toon/ToonPass_FaceOverlay.{h,cpp}` |
| 与内描线的先后 | `Engine/Source/Runtime/Renderer/Private/Toon/ToonPass_Light.cpp` |
| 像素着色器（判定、档位、toon 光照） | `Engine/Shaders/Private/Toon/ToonFaceOverlayPS.usf` |
| 材质节点（5 个引脚） | `Engine/Source/Runtime/Engine/Public/Materials/MaterialExpressionMoonToonFaceOverlay.h` |
| 角色档位 | `Engine/Source/Runtime/Engine/Classes/Components/MoonToonActorComponent.h`、`ToonActorProxy.h`、`ToonActorTextureCS.usf` |
| 材质参数 | `Plugins/MoonToon/DShader/MaterialFunctions/Shared/MF_ToonSeeThroughOverlay.dsf` |
| 输出连线 | `Plugins/MoonToon/DShader/Materials/Core/M_MoonToon.dsm`、`M_MoonToonOutline.dsm` |

节点引脚：0 = 颜色 + 不透明度，1 = 阈值/淡出/视角窗口，2 = 额外接受的遮挡类别，3 = 本材质的遮挡类别，4 = 部位（0 自动 / 1 眉 / 2 眼）。
加引脚需要重编引擎并重启编辑器。
