# 脸部 SDF 阴影 (Face SDF)

经典的"阈值图"式脸部阴影：一张 SDF 贴图记下每个像素在灯转到多少度时变暗，着色时拿灯相对脸的方位角和它比较，
脸上的明暗线就按美术画好的形状移动，而不是按法线。这页讲精度（T24）、两个可选扩展通道（T26 / T27）
和贴图导入规范（T28）。全角度 SDF（`Use Full Angle SDF`，顶光 / 底光 / 背光都对）另见 `Tools/SDF360`。

## 基本配置

在 **M_MoonToon** 的材质实例上：

1. 勾 **`Is DFFacialShadow`**。
2. **`Distance Field Facial Shadow Map`** 指向 SDF 贴图，**`Distance Field Facial Shadow Map Channel`** 选通道。
3. 网格要带头朝前向量（`Use Highlight Tangent baked in Houdini UV23` 那一套，UV1 / UV2）。
   角色挂了 `MoonToonActorComponent` 并设了头部坐标系时，左右判断和方位角改用头部骨骼的朝向，转头也准。

灯在脸的正前方或正后方 ±3° 内，左右两半 SDF 会交叉过渡，不会裂成斑块。

## 精度：8 位和 16 位

SDF 值交给每一盏灯时走 GBuffer / Substrate 记录里的两个 8 位通道，也就是 256 级：
灯慢慢转的时候，明暗线是一格一格跳的（每格不到 1°），贴图很平的地方会出现等高线一样的台阶。

**`Facial SDF Extended Payload`**（组 `02 - Distance Field Facial Shadow`，静态开关，默认关）把精度提到每侧 16 位：
高 8 位仍走原来的通道，低 8 位走 RT4 / Data9（这条通道原来给 Matcap / 油膜修饰器用，T29 / T30 把它们搬进材质后，
它专门给脸 SDF 用）。所有灯都拿到 16 位，副光、补光灯也一样。

- 关着 = 和以前逐字节一样（8 位）。
- 贴图本身超过 8 位时收益最大：16 位贴图（R16F）或 R+G 两通道贴图（见下）。8 位贴图开了，相邻纹素之间的
  双线性插值会更细，但贴图自己的 256 级台阶还在。
- 用 `Tools/FaceSDF/check_face_sdf_textures.py` 能找出"贴图 16 位但没开扩展"的实例。

## 两个可选扩展（需要先开 Extended Payload）

**真实 N·L 遮罩（T26，学终末地 mask.G）**

- `Facial SDF NoL Mask`：一张线性灰度图，1 的地方回到灯自己的 N·L 明暗。用在耳朵、脖子、发际线这些
  SDF 画不准的地方，让它们按真实形体受光。
- `Facial SDF NoL Mask Channel` 选通道，`Facial SDF NoL Mask Strength` 整体强度。

**阴影软渐变（T27，学鸣潮 B 通道）**

- `Facial SDF Soft Ramp` 设 1 打开。打开后，SDF 判成阴影的地方，ramp 坐标改由 `Facial SDF Soft Map` 决定；
  硬的明暗分界线不动，只是阴影里面的颜色有了渐变（0 = 最深的阴影色，1 = 靠近明暗线的阴影色）。
- `Facial SDF Soft Map Channel`、`Facial SDF Soft Power`（鸣潮用 1.5）、`Facial SDF Soft Range`（x / y = 软图 0 / 1 落在阴影半边 ramp 的哪里）。

## 贴图导入规范（T28）

SDF 是数据，不是颜色：

| 项 | 规范 | 原因 |
| --- | --- | --- |
| sRGB | **关** | sRGB 会把阈值曲线弯掉，暗端尤其明显 |
| 压缩 | **R16F（HalfFloat）** 最好；8 位贴图用 **Grayscale（G8）** 或 **BC4** | BC1 / BC3 / Default 的块端点只有 5–6 位，明暗线上会出台阶和斑块 |
| R+G 两通道 | 压缩设 **VectorDisplacementmap（不压缩 RGBA8）**，材质勾 `Distance Field Facial Shadow Map R+G Average` | 终末地的做法：R、G 各存一半精度，平均后约 9 位；任何块压缩都会把 R、G 混掉 |
| Mip | 可以关（`NoMipmaps`） | SDF 在 mip 0 采样，mip 只占内存 |

**已有贴图不动**：ww_yy 的两张 SDF 是 BC1 + sRGB，现在的观感就是按这个调出来的，改成线性会明显变样。
检查脚本把 `/Game/Assets/Meshs/Test_ww_yy/` 下的贴图（以及引用它们的实例）报成 `KEEP`，不报错误。
要把这类老贴图升级到 16 位，先用 `encode_face_sdf.py --from-srgb` 把 sRGB 解码烘进数值，观感才不变。

## 工具（`Tools/FaceSDF/`）

```
# 从光照遮罩直接烘 16 位 SDF（遮罩可以用 Tools/SDF360/gen_masks.py 生成再手改）
uv run --with numpy --with pillow python bake_face_sdf.py --masks masks/ --out face_sdf [--rg] [--bits 16] [--coverage coverage.png]

# 把已有的 SDF 转成 16 位 / R+G；老的 8 位 sRGB 贴图加 --from-srgb，8 位台阶可以用 --dequantize 1.0 抹平
uv run --with numpy --with pillow python encode_face_sdf.py old_sdf.png --from-srgb --dequantize 1.0 --out16 face_sdf16.png --out-rg face_sdf_rg.png
```

**检查脚本**：在编辑器里跑（UnrealBridge exec 或 Python 控制台），只读，结果写到 `Saved/MoonToon/FaceSdfCheck.json`。

```
check_face_sdf_textures.py    # 可选前置变量 ROOTS / GRANDFATHERED / REPORT
```

| 级别 | 含义 |
| --- | --- |
| `ERROR` | sRGB 开着；BC1 / BC3 / Default 压缩；R+G 模式用了压缩贴图 |
| `WARN` | BC7 / BC4（8 位级别够用，16 位烘焙不够）；贴图超过 8 位但没开 Extended Payload |
| `INFO` | 有 mip |
| `KEEP` | 老资产，观感依赖旧导入方式，保持不动 |

开了 `Use Full Angle SDF` 的实例不读经典贴图，脚本跳过。

## 实现位置

- 材质：`DShader/MaterialFunctions/Shared/MF_ToonDFFacialShadow.dsf`（采样、R+G 平均）、
  `MF_ToonFacialSdfExt.dss`（拆高低字节、N·L 遮罩、软渐变编码）、`M_MoonToon.dsm` 的 SDF 叶子。
- 引擎：`Toon/ToonBufferCommon.ush` 的 `DecodeToonDataFromBuffer`（RT4 解码，没写扩展的材质 RT4 为 0 = 关）、
  `Toon/ToonShadingFeature.ush` 的 `ApplyDistanceFieldFacialShadow`。
