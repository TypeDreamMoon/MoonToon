"""Writes the default oil-film specular ramp (T30): T_ToonOilFilmSpecRamp.png, 256 x 1, sRGB.

The toon specular ramp's U runs from the highlight's edge (0) to its centre (1), the same axis as
Arknights: Endfield's spec ramp T4831 (peak-normalised GGX lobe). The colours are T4831's, sampled
from the capture (Toon/Diagrams/Endfield/notes/body.md section 6): its rough row runs red-orange ->
yellow from tail to peak, its smooth row pink/lavender -> cyan. A toon ramp has one row, so the two
become two rings: warm outside, cool inside. U below the first key is black -- outside the highlight
there is no specular, which is what the ramp is also responsible for in a toon specular.

Usage: python make_oil_film_ramp.py [out.png]
"""
import sys
import numpy as np
from PIL import Image

KEYS = [
    (0.00, (0, 0, 0)),
    (0.18, (0, 0, 0)),
    (0.28, (110, 40, 34)),     # the lobe's tail fades in from black, already red
    (0.38, (255, 121, 103)),   # T4831 rough row, tail: red-orange
    (0.50, (255, 217, 136)),   # rough row, peak: yellow
    (0.60, (248, 219, 255)),   # smooth row, tail: lavender / pink
    (0.78, (205, 240, 255)),   # smooth row, peak: cyan
    (1.00, (240, 255, 250)),   # the centre, near white
]


def build(width=256):
    u = (np.arange(width) + 0.5) / width
    xs = [k[0] for k in KEYS]
    rgb = np.stack([np.interp(u, xs, [k[1][c] for k in KEYS]) for c in range(3)], axis=1)
    return np.clip(np.round(rgb), 0, 255).astype(np.uint8)[None, :, :]


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "T_ToonOilFilmSpecRamp.png"
    Image.fromarray(build()).save(out)
    print("wrote", out)
