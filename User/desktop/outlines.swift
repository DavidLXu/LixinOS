// Export quadratic/cubic outlines and advances in 1/1024 CSS em units.
// Each page starts with 256 little-endian offsets, zero for missing characters.
import CoreText
import Foundation

let dest = URL(fileURLWithPath: CommandLine.arguments[1])
try FileManager.default.createDirectory(at: dest, withIntermediateDirectories: true)
let faces = ["PingFangSC-Regular", "PingFangSC-Regular", "PingFangSC-Semibold", "PingFangSC-Semibold",
             "Menlo-Regular", "Menlo-Italic", "Menlo-Bold", "Menlo-BoldItalic", "PingFangSC-Medium", "AppleSDGothicNeo-Regular"]
for (style, face) in faces.enumerated() {
    var slant = CGAffineTransform(a: 1, b: 0, c: 0.18, d: 1, tx: 0, ty: 0)
    let size: CGFloat = style >= 8 ? 1024 : style >= 4 ? 832 : 896
    let font = (style == 1 || style == 3) ? CTFontCreateWithName(face as CFString, size, &slant)
                                         : CTFontCreateWithName(face as CFString, size, nil)
    precondition(CTFontCopyPostScriptName(font) as String == face)
    for page in (style == 9 ? Array(0xac...0xd7) : style == 8 ? Array(0x30...0x9f) + [0xf9,0xfa,0xff] : [0,1,2,0x20,0x21]) {
        var offsets = [UInt32](repeating: 0, count: 256)
        var records = Data()
        for index in 0..<256 {
            var ch = UInt16(page*256 + index), glyph = CGGlyph(), advance = CGSize.zero
            guard ch >= 32, CTFontGetGlyphsForCharacters(font, &ch, &glyph, 1) else { continue }
            CTFontGetAdvancesForGlyphs(font, .horizontal, &glyph, &advance, 1)
            guard advance.width > 0 else { continue }
            var vertices = [Int16]()
            if let path = CTFontCreatePathForGlyph(font, glyph, nil) {
                path.applyWithBlock { element in
                    let e = element.pointee
                    // stb vertices store endpoint, then control points. Contours close implicitly.
                    let order: [Int]
                    switch e.type {
                    case .moveToPoint, .addLineToPoint: order = [0]
                    case .addQuadCurveToPoint: order = [1,0]
                    case .addCurveToPoint: order = [2,0,1]
                    default: return
                    }
                    vertices.append(Int16(e.type.rawValue + 1))
                    for i in order {
                        vertices.append(Int16(clamping: Int(e.points[i].x.rounded())))
                        vertices.append(Int16(clamping: Int(e.points[i].y.rounded())))
                    }
                }
            }
            offsets[index] = UInt32(1024 + records.count)
            let words = [Int16(style >= 8 ? 1024 : style >= 4 ? 512 : Int(advance.width.rounded())), Int16(vertices.count)] + vertices
            words.withUnsafeBytes { records.append(contentsOf: $0) }
        }
        var data = offsets.withUnsafeBytes { Data($0) }
        data.append(records)
        try data.write(to: dest.appendingPathComponent(String(format: "%d-%02x.out",style,page)), options: .atomic)
    }
}
