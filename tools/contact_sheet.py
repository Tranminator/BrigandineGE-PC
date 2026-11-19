"""Debug helper: tile BMP snapshots into one labelled PNG.  usage: contact_sheet.py out.png cols file1 file2 ..."""
import sys
from PIL import Image, ImageDraw
out, cols, files = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
ims = [Image.open(f).convert("RGB").resize((320, 240)) for f in files]
rows = (len(ims) + cols - 1) // cols
sheet = Image.new("RGB", (cols * 320, rows * 252), "black")
d = ImageDraw.Draw(sheet)
for i, (im, f) in enumerate(zip(ims, files)):
    x, y = (i % cols) * 320, (i // cols) * 252
    sheet.paste(im, (x, y + 12))
    d.text((x + 4, y), f.split("/")[-1], fill="yellow")
sheet.save(out)
