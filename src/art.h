// The guided mode's pictures: small PNGs made by tools/make_art.py from
// art/generated/, embedded by the generated src/art_blob.S and src/art_table.cpp.
#pragma once
#include <stdint.h>

namespace art {

struct Entry {
    const char *name;
    const uint8_t *data;
    uint32_t len;
};
extern const Entry TABLE[];
extern const int COUNT;

// Draws picture `name` in the box; a simple drawn pictogram if it is missing.
void draw(const char *name, int x, int y, int w, int h, uint16_t accent);

}  // namespace art
