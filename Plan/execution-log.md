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
| P3 RT 改名 + context 拆分 | ✅ **完成** | `ab3218fd` `7ad255d7` `962fa89f` + 本次 | 429 处改名含 Engine/Plugins;`FMoonToonContext` 按生命周期拆成 3 份、删 2 个死字段、3 个宏变函数;顺带修掉 P1 引入的 SDF 恒零 bug;两个目标编译 0 错误 |
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

## Matcap:从"死特征"到修饰符(M1–M6)

用户指出 matcap 应该有贴图和 mask。查下来比这更糟:`In_Matcap_ColorRGB` 是个默认
`(0,0,0,0)` 的 VectorParameter,描述里写着"由材质图用视空间法线当 UV 采样贴图后接进来",
**但没有任何东西那样接** —— matcap 恒输出黑色,整个特征从落地起就是死的。

### 查了标准再动手

用户让去查 NPR matcap 的通行做法,结论和我原来的设计相反:

- **MToon 1.0(VRM 规范)**:`The result of matcap is additive blended to the lighting result.`
  遮罩来自 `rimMultiplyTexture`,另有 `rimLightingMix` 控制吃不吃光照。
- **Unity Toon Shader**:混合模式 Multiply/Add,并有独立的 MatCap Mask + Mask Level + Invert。

UV 也不是"视空间法线 .xy*0.5+0.5",而是拿视线现搭一个坐标架:

```
worldViewX = normalize(Vector3(V.z, 0.0, -V.x))   // y 分量硬写 0
worldViewY = cross(V, worldViewX)
matcapUv   = Vector2(dot(worldViewX,N), dot(worldViewY,N)) * 0.495 + 0.5
```

`worldViewX.y = 0` 是**抗相机 roll** 的关键(Unity 把这件事做成了 Stabilize Camera Rolling
开关);`0.495` 是防止采样溢出球边的半像素余量。MToon 是 Y-up,UE 是 **Z-up**,所以照抄会绕
错轴 —— 这里写成 `normalize(float3(V.y, -V.x, 0))`。另外规范没写的一条:正上方俯视时该坐标架
退化(V.xy≈0),这里加了固定轴回退,否则出 NaN。

### 结构后果:matcap 根本不该是特征

既然是**加性**,matcap 表面同时还是皮肤/头发/布料表面。做成互斥的 `ShadingFeatureID` 意味着
"勾了 matcap 就关掉皮肤着色" —— 和 SDF 脸部阴影**同一个类别错误**(阴影*技术*不是表面*模型*)。
VRoid 的 SphereAdd 正是同时铺在全身各材质上的。

所以走了 A 方案:**加第 5 张 RGBA8(`ToonFeatureRT4`)**,matcap 进 feature-independent 通道。
`.xyz` = 已乘遮罩和强度的 matcap 颜色(遮罩折在授权侧,引擎只做乘法,GBuffer 不用多花通道),
`.w` = lighting mix。特征 id 13 **退役不复用**,`MOON_TOON_SLOTS_MATCAP` / `FToonMatcapParams` /
`bIsMatcap` 两个分支全部删除,着色只剩最后一句加法。

### 三个值得记的坑

1. **`FSceneTextureParameters` 和 `FSceneTextureUniformParameters` 是两个结构体**,分别在
   `SceneTextureParameters.h` 和 `SceneTexturesConfig.h`。只加前者会编译过一半再炸。
2. **`cross` 不是 DreamShaderLang 内建**(内建只有 lerp/dot/pow/min/max/clamp/saturate/sin/cos/
   abs/floor/ceil/frac/sqrt/normalize/fmod)。矢量数学走 `Custom` 节点,反而让整段 MToon 公式
   能按规范原样读。
3. **`VirtualFunction` 是重述签名**:给 `MF_MoonToonBaseInput` 加输出,必须同时改 `M_MoonToon.dsm`
   里的那份声明,否则报 `OutputIndex is out of range`。
4. **`MoonToonShadingFeature.h` 是特征表的第三份拷贝**(C++ UENUM + static_assert 联动)。
   删 id 时它编译报错 —— 这正是那套 static_assert 存在的意义,机制生效了。

---

**0. 面板改名 `Feature_Is_X` → `Is X`(已完成,提交 `0c83719`)**

十二个特征开关是面板里唯一还带子系统前缀 + 下划线的一组,而同一张列表里它们的邻居是
`Is Face` / `Is Hair`。DSL 标识符不能带空格,所以两种拼写一起声明:标识符留下划线
(`Is_KajiyaHair`),`ParameterName` 带空格 —— `MF_MoonToonBaseInput` 里 `Global Mask Map`
早就是这个写法,不需要给语言加东西。

**改参数名不会删掉材质实例里存的 override,只会让它失效** —— 与 P6 删 `ShadingFeatureID`
同一个坑。所以顺序是:趁旧名字还能解析先把 12 个实例的选择读出来 → 改名 → 按新名字重打 → 存盘。
事后核验:12 个实例各选中且只选中一个特征;没被碰的 `Is Face` / `Is Hair` 计数仍是 8 和 6;
`M_MoonToon` 指令数 432/393 与基线一致。

**遗留:`MI_头饰` 没有迁移。** `mi-snapshot-before.json` 里它有
`ShadingFeatureID=10.0`(= ToonMetal,与 `MI_Metal` 同值),但用户那轮迁移只覆盖了 12 个,
它现在一个特征都没选 ⇒ 回落成普通 toon。要不要开是内容决定,没替用户勾。

**1. MI 迁移(用户已自行完成 12 个;下表保留作记录)**

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

**2. `FMoonToonContext` 拆分** —— 已完成,见下节。

---

## P3 下半场:`FMoonToonContext` 拆分

`FGBufferData` 上原来挂着一个 13 字段的 `FMoonToonContext`,里面混着**三种生命周期**,类型上没有
任何东西能把它们区分开:

| 字段 | 真实生命周期 |
| --- | --- |
| `ToonBuffer` / `ToonGBuffer` | 每像素 |
| `EncodedToonSurfaceRT` | 每像素,而且根本不是 toon 状态 —— 是一张原始 GBuffer 槽 |
| `ToonLight` / `LightType` / `LightColor` / `TintShadow` | **每盏灯** |
| `PixelPos` / `BufferUV` / `ViewportUV` | **每视图** |
| `Exposure` | 每视图,而且是 `GetMoonExposure()` 的一份镜像 |
| `ShadowMap` / `IsEditorPreviewWorldType` | **没有任何地方读** |

代价不是抽象的:所有 helper 的签名都写着 `FMoonToonContext`,你得读函数体才知道它到底依赖什么。

### 改成

新文件 `Toon/ToonShadingContext.ush`:`FToonLightContext`(每灯)、`FToonViewContext`(每视图),
外加 `InitToonLightContext()` / `InitToonViewContext()` / `MoonToonClassifyLightType()` /
`MoonToonSetViewPixelPos()`。每像素那半不需要包装 —— `FToonGBufferData` / `FToonBuffer` 本来就是
那两个类型,直接挂在 `FGBufferData` 上(`ToonGBuffer` / `ToonFeature`)。`EncodedToonSurfaceRT`
搬到顶层与 `CustomData` 并排,它一直就是那个东西。

`ShadowMap` / `IsEditorPreviewWorldType` 删除。`Exposure` 删除,唯一的读者改为直接调
`GetMoonExposure()`。三个 `SetMoonToonContext_*` 宏(伸进两层嵌套改字段,还隐含依赖作用域里
有个叫 `GBuffer` 的变量)变成普通函数。

签名现在自己说明生命周期:

```
GetShadowColorIntensity(FToonLightContext)                        // 只要灯
GetToonFilteredSurfaceShadow(FToonLightContext, FToonViewContext, ...)  // 真的跨两种
GetMainLightShadowTint(FGBufferData, FToonGBufferData, FToonLightContext)
```

### 顺手抓到一个真 bug(P1 引入的,一直是活的)

`GBUFFER_REFACTOR` 恒为 1(`ShaderGenerationUtil.cpp:2259` 写死 `bUseRefactor = true`),所以走的
是**生成**的 GBuffer 解码器。而它的顺序是:

```
...槽位赋值(含 Ret.EncodedToonSurfaceRT)...
GBufferPostDecode(Ret, ...);          // 这里面读 Ret.ToonFeature
Ret.ToonFeature = DecodeToonDataFromBuffer(...);   // 才在这里赋值
```

这不是读代码推的,是从**实际生成的文件**里读出来的 ——
`Intermediate/ShaderAutogen/PCD3D_SM6/AutogenShaderHeaders.ush`:`DecodeGBufferDataDirect`
(约 106 行)调 `GBufferPostDecode` 而**整个函数从头到尾没有 `Ret.ToonFeature` 的赋值**;
MRT 版(约 166 行)调完 `GBufferPostDecode` 之后才在 175 行赋值。

`GBufferPostDecode` 里 `DecodeToonGBufferDataFromMRT(..., Ret.ToonFeature, ...)` 把 SDF 对拷进
`FToonGBufferData`,而此时 `Ret.ToonFeature` 还是 `(FGBufferData)0` 的零值。
`ApplyDistanceFieldFacialShadow` 读的正是这份拷贝 ⇒ **`shadowSdf` 恒为 0,SDF 脸部阴影是死的。**

P1 之前不会:那时 SDF 从 `Ret.Metallic` / `Ret.Anisotropy` 里捞,而它们是普通槽位,在
`GBufferPostDecode` 之前就填好了。P1 把 SDF 挪到 ToonFeatureRT3 之后,读取点跟着挪,顺序依赖
就此成立而没人发现 —— **L 类验收(编译 0 错误)对这种 bug 完全失明。**

修法是取消那份拷贝,不是调顺序:SDF 只有一个家(`FToonBuffer` 的 ToonFeatureRT3 通道),
`ApplyDistanceFieldFacialShadow` 直接从它已经收到的 `TBuffer` 读。于是
`DecodeToonGBufferDataFromMRT` 的 `FToonBuffer` 参数整个删掉,顺序依赖从根上没了。

同族清理(都是签名说谎):

- `EncodeToonGBufferDataToMRT` 的 `FToonBuffer` 参数 —— 函数体一行都没读过
- `ApplyDistanceFieldFacialShadow` 的 `FGBufferData` / `FToonGBufferData` 两个参数 —— 拷贝取消后全部无用
- BasePass 里 `ToonFeatureRT0Texture.Load` + 解码 —— 只为喂上面那个没人读的参数,
  而且 ToonFeatureRT0 是本 pass 自己的 MRT(源码上等于在读正在写的 render target)。
  **实测:删掉前后 `M_MoonToon` 指令数一模一样(VS 432 / PS 393),说明 DXC 早就把它整条
  消掉了 —— 这是源码清晰度修复,不是性能修复,别把它说成性能收益。**

### 验收

- `UnrealEditor Win64 Development`:`Result: Succeeded`,0 错误
- `DevTestEditor Win64 Development`:`Result: Succeeded`,0 错误
- 全树残留:`MoonToonContext` 只剩注释里的历史说明;`Engine/Plugins` 零命中(**这次是改之前就查了**)
- 编辑器重启后全量 shader 编译:**6,411 个 job 全部完成,0 个 `error X####`,0 个
  ShaderCompileWorker 失败**,日志里没有任何一条来自 toon 文件的诊断
- `M_MoonToon` 432/393、`M_MoonToonOutline` 488/415、`M_EyeBase` 521/375 —— 与改动前逐位一致
- 唯一的材质编译失败是既有的 `M_BL_Textures_Parity`
  (`PreSkinnedPosition`/`PreSkinnedNormal` 不能在 pixel shader 里用),与 toon 无关

### 没做的部分,以及为什么

计划里写的是「`FGBufferData` 不再嵌套 toon context」。**没做到,而且不是疏忽。**
`ToonBxDF` 的签名由 `IntegrateBxDF`(`ShadingModels.ush`)固定,引擎里每条光照路径都在调它,
toon 载荷没有别的入口。正确的去处是把每灯那半挂到 `FAreaLight` 上 —— 它本来就是 `ToonBxDF` 的
参数,而且已经带着 `FToonLight` —— 但 `FRect` / `FCapsule` 在 deferred lighting 之外还有十几处
构造点,那些地方会把新字段留成未初始化,而今天它们拿到的是 `InitToonLightContext()` 的默认值。
那是另一件事,得配自己的测试。

---
---

# v3 执行日志

计划:`refactor-plan-v3.md`(注册表 + hook 派发 + Modifier 轴)。轴规则:`axes.md`。

## P0 ✅ 完成 2026-08-19 —— spike PASS,枚举可删

详见 `P0-v3-result.md`。两句话:

- **spike PASS,0 诊断。** `case MOON_SHADING_FEATURE_ID_##ID:` 写在函数体内、同一张表在同一 TU
  展开多次、`##ID` 一行用两次 —— 全过。粘出来的记号会被**重新扫描**,落地是 `case 6 :` 而不是宏名。
  退化方案不启用。
- **`EMoonToonShadingFeature` 活资产零引用。** 全库 23058 个 `.uasset` 二进制扫描,唯一命中是
  `Content/Project/` 下 2026-08-10 的重构前遗留副本(只被 `test` 目录里一个 MI 间接引用)。
  → P1 删除。

v2 的 `ppdrv` 驱动没保留,重写了一份(scratchpad `p0spike/`)。v2 记的两个坑还在:
`-msse4.2` 必须给,`STB_LCG_NEXT` 要自己补,外加文件 buffer 的 16 字节 padding。

## P1a ✅ 完成 2026-08-19 —— 注册表 + 展开 + 生成器校验

引擎提交 `6c2fcb17`。插件提交见下。

**做了什么**

1. 新建 `Engine/Shaders/Shared/MoonToonFeatureRegistry.h`:`MOON_TOON_MODULE_LIST`(10 个模块 →
   槽位表)+ `MOON_TOON_FEATURE_LIST`(14 个 id → 模块)。别名从此是表里的事实,不再是某个 `if` 的性质。
2. `ToonFeatureParams.ush` 的 10 行手抄 → `MOON_TOON_MODULE_LIST(MOON_TOON_DEFINE_FEATURE_PARAMS)`。
3. `MoonToonFeatureSlots.h` 头注释指向注册表。
4. `gen_feature_functions.py`:`FEATURES` 删掉槽位表那一列,改由注册表 id → 模块 → 槽位表推导;
   新增 `check_registry_agreement()` 双向校验 + `FEATURES_WITHOUT_FUNCTION` 白名单。

**验收(全部实测)**

- 预处理**记号流相同**:1092 : 1092。
- `gen_feature_functions.py --check` **exit=0**,14 个生成文件逐字节零 diff。
- **故障注入证明校验抓得住错**(校验通不过才有意义):
  - 往注册表塞一个 `OIL_FILM` id → `id OIL_FILM is in MOON_TOON_FEATURE_LIST but has no FEATURES entry`
  - 塞一个没人路由到的 `OilFilm` 模块 → `module OilFilm is in MOON_TOON_MODULE_LIST but no id routes to it`

### 计划的验收口径改了一处,理由记下来

v3 P1 原文写的是"**逐字节相同**"。做不到,而且不该做到:一次列表展开会把十条声明放到**同一行**,
而原来是十行。**改成"记号流相同"** —— 去掉 `#line`、注释、折叠空白之后逐记号比对。
HLSL 在字符串和预处理指令之外对空白不敏感,所以记号流相同就是零行为变化,而且同样是机器判定。
后续 P2 沿用这个口径。

### 一个自找的坑,记下来防止再犯

生成器里那个"匹配反斜杠续行的 `#define` 块"的正则,写进脚本时**丢了一个反斜杠**
(`[^\n]*\n` 而不是 `[^\n]*\\n`),结果两张表都解析成空。表现是 `id DEFAULT is not in
MOON_TOON_FEATURE_LIST` —— 看起来像注册表写错了,其实是解析器错了。
**改成逐行扫描,不再依赖那个转义。** 教训:多层引用(heredoc → Python 字面量 → 正则)里的反斜杠,
能不用就不用。

## P2 ✅ 完成 2026-08-19 —— 主干抽横切

引擎提交 `e59339df`,插件提交见下。

**做了什么**

1. **shadow tint 六处抄写 → 一个 helper。** `ApplyToonShadowTint` + `FToonShadowTint` 进
   `ToonShadingHelpers.ush`;feature 只声明自己要的**形状**(LERP / MODULATE / NONE),主光判定
   收进结构体。丝袜那个 `if (bIsNPRStockings && bToonMainLightPath)` 变成
   `bIsNPRStockings ? MODULATE : NONE` —— 正好是 P3 要的 policy 形状。
2. **两种光色改名。** `LightColorAndAttenuation` → `LightBanded`(29 处),
   `RawLightColorAndAttenuation` → `LightUnbanded`(6 处),并在声明处写清规则:
   **背光项永远取 Unbanded**(它存在于 band ≈ 0 的地方,喂 banded 等于关掉效果)。
3. **bloom 权重去掉魔数。** `ToonBlur.usf` 的 `FeatureId == 0 || FeatureId == 1` →
   `MoonToonFeatureHasBloomWeight`,从注册表新增的 `MOON_TOON_FEATURES_WITH_BLOOM_WEIGHT` 展开;
   生成器加 `check_bloom_weight_list` 把这张列表钉死在槽位表上。
4. 计划里的第 4 项(`bUseFacialShadowOverride` / 主光选择 / `ToonShadowTintScale` 各收成一次)
   —— **已经是现状**,写计划时看漏了,无事可做。

**验收(逐条实测)**

- **改名是纯改名。** 用 stb_preprocess 跑两套 permutation(deferred 副光;主光 + 前向 + 各向异性),
  把两种拼写折叠成同一个规范记号后比对:**IDENTICAL,8771 / 9343 tokens**。
  用"折叠"而不是"替换"才算数 —— 万一把 Unbanded 写成 Banded,折叠法照样报错。
- **tint 收拢共 31 处记号改动**,两套 permutation 各 31,逐条点清:helper 1 + 构造 1 + 丝袜 4 +
  五个同形点各 5。数学是恒等变换,要验的是接线,而这份清单就是接线证明。
- **0 个 `error X####`**:整机启动全量编译(FDeferredLightPS 编了 608 次)+ 事后
  `recompileshaders changed`。
- Showcase 目检正常:丝袜仍有 skin/fabric 混合与密度过渡,脸和布料仍有明暗带。

### 验收口径:P2 为什么不能用"记号流相同"

P1 能用,是因为那是纯粹的展开搬家。P2 的 tint 收拢按定义会改记号(6 处内联表达式 → 1 个函数调用)。
所以拆成两段分别证:改名段用规范化记号流(可机器判定),tint 段用**枚举全部记号改动并逐条核对**
(也可机器判定,只是要人读一遍清单)。计划 §3 早写了这种情况,这次是第一次真用上。

### 校验抓到的两件事

1. **改名脚本把一行声明弄丢了。** 我用注释块替换 `float3 RawLightColorAndAttenuation = ...;` 那行时,
   新文本忘了把声明本身写回去 —— `LightUnbanded` 未声明就被赋值,shader 根本编不过。
   **规范化记号流比对当场报了出来**(before 比 after 多 5 个记号 `float3 @UNBANDED = @BANDED ;`)。
   这就是"验收要能机器判定"的意义:目检这种 diff 极容易放过去。
2. **bloom 校验一上来就报了 EYEBROW。** 它路由到 Default 模块,于是继承了 Default 的 BloomWeight 槽,
   但旧代码 `Id==0||Id==1` 从不把它算进去。查清后的结论:**没有 `Is Eyebrow` 开关,没有材质能选中它**,
   所以它不可能有被授权的 bloom 权重 —— 校验改成跳过 `FEATURES_WITHOUT_FUNCTION` 里的 id,
   行为与旧代码完全一致。哪天它变成可授权的,这条校验会立刻要求把它列进去。

**故障注入**(校验通不过才有意义,两条都验了):
拿掉 `X(PBR_SPECULAR)` → `PBR_SPECULAR has a BloomWeight slot but is missing from ...`;
塞进没有该槽的 `X(SKIN)` → `lists SKIN, but its slot table has no BloomWeight slot`。

### 又踩了一次反斜杠

`MOON_TOON_FEATURES_WITH_BLOOM_WEIGHT` 第一次写进注册表时,续行反斜杠在
**bash heredoc → Python 字面量 → 文件**这条路上被吃掉,整个宏挤成了一行(合法,但和别的表不一致)。
和 P1a 那次是同一类问题。**结论:凡是要落地带反斜杠的文本,一律先 `cat > 片段文件 <<'EOF'` 写成纯文本,
再用 Python 读文件拼接 —— 不要让反斜杠经过 Python 字符串字面量。**

## P3 ✅ 完成 2026-08-19 —— hook 契约 + 派发器 + Skin 模块

引擎 `456eedee`,插件 `52bb12c`。

**做了什么**

1. **`FToonShadeContext`**(新 `ToonShadeContext.ush`)—— 模块能看到的全部,以及仅此而已。
   **分三阶段填充**,因为数据流卡死:band input 喂 ramp 采样 → ramp 采样定下阴影 → BxDF context
   在漫反射之后才建。每阶段的字段注明了哪个 hook 可读;阶段 2/3 的字段在其阶段前是**零而不是未定义**。
2. **三个 hook**:`BandInput` / `Diffuse` / `Specular`。Diffuse 返回 (Albedo, DiffuseScale, Additive),
   主干合成 `Lambert(Albedo) * LightBanded * Scale + Additive` —— Additive 在乘法**之后**,
   因为散射和透射不是 lambert。
3. **`FToonFeaturePolicy` 只装"主干替模块决定的事"**,今天只有一个字段(装哪个已解析的阴影)。
4. **`ToonFeatureDispatch.ush`** 四个 switch 全从 `MOON_TOON_MIGRATED_FEATURE_LIST` 展开。
5. **Skin 是第一个模块**,并把 DFF 一起带走(注册表早就说了两个 id 都走 Skin)。

**与计划的偏差(两处,都是有意的)**

- **计划写 4 个 hook,只做了 3 个。** Tangent 没做:`GetToonWorldTangent` 内部已经自己分派 Kajiya,
  此刻加一层"只有一种实现"的 hook 是脚手架。它跟 KajiyaHair 模块一起来(P4 #8),那才是第一个需要它的。
- **policy 只有 1 个字段,不是计划的 6 个。** `bUsesDiffuseRamp` / `bUsesCelBand` 必须跟 Stockings 一起来:
  在 P3 加上它们,主干就会去查一个对**所有未迁移 id(含 PBR 丝袜)仍是默认值**的 policy,直接shade 错。
  `bMultiBandCel` 是 Default 从槽位读的,`bHasBloomWeight` 已经是 ToonBlur 用的独立注册表谓词。

**验收(四条,逐条实测)**

- **接线证明(机器判定)。** 模块体按 `Ctx.` 映射还原(`Ctx.L` → `AreaLight.DiffuseL` 等)、
  legacy 的纯别名局部内联后,与它们的来源分支做记号流比对:
  `Skin_Specular` 87 记号、`Default_Specular` 55、`Skin_BandInput` 12,**全部 MATCH**。
- **主干 diff 恰好是那 10 步**,未迁移 id 的 legacy 链**一字未动**(`git diff` 逐条核对)。
- **1,324 个 shader 编译,0 个 `error X####`。**
- **跨改动像素 A/B。** 见下,这次学到东西了。

### A/B 方法:第一次做跨改动的画面对照,踩了两个坑

shader 改动没法像 cvar 那样同会话 A/B,只能"拍改后 → `git stash -u` → 重启拍改前 → `stash pop`"。

**坑 1:场景自己在动。** 第一轮 mean 差 9.3,而同会话噪声底只有 0.31 —— 差异图显示是
**Ultra Dynamic Sky 在两次启动之间推进了**(云在动、太阳在转),连带把角色受光也改了。
**解法:两次都跑同一个脚本,删掉 UDS 换成固定方向光 + 天光,并关掉体积云/大气。**
之后天空区域差异 **0.0000**,证明场景真的可复现了。

**坑 2:`git stash` 把文件换行改成了 CRLF。** pop 回来后 6 个文件全部与备份 `cmp` 不同 ——
内容逐字节相同,只是 LF→CRLF(仓库里其它 Toon 文件都是 LF)。**教训:stash/pop 跨 autocrlf
往返之后要核对换行**,我是先把 6 个文件备份到 scratchpad 才敢 stash 的,值得保留这个习惯。

**最终判据不是"差异够小",而是"差异落在我改不到的地方":**

| 区域 | 同会话噪声 | 改前 vs 改后 |
| --- | --- | --- |
| 天空(固定光后无变化) | 0.0000 | **0.0000** |
| **角色(toon,我改的)** | 0.47 | **1.51** |
| **纯地面(非 toon,ToonBxDF 根本不跑)** | 0.52 | **3.24** |

地面差异是角色的**两倍多**,而 `ToonBxDF` 对地面一行都不执行。32× 放大的差异图上,
角色是暗的、地面是 Lumen 屏幕探针的摩尔纹。**我改得到的表面,比我改不到的表面动得更少。**
残差是 Lumen/GI 时序收敛,不是着色改动。

> 这条口径值得写进 v3 §3:跨改动 A/B 时,**场景里放一个改动证明影响不到的对照面**,
> 比追求"差异接近零"更有用 —— 后者在有 GI 的场景里做不到,前者是可证伪的。

### 生成器:迁移清单必须与 id 映射一致

`MOON_TOON_MIGRATED_FEATURE_LIST` 是**会自我消解的脚手架**,但它重述了 FEATURE_LIST 的一部分,
正是这个生成器存在的理由所要防的漂移。`check_migrated_list` 两个方向都查,并在两张表相等时
**打印一条"可以删了"** —— 不会自报终点的脚手架容易永远留着。
故障注入两条都验了:把 SKIN 在清单里路由到 Default → 报模块冲突;塞一个不存在的 id → 报缺失。

**第一次注入测试自己有 bug**:`str.replace` 把两张表里的同名行**一起**改了,于是它们仍然一致、
校验当然不报错。改成只在 `MOON_TOON_MIGRATED_FEATURE_LIST` 之后的文本里替换才测出来。
**教训:故障注入要先确认注入到了目标位置。**

## P1b ⏳ 待做 —— 删除 `EMoonToonShadingFeature`

单独一个提交,因为它是 v3 里**唯一要重编 C++ 的改动**(其余全是 shader + Python)。

静态证据已齐:ripgrep 全 `Engine/Source/Runtime` 扫描,**没有任何文件 include 那个头,也没有任何
代码用那个符号** —— 它纯粹靠 UHT 扫描生成反射,给材质编辑器的 Enumeration 下拉用。
删掉之后那个遗留资产的 `Enumeration` 指针变空,面板从下拉退回数字输入,**不影响着色**。

**没做的原因:验不了。** 要重编 `UnrealEditor-Engine.dll` 才算数,而验收口径就是"编译通过"。
留给下一次能跑构建的时候。
