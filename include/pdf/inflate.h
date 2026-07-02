#ifndef PDF_INFLATE_H
#define PDF_INFLATE_H
#include <stddef.h>
/* Raw DEFLATE (RFC 1951), no zlib header. dest/destlen in/out (cap/used). 0=ok, <0=error. */
int  puff(unsigned char *dest, unsigned long *destlen,
          const unsigned char *source, unsigned long *sourcelen);
/* FlateDecode: skip the 2-byte zlib header (0x78 ..), inflate, ignore trailing adler32.
   Returns bytes written into out, or -1 on malformed input / overflow. */
long pdf_flate_decode(const unsigned char *in, size_t in_len,
                      unsigned char *out, size_t out_cap);
#endif
