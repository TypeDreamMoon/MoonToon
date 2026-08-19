# MoonToon 的三条轴 —— 新东西该往哪放

`refactor-plan-v3.md` P0 的产物。**每加一个新东西之前先读这一页。**

历史上有三次把不该是 feature 的东西做成了 feature:SDF 脸阴影(劫持 Metallic/Anisotropy)、
Matcap(勾上就把皮肤着色关掉了)、以及差一点的 NPR 丝袜。三次都是同一个错:
**没有先问"它属于哪条轴"。**

---

## 1. 三条轴

| 轴 | 语义 | 载体 | 互斥性 | 今天的成员 |
| --- | --- | --- | --- | --- |
| **Feature** | 表面模型本身。它**替换**着色方式 | `ShadingFeatureID`(ToonFeatureRT0.x, 8-bit)+ RT0–2 的 11 个槽 | 选一个 | Default / PBRSpecular / KajiyaHair×2 / HairHighlightMask / Skin(+DFF 别名) / Stockings×2 / Eye / Cloth / Metal / Ink |
| **Modifier** | 叠加层。它**加在**任意 feature 的结果上 | `ModifierID`(ToonFeatureRT3.w, 8-bit)+ RT4 的 4 个槽 | 选一个(暂) | Matcap;下一个:油膜 |
| **Flag** | 技术开关,不带参数 | ToonFeatureRT3.z 位域(还剩 7 位) | 可并存 | SDF 脸阴影 |

---

## 2. 归类三问

按顺序问,第一个命中的就是答案:

1. **它替换表面模型吗?**
   —— 是 → **Feature**。判据:选了它以后,皮肤/头发/布料的着色**不应该**还在跑。
   —— 否 → 继续。
2. **它带参数吗?**
   —— 是 → **Modifier**。
   —— 否 → **Flag**。

反例(三次都栽在第 1 问):

- Matcap:MToon 1.0 说得很清楚 `color = color + rim`,它是**加**在光照结果上的。
  一个用了 matcap 的皮肤**仍然是皮肤**。→ Modifier,不是 Feature。
- SDF 脸阴影:它是一种**阴影技术**,不是表面模型。一张用 SDF 的脸仍然要按皮肤着色。
  → Flag(它的数据是两个 SDF 字节,不是参数)。
- NPR 丝袜:它**确实**替换着色路径(走 ramp + cel band,PBR 那个不走),所以第二个 id 是对的。
  这次没栽,是因为先问了这个问题。

---

## 3. 边界规则:这个量需要 L 吗

**决定它能不能做成材质节点的,不是"想不想",是"需不需要光方向"。**

UE 的材质图被翻译成一组**每像素求值一次的叶子函数**(`GetToonMaterialOutput0..4(MaterialParameters)`)。
`FMaterialPixelParameters`(`MaterialTemplate.ush:427`)里没有 L —— 唯一的 `LightVector` 字段注释写着
*"only valid when rendering a light function"*。**不存在"对每盏灯调用一次这段材质代码"的机制。**
deferred 光照 shader 是 `FGlobalShader`(`LightRendering.cpp:887`),连材质绑定通道都没有。

于是:

| | 能不能来自材质图 | 放哪 |
| --- | --- | --- |
| 只依赖 N / V / UV / 贴图 / 常量 | ✅ **能** | 材质节点 → payload 槽位,或 ramp 图集 |
| 依赖 L / H / BxDFContext / SurfaceShadow | ❌ **不能** | 引擎侧 hook(`Toon/Features/ToonFeature_X.ush`) |

实测分类(2026-08-18,`ToonShadingFeature.ush` 全部 27 个 helper):
**15 个逐光**(所有 wrap lighting、双瓣高光、Kajiya 瓣、透射、屏幕空间发丝阴影、SDF 脸)、
**12 个纯每像素**(`GetToonWorldTangent`、`GetStockingsDiffuseAlbedo`、`GetSkinSpecularColor`、
`GetEyeLimbalDiffuseScale`…)。`ToonBxDF` 本体 581 行里 24% 触碰逐光状态。

> **一个量横跨两边时,拆开。** 油膜就是典型:干涉色 = f(视角, 厚度) 每像素算得完 → 材质节点 + modifier 槽位;
> 高光瓣的位置需要 L → 引擎侧 hook。不要因为"有一半能搬"就整个搬,也不要因为"有一半不能"就整个留。

Light Function 是唯一的例外通道,但它输出只有一个乘性 mask(`HLSLMaterialTranslator.cpp:1617` 只取
EmissiveColor),而且是单独一趟 pass 或烘进图集 —— 能改光的**颜色**,改不了 BxDF 的**形状**。

---

## 4. 载荷预算(2026-08-18 现状)

| lane | 格式 | 内容 | 余量 |
| --- | --- | --- | --- |
| ToonSurfaceRT | RGBA16 (GBuffer 第 8 个 MRT) | ToonGBuffer(ramp 索引/偏移、高光色、rim、RT 阴影标志…) | 64 bit 全满 |
| ToonFeatureRT0 | RGBA8 | x = ShadingFeatureID, yzw = ScalarA/B/C | 满 |
| ToonFeatureRT1 | RGBA8 | ScalarD/E/F/G | 满 |
| ToonFeatureRT2 | RGBA8 | FeatureInputVector | 满 |
| ToonFeatureRT3 | RGBA8 | x,y = SDF 左/右, z = flags 位域, **w = ModifierID(P5 启用)** | flags 还剩 7 位 |
| ToonFeatureRT4 | RGBA8 | **Modifier 的 4 个槽**(今天是 matcap rgb + mix) | 满 |
| TObjectID | R32_UINT | ToonActor id | — |

**Feature 槽位表已经满了**(7 scalar + 1 float4 = 11 通道,全部被读)。所以:
新 feature 只能复用现有 11 槽的布局;**要更多参数就必须先加 RT**。

实测代价(RTX 4070 Ti SUPER @2.2 Mpx):多一张全屏 RGBA8 = clear +0.012 ms + mesh pass 多写 4 B/px
+ 解码多一次 Load。**在噪声以内**,所以"加一张 RT"是可以做的决定,不是禁区 —— 但要为它写清理由。

### Modifier 载荷决定(P0 定,P5 实施)

- `ToonFeatureRT3.w` = ModifierID(8-bit)。RT3.w 今天是空的,**零成本**。
- `ToonFeatureRT4` 的 4 个通道 = 该 modifier 的槽位。Matcap 现在就占满这 4 个,所以它是 modifier #1,
  语义不变,只是含义改由 RT3.w 决定。
- 授权路径:`ToonMaterialOutput` 新增 Pin[4](`GetNumOutputs` 4 → 5)。
  **不能走 `MoonEncodedAttribute*`** —— 0–4 全满(`ToonBufferCommon.ush:314-332`)。
- **一次只能有一个 modifier。** 需要并存时再加 RT5,机制照抄,不返工。

---

## 5. 参照:Epic 在 UE 5.8 Substrate 里怎么做的

调研于 2026-08-18。结论先写:**架构和我们一样,覆盖度远不如我们,不迁。**

**Substrate 不在材质里算光照。** 材质图产出的是一棵 BSDF + 算子的树(`BasePassPixelShader.usf:1127`),
打包进 material buffer,由光照 pass 逐 closure 解包求值(`SubstrateDeferredLighting.ush:112`,
数学在 `SubstrateEvaluation.ush:467`)。**和 MoonToon 是同一个架构** —— 材质写 payload,引擎读 payload。
区别只在通用程度:Substrate 是 closure 列表 + 循环,我们是 feature id + switch。

**BSDF 集合是封闭的。** 7 个硬编码类型,存在 3 bit 里(`Substrate.ush:509`),已满。
第 8 个槽位的原文是 `//define SUBSTRATE_BSDF_TYPE_CUSTOM 7 Reserved for Custom shading model`
—— **被注释掉了**。加一个新 BSDF 要改四层:共享 `#define`、三处 HLSL switch、`FMaterialCompiler`
上一个手写纯虚函数、翻译器里带硬报错 default 的 switch。全树 grep `RegisterShadingModel` /
`MaterialShadingFunction` / `CustomShadingModelPlugin` 零命中。

**Epic 让 toon 可授权的方式是 profile 资产 + 纹理图集** —— `UToonProfile` 的曲线烘成 atlas,
硬编码的 BSDF 分支去采,材质节点只携带一个 slot id。
**这正是 `MoonGlobalDiffuseColorRampAtlas` + `DiffuseColorRampIndex`。** 我们独立走到了同一个模式,
而且更远:我们的图集是运行时动态的。

### 覆盖度对照

| | MoonToon | Epic Substrate Toon (5.8, **Experimental**) |
| --- | --- | --- |
| 漫反射 ramp | ✅ 动态图集 + 逐材质索引 + 偏移 | ✅ profile 曲线 + 偏移贴图 |
| 高光 ramp | ✅ 图集 | ✅ profile 曲线 |
| **高光模型数** | **~10** | **1** —— 只有 GGX 的 D 项,Vis/Fresnel 在 `SubstrateToonBSDF.ush:129-132` 被注释掉 |
| **逐光控制** | ✅ Smooth/Offset/Flatten/FlattenRange/Penumbra 逐灯 | ❌ **没有** |
| 多光行为 | ✅ 每盏灯各自硬 cel 带 | 反向:`ToonUnifiedDiffuse` 累加后只上一次 ramp(默认关) |
| SDF 脸 | ✅ 左右 SDF + 方位角 + 仰角提亮 | ~ 只有一个通用 ramp 偏移贴图(注释推荐用距离场) |
| 排线 hatching | ❌ | ✅ RGBA 四档密度 + 分布曲线 |
| Matcap / 描边 / 屏幕空间发丝阴影 / 半透明 toon | ✅ | ❌ |

**不迁。** 迁过去是"14 个着色模型退成 1 个 slab 加风格化"。
`f532d2e1` 那个 MSM_Toon → Substrate Toon 的桥的注释本身就是迁移成本清单:profile 固定 slot 0、
只桥接静态单一着色模型、逐像素混合掉回 slab。

**值得偷的一样:** hatching —— 一张 RGBA 当四档密度,用一条曲线在四档间选。
天然是 modifier #3,机制现成。

---

## 6. 既有分歧备案

**逐光硬 cel 带 vs Epic 的 unified diffuse。**

我们:每盏副光各自碾出自己的 cel 带,带与带相加(YivanToon / MooaToon 的做法)。
Epic:`ToonUnifiedDiffuse` 把所有灯先累加进两张 RT,最后在 resolve pass 里**只上一次** ramp
(`DeferredLightApplyToonDiffusePS`,`DeferredLightPixelShaders.usf:729-795`)。

**我们选前者,而且是踩过坑之后回来的** —— 2026-06 那次"软填充"重写把副光换成
`saturate(NoL + Offset)` 渐变,结果每盏副光都读成 PBR wrap、`ToonLightSmooth` 静默失效、
SDF 脸阴影算了却被丢掉。理由完整写在 `ToonShadingModel.ush:284-307`。

**记在这里是为了防止将来有人把它"修正"回去。** 重叠处过亮是打光强度的授权问题,
不是 BxDF 该靠丢掉 band 来掩盖的事。

---

## 7. 新东西检查清单

加任何东西之前,把这七条填完:

1. 走三问 → 它是 Feature / Modifier / Flag 中的哪个?
2. 它需要 L 吗?哪部分需要、哪部分不需要?(§3 的边界规则,横跨就拆开)
3. 参数几个?现有槽位够吗?不够的话,是复用别的 feature 的布局,还是加 RT?加 RT 的理由是什么?
4. 材质侧的参数名 —— 有没有能沿用的?(沿用 = MI 零迁移,见 v2 P7 的教训)
5. 它和现有的东西正交吗?能不能和任意 feature 并存?不能的话为什么?
6. 验收怎么做?(L4b 同 exec 背靠背截图 diff ≤ 噪声底;新效果另加目检)
7. 需要注册的地方:ID 表、槽位表、注册表、`Features/ToonFeature_X.ush`、生成器 PARAMS/DESC。
   **只有这五处** —— 如果你发现还要改别的,说明轴选错了,回第 1 条。
