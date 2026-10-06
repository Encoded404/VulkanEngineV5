# Test fonts

Fixtures for the device-free text tests. The engine itself ships no font: an
application supplies its own, so this directory exists only to give the tests a
real font file.

## Lato-Regular.ttf

- Source: <https://github.com/google/fonts/blob/main/ofl/lato/Lato-Regular.ttf>
  (fetched verbatim; do not edit it)
- License: SIL Open Font License 1.1, see `OFL.txt`
- Copyright (c) 2010-2014 by tyPoland Lukasz Dziedzic, with Reserved Font Name
  "Lato"

The file is redistributed **unmodified**, which is what the reserved font name
in the OFL permits. If it is ever subsetted, converted, or otherwise altered,
that OFL clause applies: the modified copy must be renamed and must not claim
the reserved name.

### Why this font

The shaping tests need real OpenType layout data, not a synthetic font: kerning
comes from `GPOS` and the `fi`/`fl` ligatures from `GSUB`, and hand-authoring
those tables would be both larger and less convincing than using a font that
ships them. Lato is a static (non-variable) TrueType-flavoured font, so its
metrics are fixed and the tests do not need to pin variation axes; it is still
published as a plain `.ttf` rather than only as a variable font, which most of
the Google Fonts library no longer is.

### Refreshing

Re-download both files from the URLs above. The tests assert metrics and shaped
output rather than whole-file hashes, so a version bump only needs the
expectations updated if Lato's own metrics change; a `git diff --stat` on the
binary is enough to see that a genuine replacement happened.
