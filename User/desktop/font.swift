// Produce a packed grayscale atlas with a 9-pixel Latin and 18-pixel Han advance.
import CoreGraphics
import CoreText
import Foundation

let latin = CTFontCreateWithName("Menlo-Regular" as CFString, 15, nil)
let han = CTFontCreateWithName("PingFangSC-Medium" as CFString, 16, nil)
let extra = "文件夹上一级名称类型大小空目录页字节本编辑器终端"
var atlas = Data()
for code in Array(UInt16(32)...126) + Array(extra.utf16) {
    var ch = code, glyph = CGGlyph()
    let font = code < 127 ? latin : han
    precondition(CTFontGetGlyphsForCharacters(font, &ch, &glyph, 1))
    var pixels = [UInt8](repeating: 0, count: 18 * 22)
    pixels.withUnsafeMutableBytes { data in
        let ctx = CGContext(data: data.baseAddress, width: 18, height: 22,
            bitsPerComponent: 8, bytesPerRow: 18, space: CGColorSpaceCreateDeviceGray(), bitmapInfo: 0)!
        ctx.setFillColor(gray: 1, alpha: 1)
        var origin = CGPoint(x: 0, y: 5)
        CTFontDrawGlyphs(font, &glyph, &origin, 1, ctx)
    }
    atlas.append(contentsOf: pixels)
}
try atlas.write(to: URL(fileURLWithPath: CommandLine.arguments[1]), options: .atomic)
