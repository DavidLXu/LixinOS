#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include "aconf.h"
#include "GlobalParams.h"
#include "PDFDoc.h"
#include "ErrorCodes.h"
#include "SplashOutputDev.h"
#include "SplashBitmap.h"
#include "pdf.h"

struct PdfFile {
    explicit PdfFile(const char *path) : doc(new GString(path)) {}
    PDFDoc doc;
};

PdfFile *pdf_open(const char *path, const char **error) {
    if (!globalParams) globalParams = new GlobalParams("/lib/pdfview/xpdfrc");
    std::unique_ptr<PdfFile> file(new PdfFile(path));
    if (!file->doc.isOk() || file->doc.getNumPages() < 1) {
        *error = file->doc.getErrorCode() == errEncrypted ? "此 PDF 需要密码" : "无法读取 PDF，请检查文件或路径";
        return nullptr;
    }
    return file.release();
}

void pdf_close(PdfFile *doc) { delete doc; }
int pdf_pages(PdfFile *doc) { return doc->doc.getNumPages(); }
double pdf_width(PdfFile *doc, int page) {
    return doc->doc.getPageRotate(page) % 180 ? doc->doc.getPageCropHeight(page) : doc->doc.getPageCropWidth(page);
}
unsigned char *pdf_render_rgb(PdfFile *file, int page, double scale, int *width, int *height) {
    if (!file || page < 1 || page > pdf_pages(file)) return nullptr;
    double w = file->doc.getPageCropWidth(page) * scale, h = file->doc.getPageCropHeight(page) * scale;
    if (!std::isfinite(w) || !std::isfinite(h) || w < 1 || h < 1 || w > 16384 || h > 16384 || w*h > 16000000) return nullptr;
    SplashColor paper = {255, 255, 255};
    SplashOutputDev out(splashModeRGB8, 1, gFalse, paper);
    out.startDoc(file->doc.getXRef());
    file->doc.displayPage(&out, page, scale * 72, scale * 72, 0, gFalse, gTrue, gFalse);
    SplashBitmap *bitmap = out.getBitmap();
    *width = bitmap->getWidth();
    *height = bitmap->getHeight();
    size_t stride = static_cast<size_t>(*width) * 3;
    auto *pix = static_cast<unsigned char *>(std::malloc(stride * *height));
    if (!pix) return nullptr;
    for (int y = 0; y < bitmap->getHeight(); ++y)
        std::memcpy(pix + y * stride,
                    bitmap->getDataPtr() + y*bitmap->getRowSize(), bitmap->getWidth()*3);
    return pix;
}
