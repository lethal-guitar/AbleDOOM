# AbleDOOM - Doom on Ableton Push 3 Standalone!
#
# Copyright (C) 2024 Nikolai Wuttke-Hohendorf
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

import struct
import sys

from PIL import Image


def encode(r, g, b):
    return ((b & 0xF8) << 8) + ((g & 0xFC) << 3) + (r >> 3)


if __name__ == "__main__":
    filename = sys.argv[1]
    image = Image.open(filename).convert("RGB")

    encoded = []

    for y in range(160):
        for x in range(1024):
            if x < 960:
                r, g, b = image.getpixel((x, y))
                encoded.append(encode(r, g, b))
            else:
                encoded.append(0)

    encoded_string = ",".join("0x%02X" % value for value in encoded)

    print("const uint16_t CONTROLS_IMAGE[] = {")
    print(encoded_string)
    print("};")
