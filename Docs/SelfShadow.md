# Disable Self Shadow (关闭自阴影)

The `Disable Self Shadow` static switch on a MoonToon material means: **this surface is never
shadowed by the object it belongs to, but everything else still shadows it.** Its own hair, head,
arms and clothing are excluded; props, other characters and the environment are not.

Turn it on for skin, cloth, hair — anything whose shading should come from the toon ramp, the SDF
facial shadow and the stylized screen-space hair shadow rather than from a projected shadow map.

Historically this switch only existed in the ray-traced shadow path, where every hit knows which
instance it struck and "is this occluder me" is a direct test. The raster paths below reconstruct
that meaning per shadow technique, because a rasterized shadow map stores depth and nothing else.

## Which shadow path covers which light

| Light setup | Shadow path | How the switch is honoured |
| --- | --- | --- |
| **`r.RayTracing.Shadows 1`** (any light set to Use Project Setting) | RT hit shader | Caster-side test: same toon **stencil** group, OR same toon **actor id** (**needs a `MoonToonActorComponent`**) |
| Movable directional / spot / point, VSM on, RT shadows off | VSM projection (SMRT) | Ray starts at the receiver's own-volume exit |
| Stationary any type shadowing a movable character, RT shadows off | Per-object shadow map | Receiver pixels of the shadow's own subject actor skip it (**needs a `MoonToonActorComponent`**) |
| VSM off entirely (whole-scene CSM / cube) | Conventional whole-scene | **Not covered** — no subject identity exists |

**Which path is actually running?** If `r.RayTracing.Shadows` is 1, it is the RT path for every light
left on *Use Project Setting* — toggling VSM changes nothing about those shadows. This misled an
entire debugging session; check the CVar first.

## The VSM gate — "outside my own volume"

The SMRT ray does not start at the surface; it starts where the light ray leaves the receiver's own
volume:

```
        light
          |
    +-----|-----+  <- ray starts here (box exit): the head cannot be hit
    |     v     |
    |   (head)  |
    |   (chest) |  <- shading this pixel
    +-----------+
```

Nothing inside the box can be hit, so no part of the character shadows any other part. Everything
outside is still traced normally — the ray keeps its full length, only its start moves, so distant
casters are not lost. For local lights the start is additionally clamped to just short of the light,
so a light inside the character's own volume reads fully lit instead of inverting the ray.

The volume is, in priority order:

1. **An authored box** on the character's `MoonToonActorComponent` (`Toon Actor > Self Shadow`):
   oriented, follows the component transform, sized by hand. When present it *replaces* the derived
   bounds for every primitive carrying that actor's id, so it may be tighter than the bounds.
   Enable `Draw Self Shadow Volume` while adjusting, and turn it off afterwards (it ticks).
2. **The mesh bounds** of every visible toon primitive, world-space AABB, rebuilt per view per
   frame. Zero configuration, but it follows the physics asset (breathes with the pose), inflates
   with `Bounds Scale`, freezes with `Fixed Bounds`, and is axis-aligned (up to √2 too wide on a
   rotated character) — author a box when any of that matters.
3. **A fixed radius** (10cm, 45cm on SDF faces) when the pixel is in no volume at all — the
   original contact-acne threshold, kept as the floor.

### Why a fixed radius was not enough

A fixed offset cannot work along an object's long axis: with the light straight down, a character's
own head sits **40–100cm** above its own chest, so the hair kept shadowing the body while the switch
claimed self shadow was off. No fixed number separates that from a prop hanging at the same height —
only the receiver's own extent does.

## The RT gate — stencil groups, now backed by the actor id

The ray-traced path always had a self-shadow test, keyed on the **toon stencil** (`ID Offset` /
ID Map, 5-bit fold): a caster with `Disable Self Shadow` whose stencil equals the receiver's is
ignored. It has a structural weakness: the receiver's stencil comes from the **rasterized**
GBuffer while the caster's is **re-evaluated by the hit shader**, and ID sources like vertex
colour or an ID map can produce different values in the two pipelines. A character's mouth
interior then reads as a different group — and its **teeth shadow through the face** ("light
passing through the first layer"; the face itself is the first thing the ray hits, but its hit is
ignored as self, while the teeth's is not).

The test now also accepts **same toon actor id**: the raygen packs the receiver's `TObjectID`
into 11 spare payload bits, and the hit shader compares it against the caster's
`CustomPrimitiveData[35]`. Both sides read the *same stored value* — nothing is re-evaluated, so
nothing can diverge, and no per-material stencil authoring is needed. Characters without a
`MoonToonActorComponent` have id 0 and fall back to the stencil-only behaviour.

Verified on SK_Lin: teeth shadow gone under `r.RayTracing.Shadows 1` with all `ID Offset` at 0
(the previously-broken state). Diagnosis trick for stencil divergence: set `ID Offset 255` on both
materials — the 5-bit fold clamps both sides to 31, and if the shadow disappears the mechanism
works and the stencils simply disagree.

## The per-object gate — "the subject is me"

Stationary lights shadow movable characters through **per-object shadow maps** that never touch the
VSM projection, so the volume gate cannot help there. The user-visible symptom: a stationary point
light projected a character's own **teeth** through the face — the face material does not cast, so
the mouth interior is the first depth the shadow map sees ("light going through the first layer").

Per-object shadows know their subject on the CPU, so this path gets the *exact* test back: the
subject's toon actor id rides into the projection shader, and flagged toon receiver pixels whose
`TObjectID` matches it skip the shadow. Preshadows (static environment casting onto the character)
and whole-scene shadows are never gated — their casters are not the subject.

**This requires the character to carry a `MoonToonActorComponent`** (it stamps the actor id into
`CustomPrimitiveData[35]`). A character without one has id 0 and the per-object gate stays off for
it — deliberately, since matching unidentified subjects against unidentified receivers would erase
per-object shadows between plain characters.

## Console variables

```
r.MoonToon.SelfShadow.UseObjectBounds 1    // VSM gate: 0 = fixed radius only, 1 = volumes (default)
r.MoonToon.SelfShadow.MaxVolumeExtent 400  // cm; toon primitives bigger than this are not volumes
r.MoonToon.SelfShadow.MaxOffset      1000  // cm; upper clamp on the VSM ray start offset
r.MoonToon.SelfShadow.PerObjectShadows 1   // per-object gate: 0 = legacy, 1 = subject-id skip (default)
```

`UseObjectBounds 0/1` is the A/B switch for the VSM gate; `PerObjectShadows 0/1` for the stationary
per-object gate.

`MaxVolumeExtent` exists because the derived volume is a box, not the mesh: a toon-shaded ground
plane or backdrop would otherwise be a volume swallowing every shadow cast on anything standing
inside it. Authored component boxes are exempt (an artist sized them on purpose).

## Limits

| | |
| --- | --- |
| **VSM off entirely** | Whole-scene CSM / one-pass point cube have no subject and no gate. The project renders through VSM; if you disable it, self shadows return |
| **Volumes, not the mesh** | A prop *inside* a character's volume (a held weapon crossing the torso) will not cast onto them. Anything outside is unaffected |
| **Overlapping characters** | Two characters close enough to share derived bounds stop shadowing each other in the overlap — author tighter boxes on their components if it shows |
| **MegaLights** | Calls the VSM trace functions with explicit settings and bypasses the gate parameter. Not wired up; the project does not use MegaLights |

## Verified

VSM gate — `Lvl_Toon`, `SK_ww_yy`, directional light straight down (pitch −88.9°), mean luma over
the chest skin:

| | legacy fixed radius | object bounds | object bounds + blocker 60cm above the head |
| --- | --- | --- | --- |
| chest skin | 184.9 | **225.5** | 184.6 |
| face (SDF) | 229.1 | 228.9 | 192.2 |
| floor (control) | 148.7 | 148.7 | 148.0 |

The chest loses its self shadow and an external caster puts it straight back; the SDF face still
takes the external caster; the floor confirms no exposure drift. The local-light (point) gate was
verified visually on the same character. The per-object gate compiles but had not been visually
verified at the time of writing — verify with a **stationary** point light over a character that has
a `MoonToonActorComponent`, A/B via `r.MoonToon.SelfShadow.PerObjectShadows 0/1`.

## Debugging "a shadow won't go away"

1. **`r.RayTracing.Shadows` first.** If it is 1, the shadow is ray traced and neither the VSM nor
   the per-object gates are involved; toggle it 0/1 to confirm the path. Lights individually set
   to *Cast Ray Traced Shadows: Enabled/Disabled* override the project setting.
2. Is the receiving material's `Disable Self Shadow` actually on? (Per-MI static switch.)
3. On the RT path: does the character carry a `MoonToonActorComponent`? Without one the test falls
   back to stencil groups, which silently break when raster and RT evaluate the ID differently
   (see the RT gate section — the teeth-through-face case).
4. Toggling `r.Shadow.Virtual.Enable` **swaps VSM for CSM, it does not turn shadows off**; a shadow
   that disappears with the light's `Cast Shadows` unchecked is a projected shadow for certain.
5. Stationary light, RT off? That is the per-object path — needs the component and
   `r.MoonToon.SelfShadow.PerObjectShadows 1`.
6. Shapes that no toggle removes are not shadow maps: the screen-space hair shadow band and the SDF
   facial shadow are stylized shading with their own switches (PPV `Moon Enable Screen Space Hair
   Shadow * Lights`, `Moon Enable Distance Field Facial Shadow * Lights`, per-MI intensities).

## Source

| Part | File |
| --- | --- |
| VSM: volume gather, CVars, buffer upload | `Renderer/Private/VirtualShadowMaps/VirtualShadowMapProjection.cpp` |
| VSM: ray start offset, OBB slab exit | `Shaders/Private/VirtualShadowMaps/VirtualShadowMapProjection.usf` |
| VSM: directional consume | `VirtualShadowMapProjectionDirectional.ush`, `TraceDirectional` |
| VSM: local-light consume | `VirtualShadowMapProjectionSpot.ush`, `TraceLocalLight` |
| Per-object: subject id + CVar | `Renderer/Private/ShadowRendering.cpp`, `GetMoonToonPerObjectSelfShadowSubjectId` |
| Per-object: receiver test | `Shaders/Private/ShadowProjectionPixelShader.usf` |
| RT: payload actor id (bits 21-31) | `Shaders/Private/RayTracing/RayTracingCommon.ush`, `Get/SetToonActorId`, `MoonTraceVisibilityRay` |
| RT: receiver id pack | `Shaders/Private/RayTracing/RayTracingOcclusionRGS.usf` (reads `TObjectIDTexture`) |
| RT: caster test | `Shaders/Private/RayTracing/RayTracingMaterialHitShaders.usf`, any-hit Moon block |
| RT: dummy binding for toon-free frames | `Renderer/Private/RayTracing/RayTracingShadows.cpp` |
| Authored volume + actor id | `Engine/Classes/Components/MoonToonActorComponent.h` |
| Receiver flag (GBuffer bit) | `CustomData.w` bit 4, see `EncodeToonGBufferDataToMRT` in `ToonBufferCommon.ush` |
| Material switch | `Disable Self Shadow` in `MF_MoonToonBaseInput.dsf` |
