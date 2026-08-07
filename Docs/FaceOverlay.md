# See-Through Bangs (眉透 / 睫透 / 瞳透)

Brows, lashes and irises re-blended over the hair that covers them, so the face reads through a
fringe the way hand-drawn anime does. Implemented as `EMeshPass::Moon_MeshPass_FaceOverlay`: the
enrolled meshes are drawn a second time after lighting, only where they are occluded, and alpha
blended onto the lit scene colour. From the hair's point of view this is identical to "lower the
hair's opacity over the eye region".

## Configuring a character

Materials must be based on **M_MoonToon**. VRM4U's imported MToon instances (the ones with
`mtoon_*` parameters) do not carry any of this — convert them first.

### 1. Hair — set `ShadingFeatureID`

**The step people forget.** *Every* hair material that can cover the face — fringe, side locks,
back hair, ahoge — must use one of:

| Value | Dropdown name |
| --- | --- |
| 2 | Kajiya Kay Hair Specular |
| 4 | Hair Highlight Mask |
| 5 | Toon Kajiya Kay Hair Specular |

A hair material left at `Default` (0) is not recognised as hair, and nothing shows through it.

### 2. Face parts — enable the overlay

Turn on the **`Enable See Through Overlay`** static switch on the brow / lash / iris / eye-highlight
materials (eye white optional). The switch is a real static permutation: while it is off the
material is not enrolled in the pass at all and costs nothing.

### 3. Set `Through Hair Opacity`

Starting points that read well:

| Part | Value |
| --- | --- |
| Eyebrow | 0.6 – 0.7 |
| Eyelash | 0.55 – 0.65 |
| Iris | 0.4 – 0.5 |
| Eye highlight | 0.45 – 0.55 |
| Eye white (optional) | 0.25 – 0.35 |

The value is a straight `lerp(hair, face part, opacity)`. Keep the iris under ~0.55 or the hair
stops reading as hair.

### 4. Optional per-material tuning

All in the same **`24 - See Through Overlay`** group.

| Parameter | Default | When to touch it |
| --- | --- | --- |
| `Through Hair Tint` | white | Tint the show-through separately from the base colour |
| `Through Hair Depth Threshold` | 0 | Max occluder distance in cm. **0 = use the global CVar (30).** Drop to 15–20 for long-haired characters whose distant hair triggers falsely |
| `Through Hair Depth Fade` | 0 | Feathers opacity to zero over the last N cm. **Only read when Depth Threshold > 0**; leave both at 0 to inherit the global fade |
| `Through Hair View Fade Start` / `End` | 0.12 / 0.40 | N·V window. Raise Start toward 0.2 to kill profile-view residue; lower it to keep the effect at grazing angles |
| `Through Hair Occluder ID A` / `B` | -1 | Accept an extra occluder by its toon stencil ID — see below |

### 5. Global switches

```
r.MoonToon.FaceOverlay 1                    // master on/off, also the A/B switch
r.MoonToon.FaceOverlay.DepthThreshold 30    // default for materials leaving Threshold at 0
r.MoonToon.FaceOverlay.FadeRange 10         // default for materials leaving Fade at 0
```

## Showing through things that are not hair — UNVERIFIED

> **Status: implemented but never observed working.** The code path is in place and compiles, but
> the CustomDepth stencil read returned 0 in every test, and the last fix for that (an explicit
> `ESceneTextureSetupMode::All` uniform buffer in `AddToonFaceOverlayPass`) was never visually
> confirmed. Treat everything in this section as untested until someone sees it work.

Hat brims, hair accessories and glasses frames are not hair-shaded, so the `ShadingFeatureID` test
rejects them. The intended workaround tags them with a stencil:

1. On the **occluder** material, tick `Material Stencil Value` under Material Property Overrides and
   set a value 1–255.
2. On the occluder's **mesh component**, enable `Render CustomDepth Pass`.
3. On the **face** material, put the same number into `Through Hair Occluder ID A` (or `B`).

The project already has `r.CustomDepth=3` (enabled with stencil), which this needs.

Note this is the stock UE per-material stencil that lands in the CustomDepth stencil buffer — not
the MoonToon toon stencil (`ID Offset`, group `13 - Ray Tracing Shadow`), which is a 5-bit ID-map
value used for ray-traced shadow grouping and is left untouched by this feature.

### To debug it

`ToonFaceOverlayPS.usf` had a temporary block that output the stencil as colour
(`float4(OccluderStencil / 255, featureGatePassed, 0, 1)`) right before gate 3's `clip`. Re-add it,
then `RecompileShaders Changed`. Red ≈ 0.94 over the occluder means the stencil arrives; black means
it is still reading 0 and the uniform buffer binding is still the problem.

**Recompiling shaders is asynchronous** — issue `RecompileShaders Changed`, then take the screenshot
in a *separate* step, or you will be looking at the previous shader.

## How a fragment qualifies

The overlay draws a pixel only when all four hold. Any failure means nothing is drawn at all, which
is why "no effect" is the usual symptom of a misconfiguration.

| # | Gate | Purpose |
| --- | --- | --- |
| 0 | Fragment faces the camera (`TwoSidedSign > 0`) | A skull is thinner than the depth threshold and back hair is same-actor hair, so without this the far-side eye ghosts through the head |
| 1 | Occluder within the depth threshold | Rejects hair that is metres away rather than lying on the face |
| 2 | Occluder is the same toon actor (`TObjectID` vs `CustomPrimitiveData[35]`) | Rejects walls and other characters. Equal ids pass, including `0 == 0` for actors with no ToonActorComponent |
| 3 | Occluder is hair-shaded **or** carries an accepted stencil | Stops the overlay ghosting through the character's own cheek or nose in profile |

Opacity is then scaled by the depth feather and by a smoothstep over N·V, so the effect fades out
toward profile views instead of clinging to grazing lash-card edges.

## When it does not work

| Symptom | Look at |
| --- | --- |
| **Everything stopped working after the base material was regenerated** | `Enable See Through Overlay` is a **static switch**, and UE matches static-parameter overrides by `ExpressionGUID`. Recompiling `M_MoonToon.dsm` mints new GUIDs, so every material instance silently loses the override and the feature goes dark. Re-tick it on each face MI after any regeneration — this has already bitten once |
| No effect at all | The static switch above; hair `ShadingFeatureID` still 0; or you are looking at the back of the head — the level animation turns the character |
| Only some hair lets it through | That strand is a different material instance whose `ShadingFeatureID` was never set |
| Non-hair occluder ignored | Stencil not authored (`ID Offset` still 0), or the ID on the face material does not match after the 5-bit fold |
| Distant geometry triggers it | Lower `Through Hair Depth Threshold` to 15 |
| Residue on profile views | Raise `Through Hair View Fade Start` toward 0.2 |
| Visible (unoccluded) parts look brighter | Should not happen — the pass only draws occluded fragments. Report it |

### Comparing before / after

The character in `Lvl_Toon` is animated and turns on the spot, so two screenshots taken seconds
apart are different poses and cannot be compared. Freeze first:

1. Select the actor, set `Global Anim Rate Scale` to 0 on its skeletal mesh component
2. Frame the face
3. Toggle `r.MoonToon.FaceOverlay 0` / `1`

## Source

| Part | File |
| --- | --- |
| Mesh pass + draw state | `Engine/Source/Runtime/Renderer/Private/Toon/ToonPass_FaceOverlay.{h,cpp}` |
| Pixel shader (the four gates) | `Engine/Shaders/Private/Toon/ToonFaceOverlayPS.usf` |
| Material node | `Engine/Source/Runtime/Engine/Public/Materials/MaterialExpressionMoonToonFaceOverlay.h` |
| Enrollment (value-based) | `HLSLMaterialTranslator::CustomOutput`, output 0 only |
| Material parameters | `Plugins/MoonToon/DShader/MaterialFunctions/MF_MoonToonBaseInput.dsf` |
| Output routing | `Plugins/MoonToon/DShader/Materials/Core/M_MoonToon.dsm` |

The vertex shader is shared with the toon base pass (`ToonMeshPassVS.usf`), so the overlay inherits
the MoonToon perspective correction and the `INVARIANT` clip transform and depth-compares exactly
against what the base pass wrote. Adding a fourth output pin to the node requires an engine rebuild
and an editor restart; pin 2 keeps `z`/`w` free so two more stencil slots can be added from the
`.dsf` alone.
