"""Generate the application icon (run with Pillow installed)."""
from PIL import Image, ImageDraw, ImageFilter

S = 1024
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))

# Blue gradient rounded square.
grad = Image.new("RGBA", (S, S), (0, 0, 0, 0))
gd = ImageDraw.Draw(grad)
for y in range(64, 960):
    t = (y - 64) / 896.0
    r = int(58 + (10 - 58) * t)
    g = int(140 + (75 - 140) * t)
    b = int(255 + (210 - 255) * t)
    gd.line((0, y, S, y), fill=(r, g, b, 255))
mask = Image.new("L", (S, S), 0)
md = ImageDraw.Draw(mask)
md.rounded_rectangle((64, 64, 960, 960), radius=210, fill=255)
img.paste(grad, (0, 0), mask)

# Subtle inner highlight border.
d = ImageDraw.Draw(img)
d.rounded_rectangle((64, 64, 960, 960), radius=210, outline=(160, 210, 255, 150), width=10)

# Eye shape: two quadratic curves.
def quad(p0, p1, p2, steps=80):
    out = []
    for i in range(steps + 1):
        t = i / steps
        x = (1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t ** 2 * p2[0]
        y = (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t ** 2 * p2[1]
        out.append((x, y))
    return out

left = (196, 512)
right = (828, 512)
upper = quad(left, (512, 258), right)
lower = quad(right, (512, 766), left)
eye = upper + lower
d.polygon(eye, fill=(255, 255, 255, 255))

# Iris and pupil.
d.ellipse((392, 392, 632, 632), fill=(18, 92, 190, 255))
d.ellipse((414, 414, 610, 610), fill=(8, 39, 105, 255))
d.ellipse((452, 440, 520, 508), fill=(255, 255, 255, 235))
d.ellipse((556, 536, 582, 562), fill=(255, 255, 255, 180))

# Small toggle badge at lower right: white pill with a blue knob.
d.rounded_rectangle((664, 724, 872, 812), radius=44, fill=(255, 255, 255, 245))
d.ellipse((788, 736, 860, 808), fill=(17, 105, 235, 255))

img.save("app.png")
img.save("app.ico", sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40),
                            (48, 48), (64, 64), (128, 128), (256, 256)])
print("wrote app.ico and app.png")
