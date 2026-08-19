# MoonToon 渲染管线重构 v3 —— ToonFeature 结构:注册表 + hook 派发 + Modifier 轴

接 `refactor-plan-v2.md`(P0–P8 已全部完成,见 `execution-log.md`)。v2 解决的是"字节怎么被谈论"
(槽位表、具名读取、静态派发、面板);v3 解决的是"加一个 feature / modifier 要碰几处、碰哪里"。

范围:**引擎 shader + 引擎 C++(两处小改)+ 插件生成器**;DreamShader 上游扩展点作为独立末尾阶段。
日期:2026-08-18。§8 的测量数据都是这一天在 DevTest / `Lvl_Toon_Showcase` 上量的。

本文只放**能照着做的东西**:阶段、验收、风险、待决。设计讨论的结论直接写进阶段里,不复述推导。

> **加新东西之前先读 [`axes.md`](axes.md)** —— 三条轴的定义、归类三问、"这个量需要 L 吗"的边界规则、载荷预算,以及新东西检查清单。

---

## 0. 诊断:现在加一个 feature 要碰哪里

以 NPR stockings(id 14,提交 `92000b51` + `7af9f1f1` + `258a4837`)为样本:

| 层 | 文件 | 改什么 | v3 之后 |
| --- | --- | --- | --- |
| 引擎 shader | `Shared/MoonToonShadingFeatureDefinitions.h` | 一行 ID | 一行 ID(不变) |
| | `Shared/MoonToonFeatureSlots.h` | 槽位表 | 槽位表(不变) |
| | `Private/Toon/ToonFeatureParams.ush` | 手抄一行 `MOON_TOON_DEFINE_FEATURE_PARAMS` | **从注册表展开,不再碰** |
| | `Private/Toon/ToonShadingFeature.ush` | helper | **解散,helper 进各模块文件** |
| | `Private/Toon/ToonShadingModel.ush` | `bIsX` + band-input / diffuse / specular 三处 `else if` + 横切逻辑抄一遍 | **只剩主干,不再碰** |
| 引擎 C++ | `Engine/Public/MoonToonShadingFeature.h` | 枚举 + static_assert → **重编引擎** | static_assert 从注册表展开;枚举去留见 §6 |
| 插件 | `Tools/gen_feature_functions.py` | FEATURES / PARAMS / DESC / DEBUG_COLORS 四张表 | 同,但 `--check` 双向校验;P6 起从 manifest 生成 |
| 零散 | `ToonBlur.usf:35`、`ToonShadingHelpers.ush:121`、`ToonShadingFeature.ush:39` | 硬编码 id 比较 | policy / 模块内 |
| **新增** | `Shared/MoonToonFeatureRegistry.h` | — | **一行注册** |
| **新增** | `Private/Toon/Features/ToonFeature_X.ush` | — | **一个新文件** |

终态:加 feature = ID 一行 + 槽位表 + 注册表一行 + 一个 `.ush` + 生成器两张表。加 modifier 同一套动作。

两个已经量出来、决定设计边界的事实(§8 有数):

1. **性能不是结构设计的约束。** 主光 `RenderLight` 整个 draw 0.16 ms(2.2 Mpx,4070 Ti SUPER),
   点光 0.02–0.08 ms/盏。ToonBxDF 内部再多几个 switch,退化 20% 也是 0.03 ms。**结构按最干净的做。**
2. **多一张 RGBA8 lane 在噪声以内**(clear +0.012 ms,mesh pass 多写 4 B/px)。但 Modifier 轴今天
   根本不需要新 RT:`ToonFeatureRT3.w` 空着(`ToonBufferCommon.ush:48`),RT4 已经是"feature 无关的
   4 个通道"。

---

## 1. 排序原则

**零行为变化的先做,派发器一步到位,模块逐个搬。**

- P1 / P2 不改任何着色结果,用"预处理后 HLSL 逐字节相同"验收 —— 这是能机器判定的
- P3 派发器一进来,所有 feature 的 HLSL 都变;所以 P3 一次搭好契约 + 派发 + **一个**模块,其余 feature
  走 `default:` 的 legacy 内联路径,再逐个搬(P4)。每搬一个一提交,一次截图对照
- Modifier 轴(P5)复用 feature 的全部机制,所以排在 P4 之后:机制先在 feature 上定型,再复制
- Manifest 生成器(P6)和 DreamShader 扩展点(P7)最后 —— 前面五步会告诉它们该长什么样

每个阶段一个提交(P4 每模块一个),每个可单独回滚。

---

## 2. 阶段

### P0 · 决策 + spike(半天,不写产品代码) —— ✅ **完成 2026-08-19**

> 产物:[`axes.md`](axes.md)(轴定义 + 边界规则 + Substrate 对照 + 分歧备案)、[`P0-v3-result.md`](P0-v3-result.md)(spike 与扫描结果)。
> 结论:X-macro `case ##ID:` **PASS**(0 诊断,退化方案不启用);`EMoonToonShadingFeature` 活资产**零引用** → P1 里删除。

1. **定轴文档**(一页,放本目录 `axes.md`):

   | 轴 | 语义 | 载体 | 今天的成员 |
   | --- | --- | --- | --- |
   | Feature | 互斥的表面模型,选一个 | `ShadingFeatureID`(RT0.x)+ RT0–2 槽位 | Default / PBRSpecular / Kajiya×2 / HairMask / Skin(+DFF) / Stockings×2 / Eye / Cloth / Metal / Ink |
   | Modifier | 叠加层,选一个 | `ModifierID`(RT3.w)+ RT4 四槽 | Matcap;下一个:油膜 |
   | Flag | 技术开关,可并存 | RT3.z 位域 | SDF 脸 |

   归类规则写死:**"它替换表面模型吗?否 → 不是 feature。它能和任意 feature 同时存在吗?是 → modifier
   或 flag。它带参数吗?带 → modifier,不带 → flag。"** 以后每个新东西先过这三问。

2. **Modifier 载荷决定:** RT3.w = ModifierID(8-bit),RT4 = 该 modifier 的 4 个槽,**一次一个 modifier**。
   ModifierID 由材质授权,走 `ToonMaterialOutput` 新增的 Pin[4].x(见 P5;`MoonEncodedAttribute0–4`
   已满,`ToonBufferCommon.ush:314-332`,不能走属性路)。需要两个并存时再加 RT5,机制不变。
3. **C++ 枚举去留:** 用 unreal-bridge 扫全部 `UMaterial`(含 `Engine/Plugins`、`/MooaToon/`)的
   `ScalarParameter` 是否有引用 `EMoonToonShadingFeature` 的枚举下拉。插件 Source / DShader 已确认零引用。
   零引用 → P1 里删,引擎 C++ 从此不再进"加 feature"的清单。
4. **Spike:** v2 P0 的 `ppdrv` 只验证过 `struct FToon##Name##Params`。再跑一次
   `switch (Id) { MOON_TOON_FEATURE_LIST(X) }` 配 `#define X(ID, M) case MOON_SHADING_FEATURE_ID_##ID: return M##_Specular(Ctx);`
   —— 同一预处理器、同一机制,但 `case` 里的 `##` 没跑过。5 分钟。

**验收:** 三份文字产物(轴文档、扫描结果、spike 输出)。P0 不通过的只有 spike;退化方案是派发器
用一次性脚本生成后当普通源码提交(和 v2 P0 的退化方案同一形态),后续阶段不受影响。

---

### P1 · 注册表 + 三处展开 + 生成器校验(零行为变化)

引擎侧 + 插件生成器。

1. 新建 `Engine/Shaders/Shared/MoonToonFeatureRegistry.h`,两张表:

   ```c
   // 模块:一个模块 = 一份槽位表 + 一个 .ush;多个 id 可以指向同一模块
   #define MOON_TOON_MODULE_LIST(X) \
       X(Default,          MOON_TOON_SLOTS_DEFAULT)              \
       X(PBRSpecular,      MOON_TOON_SLOTS_PBR_SPECULAR)         \
       X(KajiyaHair,       MOON_TOON_SLOTS_KAJIYA_HAIR)          \
       X(HairHighlightMask,MOON_TOON_SLOTS_HAIR_HIGHLIGHT_MASK)  \
       X(Skin,             MOON_TOON_SLOTS_SKIN)                 \
       X(Stockings,        MOON_TOON_SLOTS_PBR_STOCKINGS)        \
       X(Eye,              MOON_TOON_SLOTS_EYE)                  \
       X(Cloth,            MOON_TOON_SLOTS_CLOTH_VELVET)         \
       X(Metal,            MOON_TOON_SLOTS_TOON_METAL)           \
       X(Ink,              MOON_TOON_SLOTS_TOON_EMISSIVE_INK)
   // id → 模块。IdName 拼成 MOON_SHADING_FEATURE_ID_##IdName;EnumName 只给 static_assert 用(枚举删了这列一起删)
   #define MOON_TOON_FEATURE_LIST(X) \
       X(DEFAULT,                      Default,           Default)                   \
       X(PBR_SPECULAR,                 PBRSpecular,       PBRSpecular)               \
       X(KAJIYA_HAIR_SPECULAR,         KajiyaHair,        KajiyaHairSpecular)        \
       X(DISTANCE_FIELD_FACIAL_SHADOW, Skin,              DistanceFieldFacialShadow) \
       X(HAIR_HIGHLIGHT_MASK,          HairHighlightMask, HairHighlightMask)         \
       X(TOON_KAJIYA_HAIR_SPECULAR,    KajiyaHair,        ToonKajiyaHairSpecular)    \
       X(SKIN,                         Skin,              Skin)                      \
       X(PBR_STOCKINGS,                Stockings,         PBRStockings)              \
       X(EYE,                          Eye,               Eye)                       \
       X(CLOTH_VELVET,                 Cloth,             ClothVelvet)               \
       X(TOON_METAL,                   Metal,             ToonMetal)                 \
       X(TOON_EMISSIVE_INK,            Ink,               ToonEmissiveInk)           \
       X(EYEBROW,                      Default,           Eyebrow)                   \
       X(NPR_STOCKINGS,                Stockings,         NPRStockings)
   ```

   ID 数值仍只在 `MoonToonShadingFeatureDefinitions.h`;13 号 MATCAP 退役不进表。
2. `ToonFeatureParams.ush` 那 12 行手抄 → `MOON_TOON_MODULE_LIST(MOON_TOON_DEFINE_FEATURE_PARAMS)`
   (现有宏签名 `(FeatureName, SlotTable)` 正好对上)。
3. `MoonToonShadingFeature.h` 的 14 条 static_assert → 从 `MOON_TOON_FEATURE_LIST` 展开;
   若 P0 决定删枚举,则整个文件删除,`EnumName` 列不写。
4. `Tools/gen_feature_functions.py`:
   - `FEATURES` 里 `MOON_TOON_SLOTS_*` 列删除,改由注册表 IdName → Module → SlotTable 推导
   - `--check` 双向:注册表里每个 id 必须有 FEATURES 键、每个 FEATURES 键必须在注册表里;每个模块的
     槽位表每个 `Name` 必须有 PARAMS / DESC(后者今天已有)
5. 引擎 `MoonToonFeatureSlots.h` 头注释补一句"消费方见 MoonToonFeatureRegistry.h"。

**验收:**
- `ppdrv` 预处理 `ToonShadingModel.ush`(deferred 主光 permutation 的 define 集)前后 **逐字节相同**
- 引擎 C++ 编译通过(或枚举文件删除后编译通过)
- `gen_feature_functions.py --check` 全绿,重跑生成器 `.dsf` **零 diff**

---

### P2 · 主干抽横切(ToonBxDF 内,数学不动)

引擎侧。**目标是把"每加一个 feature 就要抄一遍"的东西收成主干里的一份**,为 P3 的契约腾地方。

1. shadow tint:`ToonShadingModel.ush:413 / 427 / 445 / 455 / 468` 五处
   `X = lerp(MainLightShadowTint, X, saturate(MainLightRampMask))` + stockings 的 modulate 变体(374)
   → 主干一处 `ApplyMainLightShadowTint(Albedo, TintMode)`,`TintMode ∈ {None, Lerp, Modulate}`
2. 两种光色的用途显式化:`LightColorAndAttenuation`(带 cel band)/ `RawLightColorAndAttenuation`
   (不带,skin scatter 与 stockings transmission 用)→ 命名 `LightBanded` / `LightUnbanded`,
   注释写清"背光项永远用 Unbanded"
3. `ToonBlur.usf:35` 的 `FeatureId == 0 || FeatureId == 1` → `MoonToonFeatureHasBloomWeight(Id)`,
   实现从注册表展开(为 P3 的 policy 预留位置,先做成函数)
4. 主/副光判定、`bUseFacialShadowOverride`、`ToonShadowTintScale` 各收成一次计算,feature 分支只读

**验收:** 首选 `ppdrv` 逐字节相同。表达式重排(比如 lerp 收拢导致求值顺序变)会让字节不同,
此时改用 §3 的 L4b(同 exec 背靠背截图 diff ≤ 噪声底)+ 反汇编 light PS 指令数不增。
**不允许"看起来一样"当验收。**

---

### P3 · Hook 契约 + 派发器 + 第一个模块(Skin)

引擎侧。**这一步之后所有 feature 的 HLSL 都会变,所以验收是全特征。**

1. `Toon/ToonShadingContext.ush` 增加 `FToonShadeContext`,主干算好一次、四个 hook 只读:

   | 字段组 | 内容 |
   | --- | --- |
   | 像素 | `GBuffer` `ToonGBuffer` `ToonFeature` `WorldTangent` `FacialForwardDir` |
   | 灯 | `ActiveToonLight` `ToonLighting` `AreaLight` `bToonMainLightPath` |
   | 视图 | `ToonView` `V` `NoV` |
   | 几何 | `N` `L` `H` `NoL_Full` `NoL_Half` `DiffuseNoL` `DiffuseNoL_Half`(FlatNormal 后) |
   | 光 | `LightBanded` `LightUnbanded` |
   | 阴影 | `ToonSurfaceShadow` `PBRSurfaceShadow` `MainLightRampMask` `MainLightShadowTint` |

2. 四个 hook + policy(每个模块一个文件 `Toon/Features/ToonFeature_<Module>.ush`):

   ```hlsl
   float3 Skin_Tangent   (FToonShadeContext Ctx);                       // 默认:现有 GetToonWorldTangent 的非 Kajiya 路径
   float  Skin_BandInput (FToonShadeContext Ctx, float NoLHalf);        // 默认:直通
   void   Skin_Diffuse   (FToonShadeContext Ctx, out float3 Albedo, out float DiffuseScale, out float3 Additive);
                                                                        // 默认:DiffuseColor, 1, 0
   float3 Skin_Specular  (FToonShadeContext Ctx);                       // 默认:ramp specular
   FToonFeaturePolicy Skin_Policy(uint Id);                             // 见下
   ```

   ```hlsl
   struct FToonFeaturePolicy
   {
       bool bUsesDiffuseRamp;   // PBRStockings = false
       bool bUsesCelBand;       // PBRStockings = false
       bool bShadowFromToonSnap;// PBRStockings = false(用 PBRSurfaceShadow)
       uint TintMode;           // None / Lerp / Modulate(Stockings)
       bool bMultiBandCel;      // Default 按槽位值
       bool bHasBloomWeight;    // Default / PBRSpecular
   };
   ```

   未实现的 hook 用 `#ifndef Module_Hook` 兜到 `Default_Hook`;两个 id 一个模块靠注册表两行;
   同模块按 id 分 policy(Stockings 7/14、Kajiya 2/5)由 `Module_Policy(Id)` 决定。
3. `Toon/ToonFeatureDispatch.ush`:从 `MOON_TOON_FEATURE_LIST` 展开 4 个 `switch` + 1 个 policy switch;
   **`default:` 落到主干里保留的 legacy 内联分支**——未搬的 feature 走老路,这是 P4 能逐个搬的前提。
   宏形态就是 P0 spike 跑过的那个。
4. `Toon/Features/ToonFeature_Skin.ush`:`GetSkinWrappedLighting` → `Skin_BandInput`,
   `GetSkinScatterLighting` → `Skin_Diffuse` 的 Additive(用 `Ctx.LightUnbanded`),
   `GetDualSkinSpecular` + 采 ramp → `Skin_Specular`。DFF(3)在注册表指向 Skin。
   删 ToonBxDF 内 `bIsSkinFeature` 全部分支。
5. `ToonShadingModel.ush` 主干调 hook 的四个位置:BandInput 在采 ramp 之前;Diffuse 在 ramp mask /
   shadow tint 之后;Specular 在 BxDFContext 之后;Tangent 最前。顺序被数据流卡死,写在主干注释里。

**验收:**
- L4b 全特征(Showcase 六个角色覆盖 Default / Skin / Kajiya / ToonKajiya / Eye / Cloth / Stockings…
  缺的用 `MI_Moon_Toon*` 补一个 spot 场景),diff ≤ 噪声底
- ProfileGPU `RenderLight StandardDeferred`(主光)与 P2 后对比 **±10% 以内**;超出先看反汇编再决定
  是否退到"一个 `Module_Shade(Ctx, inout Lighting)` 一个分支"的形态(§6)

---

### P4 · 逐模块搬迁(每模块一提交)

引擎侧。顺序按复杂度从低到高,每个的特殊点:

| # | 模块 | 搬什么 | 特殊点 |
| --- | --- | --- | --- |
| 1 | Eye | BandInput(wrap)/ Diffuse(EyeTint 兜底 DiffuseColor,×limbal)/ Specular(双瓣) | 无 |
| 2 | Cloth | BandInput / Diffuse(×grazing,+retro)/ Specular(`ShadeClothSpecular` 已是纯函数) | 无 |
| 3 | Metal | Diffuse(×DiffuseIntensity)/ Specular | 无 |
| 4 | Ink | BandInput(`GetEmissiveInkBandLighting`)/ Diffuse(blend,×edge,×lift)/ Specular | 无 |
| 5 | HairHighlightMask | Specular | 读 `DiffuseNoL_Half`,Ctx 里要有 |
| 6 | PBRSpecular | Specular(GGX / LTC / 各向异性) | policy `bHasBloomWeight` |
| 7 | Default | Specular(ramp,即所有默认实现)| `bMultiBandCel` 进 policy(按 `Plain.MultiBandCelEnable`);EYEBROW(12) 指向它 |
| 8 | KajiyaHair | Tangent(旋转 / aniso 方向)/ Specular(2 与 5 按 id 分 Kajiya / ToonKajiya) | `GetSpecularColorRampUAndMaxSpecularValue` 里的 Kajiya 分支(`ToonShadingHelpers.ush:121`)迁入模块,helper 去分支——其他模块调它时永远不是 Kajiya id,行为不变 |
| 9 | Stockings | Diffuse(albedo mix,+transmission 用 Unbanded)/ Specular(GGX sheen + pearl ramp)| policy 按 id:7 = {ramp 否, band 否, PBR shadow, Modulate};14 = {是, 是, Toon shadow, Modulate};`Stockings_Diffuse` 内 `bIsNPR` 只剩 albedo 是否走 tint |

终态:
- `ToonShadingModel.ush` 只剩主干,`bIsX` 一个不剩,`default:` 分支删除
- `ToonShadingFeature.ush` 解散:模块私有 helper 进各自文件,真正共享的
  (`GetToonWorldTangent` 的非 Kajiya 部分、`GetScreenSpaceDepthTestHairShadow`、
  `GetToonFilteredSurfaceShadow`、`ApplyDistanceFieldFacialShadow`)留 `ToonShadingHelpers.ush`
- `ToonFeatureParams.ush` 不动(它已经是从表展开的)

**验收:** 每模块 L4b;全部搬完后 ProfileGPU 与 P3 前对比,写进 `execution-log.md`。

---

### P5 · Modifier 轴:matcap 迁入,油膜第一个新用户

**P5a 引擎:**

1. `Shared/MoonToonModifierDefinitions.h`:`MOON_TOON_MODIFIER_ID_NONE 0` / `MATCAP 1` / `OIL_FILM 2`;
   `MOON_TOON_MODIFIER_LIST(X)`;`Shared/MoonToonModifierSlots.h`(Matcap:`RT4.xyz Color`、`RT4.w LightingMix`)
2. `UMaterialExpressionToonMaterialOutput`:`GetNumOutputs` 4 → 5,头文件加 `FExpressionInput ToonDataE`
   (`MaterialExpressions.cpp:23770`)。**v3 仅有的两处引擎 C++ 改动之一**(另一处是 P1 的
   static_assert / 枚举)。
3. `ToonMaterialCommon.ush` `GetToonBuffer`:`ToonFeatureRT3.w = GetToonMaterialOutput4(...).x`
   (`ComposeToonBufferD` 的 `.w` 由 0 改为该值);`GetToonBufferInline` 同步
4. `FToonBuffer`:`MatcapColor / MatcapLightingMix` → `uint ModifierID` + `float4 ModifierInputVector`;
   具名视图 `FToonMatcapParams` 从 modifier 槽位表展开(同 `MOON_TOON_DEFINE_FEATURE_PARAMS`)
5. hook:`Modifier_Apply(FToonShadeContext Ctx, inout FDirectLighting Lighting)` +
   `Modifier_Policy(Id).bMainLightOnly`;主干在 feature 着色完、`MainLightRampTint` 乘完之后调一次。
   `ToonShadingModel.ush:494-510` 的 matcap 块原样搬进 `Toon/Modifiers/ToonModifier_Matcap.ush`
6. `ToonModifier_OilFilm.ush`:第二个高光瓣(薄膜干涉或清漆,P5b 决定),读 `Ctx.LightBanded`、
   `Ctx.ToonSurfaceShadow`;槽位 thickness / intensity / roughness scale / hue shift

**P5a 插件:**

7. 生成器新表 `MODIFIERS`(键 → id 宏、槽位表、组名)+ `MOD_PARAMS` / `MOD_DESC`,
   产出 `Modifiers/MF_ToonModifier_<X>.dsf` + `MF_MoonToonModifierSelect.dsf`
   (`Is Modifier X` 静态开关互斥、都不勾 = NONE、输出 `ToonBufferE` = float4(ModifierID,0,0,0) 与 `ToonBufferD`)
8. `MF_ToonMatcap` 保留采样逻辑,输出接进 `MF_ToonModifier_Matcap`;`MF_MoonToonBaseInput` 里
   `#Region "Matcap"` 与 `MatcapData` 输出移除;`Enable Matcap` 开关 → `Is Modifier Matcap`
9. `M_MoonToon.dsm` 多一路 `Expression(Class="ToonMaterialOutput").Pin[4] = ToonDataE`
10. MI 迁移:matcap **参数名不变 → 零迁移**;开关改名走 v2 P7 的快照法(导出 → 改 → 回填 → 语义比对)

**P5b 油膜(独立提交,新效果):** 材质侧 `MF_ToonModifier_OilFilm` + 目检。

**验收:**
- Matcap 材质 L4b 与 P5 前一致;无 modifier 的材质 `RT3.w` 解码为 0
- 任意 feature + Matcap 组合可用(P5 真正解锁的东西,同 v2 P1 的"任意表面模型 + SDF")
- ProfileGPU 无可见变化(没有新 RT)

---

### P6 · Manifest 双向生成器(吃掉手抄)

插件工具。现状真源在两处:引擎三张 `.h` 表 + 生成器四张 Python 表。合并成一份 manifest
(格式见 §6),`gen_feature_functions.py` 升级为 `gen_toon.py`:

- 生成引擎侧:`MoonToonShadingFeatureDefinitions.h` / `MoonToonFeatureRegistry.h` / `MoonToonFeatureSlots.h` /
  `MoonToonModifier*.h` / static_assert(若枚举还在)/ `ToonFeature_X.ush` **骨架**(仅首次,之后不覆盖)
- 生成插件侧:今天的全部 `.dsf`
- `--check` 进 pre-commit:从 manifest 重生成,任何文件有 diff 即失败

**验收:** 仓库里全部生成物与从 manifest 重生成的结果零 diff;删掉手抄表后 `--check` 仍绿。

---

### P7 · DreamShader 扩展点(上游)+ `DreamShaderMoon` 模块(最后做,可选)

**不分叉 DreamShader。** 它是公开发布的插件(MIT、1.7.1、文档站、外部用户),语义分叉 = 双份 bug、
双份文档、每次 upstream 都 rebase。DreamShader 今天没有任何扩展点(Public 只有 `IDreamShaderCompiler`
两个函数,ExpressionFactory / Generator 全在 Editor 模块 Private),所以:

1. **上游只加通用能力**(每个都是独立 PR,与 MoonToon 无关):
   - 编译前 / 后钩子(跑外部生成器,P6 的 `--check` 挂这里)
   - target / profile 校验:注册处理器,`ShadingModel="Toon"` 时由处理器校验 `ToonMaterialOutput`
     的 pin 数与类型、给默认绑定
   - 自定义顶层块注册:`ToonFeature(Name="Skin", Id=6) { Slots = {...}; Params = {...}; }`
     由注册的处理器接管 → 这就是 P6 的 manifest 变成 DSL 的形态
2. MoonToon 插件加 `DreamShaderMoon` 编辑器模块实现处理器
3. 若仍要开分支:当**孵化分支**,规则写死——通用的 upstream、专属的搬扩展模块、**分支终态可删**

**验收:** DreamShader 上游仓库里 grep 不到任何 MoonToon 特定 token。

---

### P8 · 主材质参数(独立,按需)

与 feature 结构无关,单列。数据:DShader 源 ~333 处声明 / 239 个唯一参数;`MF_MoonToonBaseInput.dsf`
1491 行、12 组约 110 个基础参数;feature 各 3–11 个,静态开关裁剪后 MI 面板 ~120 行。
**artist 面板已经不算爆炸,爆炸的是维护者视角。**

- **P8a 参数审计:** 12 个基础组逐个分类"每 MI 会调 / 项目级一次设定"(`13 - Ray Tracing Shadow` 14 个、
  `00 - Global Settings` 13 个是重点),项目级下沉 MPC / PostProcess / ToonActorComponent
- **P8b 按原型拆 master:** `M_MoonToon_{Skin,Hair,Cloth,Eye,Generic}`,同一 DSL 源生成 N 个材质,
  各自只 include 需要的 feature 函数与参数组;MI 换父材质脚本 + 快照比对(v2 P7 的方法)
- **P8c 死文件:** `MF_MoonToonBuffer.dsf`(444 行 / 80 参数,除 BaseInput 一条注释外零引用)删除

---

## 3. 验证策略

沿用 v2 的 L1–L4,新增两条这次已经用过的:

| 层 | 手段 | 能证明什么 | 用在 |
| --- | --- | --- | --- |
| L1 源码 | `.dsf` 反编译对拍 | 图结构等价 | P5 插件侧 |
| L2 编译 | `dsc compile` 全绿 | 只证明能过 `-nullrhi` | 每次 |
| L2b HLSL | fxc/dxc 编译生成的 `.ush` | 真的能编译 | 每次 |
| **L2c 预处理** | **`ppdrv` 预处理 `ToonShadingModel.ush` 前后 diff** | **逐字节相同 = 零行为变化,机器判定** | **P1 / P2 / P6** |
| L3 参数 | MI override 快照前后语义比对 | 没有静默丢值 | P5 开关改名 |
| L4 画面 | 逐特征目检截图 | 最终判据 | P5b 新效果 |
| **L4b 画面 diff** | **同一 exec 里关 TAA 背靠背截 A → B → A,`|A−B|` 与 `|A−A'|` 比** | **差异 ≤ 时序噪声底(Showcase 实测 mean 0.27 / >4 的像素 0.31%)= 不可分** | **P2 / P3 / P4 / P5a** |
| **L5 性能** | **ProfileGPU,同视口同 SP,取两次最小值;只信单 draw 事件与 ParallelDraw 求和** | **主光 `RenderLight` ±10%** | **P3 / P4 末 / P5** |

L4b 的操作细节在 `.claude` memory(`devtest-project-unreal-bridge.md`)和本次 scratchpad 的
`parse_profilegpu.py` 里,搬进 `Tools/` 再用。**编辑器帧是 CPU-bound,GPU 空泡会随机落进某个 pass 的
exclusive,单次 ProfileGPU 的整帧数字不可信。**

---

## 4. 风险台账

| 风险 | 缓解 |
| --- | --- |
| `case MOON_..._##ID:` 过不了 stb_preprocess | P0 spike;退化方案 = 脚本生成派发器当普通源码提交 |
| P3 派发器让所有 feature 的 HLSL 同时变,回归面大 | P3 只搬 Skin,其余走 `default:` legacy;L4b 全特征;每模块一提交可单独回滚 |
| hook 拆分后编译器合并不了分支,light PS 变慢 | L5 ±10% 门槛;超出退到 §6 的单 `Module_Shade` 形态,契约不变只是派发粒度变 |
| L4b 噪声底被动态天空吃掉真实差异 | 关 TAA;控制组 A→A' 必须同 exec;必要时 UDS 静止(`Time` 节点冻结)再测 |
| Modifier 一次只能一个,将来要并存 | RT5 加一张(实测一张 RGBA8 在噪声内),机制不变;先不做 |
| `ToonMaterialOutput` 加 pin 触发全部 toon 材质重编 | 已接受;和任何 shader 改动一样是一次 DDC 作废 |
| matcap 开关改名丢 MI override | v2 P7 快照法;参数名不动 |
| 删 C++ 枚举后内容里仍有材质用它当下拉 | P0 先扫;扫不到才删 |
| DreamShader 上游 PR 节奏拖住 P7 | P7 排最后且可选;前六步不依赖它 |
| Stockings 7/14 policy 搬错 | 排最后,单独提交,L4b 用 stockings MI 单独 spot 场景 |

---

## 5. 明确不做

1. **不把高光轴正交化。** v2 §5 的槽位预算论证仍成立。hook 结构是它的前置而不是它本身:哪天真多了
   一张 RT,把 `Specular` hook 的派发键从 `ShadingFeatureID` 换成独立字段是局部改动。
2. **不消灭二次几何 pass。** 实测 0.15–0.22 ms、几何瓶颈、随角色数线性,比例上始终是 base pass
   draw 的 ~60%;RTV 预算已满(SceneColor + GBufferA–F + ToonSurfaceRT = 8)也塞不进 base pass。
3. **不给 Modifier 加 RT5。** 需要两个并存时再说。
4. **不分叉 DreamShader。** 见 P7。
5. **不重设计 wire 格式**(沿用 v2 §5.1)。RT3.w / RT4 的用法只是给已有字节一个由 RT3.w 决定的含义。

---

## 6. 待决

| 事项 | 选项 | 建议 |
| --- | --- | --- |
| ~~C++ 枚举 `EMoonToonShadingFeature`~~ | ✅ **P0 已决:删除** | 全库 23058 个 `.uasset` 扫描,活资产零引用;唯一命中是 `Content/Project/` 下 08-10 的重构前遗留副本(见 `P0-v3-result.md` §2) |
| hook 粒度 | 4 个 hook 各一个 switch / 1 个 `Module_Shade(Ctx, inout Lighting)` 一个分支 | **4 个**(可读性;性能已证明无关);L5 超标再退 |
| ModifierID 授权 | `ToonMaterialOutput` Pin[4] / 新增 `MoonEncodedAttribute5` | **Pin[4]**(shader 一行 + C++ 一个数字;属性路要动 translator) |
| Manifest 格式 | TOML / JSON / DreamShader `.dsh` 风格自定义块 | P6 先用 **TOML**(生成器是 Python);P7 上游有了自定义块再迁成 DSL |
| DreamShader | 扩展 API 上游 / 孵化分支 | **扩展 API**;分支只当孵化 |
| 主材质拆不拆(P8b) | 拆原型 / 只审计下沉 | 先 P8a 审计,看下沉后还剩多少再决定 |

---

## 7. 没验证的,别当结论

1. `case ... ##ID:` 形态的 X-macro 在 stb_preprocess 下是否可用 —— P0 跑。
2. GBuffer 解码器顶部无条件 `Load` 五张 ToonFeatureRT(`ShaderGenerationUtil.cpp:1726`),编译器是否下沉进
   `if (TOON)` 分支 —— 从主光 draw 0.16 ms 看即便没下沉也不贵,但**没看反汇编**。与 v3 无关,顺手看一眼。
3. 半分辨率暗部晕染(`e592ae3e`)只在 Showcase 正面机位对过 A/B;近景脸、多灯没截过。
4. TSR 开着时 L4b 的噪声底是多少 —— 本次全部关 TAA 测的。
5. `Modifier_Apply` 放在 `MainLightRampTint` 乘完之后 —— 与今天 matcap 的位置一致(`ToonShadingModel.ush:492-510`),
   但油膜作为高光项可能希望在 `Lighting.Specular` 累加之后、`ToonShadowTintScale` 之前;P5b 定。

---

## 8. 附:2026-08-18 测量摘要

条件:RTX 4070 Ti SUPER,D3D12 SM6,`Lvl_Toon_Showcase`(6 角色 ~2.8M tri/帧,UDS 太阳 + 光追阴影,
Lumen HWRT 反射 + ScreenProbeGather,TSR Epic,体积云),编辑器视口内部分辨率 2098×1045(≈1080p)。
取两次运行最小值。

| 项 | ms | 备注 |
| --- | --- | --- |
| Graphics 整帧 / async compute | 6.55 / 3.15 | Lumen SPG 2.06、TSR 0.35 在 async |
| ToonFeatureRTClear(5×RGBA8)| 0.062 | 按最大 extent 全清,固定成本 |
| TObjectID clear | 0.027 | |
| `Moon_MeshPass_Base`(308 draws)| 0.219(draw 求和 0.146)| 100% 时 0.187 / 0.125 → 几何瓶颈 |
| ToonPass_Blur 合计 | 0.484 | 暗部晕染全分辨率高斯 0.269(**已改半分辨率 → 0.17,提交 `e592ae3e`**)、FaceOverlay 0.030、toon bloom ~0.09 |
| ToonTranslucentPrepass | 0.030 | |
| 主光 `RenderLight StandardDeferred` 整个 draw | 0.159 | 含非 toon 像素;stock 方向光同卡 ~0.1–0.15 |
| 临时点光 ×3 的着色 draw | 0.079 / 0.049 / 0.021 | 各自光追阴影 0.42–0.62 + 去噪 0.51 |
| **toon 专属小计** | **≈0.82–0.98(12–15%)** | 半分辨率后 ≈0.65–0.8 |

对照:BasePass 0.35(draw 求和 0.22)、Velocity 0.28、太阳光追阴影 0.38 + 去噪 0.48、体积云 0.37、
Lumen 反射 0.20、TSR 0.64(gfx)。

结论用在 v3 里的只有两条:性能不约束结构;一张 RGBA8 lane 在噪声内。整帧能不能在中端 60 fps
是光追阴影 / Lumen HWRT / TSR / 体积云的问题,不是 toon 的。
