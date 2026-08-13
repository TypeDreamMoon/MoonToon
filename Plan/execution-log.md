# 执行日志

无人值守跑 P0–P8。记录**计划之外的发现**和**与计划的偏差**,方便你回来时只看这一份就知道哪里变了。

---

## P0 ✅ X-macro spike — PASS

见 `P0-spike-result.md`。结论:X-macro 可用,`##` 也可用,退化方案不启用。

---

## P1 ✅(代码完成,编译验证中) SDF 迁出 Metallic/Anisotropy

### 与计划的偏差

**1. 没有新增 `ToonDataD` 材质输出针脚 —— 反而撤掉了。**

计划里写"材质通过第 4 个 ToonMaterialOutput pin 写 TBufferD"。实现时发现这会造出**第二个真值源**:
SDF 已经通过 `MoonEncodeToonAttributes` → `MoonEncodedAttribute4.xy` 授权了,再开一个 pin 就是两条
路可以互相矛盾 —— 正是这次重构要消灭的病。

改为:toon mesh pass 的 PS 用 `GetMaterialMoonEncodedAttribute4(PixelMaterialInputs)` 读**已有的**
授权值,组装成 TBufferD(`ComposeToonBufferD`)。

**后果:现有材质一行都不用改。** P1 对插件侧零影响,风险大幅下降。

**2. `DecodeToonDataFromBuffer` 的第 4 个参数是复用的,不是新增的。**

那个参数原本叫 `CustomData`,而函数体里**从来没读过它**。直接改成 TBufferD,所有调用点的参数个数不变。

**3. `EncodeToonBuffer` 保持 4 参数不变。**

一度改成 5 参数带 TBufferD,又撤回了:它只负责 feature 槽位,而 TBufferD 是 feature-无关的另一条
路。混进去会波及插件的 `.dsf`(`MoonToonBufferWrite` 调它),而且概念上是错的。

**4. 顺手做了计划没写的减法。**

- `EncodeToonGBufferDataToAnisotropy` **整个删除**(它唯一的作用就是那次劫持)
- `DecodeToonGBufferDataFromMRT` 去掉 `inout float Metallic` 和 `float DecodedAnisotropy` 两个参数
  —— 洗钱的两头都没了,这函数现在完全不碰 stock GBuffer
- 那个只被一处调用的 `float4 InTBufferA` 重载删除

### 新发现(计划里没有的)

**⚠ `VirtualShadowMapProjection.usf` 直接读 legacy ID 的 bit[3:2]。**

`VirtualShadowMapProjection.usf:136` 用 `(ToonCustomDataW >> 2) & 0x3 == 0x3` 判断"是不是 SDF 脸",
以此选自阴影的 occluder 距离阈值。**P2 如果按原计划直接删掉 legacy 2 bit,这里会静默失效** ——
SDF 脸会拿到普通阈值,自阴影行为变化,而且没有任何编译错误。

**这反而让 P2 的方案更好了:** VSM 要的本来就不是 feature id,是"是不是 SDF 脸"。所以

```
旧 CustomData.w: [ReflectionIntensity 3 | DisableShadowMapShadow 1 | LegacyFeatureId 2 | RTFlag 2]
新 CustomData.w: [ReflectionIntensity 4 | DisableShadowMapShadow 1 | FacialShadowSdf 1 | RTFlag 2]
```

ReflectionIntensity 拿回第 4 bit,VSM 改读那个专用 bit,`ResolveToonFeatureIdFallback` 整个删除。

### 布局

```
TBufferD (RGBA8, 新增第 4 张 toon 特征 RT)
  .x  FacialShadowSdfLeft
  .y  FacialShadowSdfRight
  .z  MOON_TOON_FLAG_* 位域(bit0 = SDF 脸阴影启用)
  .w  备用
```

toon pass MRT 4 → 5(上限 8)。RT 顺序改成 A/B/C/D/TObjectID,`TObjectID` 从 `SV_Target3` 挪到
`SV_Target4`。分配仍受 `ViewFamilyHasToonContent` 条件门控,非 toon 场景零成本;toon 场景 +4 B/px。

`ApplyDistanceFieldFacialShadow` / `bUseFacialShadowOverride` / `ShouldSuppressToonDefaultShadow`
三处的门从"feature id == DFF"改成"TBufferD 的 SDF 标志位"。**当前行为逐位相同**(标志位正好在
feature==DFF 时置位),但阴影技术不再和表面模型绑死 —— 这是根因那一刀真正落下的地方。

`bIsSkinFeature = SKIN || DFF` **暂时保留**,按计划归 P6:要等材质侧能给 DFF 写参数,去掉它才不会
让 DFF 脸从"零高光"变成"零高光且没有皮肤散射"。

---

## P5 预研:一个计划标注"必须实测"的问题,从源码答掉了

计划 §P5 写着"`TextureObjectParameter` 在 MI 里按名绑定,要实测确认而不是假设"。不用实测:

- `UMaterialExpressionTextureObjectParameter` 和 `UMaterialExpressionTextureSampleParameter2D`
  **同属** `UMaterialExpressionTextureSampleParameter`
- MI override 查找是 `GetParameterOverrideValue(EMaterialParameterType::Texture, ParameterInfo.GetName(), ...)`
  (`MaterialExpressions.cpp:13619`)—— **按类型+名字**,不看节点类

所以换节点类、保持参数名不变,MI 的贴图绑定不会丢。P5 的双采样抽取可以照做。

---

## ⚠ 事故记录:F: 盘被我自己塞满,已恢复

**别用 Rider MCP 的 `build_solution_start` 编这个解决方案。** 它是 "Build Solution" 语义 ——
编解决方案里**每一个目标**,不是编辑器。它按顺序编了 `AnimationWarpingRuntimeTests`(10.7 GB)、
`ASDTool`(4.9 GB)、`BaseTextureBuildWorker`、`BenchmarkTool`,把 F: 从 ~18 GB 吃到 1.67 GB,
然后所有编译以 `fatal error C1085/C1088: No space left on device` 失败。

**这些失败与本次重构的代码无关**,一条都不是。

处理:删掉那 4 个目标在 `Engine/Intermediate/Build/Win64/x64/` 下的中间产物(纯 obj/pch,可再生,
而且是这次误操作刚生成的),回收 16.35 GB → F: 剩 23.10 GB。
**没有动** 46 GB 的 `UnrealEditor` 中间产物(12-24,增量编译的本钱)、Binaries、Source、Content。

正确做法:`Engine/Build/BatchFiles/Build.bat DevTestEditor Win64 Development -Project=<uproject>`,
单目标。

**F: 长期只有 ~20 GB 余量,对一次引擎全量编译是偏紧的**,你回来后可能要清一下。

## 环境限制(影响验收口径)

**UE 编辑器没有运行**(`ue_health` → `connected:false`),而你不在电脑前,所以:

- 材质/着色器的**真实编译**验证不了(`dsc` 需要编辑器二进制,而且要等引擎编译完)
- P7 的 MI override 快照对比做不了(需要 unreal-bridge 连编辑器)
- 画面目检按约定归你

能做的验证:C++ 编译、静态一致性检查(删掉的符号无残留引用、参数个数对齐)、
以及用引擎自己的 stb_preprocess 做预处理级检查。每一项的实际验收口径会在这里如实记录。
