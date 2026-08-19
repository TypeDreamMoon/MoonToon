# v3 P0 结论:spike PASS,枚举可删

**日期:** 2026-08-19
**计划:** `refactor-plan-v3.md` §2 P0
**产物:** 本文 + `axes.md`

---

## 1. Spike:`case MOON_SHADING_FEATURE_ID_##ID:` 能不能穿过 UE 的 shader 预处理器

**结果:PASS,0 诊断。退化方案(脚本生成派发器当普通源码提交)不启用。**

v2 P0 只验证过 `struct FToon##Name##Params` —— 把宏当参数传进表里让表调用它。
v3 的派发器多了三个没验证过的形态,所以重跑。

### 怎么测的

和 v2 一样,不读预处理器源码下结论,而是**把引擎自己的预处理器编出来跑**。
stb_preprocess(`Engine/Source/Developer/ShaderPreprocessor/Private/stb_preprocess`)是纯 C,可独立编译。
v2 那个 `ppdrv` 驱动没保留,重写了一份(scratchpad `p0spike/ppdrv.c`,实现 loadfile / freefile /
resolveinclude 三个回调)。两个坑和 v2 记的一样,都还在:

```
gcc -O1 -w -msse4.2 "-DSTB_LCG_NEXT()=((unsigned int)rand())" \
    -I<stb_preprocess> ppdrv.c preprocessor.c cond_expr.c stb_alloc.c stb_ds.c -o ppdrv.exe
```

- `-msse4.2` 必须给(`_mm_cmpistri` 是 always_inline)
- `STB_LCG_NEXT` 由引擎的 `StbConfig.h` 提供,独立编译要自己补
- 文件 buffer 要留 **16 字节 padding** —— 预处理器用 SSE 读会越过终止符

### 测了什么(四个形态,一个文件里全上)

1. `MOON_TOON_MODULE_LIST` 驱动 `MOON_TOON_DEFINE_FEATURE_PARAMS` 生成 4 个参数结构体(P1 的形态)
2. **`case MOON_SHADING_FEATURE_ID_##ID: return Module##_Specular(Ctx);`,写在函数体内**,
   配 `#define X` / `#undef X`(P3 派发器的形态)
3. **同一张表在同一 TU 里用不同的 X 展开第二次**(四个 hook 各展开一次)
4. `##ID` 在同一行里用两次(既做 case 标签又做函数实参)

### 结果

全部通过,0 诊断。关键的一条:**粘出来的记号会被重新扫描并继续展开** ——
`case MOON_SHADING_FEATURE_ID_##ID:` 最终落地为 `case 0 :` 而不是留着宏名。

别名也对:

```
case 0 : return Default_Specular(Ctx);
case 2 : return KajiyaHair_Specular(Ctx);
case 3 : return Skin_Specular(Ctx);      <- DFF 别名到 Skin
case 5 : return KajiyaHair_Specular(Ctx); <- 2 与 5 同模块
case 6 : return Skin_Specular(Ctx);
case 7 : return Stockings_Specular(Ctx);
case 14 : return Stockings_Specular(Ctx); <- 7 与 14 同模块
```

> 只验证了**预处理**。生成的 HLSL 能不能过 dxc 是 P3 的事(验证策略 L2b),不在 P0 范围。

---

## 2. 扫描:还有材质在用 `EMoonToonShadingFeature` 吗

**结果:活的资产零引用。唯一命中是重构前的遗留副本。→ 建议删。**

### 怎么扫的

`UMaterialExpressionScalarParameter` 有个 `Enumeration` 属性(`TObjectPtr`,
`EditCondition = ControlType == EMaterialScalarParameterControlType::Enumeration`,
`MaterialExpressionScalarParameter.h:47`)。材质真用了枚举下拉的话,包的 import 表里会有枚举名。
所以对全部 `.uasset` 做二进制 grep 就够,不需要开编辑器。

同样的方法 grep 一个一定存在的引用(`MoonToonFaceOverlay`)确认扫描有效。

### 结果

| 位置 | .uasset 数 | 命中 |
| --- | --- | --- |
| `DevTest/Content` | 4141 | **1** —— `Content/Project/MF_MoonToonBaseInput.uasset` |
| `DevTest/Plugins` | 751 | 0 |
| `Engine/Content` | 5248 | 0 |
| `Engine/Plugins` | 12918 | 0 |

那一个命中是**重构前的遗留副本**:

- `Content/Project/` 下有一组 **2026-08-10** 的资产(`MF_MoonToonBaseInput` / `M_MoonToon` /
  `M_MoonToonOutline`),比 v2 重构(08-14 起)还早
- 反向引用链只有一条:`MF_MoonToonBaseInput` ← `M_MoonToon` ← **`MI_RobinSummer_Inst`**
  (在 `Content/MMD/StarRail/知更鸟晴歌/Material/test/`,目录名就叫 test)
- 活的那份是插件里 DreamShader 生成的 `Plugins/MoonToon/Content/MaterialFunctions/MF_MoonToonBaseInput.uasset`
  (2026-08-18),对枚举**零引用** —— 因为 v2 P6 之后特征选择走的是 `Is X` 静态开关,
  id 由 `MF_MoonToonFeatureSelect` 直接输出,不再经过枚举下拉
- 插件的 `.dsf` 源码同样零引用(之前已确认)

### 影响面

删掉 `EMoonToonShadingFeature` 之后,那个遗留资产的 `Enumeration` 指针变空,
标量参数的面板显示从下拉退回数字输入。**不影响着色** —— 参数值本来就是个 float,
枚举纯粹是编辑器 UI 属性。

### 建议

**删。** 它是"加一个 feature 要重编引擎"的唯一原因。
如果想更保险,先把 `Content/Project/` 那三个 08-10 的遗留资产和那个 test MI 一起清掉
(它们和插件里的活资产完全重复),再删枚举 —— 但这是 P8 死资产清理的活,不 gate P1。

---

## 3. P0 定下来的决定

| 事项 | 决定 | 依据 |
| --- | --- | --- |
| 派发器实现 | X-macro `case ##ID:`,**不用退化方案** | §1 spike |
| C++ 枚举 | **删除**(P1 里执行);`MoonToonShadingFeature.h` 整个文件删,static_assert 一并消失 | §2 扫描 |
| Modifier 载荷 | `ToonFeatureRT3.w` = ModifierID + `ToonFeatureRT4` 四槽,授权走 `ToonMaterialOutput` Pin[4] | `axes.md` §4 |
| Modifier 并存 | 一次一个;要并存再加 RT5 | 同上 |
| 迁 Substrate | **不迁**,但偷 hatching(modifier #3) | `axes.md` §5 |

**P0 完成。下一步 P1(注册表 + 三处展开 + 生成器校验),零行为变化,验收是 ppdrv 逐字节相同。**
