// MoonToon SDF360 (full-angle face SDF) - reference sampling code.
//
// Documentation / paste source for the material side. The logic matches preview.py (class Reconstructor)
// step for step: angles -> 4-cell lookup -> saturate/mirror face UV -> bilinear samples -> decode -> lerps.
// Plain HLSL, no includes; every grid parameter is a function argument (copy the values from sdf360.json).
//
// Conventions (see sdf360.json "frame", "grid", "encoding", "atlas"):
//   Head frame : Up, Forward (face looks along it), Right = cross(Up, Forward)   (UE: X fwd, Y right, Z up)
//   L          : unit vector from the shaded surface TOWARD the light
//   theta      = degrees(atan2(dot(L,Right), dot(L,Forward)))  0 = front, +90 = character's right
//   phi        = degrees(asin(dot(L,Up)))                       +90 = from above
//   Full mode  : theta grid -180 + i*360/N (i = 0..N-1), periodic, index N-1 wraps to 0
//   Mirror mode: theta grid i*180/(N-1) (i = 0..N-1); theta < 0 uses |theta| and FaceUV.x -> 1 - FaceUV.x
//   phi grid   : PhiMin + j*(PhiMax-PhiMin)/(PhiCount-1), clamped (PhiCount = 1: single row)
//   Texel value: v = saturate(0.5 + d / (2R)), d = signed distance to the boundary in cell texels, lit > 0
//   Returned s : lerp of (v - 0.5) * 2 over the 4 cells, i.e. d / R (clamped to [-1, 1] per cell)
//   Atlas UV   : ((i*(W+2P) + P + uv.x*W) / AtlasW, (j*(H+2P) + P + uv.y*H) / AtlasH), uv = saturate(FaceUV),
//                row j = 0 (phi = PhiMin) at the TOP of the texture, AtlasW = ThetaCount*(W+2P), AtlasH = PhiCount*(H+2P)
//                Standard texel convention: uv 0 / 1 are the outer EDGES of the first / last cell texel, texel k is
//                centred at (k + 0.5) / W; the replicated padding makes the atlas behave like a clamp-addressed slice.
//   Array slice: k = j * ThetaCount + i, sample at saturate(FaceUV) directly (no padding).
//
// Usage in the toon material:
//   float s = MoonSdf360Evaluate(...);
//   SdfLeft = saturate(0.5 + s * Gain);      // the engine thresholds SdfLeft at 0.5 (lit when s > 0)
//   Gain = R / (2 * SoftTexels): SdfLeft goes 0 -> 1 across 2*SoftTexels cell texels (R = encoding.spread_texels).
//   For a screen-space anti-aliased edge use Gain = 0.5 / max(fwidth(s), 1e-5) instead.
//
// Sampler: bilinear, clamp, sRGB off. Always SampleLevel(..., 0): neighbouring pixels may pick different cells
// (per-pixel L of local lights), which makes UV derivatives meaningless across cell borders. With mips, padding P
// must cover the mip footprint (P >= 2^mip) - the bake is meant for no-mip textures.

// ------------------------------------------------------------------------------------------------------------
// Building blocks
// ------------------------------------------------------------------------------------------------------------

void MoonSdf360Angles(float3 L, float3 Forward, float3 Up, out float ThetaDeg, out float PhiDeg)
{
	float3 U = normalize(Up);
	float3 F = normalize(Forward - U * dot(Forward, U));
	float3 R = cross(U, F);
	float3 Ln = normalize(L);
	float X = dot(Ln, F);
	float Y = dot(Ln, R);
	ThetaDeg = (X * X + Y * Y) < 1e-12 ? 0.0 : degrees(atan2(Y, X));   // pole guard: theta undefined at +-Up
	PhiDeg = degrees(asin(clamp(dot(Ln, U), -1.0, 1.0)));
}

// Cell indices (as floats), interpolation weights and the mirror flag for a light direction.
void MoonSdf360Lookup(float ThetaDeg, float PhiDeg, float ThetaCount, float PhiCount, float PhiMin, float PhiMax,
	float Mirror, out float I0, out float I1, out float J0, out float J1, out float TTheta, out float TPhi,
	out float Flip)
{
	if (Mirror > 0.5)
	{
		Flip = ThetaDeg < 0.0 ? 1.0 : 0.0;
		float Step = 180.0 / max(ThetaCount - 1.0, 1.0);
		float U = clamp(abs(ThetaDeg) / Step, 0.0, ThetaCount - 1.0);
		I0 = min(floor(U), max(ThetaCount - 2.0, 0.0));
		TTheta = U - I0;                                  // theta = 180 -> I0 = N-2, TTheta = 1
		I1 = min(I0 + 1.0, ThetaCount - 1.0);
	}
	else
	{
		Flip = 0.0;
		float Step = 360.0 / ThetaCount;
		float U = (ThetaDeg + 180.0) / Step;              // [0, ThetaCount]
		float F0 = floor(U);
		TTheta = U - F0;
		I0 = F0 - ThetaCount * floor(F0 / ThetaCount);    // wrap; U == ThetaCount (theta = +180) -> cell 0
		I1 = I0 + 1.0;
		if (I1 >= ThetaCount)
		{
			I1 -= ThetaCount;                             // last cell interpolates with cell 0 (theta -180)
		}
	}
	if (PhiCount > 1.5)
	{
		float Step = (PhiMax - PhiMin) / (PhiCount - 1.0);
		float V = clamp((PhiDeg - PhiMin) / Step, 0.0, PhiCount - 1.0);
		J0 = min(floor(V), PhiCount - 2.0);
		TPhi = V - J0;
		J1 = J0 + 1.0;
	}
	else
	{
		J0 = 0.0;
		J1 = 0.0;
		TPhi = 0.0;
	}
}

float2 MoonSdf360AtlasUV(float2 Uv, float I, float J, float ThetaCount, float PhiCount, float2 CellSize, float Padding)
{
	float2 Stride = CellSize + 2.0 * Padding;
	float2 AtlasSize = float2(ThetaCount, PhiCount) * Stride;
	return (float2(I, J) * Stride + Padding + Uv * CellSize) / AtlasSize;
}

// ------------------------------------------------------------------------------------------------------------
// Atlas (Texture2D) variant: returns s in R-normalised units (lit when s > 0)
// ------------------------------------------------------------------------------------------------------------

float MoonSdf360Evaluate(Texture2D Atlas, SamplerState AtlasSampler, float2 FaceUV, float3 L, float3 Forward, float3 Up,
	float ThetaCount, float PhiCount, float PhiMin, float PhiMax, float Mirror, float2 CellSize, float Padding)
{
	float Theta, Phi;
	MoonSdf360Angles(L, Forward, Up, Theta, Phi);
	float I0, I1, J0, J1, TTheta, TPhi, Flip;
	MoonSdf360Lookup(Theta, Phi, ThetaCount, PhiCount, PhiMin, PhiMax, Mirror, I0, I1, J0, J1, TTheta, TPhi, Flip);
	float2 Uv = saturate(FaceUV);
	if (Flip > 0.5)
	{
		Uv.x = 1.0 - Uv.x;
	}
	float S00 = (Atlas.SampleLevel(AtlasSampler, MoonSdf360AtlasUV(Uv, I0, J0, ThetaCount, PhiCount, CellSize, Padding), 0).r - 0.5) * 2.0;
	float S10 = (Atlas.SampleLevel(AtlasSampler, MoonSdf360AtlasUV(Uv, I1, J0, ThetaCount, PhiCount, CellSize, Padding), 0).r - 0.5) * 2.0;
	float S01 = (Atlas.SampleLevel(AtlasSampler, MoonSdf360AtlasUV(Uv, I0, J1, ThetaCount, PhiCount, CellSize, Padding), 0).r - 0.5) * 2.0;
	float S11 = (Atlas.SampleLevel(AtlasSampler, MoonSdf360AtlasUV(Uv, I1, J1, ThetaCount, PhiCount, CellSize, Padding), 0).r - 0.5) * 2.0;
	return lerp(lerp(S00, S10, TTheta), lerp(S01, S11, TTheta), TPhi);
}

// ------------------------------------------------------------------------------------------------------------
// Texture2DArray variant (slice k = j * ThetaCount + i, no padding, clamp addressing)
// ------------------------------------------------------------------------------------------------------------

float MoonSdf360EvaluateArray(Texture2DArray Slices, SamplerState SlicesSampler, float2 FaceUV, float3 L, float3 Forward,
	float3 Up, float ThetaCount, float PhiCount, float PhiMin, float PhiMax, float Mirror)
{
	float Theta, Phi;
	MoonSdf360Angles(L, Forward, Up, Theta, Phi);
	float I0, I1, J0, J1, TTheta, TPhi, Flip;
	MoonSdf360Lookup(Theta, Phi, ThetaCount, PhiCount, PhiMin, PhiMax, Mirror, I0, I1, J0, J1, TTheta, TPhi, Flip);
	float2 Uv = saturate(FaceUV);
	if (Flip > 0.5)
	{
		Uv.x = 1.0 - Uv.x;
	}
	float S00 = (Slices.SampleLevel(SlicesSampler, float3(Uv, J0 * ThetaCount + I0), 0).r - 0.5) * 2.0;
	float S10 = (Slices.SampleLevel(SlicesSampler, float3(Uv, J0 * ThetaCount + I1), 0).r - 0.5) * 2.0;
	float S01 = (Slices.SampleLevel(SlicesSampler, float3(Uv, J1 * ThetaCount + I0), 0).r - 0.5) * 2.0;
	float S11 = (Slices.SampleLevel(SlicesSampler, float3(Uv, J1 * ThetaCount + I1), 0).r - 0.5) * 2.0;
	return lerp(lerp(S00, S10, TTheta), lerp(S01, S11, TTheta), TPhi);
}

// ------------------------------------------------------------------------------------------------------------
// Unreal Custom node body (copy the lines between the markers into the Code field).
// Output type: CMOT Float 1. Inputs (names matter):
//   Atlas      Texture Object (the atlas; UE adds AtlasSampler automatically)
//   FaceUV     float2   face UV channel
//   L          float3   light direction, surface -> light (world space)
//   Forward    float3   head forward (world space)
//   Up         float3   head up (world space)
//   Grid       float4   (ThetaCount, PhiCount, PhiMin, PhiMax)
//   Layout     float4   (CellW, CellH, Padding, Mirror 0/1)
// A Custom node cannot declare free functions, so the helpers live in a local struct.
// The block is excluded from compilation here (#if 0) so this file can also be #included as is.
// ------------------------------------------------------------------------------------------------------------
#if 0
// ---8<--- CUSTOM NODE BODY BEGIN
struct FMoonSdf360
{
	float Eval(Texture2D Tex, SamplerState Smp, float2 FaceUv, float3 Light, float3 Fwd, float3 HeadUp, float4 G, float4 Lay)
	{
		float Nt = G.x;
		float Np = G.y;
		float2 Cell = Lay.xy;
		float Pad = Lay.z;
		float3 U = normalize(HeadUp);
		float3 F = normalize(Fwd - U * dot(Fwd, U));
		float3 R = cross(U, F);
		float3 Ln = normalize(Light);
		float X = dot(Ln, F);
		float Y = dot(Ln, R);
		float Theta = (X * X + Y * Y) < 1e-12 ? 0.0 : degrees(atan2(Y, X));
		float Phi = degrees(asin(clamp(dot(Ln, U), -1.0, 1.0)));
		float I0, I1, J0, J1, Tt, Tp, Flip;
		if (Lay.w > 0.5)
		{
			Flip = Theta < 0.0 ? 1.0 : 0.0;
			float Step = 180.0 / max(Nt - 1.0, 1.0);
			float Uu = clamp(abs(Theta) / Step, 0.0, Nt - 1.0);
			I0 = min(floor(Uu), max(Nt - 2.0, 0.0));
			Tt = Uu - I0;
			I1 = min(I0 + 1.0, Nt - 1.0);
		}
		else
		{
			Flip = 0.0;
			float Step = 360.0 / Nt;
			float Uu = (Theta + 180.0) / Step;
			float F0 = floor(Uu);
			Tt = Uu - F0;
			I0 = F0 - Nt * floor(F0 / Nt);
			I1 = I0 + 1.0;
			if (I1 >= Nt) { I1 -= Nt; }
		}
		if (Np > 1.5)
		{
			float Step = (G.w - G.z) / (Np - 1.0);
			float V = clamp((Phi - G.z) / Step, 0.0, Np - 1.0);
			J0 = min(floor(V), Np - 2.0);
			Tp = V - J0;
			J1 = J0 + 1.0;
		}
		else
		{
			J0 = 0.0; J1 = 0.0; Tp = 0.0;
		}
		float2 Uv = saturate(FaceUv);
		if (Flip > 0.5) { Uv.x = 1.0 - Uv.x; }
		float2 Stride = Cell + 2.0 * Pad;
		float2 AtlasSize = float2(Nt, Np) * Stride;
		float S00 = (Tex.SampleLevel(Smp, (float2(I0, J0) * Stride + Pad + Uv * Cell) / AtlasSize, 0).r - 0.5) * 2.0;
		float S10 = (Tex.SampleLevel(Smp, (float2(I1, J0) * Stride + Pad + Uv * Cell) / AtlasSize, 0).r - 0.5) * 2.0;
		float S01 = (Tex.SampleLevel(Smp, (float2(I0, J1) * Stride + Pad + Uv * Cell) / AtlasSize, 0).r - 0.5) * 2.0;
		float S11 = (Tex.SampleLevel(Smp, (float2(I1, J1) * Stride + Pad + Uv * Cell) / AtlasSize, 0).r - 0.5) * 2.0;
		return lerp(lerp(S00, S10, Tt), lerp(S01, S11, Tt), Tp);
	}
};
FMoonSdf360 MoonSdf360;
return MoonSdf360.Eval(Atlas, AtlasSampler, FaceUV, L, Forward, Up, Grid, Layout);
// ---8<--- CUSTOM NODE BODY END
#endif
