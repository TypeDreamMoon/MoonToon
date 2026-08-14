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

## 完成度总表

| 阶段 | 状态 | 提交 | 验收到什么程度 |
| --- | --- | --- | --- |
| P0 X-macro spike | ✅ 完成 | `29f219d` (插件) | 用引擎自己的 stb_preprocess 实跑,0 诊断 |
| P1 SDF 迁出 Metallic/Anisotropy | ✅ 完成 | `f17230a7` (引擎) | DevTestEditor 编译 0 错误 |
| P2 删 2-bit legacy ID | ✅ 完成 | 同上 | 同上(两者交织,见提交说明) |
| P3 RT 改名 | ✅ **完成**(拆 context 除外) | `ab3218fd` `7ad255d7` `962fa89f` | 429 处改名含 Engine/Plugins;`UnrealEditor` 目标编译 0 错误。`FMoonToonContext` 拆分未做 |
| P4 槽位表 + 读侧具名化 | ✅ **完成** | `9f534e52` `7ad255d7` | 表 + 13 个生成结构体 + Matcap 修复 + **80 处裸读全部具名化** |
| P5 基础函数解耦 | OK **完成** | `2614d8b` `5fcb621` | UV 链 + 14 对贴图双采样全部抽出;基础函数 2308 -> 1882 行 |
| P6 特征拆分 + 静态派发 | ✅ 完成 | `22aa9bf` `71383a4` (插件) | 13 个函数 + 派发器 + 基础函数 + 2 个材质全部 dsc 编译通过 |
| P7 改名 + 面板整理 + MI 迁移 | ✅ **完成** | `b1d2ed8` `2614d8b` `65ea60f` | UV 12→3 参数、分组重排、每参数补描述;**88 MI / 5773 override 前后逐项对比 0 丢失 0 改变** |
| P8 Stockings 布局位 / 死资产清理 | ✅ **完成** | `65ea60f` + 引擎侧 | 值猜测删除(88 MI 零丝袜 override,可证明安全);`MoonToonInput` 删除,另两个保留 |

### P3 为什么只做一半

剩下的是 `ToonBufferA` → `ToonSurfaceRT`、`TBufferA..D` → `ToonFeatureRT0..3` 的**290 处**重命名,
外加 `FMoonToonContext` 拆分(约 100 处消费点)。三条理由推迟到功能阶段之后:

1. 纯 churn 换可读性,不 gate 任何东西
2. 会让这条分支的 diff 无法审阅
3. 重命名数据驱动 GBuffer 的 target 字符串会作废整个 shader DDC

真正会导致事故的那部分(`DecodeToonBufferA` 读的是 TBufferA、`EncodeToonBuffer` 的出参叫
ToonBufferX 却装 TBufferX)**已经修了**。

### P4 为什么只做一半

槽位表、13 个生成结构体、Matcap 的 ScalarA 修复、两份手抄表的删除都已落地。
剩下的是 `ToonBxDF` 里约 40 处 `TBuffer.FeatureInputScalarD` 改成 `Skin.PrimaryRoughnessScale`。
它们**当前是正确的**,转换只是可读性,而每一处改错都会静默改变着色 —— 在无法编译 shader 的
情况下做 40 处不可验证的改动,不划算。

### P7 已闭环(此前记的"被阻塞"作废)

编辑器起来之后快照就能抓了。改名前后各一次全项目扫描,逐项对比 0 丢失 0 改变 —— 见文末。

---

## 验收口径(如实)

| 做过的验证 | 覆盖了什么 |
| --- | --- |
| DevTestEditor UBT 编译 0 错误 | 所有引擎 **C++** 改动 |
| stb_preprocess 实跑 | 槽位表展开正确;生成的 Cloth 结构体与手写版逐字段一致 |
| `dsc compile -Force` | 13 个 feature 函数、派发器、基础函数、M_MoonToon、M_MoonToonOutline **能生成** |
| 静态一致性 grep | 删除的符号无残留引用;所有调用点参数个数对齐 |

### 补充:着色器 HLSL 已经真正编译过了(2026-08-14 后补)

上面那条"着色器从未真正编译过"的限制**已经解除**。编辑器启动后:

| 证据 | 覆盖到什么 |
| --- | --- |
| `Engine is initialized`,编辑器正常启动 | 引擎 C++ 改动在运行期没有崩 |
| `LogShaderCompilers: Shaders Compiled: 10,979`,**0 错误、0 个 ShaderCompileWorker 失败** | 所有 include `DeferredShadingCommon.ush` 的着色器 —— 即 P1–P4 的引擎 `.ush` 改动 |
| `Compiled OcclusionRGS for RTPSO in 2523 ms` | **改动最深的那条链**:`RayTracingOcclusionRGS.usf` → `RayTracingDeferredShadingCommon.ush` → 带 `InTBufferD` 的生成签名 → `ToonBufferCommon.ush` 解码 |
| `recompile_material` 后 `M_MoonToon` 报 432 VS / 393 PS 指令,`M_MoonToonOutline` 报 488 / 415 | 两个 toon 材质的 shader map **真的建起来了**(编译失败的材质会回落默认材质,报不出指令数) |
| 全日志中 `Failed to compile` 无一条涉及 MoonToon | 静态派发 + 13 个 feature 函数在真实材质编译中成立 |

**仍然没有做的**:画面目检(按约定归你)、MI override 快照对比(P1–P6 刻意没重命名任何现存
参数,唯二消失的是两个死参数)。

### 顺带发现的、与本次改动无关的既有问题

- `M_BL_Textures_Parity`(DreamShader 的 Blender 对拍资产)编译失败:
  `PreSkinnedPosition` / `PreSkinnedNormal` 在 pixel shader 里不可用。**先于本次改动存在。**
- 两条 DreamShader 错误来自 `DShader/Decompiled/` 下 MooaToon 的反编译源:
  `Functions/MoonToon/MF_MoonToonBaseInput.dsf` 路径解析不到,以及 `MF_MooaDecodeAttributes`
  资产不是 DreamShader 生成的。**先于本次改动存在。**

## 你回来后建议的第一件事

**逐特征目检。** 重点三个:**SDF 脸的高光**(P1+P6 之后第一次可能非零)、**Matcap**(全新)、
**发丝高光的视角锚定**(全新)。

## 环境限制(影响验收口径)

**UE 编辑器没有运行**(`ue_health` → `connected:false`),而你不在电脑前,所以:

- 材质/着色器的**真实编译**验证不了(`dsc` 需要编辑器二进制,而且要等引擎编译完)
- P7 的 MI override 快照对比做不了(需要 unreal-bridge 连编辑器)
- 画面目检按约定归你

能做的验证:C++ 编译、静态一致性检查(删掉的符号无残留引用、参数个数对齐)、
以及用引擎自己的 stb_preprocess 做预处理级检查。每一项的实际验收口径会在这里如实记录。

---

## 收尾轮补充(P5 / P7 / P8)

**一处我之前判断错了,已更正。** 我把 `Rim_Light_Width_Channel_1` 和
`Distance_Field_Facial_Shadow_Map_Channel_1` 说成"反编译残留的重复参数"。读代码后发现不是:
`_1` 掩的是**另一个输入**(顶点色 / 第二张 SDF 贴图)。同一个参数名、两个使用点,就必须是两个节点 ——
UE 里 ChannelMaskParameter 的遮罩通道共享而节点不共享。删掉会断图。**这一项作废,不做。**

**P5 已完成。** UV 链抽成 `MF_ToonUV`,14 对"双采样 + Enable Per Texture Sampler"抽成 4 个共享
采样函数 —— 细节见文末的「P5 收尾」一节。

**P7 的验收方式。** 改名前后各抓一次全项目 MI 快照(资产注册表全量,88 个 MI、5,773 条 override),
逐项语义对比:**0 丢失、0 改变、0 新增**。快照两份都提交在本目录,可复查。

**P8 的两条都变成了可证明安全的删除**,不是"应该没问题":
- 丝袜布局值猜测:88 个 MI 里 `In_Stockings_*` override 为零 → 没有内容依赖 legacy 布局
- `MoonToonInput.uasset`:全项目 Content 零引用 → 删除
- `MF_MoonToonBufferInput` / `Buffer/Writer` **保留** —— MooaToon 的 `M_Toon` 仍在引用,而它有 4 个活实例

---

## P5 收尾:贴图双采样抽取完成

14 对"双采样 + Enable Per Texture Sampler"抽成 4 个共享函数。为什么是 4 个而不是 1 个:
`SamplerType` 和 `SamplerSource` 在 UE 里都是**节点属性而非引脚**,做不成函数参数 ——
所以按 SamplerType 分 Color / Linear / Normal 三个,外加一个 LinearMip0 给 ID 图
(它的值是标识符,不能跨 mip 模糊)。

**贴图参数留在调用点**(改成 `TextureObjectParameter`),没有放进共享函数 ——
放进去会让 14 张贴图塌成同一个参数名。`Enable Per Texture Sampler` 反过来**放进函数**,
因为它本来就该是"一个共享决定用一个共享名字",面板上仍是一行。

6 处 `TextureSampleParameter2D` 保留:它们是各带自己 UV 的**单次采样**
(SDF 左右脸两张、发丝高光遮罩、内描边图),不是采样器配对。

### 最终 MI 验收

改动前 / 全部做完后各一次全项目快照,逐项对比:

```
MIs before=88 final=88   lost=0 changed=0 added=0
texture overrides: before=341 final=341
```

**341 条贴图 override 一条没丢** —— 这是"换节点类按名安全"的实证,不再是从引擎源码推断的结论。
三份快照(before / after / final)都在本目录。

## 全部阶段状态

P0–P8 全部完成。仍然刻意未做、并已在上文说明理由的两项:

1. **P3 的 290 处 RT 大改名** + `FMoonToonContext` 拆分 —— 纯 churn 换可读性,会作废整个 shader DDC
2. **P4 的约 40 处裸槽位读取转换** —— 当前正确,转换只是可读性

两项都不改行为,不 gate 任何东西。

---

## P3 / P4 收尾轮:两次同类错误,都记下来

P3 的 290 处改名和 P4 的 80 处具名化都做完了,但过程中我犯了**两次同一类错误** ——
改名的边界和范围都想窄了,而且两次都是"验证面比改动面窄"才拖到后面才暴露。

### 错误一:规则没有左边界,误伤不相关代码

`TBuffers` / `TBufferA` 我加了防 `TBufferArchive` 的排除,却忘了这些名字也会作为**子串**
出现在别的标识符里。第一遍把这些一起改了:

| 被误改 | 实际含义 |
| --- | --- |
| `SBTBuffers` | 路径追踪的 shader binding table |
| `STAT_D3D12RTBuffers` | 光追显存统计 |
| `PRTBuffers` | d3dx9mesh |
| DXC / libwebp 里一批 `tbuffer` 符号 | 打包的第三方编译器 |

**怎么抓到的**:提交时的文件列表里冒出了 `FreeImage` 和 `DirectXShaderCompiler` —— 一眼不对。
已 `reset --soft` 撤回、从 HEAD 还原第三方树、把引擎侧那几个改回原名。
最终提交 37 个文件,零第三方。

正确的规则要有 `(?<![A-Za-z0-9_])` 左边界,而不是逐个排除已知冲突。

### 错误二:范围漏了 Engine/Plugins

第一遍只走了 `Engine/Source` 和 `Engine/Shaders`。TextureShare 还引用着
`FSceneTextures::ToonBufferA`,引擎目标编不过。

**为什么拖到这么晚才发现**:前几次都只编 `DevTestEditor`,而 TextureShare 没被这个项目启用。
**只验证一个目标是不够的。**

一处**没有**照改:

```cpp
// 字符串字面量故意保持旧名 —— 它是外部 TextureShare 消费者索取纹理用的线上名字
static constexpr auto ToonSurfaceRT = TEXT("ToonBufferA");
```

C++ 标识符跟着改(否则编不过),但那个字符串是 nDisplay / 外部程序按名字要纹理的对外契约。
改它属于行为外溢,不该由一次内部改名顺手决定。想统一的话改一行即可。

### 验收

`UnrealEditor Win64 Development` 编译成功,0 错误。全树残留检查:引擎 C++ 只剩
`TBufferArchive`(有意排除),着色器侧零残留。

---

## 仍然欠着的

**1. MI 迁移(必须做,否则 13 个材质实例的特征选择是失效的)**

P6 删掉了 `ShadingFeatureID` 标量参数,但**13 个 MI 覆盖过它**。MI 里存的 override 不会因为
材质删了参数而消失,只是失效 —— 所以快照对比报「0 丢失」,而它们实际上已经选不到任何特征,
全部回落成普通 toon。**我在 P6 提交里写的「这个提交本身不改变任何材质实例」对这 13 个是错的。**

要迁的映射(feature id -> 静态开关):

| id | MI |
| --- | --- |
| 2 KajiyaHair | MI_N00_000_00_HairBack, MI_N00_000_Hair_01, MI_N00_000_Hair_02, MI_MI_Bangs, MI_MI_Hair |
| 3 DFFacialShadow | MI_RadDollV3_Face, MI_N00_000_00_Face, MI_MI_Face, MI_face |
| 6 Skin | MI_N00_000_00_Body |
| 9 ClothVelvet | MI_Cloth |
| 10 ToonMetal | MI_Metal, MI_头饰 |

外加 5 个开着旧 `Enable Feature *` 开关的(与上面有重叠)。

**迁移脚本必须跳过父链解析不了的 MI。** 第一次尝试崩了编辑器:
`IterateDependentFunctions` 在 `/Game/Decompiled/` 那棵 MooaToon 反编译树上解引用了空的
`MaterialFunction`(那三个缺失函数先于本次改动存在,第一次启动就在报)。引擎那里没有 null 检查。

**2. `FMoonToonContext` 拆分** —— P3 的另一半,约 100 处消费点,未做。
