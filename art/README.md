# The guided mode's pictures

`generated/NAME.png` are the sources (400x400, made with GPT-6.1 Sol's image
generation from one brief: flat pictograms on black, the same generic synth
in each, no text). `tools/make_art.py` makes `NAME.png` from them (100x100,
32 colours, about 2 KB each) and `src/art_blob.S` / `src/art_table.cpp`; the
app draws them with M5GFX's `drawPng`. A missing picture is drawn as a simple
pictogram instead (`src/art.cpp`).

Made for this project, under its MIT licence ([LICENSE](../LICENSE)).
