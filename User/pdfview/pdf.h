#pragma once

#ifdef __cplusplus
extern "C" {
#endif
typedef struct PdfFile PdfFile;
PdfFile *pdf_open(const char *path, const char **error);
void pdf_close(PdfFile *doc);
int pdf_pages(PdfFile *doc);
double pdf_width(PdfFile *doc, int page);
unsigned char *pdf_render_rgb(PdfFile *doc, int page, double scale, int *width, int *height);
#ifdef __cplusplus
}
#endif
