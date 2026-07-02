#ifndef PDF_FONT_DECODE_H
#define PDF_FONT_DECODE_H
#include <stddef.h>

/* Recover garbled text from PDFs whose fonts remap character codes via /Encoding
   /Differences (the book has no /ToUnicode). Parse the Differences arrays, map
   glyph names -> text, and decode each content stream with whichever candidate
   map (or raw WinAnsi) scores highest on English-likeness. */

typedef struct { unsigned char set[256]; char text[256][8]; } DiffMap;
typedef struct { DiffMap maps[16]; int n; } FontDiffs;

/* Map a PDF glyph name to ASCII text ("f_i"->"fi", "space"->" ", "A"->"A",
   "uniXXXX"->char; unknown -> ""). */
void   glyph_to_text(const char *name, char *out, size_t cap);
/* Parse every /Differences array in the raw PDF into a code->text map (<=16). */
void   font_diffs_parse(const unsigned char *pdf, size_t n, FontDiffs *fd);
/* Decode raw code bytes through `map` (NULL = WinAnsi: keep printable ASCII,
   drop the rest). Returns bytes written; out is nul-terminated. */
size_t decode_codes(const unsigned char *raw, size_t n, const DiffMap *map, char *out, size_t cap);
/* Fraction of alpha tokens that look like words (len>=2 with a vowel). */
double english_likeness(const char *s);
/* Decode raw codes with the candidate (WinAnsi or a Differences map) that
   maximizes english_likeness; writes the winner to out. Returns its length. */
size_t font_decode_pick_best(const unsigned char *raw, size_t rawlen, const FontDiffs *fd, char *out, size_t cap);
#endif
