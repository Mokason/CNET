#ifndef PDF_EXTRACT_H
#define PDF_EXTRACT_H
#include <stddef.h>
typedef enum { PDF_OK=0, PDF_ENCRYPTED, PDF_UNSUPPORTED_FONTS, PDF_NO_TEXT, PDF_MALFORMED } PdfStatus;
/* Extract text-born content into out (nul-terminated, truncated to cap). *out_len gets
   the byte count written. Never reads past pdf[n]. */
PdfStatus pdf_extract_text(const unsigned char *pdf, size_t n,
                           char *out, size_t cap, size_t *out_len);
#endif
