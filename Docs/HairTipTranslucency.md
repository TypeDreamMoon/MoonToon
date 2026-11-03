# 发梢真半透明 (Hair Tip Translucency)

刘海发梢做成真半透明：发梢的不透明部分裁掉，再用半透明重画一遍补上，眉眼从变薄的发梢里透出来，发根保持实心。
做法来自终末地实测：一张二值 UV 岛遮罩把刘海发梢分出来（约占 3% UV），所有不透明 pass 都 `clip(r − 0.5)`，
发梢只在一个 SrcAlpha 混合的 pass 里用 albedo alpha（0.63–1）重画（引擎仓 `Toon/ENDFIELD_CHARACTER_RENDERING_ANALYSIS.md`）。
异环的透脸 alpha 也是贴图值，发际 1.0 往发梢降到约 0.6。

## 配置

一张发梢遮罩，两个材质实例，同一个开关：

1. **头发实例**（这个槽位原来的实例，Blend Mode 必须是 Masked）：在 `21 - Translucency|Hair Tip` 组勾
   **`Use Hair Tip Translucency`**，把 **`Hair Tip Mask`** 指向遮罩图。它会把发梢从不透明 pass 里裁掉。
2. **重画实例**：以第 1 步的实例为父新建一个子实例，**Blend Mode 覆盖为 Translucent**，别的都继承。
   把它设成网格组件上**这个槽位的 Overlay Material**（组件 → Rendering → Material Slots Overlay Material，
   数组下标 = 材质槽位号；不要用组件级的 Overlay Material，那会把整个网格再画一遍）。
3. **描边**：这个槽位的描边实例（M_MoonToonOutline 的实例）也勾 `Use Hair Tip Translucency`，
   用同一张遮罩、同一个通道和阈值。描边外壳在发梢处被裁掉，发梢由重画负责。
   **要发梢也有描边（T49）**：以这个描边实例为父新建子实例，**Blend Mode 覆盖为 Translucent**，
   `Hair Tip Root / End Opacity` 设成和头发重画实例一样，然后把**这个槽位的描边**换成它（组件或网格的
   Moon Outline Material Slots）。半透明外壳画整圈：发根不透明，发梢按重画的透明度淡出；发梢范围里外壳的背面
   被单层裁剪去掉，发梢不会被压暗。
4. **角色要挂 MoonToonActorComponent**（有 toon 角色 id）。toon 半透明的单层裁剪只对带角色 id 的网格生效。
   没有组件时，重画会把每一层发丝都混进来，每个发梢像素显示的是最后画的那一层，通常是里层发丝。
   里层发丝落在贴图里另一块平滑的区域，于是发梢看起来"被压平"：没有明暗带，没有高光，侧缕边缘还会出现锯齿。

开关在两个实例上都是开的。实例扮演哪个角色由它的混合模式决定：
不透明或 Masked 的实例负责裁掉发梢，Translucent 的实例负责重画。没勾开关的实例和以前编译出来的着色器完全一样。

## 参数

| 参数 | 默认 | 作用 |
|---|---|---|
| `Hair Tip Mask` | 全黑（没有发梢） | 发梢遮罩，UV0 |
| `Hair Tip Mask Channel` | 0 | 读哪个通道：0 R、1 G、2 B、3 A |
| `Hair Tip Threshold` | 0.5 | 遮罩值大于等于它就算发梢 |
| `Hair Tip Root Opacity` | 1 | 重画：发梢开始处（遮罩 = 阈值）的不透明度，再乘底色 alpha。保持 1，和不透明部分无缝衔接 |
| `Hair Tip End Opacity` | 1 | 重画：发梢末端（遮罩 = 1）的不透明度，再乘底色 alpha |
| `Hair Tip Casts Shadow` | 1 | 裁剪：1 = 阴影 pass 不裁发梢，额头上的发影保持完整；0 = 阴影也裁掉 |
| `Hair Tip Hidden Push` | 2 cm | 重画：非发梢顶点沿视线往后推的距离（见“原理”） |

不透明度有两种给法：
- **二值遮罩 + 底色 alpha**（终末地）：遮罩只有 0/1，透明度画在底色贴图的 alpha 里（实例要开 `Opacity From Base Color Map`），
  Root / End 都保持 1。
- **渐变遮罩**（没有 alpha 可用时，比如 DevTest 的 ww_yy）：遮罩在发梢内部从 0.5 渐变到 1，
  Root = 1、End ≈ 0.45–0.6，发梢越往下越透。

## 遮罩约定

```
0            不是发梢：不透明绘制，重画不画
>= 阈值       发梢：所有不透明 pass 裁掉，重画补上
阈值 .. 1     （可选）发梢内部的渐变，发根 → 发梢
```

**发梢三角形的每个顶点都必须读到 ≥ 阈值。** 重画会把低于阈值的顶点往后推，
一个三角形如果有一个顶点被推走，三角形就会被拉坏。所以发梢最好在 UV 里是独立的 UV 岛（终末地就是这样），
或者按三角形烘焙并向外扩几个像素。下面的烘焙工具就是这么做的。

遮罩用 UV0、在顶点着色器和像素着色器里都要读，所以它是一张独立贴图，不能复用 Global Mask Map
（那几张图在像素阶段才采样）。已有打包贴图的项目可以直接把 `Hair Tip Mask` 指向打包图，再选通道。
导入时关 sRGB；精确一点用 `TC_Grayscale`。

## 烘焙工具（没有现成遮罩时）

`Tools/HairTip/`：

1. 在编辑器里运行 `export_section_triangles.py`，导出一个材质槽位的三角形（参考姿势位置 + UV0）：
   ```python
   MESH = "/Game/.../SK_Character"; SLOT = "MI_Bangs"; OUT = "C:/temp/bangs.json"
   exec(open(".../Tools/HairTip/export_section_triangles.py").read())
   ```
   需要 Geometry Script 插件（Modeling Tools Editor Mode 会一起带上）。
2. 离线烘焙：
   ```
   uv run --with numpy --with pillow python bake_hair_tip_mask.py bangs.json T_Bangs_TipMask.png --cut-fraction 0.35 --preview Bangs_D.png
   ```
   重心低于切线高度的三角形算发梢（切线高度 = 这个槽位最低点往上 `--cut-fraction` × 总高，
   或者用 `--cut-height` 直接给）。发梢内部值为 0.5 → 1 的渐变，最后向外扩 4 个像素。
   `--front -y:0` 这类参数可以只取脸前面的部分。`--preview` 会把发梢区域染成品红叠在底色上，方便检查。

## 原理

- **裁剪**：`OpacityMask × (1 − 发梢)`。Masked 的 base pass、深度、描边和屏幕空间发丝阴影看到的都是“没有发梢”的刘海。
  阴影 pass 默认不裁（`Hair Tip Casts Shadow`）。
- **重画**：`Opacity × 发梢 × lerp(Root, End, 渐变)`，走 toon 真半透明，即逐像素前向 toon 着色
  （M_MoonToon 已设 Surface ForwardShading），再经过单层预深度，所以重叠的发梢只混一层，不会互相叠暗，也不会出现排序闪烁。
  单层裁剪要求角色有 toon 角色 id（配置第 4 步）。
- **非发梢部分往后推**：重画会把整个槽位再画一遍，非发梢部分的不透明度是 0。但 toon 半透明单层预深度不看不透明度，
  内线 pass 也会把这层当成“前景半透明层”。所以重画实例在顶点着色器里把低于阈值的顶点沿各自的视线往后推
  `Hair Tip Hidden Push`（推的量 = 1 − 遮罩/阈值）。屏幕位置不变，只有深度变了，于是这部分落在不透明刘海后面：
  半透明 pass 在深度测试处就把它丢掉，不做前向着色，预深度和内线也把它当成被遮住的层。
- **和 FaceOverlay 共存**：FaceOverlay 只在“同角色几何挡在脸前”的像素上重画眉眼。发梢已经从深度里裁掉，
  那里最前面就是脸本身，FaceOverlay 不会再画一次，眉眼只透过重画的发梢透出一次，不会重复透。

## 已知限制

- 开了 toon 内线（PPV `Toon Inner Line`）时，半透明内线 pass 会沿发梢和不透明部分的交界画一条 id 边线。
  发梢在交界一侧是可见的半透明层，另一侧没有半透明层。去掉它需要改引擎着色器，
  让预深度 / 内线认出“部分覆盖层”，列在 R2。
- 发梢不在不透明深度里，屏幕空间发丝阴影、SSAO、景深都不认发梢。发梢很细，一般看不出来。
- 发梢描边（T49，半透明描边实例）：
  - 发根描边的颜色靠前向补的 rim（和延迟 rim pass 同一个计算），只按前向主平行光算，灯自己的 rim 覆盖参数
    （ToonRimIntensity / Width / Color 等）按默认值处理；改了这些参数时，这个槽位的描边会和其它槽位略有不同。
  - 半透明外壳不写深度和 GBuffer：屏幕空间 pass（内线、头发阴影等）把它当成不存在。
  - 和 Lvl_StyleTest 实测：发根描边 46 / 255 对不透明描边 42（rim 打出来的那层灰），发梢区域亮度不变。
- 重画实例的父实例必须是同一个头发实例，或者至少用同一张遮罩、同一阈值，否则裁剪和重画对不上，会出现缝或重叠。

## DevTest 示例

`/Game/MoonToonStyleTest/HairTip/`（关卡 `Lvl_StyleTest`，角色 `MT_WW`，ww_yy 的刘海槽位 24）：
- `T_ww_yy_Bangs_TipMask`：`--cut-fraction 0.35` 烘焙出的渐变遮罩
- `MI_Bangs_TipCut`：MI_MI_Bangs + 开关
- `MI_Bangs_TipTranslucent`：Translucent，End Opacity 0.45
- `MI_Outline_Bangs_TipCut`：刘海描边 + 开关
- `MI_Outline_Bangs_TipTranslucent`：它的 Translucent 子实例，End Opacity 0.45（发梢描边，T49）

切换脚本：`DevTest/Saved/MoonToonStyle/scripts/hairtip_ab2.py`（MODE "A" = 原样，"B" = 发梢半透明）；
`t49_setup.py`（MODE "prod" / "tipcut" / "tipline"，"tipline" = 再加半透明发梢描边，并自动挂 MoonToonActorComponent）。
`MT_WW` 本身是一个没挂组件的 SkeletalMeshActor，验收前先用 `t12_toon_actor.py`（`ADD = True`）挂上 MoonToonActorComponent。
