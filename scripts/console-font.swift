// Build a grayscale ASCII glyph atlas from the Mac's installed Menlo font.
import CoreGraphics
import CoreText
import Foundation

let width = 22, height = 44
let font = CTFontCreateWithName("Menlo-Regular" as CFString, 36, nil)
var atlas = Data()
for ascii in 32...126 {
    var pixels = [UInt8](repeating: 0, count: width * height)
    pixels.withUnsafeMutableBytes { bytes in
        let context = CGContext(data: bytes.baseAddress, width: width, height: height,
                                bitsPerComponent: 8, bytesPerRow: width,
                                space: CGColorSpaceCreateDeviceGray(), bitmapInfo: 0)!
        context.setFillColor(gray: 1, alpha: 1)
        var character = UniChar(ascii), glyph = CGGlyph()
        CTFontGetGlyphsForCharacters(font, &character, &glyph, 1)
        var origin = CGPoint(x: 0, y: 10)
        CTFontDrawGlyphs(font, &glyph, &origin, 1, context)
    }
    atlas.append(contentsOf: pixels)
}
try atlas.write(to: URL(fileURLWithPath: CommandLine.arguments[1]), options: .atomic)
