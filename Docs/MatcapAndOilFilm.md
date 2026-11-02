# Matcap 与油膜 (Matcap / Oil Film)

两者以前是延迟光照里的"修饰器"（占 GBuffer / Substrate 记录里 4 个字节 + 1 个编号）。
2026-11 起（T29 / T30）它们都在材质里完成，不再占载荷；腾出来的 RT4 / Data9 给了脸部 SDF 的 16 位扩展（见 `FaceSDF.md`）。
两个都是 **M_MoonToon** 上的静态开关，关着时着色器和以前完全一样。

## Matcap（组 `44 - Matcap`）

1. 勾 **`Use Matcap`**。
2. `Matcap Map`：一张正对镜头的球体渲染；`Matcap Mask Map`（R 通道）控制哪里出 matcap；`Matcap Intensity` 强度。
3. **`Matcap Lighting Mix`** 决定 matcap 怎么分：
   - 不受光的份额 `Color × (1 − Mix)` 进**自发光**：不管灯怎么打都一样亮，暗部也保持（常规用法，Mix = 0）。
   - 受光的份额 `Color × Mix` 加到**底色**上，和底色一起被打光。

取样用 MToon 1.0 的视线坐标架（相机滚转时 matcap 不跟着转），UE 是 Z 朝上，已经换算过轴。

和旧修饰器的区别（已对拍，`Saved/MoonToonStyle/captures/mm/T29_matcap_ab_sheet.png`）：

- Mix = 0：和旧版一致。
- Mix > 0：旧版受光份额只跟主光走，而且不乘底色；现在这一份在底色里，所有灯和 GI 都会照到它，也会被暗部颜色、ramp 影响，
  看起来更暗、更像漫反射。需要旧版那种"跟着主光亮"的金属感时，用 Mix = 0 再自己在 Matcap Map 里画光。
- 在 Lumen 下，自发光份额会轻微照亮周围（和任何自发光一样）。

## 油膜（组 `45 - Oil Film`）

做法来自终末地的油膜高光：颜色查一张"油膜色"的高光 ramp，再让它随视角流动。

1. 勾 **`Use Oil Film`**。
2. **高光要先能看见**：油膜改的是这个材质自己的高光颜色，亮度和大小仍由 `Specular` / `Roughness` 决定。
   M_MoonToon 默认 `Specular` = 0、`Roughness` = 1，这两个默认值都会让高光为 0；油膜一般用 Specular 0.5–1、Roughness 0.4–0.6。
3. `Oil Film Ramp`：默认 `T_ToonOilFilmSpecRamp`（终末地 T4831 的取色：外圈红→黄，内圈粉→青）。横轴和 `Specular Color Ramp`
   一样，0 = 高光斑边缘、1 = 中心，左端要黑。会自动注册进动态 Specular 图集，不用手填 Index。
   重新生成默认贴图：`Tools/OilFilm/make_oil_film_ramp.py`。
4. 视角流动：`Oil Film View Shift`（默认 0.6）× `(1 − N·V)^Oil Film View Power`，再加 `Oil Film Hue Shift`。
   它推的是逐像素的 Specular Ramp UV Offset，满量程 1 = 后处理体积的 `Specular Ramp UV Offset Max Range`（默认 U 的 0.25）。
5. 可选 **`Use Oil Film Thin Film`**：用 UE 自带的 `F_ThinFilm` 按膜厚算干涉色，乘到 toon 高光颜色上（只改色相不改亮度）。
   - `Oil Film Thickness Map`（R，线性）在 `Oil Film Thickness Min`–`Max` 之间插值，单位是 `F_ThinFilm` 的（乘 10 才是光程，微米）；
     0.03–0.15 是彩色区间，再厚会冲淡成白。
   - `Oil Film IOR`（油约 1.45）、`Oil Film Thin Film Strength`。
   - 干涉色在高光中心处求值（那里 V·H = N·V），所以它随镜头变色，但不随灯变。

效果图：`Saved/MoonToonStyle/captures/mm/T30_oil_film_sheet.png`（三个机位，视角流动开 / 关，薄膜开 / 关）。

和旧修饰器相比放弃了：独立的第二层高光（现在是替换本身的高光，不是叠加）、`Highlight Scale` / `Falloff Power` 两个锐化旋钮、
按粗糙度换色的第二维。终末地"半程向量偏向相机"（高光跟着镜头走）留到下一批 C++（排期 T32）。

## 旧资产

- 生产材质实例里没有勾过 `Is Modifier Matcap` / `Is Modifier Oil Film`，这两个开关已从 M_MoonToon 删除。
- 直接在材质图里用 `MoonToonModifier` 节点的测试材质（`/Game/Mat`、`Decompiled/.../MatToon`、`ToonSubstrateV2/OpTest/M_MoonMod_*`）
  不再出 matcap / 油膜：引擎已不读这条通道。节点和材质输出引脚在下一批 C++（T33）标弃用。

## 实现位置

`DShader/Materials/Core/M_MoonToon.dsm` 的 "Matcap and oil film (T29 / T30)" 区块；
`DShader/MaterialFunctions/Shared/MF_ToonMatcap.dsf`、`MF_ToonOilFilm.dsf`。
