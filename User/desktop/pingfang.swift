// Build only the CJK ranges used by the guest. Each page has a 256-bit
// coverage map followed by 16px and 32px four-bit alpha masks for each glyph.
import CoreGraphics
import CoreText
import Foundation

let destination = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: true)
let face = "PingFangSC-Medium"
let fonts = [16, 32].map { CTFontCreateWithName(face as CFString, CGFloat($0), nil) }
precondition(CTFontCopyPostScriptName(fonts[0]) as String == face, "PingFang SC Medium is required")
let contexts = [16, 32].map { size in
    CGContext(data: nil, width: size, height: size, bitsPerComponent: 8,
              bytesPerRow: size, space: CGColorSpaceCreateDeviceGray(), bitmapInfo: 0)!
}
var count = 0
for page in Array(0x30...0x9f) + [0xf9, 0xfa, 0xff] {
    var data = Data(repeating: 0, count: 32 + 256 * 640)
    data.withUnsafeMutableBytes { (buffer: UnsafeMutableRawBufferPointer) in
        for index in 0..<256 {
            var ch = UInt16(page * 256 + index), glyph = CGGlyph()
            guard CTFontGetGlyphsForCharacters(fonts[0], &ch, &glyph, 1) else { continue }
            buffer[index / 8] |= UInt8(0x80 >> (index % 8))
            count += 1
            var offset = 32 + index * 640
            for (font, ctx) in zip(fonts, contexts) {
                let size = ctx.width
                let pixels = ctx.data!.assumingMemoryBound(to: UInt8.self)
                memset(pixels, 0, size * size)
                ctx.setFillColor(gray: 1, alpha: 1)
                var origin = CGPoint(x: 0, y: CGFloat(size / 8))
                CTFontDrawGlyphs(font, &glyph, &origin, 1, ctx)
                for i in stride(from: 0, to: size * size, by: 2) {
                    buffer[offset] = (pixels[i] & 0xf0) | (pixels[i + 1] >> 4)
                    offset += 1
                }
            }
        }
    }
    try data.write(to: destination.appendingPathComponent(String(format: "%02x.gray", page)), options: .atomic)
}
print("PingFang SC Medium: generated masks for \(count) code points")
