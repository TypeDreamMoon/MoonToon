"""Re-encode an existing classic face SDF into the precise delivery formats (MoonToon T24 / T28).

  uv run --with numpy --with pillow python encode_face_sdf.py <in> [--channel r] [--from-srgb] [--dequantize 1.0]
         [--out16 face_sdf16.png] [--out-rg face_sdf_rg.png]

--from-srgb   the source was imported sRGB ON and the look was tuned that way (ww_yy's maps): bake the sRGB
              decode into the values, so the linear 16-bit / R+G texture shades like the old one.
--dequantize  Gaussian sigma in texels. An 8-bit source has at most 256 plateaus; writing it to 16 bits
              keeps the terraces. A small blur (0.75-1.5) recovers sub-level slopes between them.
              Leave it 0 for a map that is already smooth (a 16-bit bake).
Prints how many distinct levels the input and the output really have."""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import face_sdf_common as F  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("--channel", default="r", choices=["r", "g", "b", "a", "gray"])
    ap.add_argument("--from-srgb", action="store_true")
    ap.add_argument("--dequantize", type=float, default=0.0)
    ap.add_argument("--out16")
    ap.add_argument("--out-rg")
    args = ap.parse_args()
    if not args.out16 and not args.out_rg:
        sys.exit("give --out16 and / or --out-rg")

    v = F.load_channel(args.input, args.channel)
    print(F.levels_report(v, "input"))
    v = F.gaussian_blur(v, args.dequantize)
    if args.from_srgb:
        v = F.srgb_to_linear(v)
    v = np.clip(v, 0.0, 1.0)
    if args.out16:
        F.save16(args.out16, v)
        print(F.levels_report(np.round(v * 65535.0) / 65535.0, "16-bit  -> " + args.out16))
    if args.out_rg:
        rgba = F.encode_rg(v)
        F.save_rgba(args.out_rg, rgba)
        back = F.decode_rg(rgba)
        print(F.levels_report(back, "R+G     -> " + args.out_rg) + ", max |error| %.5f (1/1020 = %.5f)" % (
            np.abs(back - v).max(), 1.0 / 1020.0))


if __name__ == "__main__":
    main()
