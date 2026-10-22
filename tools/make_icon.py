# Builds icon.jpg (256x256 nro icon) and romfs/logo.png (wordmark for dark UI)
# from tools/tiktok-logo.png (Wikipedia's TikTok_logo.svg, rendered at 960px).
# The source draws the note and text in black; on the dark app they go white,
# like TikTok's own app icon: each pixel gets (255 - max channel) added, so black
# turns white and the cyan/red fringes keep their hue.
from pathlib import Path

from PIL import Image

root = Path(__file__).resolve().parent.parent
src = Image.open(root / "tools" / "tiktok-logo.png").convert("RGBA")


def whiten(im):
    px = [(r + 255 - max(r, g, b), g + 255 - max(r, g, b), b + 255 - max(r, g, b), a) for r, g, b, a in im.getdata()]
    out = Image.new("RGBA", im.size)
    out.putdata(px)
    return out


white = whiten(src)
white.crop(white.getbbox()).save(root / "romfs" / "logo.png")

# The note is the part left of the first wide transparent gap.
alpha = src.getchannel("A")
w, h = src.size
cols = [any(alpha.getpixel((x, y)) > 8 for y in range(h)) for x in range(w)]
x = 0
while x < w and not cols[x]:
    x += 1
while x < w and cols[x]:
    x += 1
note = white.crop((0, 0, x, h))
note = note.crop(note.getbbox())

icon = Image.new("RGB", (256, 256), (0, 0, 0))
scale = 170 / max(note.size)
note = note.resize((round(note.width * scale), round(note.height * scale)), Image.LANCZOS)
icon.paste(note, ((256 - note.width) // 2, (256 - note.height) // 2), note)
icon.save(root / "icon.jpg", quality=95)
print("icon.jpg + romfs/logo.png written")
