"""Builds the port's icon files from packaging/icon/infamous-icon-1024.png (original art made for
this project; the official game art is read from the player's dump at run time). Needs Pillow.
usage: make_icon.py launcher/infamous.ico launcher/infamous.png"""
from pathlib import Path
import sys
from PIL import Image

SOURCE = Path(__file__).resolve().parent.parent / 'icon' / 'infamous-icon-1024.png'


def main():
    ico, png = sys.argv[1], sys.argv[2]
    image = Image.open(SOURCE).convert('RGBA')
    image.resize((256, 256), Image.LANCZOS).save(png, optimize=True)
    image.save(ico, sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])


if __name__ == '__main__':
    main()
