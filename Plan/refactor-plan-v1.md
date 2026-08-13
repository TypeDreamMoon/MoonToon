# MoonToon 重构计划 v1

2026-08-13 起草。范围:`Plugins/MoonToon` 的基础材质函数解耦、MI 参数面板瘦身、ShadingFeature
子系统重构,以及与 `F:\UnrealEngine\UE_Moon` 引擎侧 Toon 输入的对齐。

**本文档只描述计划,尚未执行任何改动。** §5 和 §6.0 各有一个待拍板项。

---

## 1. 已定决策

| 决策 | 选择 | 影响 |
| --- | --- | --- |
| ShadingFeatureID | **改成静态选择** | 面板可按当前特征裁剪;MI 换特征触发着色器重编译,不可运行时动画 |
| 材质参数重命名 | **允许全面重命名** | 需要一次性 MI 迁移(§7) |
| 引擎侧范围 | **待定** | 见 §6.0,附推荐 |

---

## 2. 现状测量

数据从 `.dsf` 源码和引擎 shader 源码直接统计,不是估算。

| 项 | 值 |
| --- | --- |
| MI 面板唯一参数总数 | **202** |
| ├ `MF_MoonToonBaseInput.dsf` | 122(声明 75 + 图内内联 83,去重后) |
| └ `MF_MoonToonBuffer.dsf` | 80 |
| 其中 ShadingFeature 参数 | 80,任一时刻实际生效 **11**(7 scalar + 1 vector4) |
| `MF_MoonToonBaseInput.dsf` 行数 | 2308(Properties 387 / Graph 1540 / Layout 232) |
| `MoonToonBufferWrite` 参数个数 | 97 |
| 同一张 (feature × slot) 表的手抄份数 | **5** |
| 面板参数分类 | 132 Scalar / 32 StaticSwitch / 17 Texture / 12 ChannelMask / 9 Vector |

那 5 份拷贝:

1. `ToonBufferCommon.ush:48-107` —— 头部注释里的通道表
2. `ToonBufferCommon.ush:126-234` —— `FToonBuffer` 字段注释,同一张表抄第二遍
3. `ToonBufferCommon.ush:345-357` + `385-397` —— ScalarA 解码白名单,同一份判据抄两遍
4. `MF_MoonToonBuffer.dsf` —— 97 参数签名 + if/else 链(写侧)
5. `ToonShadingModel.ush` / `ToonShadingFeature.ush` —— 约 50 处裸 `FeatureInputScalarX` 读取

### 基础函数里的重复习语

| 习语 | 出现次数 | 占用行数(约) |
| --- | --- | --- |
| `Enable Per Texture Sampler` 双采样对 | 14 | 300 |
| UV 链(`UVChannelSwitch × Scale + Offset`) | 5 | 60,25 个参数 |
| `... From Global Mask Map` 源选择 | 8 | 130 |
| `... From SA Map` / `From Vertex Color` | 3 + 3 | 100 |
| `MoonEncodeToonAttributes` 节点(同参数,仅 OutputIndex 不同) | 5 | 100 |

`MoonEncodeToonAttributes` 和 `MF_MoonToonBuffer` 的多次调用**不会**产生重复节点 ——
DreamShader 的节点复用对 `UE.Expression` 和 `VirtualFunction` 采用两级键控,同参数不同 OutputIndex
共享同一个节点(`Docs/graph/node-reuse.md`)。所以这两项是源码可读性问题,不是运行时开销。
`StaticSwitchParameter` 则**不**参与缓存,14 对双采样是真的 28 个节点 —— 但静态开关会把未选中侧
编译掉,所以同样只是源码体积问题。

---

## 3. 已确认的缺陷清单

每条都有源码位置,不是推测。

### 3.1 DFF 人脸的高光恒为 0(可见缺陷)

`ToonShadingModel.ush:41` 把 `DISTANCE_FIELD_FACIAL_SHADOW`(id 3)并入 `bIsSkinFeature`,于是它走
Skin 的全套槽位;但 `MoonToonBufferWrite` 没有 id 3 的分支,所有槽位停在 `InitToonBuffer` 的 0。
结果 `OverallSkinSpecularIntensity = ScalarG = 0` → `Lighting.Specular = 0`(`ToonShadingModel.ush:509-517`)。

**每一张 SDF 脸都拿不到任何 toon 高光,且没有任何参数能打开它。**
漫反射侧不受影响(`GetSkinWrappedLighting` 在 ScatterStrength=0 时是恒等变换)。

### 3.2 MATCAP 双重不可达

- 写侧:`MoonToonBufferWrite` 完全没有 MATCAP 分支
- 读侧:`DecodeToonDataFromBuffer` 的 ScalarA 白名单(`ToonBufferCommon.ush:345-357`)不含 MATCAP,
  所以即使补上写侧,`MatcapIntensity` 也会被解码成 0 → 回落 1.0

两侧都要改 Matcap 才能用。

### 3.3 引擎读得到、材质写不出的槽位

| Feature | 槽位 | 引擎读作 | 位置 |
| --- | --- | --- | --- |
| DEFAULT (0) | ScalarB | 多阶 cel 开关(>0.5 跳过硬明暗交界) | `ToonShadingModel.ush:113-114` |
| DEFAULT (0) | ScalarC | 每材质明暗交界宽度 | `ToonShadingModel.ush:115` |
| DEFAULT (0) / PBR_SPECULAR (1) | Vector.w | Bloom 权重 | `ToonBlur.usf:36` |
| HAIR_HIGHLIGHT_MASK (4) | ScalarD | 天使环视角锚定混合 | `ToonShadingModel.ush:573` |
| MATCAP (13) | ScalarA + Vector.xyz | 强度 + matcap RGB | `ToonShadingModel.ush:309-310` |
| DFF (3) | A–G + Vector | 整套 Skin 布局 | 见 3.1 |

### 3.4 通道表里有假行

`ToonBufferCommon.ush:72,84` 写着 `DFFShadow → ScalarB/C = FacialShadowSdfLeft/Right`。
实际 SDF 走 `Metallic` / `Anisotropy`(`EncodeToonGBufferDataToMRT:518-521`、
`EncodeToonGBufferDataToAnisotropy:526-529`),TBuffer 的 B/C 对 DFF 从来没被读过。
表里同时**缺** DEFAULT、PBR_SPECULAR、MATCAP 三行。

### 3.5 `SecondaryShadingFeatureID` 是死参数

`MF_MoonToonBaseInput.dsf` 的 4 个调用点(652、2038、2040、2043 行)全部把 `DMask` 硬写成 `1.0`,
`MoonToonBufferWrite:107` 的 `DMask >= 0.5` 恒真,次级特征永远选不中。面板上白占一行。

### 3.6 PBR Stockings 的布局靠值猜

`HasPBRStockingsBaseColor`(`ToonShadingFeature.ush:159-164`)用 `z>0.001 || w>0.001` 区分新旧布局。
BodyColor 设成纯红 (1,0,0) 且 GrazingSpecBoost=0 时判成 legacy:底色被丢弃,同时 `Vector.x`=1 被当成
掠射高光增益(`ToonShadingModel.ush:415-417`)。

### 3.7 面板卫生问题

| 问题 | 位置 |
| --- | --- |
| 9 个 SDF 光照仰角参数无 Group 无 ParameterName,散在默认组 | `AddOffset` `DynMin` `DynMax` `ElevMin` `ElevMax` `Curve` `ClampMin` `ClampMax` `VSign` |
| `BodyBaseColor` 纹理参数无 Group 无 ParameterName | Properties 192 行 |
| `Hair_Highlight_Mask_Channel` 挂在 "02 - Distance Field Facial Shadow" 组 | Properties 195 行 |
| `"04- Kajiya Kay"` 缺空格,与其他 `"NN - X"` 组不同名 | Properties 167 行 |
| 同名参数各有两个节点(反编译残留) | `Rim_Light_Width_Channel(_1)`、`Distance_Field_Facial_Shadow_Map_Channel(_1)` |
| 三组 UV 各 4 个 scalar 而非 1 个 Vector4 | Global / Global Mask Map / Normal Map |

### 3.8 待确认(非缺陷,存疑)

`MF_MoonToonBaseInput.dsf:652` 把 `KajiyaSecondaryMaskChannel_Masked` 同时传给
`InToonKajiyaKay_PrimaryStrandShift` 和 `InToonKajiyaKay_SecondaryStrandShift` 两个 pin。
主次股偏移取同一个值 —— 可能是有意的,也可能是反编译产物。执行前需要确认。

---

## 4. 目标架构

### 4.1 槽位表:单一真值源

新建 `Engine/Shaders/Shared/MoonToonFeatureSlots.h`,沿用
`MoonToonShadingFeatureDefinitions.h` 已验证的 HLSL/C++ 共享头模式,用 X-macro 描述整张表:

```c
#define MOON_TOON_FEATURE_SLOTS_SKIN(X)                       \
    X(A,      ScatterStrength)                                \
    X(B,      TransmissionStrength)                           \
    X(C,      TransmissionPower)                              \
    X(D,      PrimaryRoughnessScale)                          \
    X(E,      SecondaryRoughnessScale)                        \
    X(F,      SecondaryRampOffset)                            \
    X(G,      OverallSpecIntensity)                           \
    X(Vec_xyz, LobeIntensities)                               \
    X(Vec_w,  WarmTintStrength)
```

由它派生:

- HLSL:`FToonSkinParams` 结构体 + `GetToonSkinParams(FToonBuffer)`,每个 feature 一份。
  引擎读侧约 50 处裸 `TBuffer.FeatureInputScalarD` 改成 `Skin.PrimaryRoughnessScale`。
- ScalarA 解码白名单:由表里"该 feature 是否占用 A 槽"自动展开,消掉现在那两份手抄。
- 两份通道表注释删除,改为指向这个头。

**风险(必须先做 spike):** UE 的 shader 预处理器对 `##` token 粘接和嵌套宏展开的支持需要实测。
若不通过,退化方案是**手写**每个 feature 的结构体和 getter(13 × 约 12 行),用一次性脚本生成后
提交为普通源码。这个退化方案同样消掉 5 份拷贝中的 4 份,只是失去自动同步 —— 价值的大头仍在。

### 4.2 写侧:每 feature 一个函数

```
DShader/MaterialFunctions/
  Features/
    MF_ToonFeature_Default.dsf              # 多阶 cel B/C + Bloom 权重
    MF_ToonFeature_PBRSpecular.dsf          # Bloom 权重
    MF_ToonFeature_KajiyaHair.dsf
    MF_ToonFeature_ToonKajiyaHair.dsf
    MF_ToonFeature_DFFacialShadow.dsf       # 新增,Skin 布局(修 3.1)
    MF_ToonFeature_HairHighlightMask.dsf    # 补 ScalarD
    MF_ToonFeature_Skin.dsf
    MF_ToonFeature_Stockings.dsf
    MF_ToonFeature_Eye.dsf
    MF_ToonFeature_ClothVelvet.dsf
    MF_ToonFeature_ToonMetal.dsf
    MF_ToonFeature_EmissiveInk.dsf
    MF_ToonFeature_Matcap.dsf               # 新增
  MF_MoonToonFeatureSelect.dsf              # 静态派发,取代 MF_MoonToonBuffer
```

每个 feature 函数只声明自己那约 11 个参数,输出 `float4 TBufferA/B/C`;打包逻辑由一个共享的
`Function MoonToonPackSlots(id, A..G, Vec, out A, out B, out C)` 承担(内部就是
`InitToonBuffer` + `EncodeToonBuffer`),**打包共享,命名各自**。

加一个新 feature 从"改 97 参数签名 + 加 8 个参数声明 + 改 97 参数调用点 + 改 4 张表"
变成"加一个 .dsf + 派发器加一行"。

### 4.3 读侧:具名读取

`ToonShadingModel.ush` / `ToonShadingFeature.ush` 里的裸槽位读取全部换成 §4.1 的结构体。
现有的 `FToonClothParams`(`ToonShadingFeature.ush:458-484`)已经是这个写法,只是从未推广 ——
这一步是把它铺开到 13 个 feature。

**验收:** 生成的 HLSL 与改动前逐字节相同(纯改名,不改算式)。用 `dsc compile` 之外的
fxc/dxc 对拍,因为 `dsc compile` 走 `-nullrhi`,绿灯不代表 HLSL 正确。

### 4.4 基础函数解耦

| 新函数 | 取代 | 约束 |
| --- | --- | --- |
| `MF_ToonUV(Channel, ScaleOffset)` | 5 处 UV 链 | 参数改成 `Vector4 <Name>_UV_ScaleOffset` + `Scalar <Name>_UV_Channel`,25 → 10 个参数 |
| `MF_ToonSampleTexture_Color` / `_LinearColor` / `_Normal` | 14 对双采样 | 纹理参数必须留在调用点(`TextureObjectParameter`),否则 14 张贴图会塌成同一个参数名。`SamplerType` 是节点属性不是 pin,所以按 SamplerType 分 3 个变体 |
| `MF_ToonMaskChannel(Source, Channel, Intensity)` | 遮罩取值 + 强度 | 源选择开关必须留在调用点(同上,开关参数名要唯一) |

`TextureObjectParameter` 生成 `UMaterialExpressionTextureObjectParameter`,在 MI 里仍是纹理参数,
按名字绑定 —— 但**这一点要在 P2 实测确认**,而不是假设。

预计 `MF_MoonToonBaseInput.dsf` 从 2308 行降到 1100 行上下。

### 4.5 面板分组规范

统一 `NN - 名称`(两位数字 + 空格 + 连字符 + 空格),修掉 3.7 的全部问题,
每个参数强制有 `Group` + `ParameterName` + `SortPriority`。

---

## 5. 待拍板:特征选择器的实现方式

三个方案都能做到"面板只显示当前特征的参数"。机制已在引擎源码确认:
`MaterialEditorInstanceDetailCustomization.cpp:407` 只显示 `VisibleExpressions` 里的参数,而
`MaterialEditorUtilities.cpp:520-545` 的遍历对静态开关**只递归选中那一侧**。

### 方案 A —— 13 个静态开关(推荐,零引擎改动)

一个 `"00 - Shading Feature"` 组里 13 个 `StaticSwitchParameter`,优先级链,"最上面为 true 的胜出"。

- 优点:纯插件改动,立刻能做,机制是引擎原生的
- 缺点:面板上是 13 个复选框而不是一个下拉;作者可能勾多个(靠优先级链兜底 + 文档)
- 面板:80 → 13 + 约 11 = 24 行

### 方案 B —— 每 feature 一个 Material Layer

`GetVisibleMaterialParametersFromExpression` 原生支持 `MaterialAttributeLayers`,层切换是静态的
(`FStaticMaterialLayersParameter`),MI 里是资产下拉框。

- 优点:真下拉,栈机制,零引擎改动
- 缺点:层的输出是单个 `MaterialAttributes`,而我们要带出 TBufferA/B/C 三个 float4 ——
  需要再占 3 个自定义材质属性槽(Moon 已用掉 `MP_MoonEncodedAttribute0..4`)。属性槽是全局资源,
  为一个功能吃掉 3 个需要单独权衡
- 面板:80 → 1 + 约 11 = 12 行

### 方案 C —— 新增引擎表达式 `StaticEnumSwitchParameter`

- 优点:UX 最好,一个下拉,可复用于项目其他地方
- 缺点:静态参数种类是 `FStaticParameterSet` 的封闭集合(目前只有 StaticSwitch、
  StaticComponentMask、MaterialLayers)。新增一种要动序列化、DDC key、MI 编辑器 UI、
  `GetVisibleMaterialParameters`。**这是本计划里最大的单项风险,不建议放进 P0**
- 面板:80 → 1 + 约 11 = 12 行

**推荐:P0 走方案 A**,把结构解耦和缺陷修复先落地;方案 C 作为独立后续项,落地后 13 个复选框
换成 1 个下拉是局部替换,不影响 §4.2 的分文件结构。

---

## 6. 分阶段实施

### 6.0 待拍板:引擎侧范围

| 选项 | 内容 | 后果 |
| --- | --- | --- |
| **全改(推荐)** | §4.1 + §4.3 + 修 3.2 的解码白名单 + 3.4 的假注释 | Matcap 才能真正可用;5 份拷贝降到 1 |
| 只改插件 | 引擎一行不动 | 3.2 的 Matcap 修不了(解码白名单在引擎);3.1/3.3 其余项可在写侧补齐 |
| 引擎只做命名 | 加结构体和 getter,逻辑逐字不变 | 安全,但白名单和假注释仍在 |

下面的阶段划分假设"全改"。选"只改插件"则 P1、P5 去掉,P3 的 Matcap 项降级为"暂缓"。

---

### P0 —— Spike:X-macro 能否穿过 UE shader 预处理器

- 写一个最小的 `MoonToonFeatureSlots.h`,只覆盖 Cloth 一个 feature,展开出 `FToonClothParams`
- 用现有的 `FToonClothParams` 手写版做对拍,要求生成的 HLSL 逐字节相同
- **不通过就走 §4.1 的退化方案**,后续阶段不变

产出:`MoonToonFeatureSlots.h` 骨架 + 结论。**这一步的结论会改变 P1 的做法,先做。**

### P1 —— 引擎:槽位表落地 + 读侧具名化

- 补全 13 个 feature 的结构体和 getter
- 约 50 处裸槽位读取改名
- 删两份通道表注释,ScalarA 白名单由表展开(顺带修 3.2 的 MATCAP 缺失、3.4 的 DFF 假行)
- **验收:除 MATCAP 白名单那一处外,生成的 HLSL 与改动前逐字节相同**

### P2 —— 插件:基础函数解耦(不改参数名)

- 抽出 `MF_ToonUV` / 3 个 `MF_ToonSampleTexture_*` / `MF_ToonMaskChannel`
- 删掉 `Rim_Light_Width_Channel_1` 等反编译残留
- 参数名**全部保持不变**,MI 不受影响
- 实测确认 `TextureObjectParameter` 在 MI 里按名绑定
- **验收:改动前后各 build 一次,反编译对拍(`.uasset` 字节比对在这里无效 —— 同一份源码
  两次构建就会产生不同字节)**

### P3 —— 插件:ShadingFeature 拆分 + 静态派发

- `MF_MoonToonBuffer.dsf` → 13 个 `MF_ToonFeature_*.dsf` + `MF_MoonToonFeatureSelect.dsf`
- 补齐 3.3 的全部缺口:新增 DFF 分支、Matcap 分支、HairHighlightMask 的 ScalarD、
  DEFAULT/PBRSpecular 的 B/C/Vector.w
- 删掉 3.5 的死参数 `SecondaryShadingFeatureID`(连同 `DMask` 恒真那条路径)
- 现有的 `Enable Feature Kajiya Hair Specular` / `Enable Feature Distance Field Facial Shadow`
  两个静态开关合并进特征选择 —— 特征一旦静态,"选特征"和"开该特征的数据"就是同一个开关了,
  现在源码里那段"两者必须分开"的注释(`MF_MoonToonBaseInput.dsf:144-149`)随之作废

### P4 —— 插件:参数重命名 + 面板整理

- §4.5 的分组规范
- UV 三组合并成 Vector4
- 修 3.7 的全部卫生问题
- **必须与 P5 的迁移脚本同一次提交**,否则 MI 会静默丢 override

### P5 —— MI 迁移

见 §7。

### P6 —— 3.6 的 Stockings 布局歧义

把值猜换成显式布局位(占 `Vector.w` 的一个 bit,或在 ScalarA 上留个标志)。
**这一项会改变已有 Stockings 材质的表现**,单独一个提交,单独目检。

---

## 7. MI 迁移方案

受影响的 MI:`MI_Moon_Toon`、`MI_Moon_Toon_VRM`、`MI_Moon_Toon_VRM_Base`、
`MI_Moon_Toon_VRM_Translucent`、`MI_Moon_Toon_VRM_Translucent_TwoSide`、`MI_Moon_Toon_VRM_TwoSide`、
`MI_Eye_Base`、`MI_EyeHighlight_Base`、`MI_FaceEyeline_Base`、`MI_Facebrow_Base`、
`MI_MoonToonOutline`,以及项目 Content 里任何派生实例(执行前要用资产注册表全量扫一遍,
不能只看插件目录)。

做法:

1. 重命名**之前**,用 unreal-bridge 跑一个 Python,把每个 MI 的全部 override
   (Scalar / Vector / Texture / StaticSwitch)导出成 JSON 快照
2. 提交重命名
3. 跑迁移脚本:按旧名 → 新名映射表回填;UV 四标量 → Vector4 的合并单独处理
4. 再导一次快照,与步骤 1 做语义对比 —— 要求每一个旧 override 都在新名下有对应值,
   没有静默丢失
5. **快照对比是唯一可信的验收**,不要靠肉眼看面板

映射表随 P4 一起写,和重命名放同一个提交里。

---

## 8. 验证策略

| 层 | 手段 | 能证明什么 |
| --- | --- | --- |
| L1 源码 | `.dsf` 反编译对拍 | 图结构等价 |
| L2 编译 | `dsc compile` 全绿 | **只证明能过 `-nullrhi`,不证明 HLSL 正确** |
| L2b HLSL | 对生成的 `.ush` 跑 fxc/dxc | 真的能编译 |
| L3 参数 | MI override 快照前后对比 | 没有静默丢值 |
| L4 画面 | 逐特征目检截图 | 最终判据 |

**L1+L2+L3 同时全绿不等于保真** —— L1 比的是同一个导出器的两份产物,L2 的 nullrhi 绿灯什么都不说明。
每个阶段都要有 L4。

各特征的目检最小集:Default(含多阶 cel)、Skin、DFF 脸(重点看 3.1 修复后的高光)、
Kajiya 头发、Toon Kajiya 头发、HairHighlightMask(重点看 ScalarD 视角锚定)、Eye、Stockings、
ClothVelvet、Metal、EmissiveInk、Matcap(全新)。

---

## 9. 风险与陷阱

| 风险 | 缓解 |
| --- | --- |
| X-macro 过不了 UE shader 预处理器 | P0 先 spike,退化方案已备好 |
| `dsc compile` 绿灯 ≠ HLSL 正确(`-nullrhi`) | 每次都跟一遍 fxc/dxc |
| `.uasset` 字节比对在这里无效(同源码两次构建字节不同) | 一律反编译对拍 |
| 重命名静默丢 MI override | 快照前后对比,不靠目检 |
| 特征改静态后 MI 换特征触发重编译 | 已接受;文档里写清楚 |
| P6 会改变已有 Stockings 材质表现 | 单独提交单独目检,可单独回滚 |
| `Enable Per Texture Sampler` 共享参数名塌陷 | 纹理参数留在调用点(§4.4) |
| 3.8 的主次股偏移同值问题 | 执行 P3 前先确认是有意还是反编译产物 |

---

## 10. 附录:完整槽位表(读侧现状)

从引擎源码逐条核对得出,这是 §4.1 单一真值源的初始内容。
"写侧"列标 ✗ 的就是 §3.3 的缺口。

| ID | Feature | A | B | C | D | E | F | G | Vector | 写侧 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | DEFAULT | — | 多阶 cel 开关 | 交界宽度 | — | — | — | — | w = Bloom 权重 | ✗ |
| 1 | PBR_SPECULAR | — | — | — | — | — | — | — | w = Bloom 权重 | ✗ |
| 2 | KAJIYA_HAIR | 切线旋转(×PI) | 次高光偏移(×PI) | 次高光遮罩 | 主偏移 | 主指数缩放 | 次指数缩放 | 总强度 | x 主强 / y 次强 / z 次偏移缩放 / w 阴影强度 | ✓ |
| 3 | DFF | (整套 Skin 布局) | | | | | | | | ✗ |
| 4 | HAIR_HIGHLIGHT_MASK | 遮罩 | 强度 | 阴影强度 | **视角锚定混合** | — | — | — | — | 部分(缺 D) |
| 5 | TOON_KAJIYA_HAIR | 切线旋转 | 次股偏移 | 次高光遮罩 | 主股偏移 | 主指数缩放 | 次指数缩放 | 总强度 | 同 id 2 | ✓ |
| 6 | SKIN | 散射强度 | 透射强度 | 透射幂 | 主粗糙缩放 | 次粗糙缩放 | 次 ramp 偏移 | 总高光强度 | x 主瓣 / y 次瓣 / z 高光阴影 / w 暖色调 | ✓ |
| 7 | PBR_STOCKINGS | 密度 | 菲涅尔幂 | 透射强度 | 粗糙缩放 | 高光强度 | 阴影强度 | 掠射变暗 | xyz 底色 / w 掠射增益(新);legacy x 掠射增益 / y 高光染色 | ✓(布局歧义) |
| 8 | EYE | 漫反射 wrap | 角膜缘变暗 | 高光阴影强度 | 主粗糙缩放 | 次粗糙缩放 | 主强度 | 次强度 | xyz 眼睛染色 / w 次 ramp 偏移 | ✓ |
| 9 | CLOTH_VELVET | 漫反射 wrap | 光泽幂 | 光泽阴影强度 | 粗糙缩放 | 光泽强度 | 逆反射强度 | 掠射变暗 | xyz 光泽染色 / w 边缘偏置 | ✓ |
| 10 | TOON_METAL | 漫反射强度 | 阴影高光下限 | ramp 偏移 | 粗糙缩放 | 高光强度 | 边缘增强 | 高光对比 | xyz 金属染色 / w 未用 | ✓ |
| 11 | TOON_EMISSIVE_INK | 受光影响 | 阴影提亮 | 墨色变暗强度 | 粗糙缩放 | 高光强度 | 边缘增强 | 墨色混合 | xyz 墨色 / w 边缘偏置 | ✓ |
| 12 | EYEBROW | (保留,已废,由 FaceOverlay 取代) | | | | | | | | — |
| 13 | MATCAP | **强度**(解码白名单也缺) | — | — | — | — | — | — | **xyz matcap RGB** | ✗ |

---

## 11. 未决事项

1. §5 特征选择器方案 A / B / C
2. §6.0 引擎侧范围
3. §3.8 主次股偏移是否有意同值
