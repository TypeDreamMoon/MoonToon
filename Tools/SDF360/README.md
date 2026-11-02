# SDF360 — 全角度脸部 SDF 烘焙工具（MoonToon T05）

## 这是什么

传统二次元脸部阴影只用一张阈值图（"SDF 脸图"）：美术画若干张"光从正面水平转到侧面时脸上哪里亮"的二值图，合成一张阈值贴图，运行时拿水平光角去比。它只能表达水平 0→180° 的光，顶光、底光、背光都做不了。全角度方案（思路参考知乎 Yu-ki016 的"360° 脸部 SDF"，本工具代码全部独立实现）改成烘焙一个**方向网格**：在头部坐标系里按方位角 θ（绕头一圈）× 仰角 φ 采样，每个方向一张二值受光 mask，每张转成**有符号距离场**，打包成图集（也可输出 Texture2DArray 切片）。运行时材质找到光方向周围的 4 个网格格子，在脸 UV 处各采一次 SDF，对**有符号距离**做双线性插值，再以 0 为阈值。插值的是距离而不是亮暗，所以明暗交界线会在相邻两张手绘 mask 之间平滑滑动，而不是跳变。

## 文件

| 文件 | 作用 |
|---|---|
| `bake_sdf360.py` | 烘焙：mask 目录 → `atlas.png` + `slice_XXX.png` + `sdf360.json` |
| `gen_masks.py` | 从脸部网格自动生成**初稿** mask（OBJ 内置解析、glTF/GLB 内置读取、其它格式走 trimesh） |
| `preview.py` | 按 shader 同样的逻辑重建明暗，跑水平/垂直扫光，出联系表、GIF 和跳变指标 |
| `sdf360_sample.hlsl` | 材质端参考 HLSL（图集版、Texture2DArray 版、可直接贴进 Custom 节点的版本） |
| `sdf_edt.py` | 自写的 8SSEDT 有符号距离变换（numpy 向量化，装了 numba 自动加速，结果逐位一致） |
| `sdf360_common.py` | 网格定义、角度换算、查表、编码、图集 UV、命名、读写图片（所有工具共用的约定） |
| `test_edt.py` | 8SSEDT 与 scipy 精确 EDT 的误差对比 + 图集映射/查表单元测试 |
| `demo_synthetic.py` | 无需任何资产的端到端演示（合成脸 → mask → 烘焙 → 预览 → 指标） |

运行环境：Windows，Python 3.11+，用 uv：

```
uv run --with numpy --with pillow python bake_sdf360.py ...
uv run --with numpy --with pillow --with numba python bake_sdf360.py ...   # 可选加速
uv run --with numpy --with pillow --with scipy python test_edt.py          # 测试需要 scipy
uv run --with numpy --with pillow python demo_synthetic.py                 # 输出到 _test_out/demo
```

路径参数用正斜杠。

## 坐标与网格约定（材质必须一致）

- 头部坐标系（Unreal 左手系，Z 朝上）：`Up` = 头顶方向，`Forward` = 脸朝向（会先正交化到 Up），`Right = cross(Up, Forward)`。UE 里 X 前 / Z 上时 Right = +Y，也就是**角色自己的右边**。
- `L` = 从表面**指向光源**的单位向量。
- `θ = degrees(atan2(dot(L,Right), dot(L,Forward)))`：0 = 正面光，**+90 = 光在角色右侧**，±180 = 背光。
- `φ = degrees(asin(dot(L,Up)))`：+90 = 正顶光，−90 = 正底光。
- 全圆模式（默认）：θ 取 `-180 + i·360/Nθ`（i = 0..Nθ−1），周期性，最后一格和第 0 格互相插值。注意 Nθ 为奇数时网格里**没有 θ=0**（例如 9 格是 −180, −140, …, −20, 20, …, 140），想要 0/±90/180 正好落在格子上就用偶数（8 格 = 45° 一格）。
- 镜像模式 `--mirror`：θ 只覆盖 `[0,180]`，`Δθ = 180/(Nθ−1)`；θ<0 时用 |θ| 的格子，并把脸 UV 的 x 换成 `1−x`。
- φ：`φmin + j·(φmax−φmin)/(Nφ−1)`，两端包含，超出范围钳制。Nφ=1 时退化为只有水平一行（需 φmin=φmax）。
- 图集：第 i 列 = θ 序号，第 j 行 = φ 序号，**第 0 行 = φmin，在图片最上面**。Array 切片序号 `k = j·Nθ + i`。

## 怎么画 mask

- 每个网格格子一张 PNG，**白 = 亮，黑 = 暗**，灰度按 0.5 二值化（抗锯齿边没问题）。可以带 alpha（alpha 会乘到灰度上）。
- 命名：`mask_t{θ}_p{φ}.png`，角度单位是度，整数直接写（`mask_t-140_p20.png`、`mask_t0_p-90.png`），不是整数时最多三位小数（`mask_t0_p22.5.png`）。不写 `+` 号、不补零。读取时按 ±0.5° 容差匹配，所以把 22.5 写成 23 也能认；全圆模式下 `t180` 和 `t-180` 等价。缺格子会直接报错并列出期望的文件名。`gen_masks.py` 生成的 `masks.json` 里有完整文件名列表，可以当模板。
- mask 画在**脸的 UV 空间**里（和脸部贴图同一个 UV、同一个朝向：图片第一行 = UE 里 v=0）。θ=+90 是"光在角色右侧"，对常见的正视图 UV 布局（角色右脸在图片左半边）来说亮的是**图片左半边**——画之前先用 `gen_masks.py` 出一套草稿对一下方向。
- 可以超采样：mask 画 2048²，烘焙时 `--cell-size 256`，距离场在 2048 上算再盒式下采样并换算成格子像素，边界精度更高。所有 mask 尺寸必须一致。
- 镜像模式要求：脸**左右对称**且 UV **左右对称、不重叠**（UV 关于 u=0.5 镜像对应脸的对称面）；θ=0 和 θ=180 两列的 mask 必须自身左右对称（工具会检查，默认自动对称化并警告）。只需要画 θ∈[0,180]，也就是光在角色右侧的那一半。
- 一致性：相邻格子的亮区要"连续地"变化——同一块阴影在相邻两格里应该是同一块形状的移动/伸缩。相邻两格的明暗正好互换（左亮变右亮）会让插值中点整片同时翻转；小岛（鼻影、眼窝）可以出现/消失，SDF 插值会让它从一点长出来。
- 全亮/全暗的格子（正面光、背光）可以直接画纯白/纯黑，工具会自动合成距离场（见"烘焙"）。

## 从网格生成初稿

```
uv run --with numpy --with pillow python gen_masks.py --mesh face.obj --out masks/ --resolution 1024 ^
    --forward 0,-1,0 --up 0,0,1 --handedness right --normals mesh --smooth-iters 4 ^
    --theta-count 8 --phi-count 7 --phi-min -90 --phi-max 90
```

- 生成的是 `dot(N_smooth, L) > --bias` 的**初稿**，没有投影、没有风格化，美术需要在此基础上重画（鼻影形状、眼窝、下巴线等）。
- `--forward/--up`：网格空间里的脸朝向和头顶方向。`--handedness`：OBJ/glTF/Blender/Maya 是右手系（默认 `right`，右 = cross(Forward, Up)）；直接从 UE 导出的左手系数据用 `left`；也可以 `--right x,y,z` 直接指定角色右方向。脚本会打印算出来的 Right 轴，务必确认它指向**角色的右手**，并看一眼 θ=+90 的 mask。
- UV：`--uv-channel`（glTF 支持任意 TEXCOORD_n，OBJ/trimesh 只有 0），`--v-origin bottom`（默认，OBJ 习惯，图片行 = 1−v；glTF 读入时已转换成这个习惯）。如果 mask 和脸部贴图上下颠倒就切到 `top`。
- 法线模式：
  - `mesh`：几何顶点法线（先按位置焊接，跨 UV 接缝连续），再做 `--smooth-iters` 轮邻域平均；
  - `file`：用文件里存的法线（例如已经传递过球面法线的脸）；
  - `ellipsoid`：在头部坐标系里拟合一个轴对齐椭球，用椭球法线——经典的"干净二次元脸"法线；`--ellipsoid-scale f,r,u` 缩放拟合半轴，`--ellipsoid-axes`、`--ellipsoid-center` 直接覆盖。
- `--dilate`（默认 8）：UV 岛外扩的像素；`--background extend`（默认）把岛外全部用最近岛像素延伸，避免在岛边界上人为产生明暗边；`dark` 则岛外 `--dilate` 以外强制为暗。
- `--select NAME`：只取名字（OBJ 的 o/g/usemtl，glTF 的 mesh/material）包含 NAME 的面。重叠 UV（镜像 UV 叠在一起）不支持，超过 1% 会警告。
- 输出：所有 mask、`coverage.png`（UV 岛覆盖，未外扩，烘焙和预览用它决定"脸上的像素"）、`normals_preview.png`（R=N·Right，G=N·Up，B=N·Forward）、`masks.json`（网格和参数，烘焙时省略网格参数就从这里读）。

## 烘焙

```
uv run --with numpy --with pillow python bake_sdf360.py --masks masks/ --out baked/ --cell-size 256 --padding 4
```

- 网格参数 `--theta-count --phi-count --phi-min --phi-max [--mirror]`，不写就从 `masks/masks.json` 读。
- 距离：8SSEDT（`--edt sweep` 默认，numpy 下也快；`--edt raster` 是经典两遍光栅扫描，没有 numba 时很慢）。半像素约定：亮像素 `d = 到最近暗像素中心的距离 − 0.5`，暗像素取负。相邻亮/暗像素存 ±0.5，双线性插值正好在边界上过 0。
- 编码：`v = saturate(0.5 + d/(2R))`，d 单位是**格子像素**，亮区 v>0.5。默认 16 位 PNG，`--bits 8` 可选。
- `--spread`（R）默认 `auto`：取"不会跳变的最小 R"。如果某个像素在一个格子里深入亮区超过 R、在同一插值四邻域的另一个格子里深入暗区也超过 R，两边都被钳成 ±1，所有这样的像素会在同一个插值权重同时翻转——就是肉眼可见的跳变。工具在脸部覆盖区（coverage 外扩 padding）里算出这个下限，打印出是哪一对格子决定的，并写进 `sdf360.json`。手动给的 R 比它小会警告。8 位时量化步长 = 2R/255 像素，R 大了会粗，所以推荐 16 位。
- 全亮/全暗格子（`--uniform-fill shift`，默认）：不再用常数 ±R（会导致交界线先停住、再在靠近该格子时一口气冲过去），而是把相邻非均匀格子的距离场平移到同号（留 1 像素余量），逐像素取最接近 0 的值。这样往它插值时交界线匀速移动，正好在该格子的角度离开脸部。格子在脸部区域内保证全亮/全暗；脸部区域外可能变号（那些像素不会被采样）。`--uniform-fill constant` 恢复常数。
- 镜像接缝：θ=0/180 两列会检查左右对称，默认对称化（`--no-symmetrize-seams` 关闭）。
- 极点：包含 φ=±90 时，那一行所有 θ 其实是同一个方向，工具把整行（镜像模式还包括其镜像）平均成一个场，输入不一致时警告。
- 输出：`atlas.png`、`slice_000.png…`（`--no-atlas`/`--no-slices` 可跳过）、`sdf360.json`（材质契约：网格、布局、编码、坐标约定、R、警告、版本）、`coverage.png`。

## 预览与验收

```
uv run --with numpy --with pillow python preview.py --bake baked/ --step 5
```

- 重建逻辑与 `sdf360_sample.hlsl` 一步一步对应（角度 → 查表 → 钳制/镜像 UV → 双线性采样图集 → 解码 → 两次 lerp），`--source slices` 用切片重建（结果应与图集完全一致，演示里差异 < 1e-14）。
- 三条扫光：`h_phi0`（φ=0，θ −180→180）、`v_theta0` 和 `v_theta90`（θ=0/90，φ 从 φmin 到 φmax），步长 `--step`。
- 跳变指标：相邻两步之间，脸部像素（coverage.png）中明暗翻转的比例。报告每条扫光的最大值、平均值；超过 `--jump-threshold`（默认 10%/步）或是相邻步平均值的 `--spike-ratio` 倍（默认 3 倍，且 >0.5%）会标记。
- 单调性：光从正面往背面（θ 0→±180）、从水平往两极（θ=0 时 φ 0→±max）移动时，亮区只应缩小；出现"暗→亮"超过 `--monotone-tol`（默认 0.2%）会标记，并报告翻转超过一次的像素比例。θ=90 的垂直扫光是交界线旋转，不做单调检查。
- 输出 `sheet_*.png` 联系表、`*.gif`（`--apng` 另出 APNG）、`preview_report.json`。有标记时退出码为 1。

## 导入 Unreal

**图集（Texture2D）**
- sRGB **关闭**；Filter = Bilinear；Address X/Y = Clamp。
- 压缩：16 位 PNG 推荐 `Grayscale`（16 位源会得到 G16，无损）或 `HDR`/`SingleFloat`（R16F）；`Alpha (BC4)` 体积最小但只有约 8 位精度且有块误差，R 较大时边界会抖，只适合柔和的效果。
- Mip Gen Settings = **NoMipmaps**（推荐）。原因：每个格子周围只有 P 像素的复制边，第 m 级 mip 的双线性足迹是 2^m 个图集像素，会把相邻格子的距离混进来，而且各格子边界在低级 mip 上不再对齐像素。确实需要 mip 时：P ≥ 2^m、格子步长 (W+2P) 是 2^m 的倍数、只开到 m 级；代价是图集变大。脸通常离镜头不远，不开 mip 用 Gain 控制边缘软硬即可。
- 图集默认是非 2 次幂尺寸（如 9×(256+8)=2376），不开 mip 没问题；要 2 次幂可以调 cell/padding（例如 8 列 × (248+2·4) = 2048）。
- 采样一律 `SampleLevel(..., 0)`。

**Texture2DArray**
- 把 `slice_000.png…` 全部导入（设置同上），按文件名顺序全选 → 右键 Create Texture Array；创建后检查 Source Textures 的顺序必须等于切片号 `k = j·Nθ + i`。Address = Clamp。切片没有 padding，直接在脸 UV 采样。

## 材质契约（shader 侧必须遵守）

材质需要的参数（全部来自 `sdf360.json`）：
- `ThetaCount, PhiCount, PhiMin, PhiMax, Mirror(0/1)`；
- 图集版另需 `CellW, CellH, Padding`（图集尺寸由它们算出：`AtlasW = Nθ·(W+2P)`，`AtlasH = Nφ·(H+2P)`）；
- `R = encoding.spread_texels`（用于换算 Gain）；
- 头部 `Forward`、`Up`（世界空间）和光方向 `L`（表面→光源）。

计算步骤（`sdf360_sample.hlsl` 里的 `MoonSdf360Evaluate` / `MoonSdf360EvaluateArray`）：
1. `U = normalize(Up)`，`F = normalize(Forward − U·dot(Forward,U))`，`R = cross(U,F)`；θ、φ 按上文公式（`L` 平行于 Up 时 θ 取 0）。
2. 查表：全圆模式 `u = (θ+180)/(360/Nθ)`，`i0 = floor(u) mod Nθ`，`i1 = (i0+1) mod Nθ`，`tθ = frac(u)`；镜像模式 `u = clamp(|θ|/Δθ, 0, Nθ−1)`，`i0 = min(floor(u), Nθ−2)`，`tθ = u − i0`，`i1 = i0+1`，θ<0 时 `uv.x = 1 − uv.x`；φ：`v = clamp((φ−φmin)/Δφ, 0, Nφ−1)`，`j0 = min(floor(v), Nφ−2)`，`tφ = v − j0`，`j1 = j0+1`。
3. `uv = saturate(FaceUV)`（再按需镜像），图集 UV = `((i·(W+2P) + P + uv.x·W)/AtlasW, (j·(H+2P) + P + uv.y·H)/AtlasH)`。uv=0/1 落在首/末格子像素的**外边缘**，像素 k 的中心在 (k+0.5)/W，和普通贴图约定一致；复制边让图集采样等价于 Clamp 寻址的单张切片。
4. 每格 `s = (v − 0.5)·2`，结果 `s = lerp(lerp(s00, s10, tθ), lerp(s01, s11, tθ), tφ)`。s>0 为亮，`s·R` 约等于到交界线的格子像素距离。
5. 材质输出 `SdfLeft = saturate(0.5 + s·Gain)`，引擎在 0.5 处取阈值。`Gain = R/(2·软边像素数)`，或用 `0.5/fwidth(s)` 做屏幕空间抗锯齿。

Custom 节点版本见 HLSL 文件末尾 `CUSTOM NODE BODY` 标记之间（输入：`Atlas` 贴图对象、`FaceUV`、`L`、`Forward`、`Up`、`Grid = (Nθ, Nφ, φmin, φmax)`、`Layout = (W, H, P, Mirror)`）。该文件已用引擎自带 dxc（ps_6_0）编译通过。

## 限制

- **极点**：φ=±90 处 θ 没有定义，整行被统一成一个场；靠近极点时 θ 变化很快，但因为整行相同，结果连续。极点行的 mask 应该只依赖光从正上/正下，各 θ 画的不一致会被平均掉（并警告）。
- **镜像模式**要求对称、不重叠的 UV 和左右对称的脸；θ=0/180 两列会被强制对称。不对称的脸用全圆模式。
- **mask 一致性**：插值只能让交界线在相邻两张 mask 之间"最短路径"地移动。相邻格子差异过大（明暗互换、拓扑剧变、角度步长太大）会在中间角度出现与真实 N·L 不同的形状甚至整片翻转——用 `preview.py` 的跳变指标检查，必要时加密网格或修改 mask。
- 网格太粗时中间角度的形状偏差会比较明显（演示里 9×9 全圆网格与真实 N·L 的最大像素差约 7%，5×9 镜像约 10%，出现在两个采样角中间）；这不是跳变，但会改变阴影形状的中间态。
- 初稿生成器只算 N·L（加可选偏置），没有自阴影/鼻影投射；它不处理 UDIM 或 0–1 以外的 UV。
- 8 位 / BC4 精度有限：需要大 R 的网格（交界线在相邻格子间移动距离大）不适合 8 位。
- 全亮/全暗格子的合成场只保证在脸部覆盖区内同号；没有 coverage.png 时会按整张图计算（更保守，交界线进入脸部会稍晚）。

## 验证（本工具自带的测试结果）

- `test_edt.py`：8SSEDT（sweep 调度，256²，15 种形状含随机斑块/细线/棋盘/椭圆带洞）与 scipy 精确 EDT 相比，最大误差 0.086 像素，平均 1.4e-5 像素，0.07% 像素有误差；经典 raster 调度最大 0.066 像素。numba 与 numpy 结果逐位一致。
- `demo_synthetic.py`：合成脸（椭球 + 鼻子凸起 + 两个眼窝）512² mask → 256² 格子。9×9 全圆（φ −80..80）和 5×9 镜像（φ −90..90 含极点）两种网格，5° 步长扫光全部无标记，每步最大翻转约 6–7%（真实 N·L 本身约 5.3–5.7%），无"暗→亮"回跳。故意用 `--spread 16 --bits 8` 烘焙时，预览会在格子中点报 17.5% 的跳变。
