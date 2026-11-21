// Grayscale browser glyphs with the same advance used for layout and painting.
// Style bits match the framebuffer frontend: italic, bold, then monospace.
import CoreGraphics
import CoreText
import Foundation

let directory = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
let pages = [0, 1, 2, 0x20, 0x21]
let monoFaces = ["Menlo-Regular", "Menlo-Italic", "Menlo-Bold", "Menlo-BoldItalic"]
for style in 0..<8 {
    let mono = style >= 4
    let face = mono ? monoFaces[style - 4] : (style & 2 != 0 ? "PingFangSC-Semibold" : "PingFangSC-Regular")
    let fonts = [1, 2].map { scale -> CTFont in
        var slant = CGAffineTransform(a: 1, b: 0, c: 0.18, d: 1, tx: 0, ty: 0)
        let size = CGFloat(scale) * (mono ? 13 : 14)
        if !mono && style & 1 != 0 { return CTFontCreateWithName(face as CFString, size, &slant) }
        return CTFontCreateWithName(face as CFString, size, nil)
    }
    precondition(CTFontCopyPostScriptName(fonts[0]) as String == face, "Missing font: \(face)")
    let contexts = [1, 2].map { scale in
        CGContext(data: nil, width: 20*scale, height: 16*scale, bitsPerComponent: 8,
                  bytesPerRow: 20*scale, space: CGColorSpaceCreateDeviceGray(), bitmapInfo: 0)!
    }
    for page in pages {
        // Zero advance marks a missing glyph. Masks follow the two advance tables.
        var data = Data(repeating: 0, count: 512 + 256 * 800)
        data.withUnsafeMutableBytes { (out: UnsafeMutableRawBufferPointer) in
            for index in 0..<256 {
                var ch = UInt16(page*256 + index), glyph = CGGlyph()
                if ch < 32 || (127...159).contains(ch) { continue }
                guard CTFontGetGlyphsForCharacters(fonts[0], &ch, &glyph, 1) else { continue }
                for (i, font) in fonts.enumerated() {
                    var offset = 512 + index*800 + (i == 0 ? 0 : 160)
                    let scale = i+1, ctx = contexts[i]
                    var advance = CGSize.zero
                    CTFontGetAdvancesForGlyphs(font, .horizontal, &glyph, &advance, 1)
                    guard advance.width > 0 else { continue }
                    out[256*i + index] = UInt8(mono ? 8*scale : max(1, Int(advance.width.rounded())))
                    let pixels = ctx.data!.assumingMemoryBound(to: UInt8.self)
                    memset(pixels, 0, ctx.width*ctx.height)
                    ctx.setFillColor(gray: 1, alpha: 1)
                    var origin = CGPoint(x: 2*scale, y: 3*scale)
                    CTFontDrawGlyphs(font, &glyph, &origin, 1, ctx)
                    for n in stride(from: 0, to: ctx.width*ctx.height, by: 2) {
                        out[offset] = (pixels[n] & 0xf0) | (pixels[n+1] >> 4)
                        offset += 1
                    }
                }
            }
        }
        try data.write(to: directory.appendingPathComponent(String(format: "%d-%02x.gray", style, page)), options: .atomic)
    }
}
