# P0 结论:X-macro 槽位表可用,无需退化方案

**日期:** 2026-08-14
**结果:** PASS —— 采用 X-macro 生成,`refactor-plan-v2.md` §2 P0 的退化方案(手写 13 份结构体)不启用。

## 怎么测的

不是读预处理器源码下结论,而是**把引擎自己的预处理器编出来跑**。

UE 5.8 的 shader 预处理器是 stb_preprocess(`Engine/Source/Developer/ShaderPreprocessor/Private/stb_preprocess`),
纯 C,可独立编译。写了一个驱动(`ppdrv.c`:实现 loadfile / freefile / resolveinclude 三个回调,
注意 buffer 要留 16 字节 padding,预处理器用 SSE 读会越过终止符),用 w64devkit 的 gcc 编译:

```
gcc -O1 -w -msse4.2 "-DSTB_LCG_NEXT()=((unsigned int)rand())" \
    -I<stb_preprocess> ppdrv.c preprocessor.c cond_expr.c stb_alloc.c stb_ds.c -o ppdrv.exe
```

两个坑:`-msse4.2` 必须给(`_mm_cmpistri` 是 always_inline);`STB_LCG_NEXT` 由引擎的
`StbConfig.h` 提供,独立编译要自己补一个。

## 测了什么

Cloth 一个 feature 的槽位表,两个变体:

1. **不用 `##`** —— 表里自带生成名这一列,只依赖"把宏当参数传进去让表调用它"
2. **用 `##`** —— `struct FToon##FeatureName##Params`

## 结果

**两个变体都通过,0 诊断 0 错误。**

变体 1 展开出的结构体与 `ToonShadingFeature.ush:458-484` 的手写 `FToonClothParams` 逐项一致 ——
字段名、顺序、类型(含 `float3 SheenTint`)、槽位映射(含 `FeatureInputVector.xyz` / `.w` 这种
带 `.` 的槽位列)全部对上。

变体 2 正确粘接出 `FToonClothVelvetParams`,说明 **token pasting 也可用**,比计划预期的headroom 更大。

## 对后续阶段的影响

- P4 直接按 X-macro 写 `MoonToonFeatureSlots.h`,不走手写路线
- 表的列设计定为 `X(Type, Slot, Name)`;`Slot` 列允许带 `.xyz` / `.w` 后缀
- 生成代码的空白格式很难看(`P. WrapStrength = B. FeatureInputScalarA ;`),HLSL 不敏感,不管

## 已知不影响结论的差异

驱动里 `STB_LCG_NEXT` 用 `rand()` 顶替,引擎用 `StbLcgNext()`。那是哈希表的 seed,
只影响桶分布不影响展开结果。
