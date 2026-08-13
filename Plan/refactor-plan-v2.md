# MoonToon 渲染管线重构 v2 —— 执行清单

取代 `refactor-plan-v1.md`(v1 只覆盖材质侧,且把选择器和正交化当成耦合的 —— 见 §0 更正)。

诊断、payload 路由图、文件布局、面板前后对照在配套的布局方案页里,本文只放**能照着做的东西**:
阶段、验收、迁移、风险。

范围:**引擎 + 插件**。已定:特征选择改静态、允许全面重命名。未定:静态选择器实现方案(§6)。

---

## 0. 对 v1 的更正

v1 里说"正交化会让选择器从 1 个下拉变成 2–3 个下拉、18 个复选框,所以两者耦合"。**这个结论是错的。**

算清楚之后:只把 SDF 抽成修饰位时,选择器要表达的是「12 选 1 的表面模型 + 1 个 SDF 开关」= 13 个控件,
和 v1 的 13 个特征复选框**完全一样**。爆炸只发生在继续拆高光轴的情况下,而高光轴被槽位预算堵死
(见 §5),本计划不做。

所以:**正交化(P1)和选择器方案(§6)互相独立,可以分开决定。**

---

## 1. 排序原则

**减法先做。** 移除劫持和 legacy ID 之后,槽位表才有可能讲出原则,后面的拆分和改名才不会建在流沙上。

P0 spike → P1/P2 引擎减法 → P3 引擎命名 → P4 引擎槽位表 → P5 插件解耦 → P6 插件特征拆分 →
P7 重命名+迁移 → P8 独立收尾。

每个阶段一个提交,每个阶段可单独回滚。

---

## 2. 阶段

### P0 · Spike:X-macro 能否穿过 UE shader 预处理器

- 写最小 `Engine/Shaders/Shared/MoonToonFeatureSlots.h`,**只覆盖 Cloth 一个 feature**
- 展开出 `FToonClothParams` + `GetToonClothParams`,与 `ToonShadingFeature.ush:458-484` 的手写版对拍
- 验收:生成 HLSL 逐字节相同

**不通过的退化方案:** 手写 13 份结构体(约 12 行/个),用一次性脚本生成后作为普通源码提交。
同样把 5 份拷贝降到 1 份,只是失去自动同步。**后续阶段不受影响。**

---

### P1 · 减法 A:SDF 搬家,解除 stock 字段劫持

引擎侧。

1. `ToonPass_Base.cpp` `CreateToonBuffers` 增加第 4 张 `PF_B8G8R8A8`,toon pass MRT 由 4 → 5(上限 8)
2. `FacialShadowSdfLeft/Right` 从 `Metallic` / `Anisotropy` 迁到新 RT 的 `.xy`,`.zw` 留空备用
3. 删除 `EncodeToonGBufferDataToMRT` 里写 Metallic 的分支、`EncodeToonGBufferDataToAnisotropy` 整个函数
4. 删除 `DecodeToonGBufferDataFromMRT` 的 `inout float Metallic` 清零行为(签名回归 const)
5. 删除 `ToonShadingModel.ush:360` 的 `if (DFF) bHasAnisotropy = false`
6. DFF 从 ID 值升级成**修饰位**:`bIsSkinFeature = SKIN || DFF` 的短接删除,SDF 改为独立开关

**成本:** +4 B/px,仅 toon 场景(`ViewFamilyHasToonContent` 条件分配已就位)。

**验收:**
- DFF 脸目检与改动前一致
- DFF 材质上把 Metallic / Anisotropy 调非零,确认生效(改动前被劫持,不可能生效)
- 「任意表面模型 + SDF」组合可用 —— 这是 P1 真正解锁的东西

---

### P2 · 减法 B:干掉第二个 ShadingFeatureID

引擎侧。

1. `EncodeToonGBufferDataToMRT` 里 `LegacyFeatureId` 相关整段删除
2. `CustomData.w` 位布局:`ReflectionIntensity` 由 3 bit 恢复 4 bit,`RayTracingShadowFlag` 保持 2 bit,余 2 bit 备用
3. `ResolveToonFeatureIdFallback` 整个删除,调用点改为直读 `ToonFeature` 的 8-bit ID
4. `ToonFeatureUsesScalarA` 与 `DecodeToonDataFromBuffer` 里内联的那份白名单 —— 先留着,P4 由槽位表统一展开

**验收:** 全特征目检;反射强度过渡应比改动前更平滑(多了一位)。

---

### P3 · 命名修复 + `FMoonToonContext` 拆分

引擎侧。纯改名 + 纯搬运,**不改任何算式**。

**改名:**

| 旧 | 新 | 为什么 |
| --- | --- | --- |
| `ToonBufferA`(RGBA16F, GBuffer 槽) | `ToonSurfaceRT` | 与下面那个差 4 个字符,已经出过事故 |
| `TBufferA/B/C`(RGBA8, 特征参数) | `ToonFeatureRT0/1/2` | 同上 |
| 新增第 4 张 | `ToonFeatureRT3` | P1 建的那张 |
| `EncodeToonBuffer` | `EncodeToonFeature` | 它写的是 TBuffer 不是 ToonBufferA |
| `DecodeToonBufferA` | `DecodeToonFeature` | 函数名说 ToonBufferA,实际解 TBufferA |
| `EncodedToonBufferA` | `EncodedToonSurface` | 同族 |

**拆 `FMoonToonContext`** → `ToonShadingContext.ush`:

| 新结构体 | 装什么 | 生命周期 |
| --- | --- | --- |
| `FToonSurface` | ToonSurface · ToonFeature | 随像素,解码一次 |
| `FToonLightContext` | ToonLight · LightType · LightColor | 随每盏灯 |
| `FToonViewContext` | PixelPos · BufferUV · ViewportUV · Exposure · WorldType | 随视图,整帧不变 |
| 就地变量 | TintShadow · ShadowMap · EncodedToonSurface | 中间量,不进结构体 |

顺带删掉 `SetMoonToonContext_*` 那几个伸进嵌套结构改字段的宏 —— 拆开后都变成普通函数传参。
`FGBufferData` 不再嵌套 toon context。

**验收:生成 HLSL 逐字节相同。** 这一阶段任何字节差异都是 bug。

---

### P4 · 槽位表落地 + 读侧具名化

引擎侧。

1. `MoonToonFeatureSlots.h` 覆盖全部 13 个 feature(或 P0 退化方案的手写版)
2. 生成 13 个 `FToon<X>Params` + getter 到 `ToonFeatureParams.ush`
3. `ToonShadingModel.ush` / `ToonShadingFeature.ush` 约 50 处裸 `FeatureInputScalarX` → 具名读取
4. ScalarA 解码白名单由表展开 —— **顺带修掉 MATCAP 缺失**(现在 ID 13 不在白名单,强度被解码成 0)
5. 删除 `ToonBufferCommon.ush` 里两份手抄的通道表注释,改为指向 `MoonToonFeatureSlots.h`
6. 修掉 DFF 那两行假注释(`ScalarB/C = FacialShadowSdfLeft/Right` —— P1 之后更是彻底作废)

**验收:除 Matcap 白名单那一处外,生成 HLSL 逐字节相同。**

---

### P5 · 插件基础函数解耦(**不改参数名**)

1. 抽出 `Shared/MF_ToonUV.dsf` —— 取代 5 处 UV 链
2. 抽出 `Shared/MF_ToonSample{Color,Linear,Normal}.dsf` —— 取代 14 对"双采样 + 静态开关"
   - 按 SamplerType 分三个变体:`SamplerType` 是节点属性不是 pin,一个函数只能有一种
   - 纹理参数必须留在**调用点**(`TextureObjectParameter`),否则 14 张贴图会塌成同一个参数名
3. 抽出 `Shared/MF_ToonMaskChannel.dsf`
4. 删反编译残留:`Rim_Light_Width_Channel_1`、`Distance_Field_Facial_Shadow_Map_Channel_1`

**实测确认(不要假设):** `TextureObjectParameter` 生成 `UMaterialExpressionTextureObjectParameter`,
在 MI 里是否仍按名绑定为纹理参数。

**验收:** 反编译对拍。`.uasset` 字节比对在这里**无效**(同一份源码两次构建就会产生不同字节)。
参数名一律不动 ⇒ MI 不受影响。

---

### P6 · 特征拆分 + 静态派发 + 补齐缺口

1. `MF_MoonToonBuffer.dsf` 拆成 `Features/` 下 12 个函数 + `MF_MoonToonFeatureSelect.dsf`
   - 打包共享(一个 `Function MoonToonPackSlots`),命名各自
   - 每个函数只声明自己那约 11 个参数
2. **补齐全部写侧缺口:**

| Feature | 补什么 |
| --- | --- |
| DEFAULT (0) | ScalarB 多阶 cel 开关 / ScalarC 交界宽度 / Vector.w Bloom 权重 |
| PBR_SPECULAR (1) | Vector.w Bloom 权重 |
| HAIR_HIGHLIGHT_MASK (4) | ScalarD 视角锚定混合 |
| MATCAP (13) | 全新分支:ScalarA 强度 + Vector.xyz matcap RGB |
| DFF | P1 已把它变成修饰位,不再需要独立分支 |

3. 删死参数:`SecondaryShadingFeatureID` + `DMask` 恒真那条路径(4 个调用点全部硬写 `1.0`)
4. 合并 `Enable Feature Kajiya Hair Specular` / `Enable Feature Distance Field Facial Shadow`
   两个静态开关进特征选择 —— 特征一旦静态,「选特征」和「开该特征的数据」就是同一个开关。
   `MF_MoonToonBaseInput.dsf:144-149` 那段"两者必须分开"的注释随之作废,一并删除

**执行前必须确认:** `MF_MoonToonBaseInput.dsf:652` 把同一个值同时传给 Toon Kajiya 的主、次股偏移
(`KajiyaSecondaryMaskChannel_Masked` 出现在两个位置)。有意还是反编译产物?

**验收:** 逐特征目检 —— Default(含多阶 cel)、Skin、DFF 脸(**重点看恢复的高光**)、
Kajiya、Toon Kajiya、HairHighlightMask(**重点看 ScalarD 视角锚定**)、Eye、Stockings、
ClothVelvet、Metal、EmissiveInk、**Matcap(全新)**。

---

### P7 · 重命名 + 面板整理 + MI 迁移

**必须与迁移脚本同一次提交。**

1. 分组规范统一为 `NN - 名称`(修掉 `"04- Kajiya Kay"` 缺空格)
2. 三组 UV 的 Scale X/Y + Offset X/Y 合并成 Vector4(12 scalar → 3 vector)
3. 补 Group / ParameterName:9 个 SDF 光照仰角参数(`AddOffset` `DynMin` `DynMax` `ElevMin`
   `ElevMax` `Curve` `ClampMin` `ClampMax` `VSign`)、`BodyBaseColor`
4. `Hair_Highlight_Mask_Channel` 从 "02 - Distance Field Facial Shadow" 移到正确的组

**迁移流程:**

1. 重命名**之前**,用 unreal-bridge 跑 Python,把每个 MI 的全部 override
   (Scalar / Vector / Texture / StaticSwitch)导出成 JSON 快照
2. 提交重命名 + 映射表
3. 跑迁移脚本回填;UV 四标量 → Vector4 的合并单独处理
4. 再导一次快照,与步骤 1 做**语义比对** —— 每一个旧 override 都要在新名下有对应值
5. **快照比对是唯一可信的验收,不要靠肉眼看面板**

**受影响的 MI(执行前用资产注册表全量重扫,不能只看插件目录):**
`MI_Moon_Toon` · `MI_Moon_Toon_VRM` · `MI_Moon_Toon_VRM_Base` · `MI_Moon_Toon_VRM_Translucent` ·
`MI_Moon_Toon_VRM_Translucent_TwoSide` · `MI_Moon_Toon_VRM_TwoSide` · `MI_Eye_Base` ·
`MI_EyeHighlight_Base` · `MI_FaceEyeline_Base` · `MI_Facebrow_Base` · `MI_MoonToonOutline`

---

### P8 · 收尾(各自独立)

**P8a · Stockings 布局歧义。** `HasPBRStockingsBaseColor` 用 `z>0.001 || w>0.001` 猜新旧布局;
BodyColor=(1,0,0) 且 GrazingSpecBoost=0 时判成 legacy,底色被丢弃且 `Vector.x`=1 被当成掠射增益。
换成显式布局位。**会改变已有 Stockings 材质表现,单独提交单独目检。**

**P8b · 死资产清理。** `MoonToonInput.uasset`(零引用孤儿)、`MF_MoonToonBufferInput.uasset`、
`Buffer/Writer.uasset`。

> ⚠ 后两个的删除会断开 MooaToon 的 `MF_MooaToonBaseInput` / `M_Toon`。**先确认 MooaToon
> 那条线是否还在用** —— 如果还在用,先把依赖方向掰正(上游 MooaToon 不该反向依赖 MoonToon),再删。
> 另外按 `MF_MoonToonBuffer.dsf` 头部的记录,`MF_MoonToonBufferInput` 内部的 ShadingFeatureID
> 卡在 0,那条线的 feature 从来没触发过 —— 这一点也要先复核。

---

## 3. 验证策略

| 层 | 手段 | 能证明什么 |
| --- | --- | --- |
| L1 源码 | `.dsf` 反编译对拍 | 图结构等价 |
| L2 编译 | `dsc compile` 全绿 | **只证明能过 `-nullrhi`,不证明 HLSL 正确** |
| L2b HLSL | 对生成的 `.ush` 跑 fxc/dxc | 真的能编译 |
| L3 参数 | MI override 快照前后比对 | 没有静默丢值 |
| L4 画面 | 逐特征目检截图 | **最终判据** |

**L1 + L2 + L3 同时全绿不等于保真** —— L1 比的是同一个导出器的两份产物,L2 的 nullrhi 绿灯什么
都不说明。每个阶段都要有 L4。

引擎侧 P3 / P4 额外要求「生成 HLSL 逐字节相同」,这是这两个阶段唯一的正确性判据。

---

## 4. 风险台账

| 风险 | 缓解 |
| --- | --- |
| X-macro 过不了 UE shader 预处理器 | P0 先 spike,退化方案已备好,后续阶段不受影响 |
| `dsc compile` 绿灯 ≠ HLSL 正确 | 每次跟一遍 fxc/dxc |
| `.uasset` 字节比对无效 | 一律反编译对拍 |
| 重命名静默丢 MI override | 快照前后语义比对,不靠目检 |
| 特征改静态后 MI 换特征触发重编译 | 已接受,文档写清楚 |
| P1 的 +4 B/px | 仅 toon 场景;若实测有压力,`.zw` 备用字节可以让出来换成 3 通道格式 |
| 删除死资产断开 MooaToon | P8b 先复核依赖,必要时先掰正分层 |
| P8a 会改变已有 Stockings 表现 | 单独提交,可单独回滚 |
| Toon Kajiya 主次股偏移同值 | P6 执行前确认是有意还是反编译产物 |

---

## 5. 明确不做

1. **不重设计 wire 格式。** RGBA8 + 条件分配是合理的,字节没错,错的全在代码怎么谈论这些字节。
2. **不把高光轴正交化。** 槽位预算堵死:「Standard 漫反射 + KajiyaKay 高光」需要 3 + 11 = 14 个槽,
   可用只有 11。SDF 能抽出来是因为它只要 2 字节,加一张 RT 就够;高光轴要抽得再加两张,不划算。
3. **不把特征 RT 挂进 GBuffer 布局。** 那样能省掉 toon mesh pass,但所有非 toon 材质都要为它付
   带宽 —— 现状的权衡更合理。

---

## 6. 待决:静态选择器实现方案

P1 之后选择器要表达的是「12 选 1 的表面模型 + 1 个 SDF 开关」。

| 方案 | 面板形态 | 引擎改动 | 风险 |
| --- | --- | --- | --- |
| **A** 静态开关链(推荐) | 12 + 1 个复选框,优先级链"最上面为 true 的胜出" | 无 | 低。可能勾多个,靠优先级链兜底 |
| **B** Material Layer | 真下拉(资产选择器) | 无 | 中。层只带一个 MaterialAttributes,带三个 float4 要再占 3 个自定义属性槽。MooaToon 里已有 `ML_ToonBaseInput` 先例 |
| **C** 新增 `StaticEnumSwitchParameter` | 真下拉,UX 最好 | 大 | 高。静态参数种类是 `FStaticParameterSet` 的封闭集合,新增一种要动序列化 / DDC key / MI 编辑器 UI |

建议 P6 走 A,C 作为独立后续项 —— 分文件结构一旦成型,复选框换下拉是局部替换,不返工。

---

## 7. 没验证的,别当结论

1. `Moon_MeshPass_Base` 的 PS 调了 `CalcMaterialParameters` + `GetMaterialCoverageAndClipping` +
   三个自定义输出,看起来像整张图算两遍,**但 HLSL 编译器的 DCE 大概率砍掉大部分**。
   实际保留多少要看反汇编或 RenderDoc shader 统计。能确定的只有:喂给特征 buffer 的贴图和
   opacity mask 链确实采样了两次。
2. Metallic 被 SDF 污染在 SSR / Lumen 上**有没有可见后果**,没测。风险是结构性的,现象未必存在 ——
   P1 的价值不依赖它。
3. `TextureObjectParameter` 在 MI 里按名绑定,P5 实测确认。
4. Toon Kajiya 主次股偏移同值,P6 前确认。
