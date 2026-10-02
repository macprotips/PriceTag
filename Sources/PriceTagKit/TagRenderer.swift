import CoreGraphics
import CoreText
import Foundation
import ImageIO

/// Draws a price as a transparent image: drop shadow, 3D depth, outline,
/// gradient fill and a little shine, all scaled to the text height.
public enum TagRenderer {
    public static func render(_ text: String, color: TagColor, style: TagStyle,
                              textHeight: CGFloat? = nil) -> CGImage? {
        let cap = max(8, textHeight ?? CGFloat(style.textHeight))
        let palette = color.palette

        var glyphs = glyphPath(for: text, capHeight: cap, spacing: CGFloat(style.spacing))
        if style.slant {
            var shear = CGAffineTransform(a: 1, b: 0, c: tan(10 * .pi / 180), d: 1, tx: 0, ty: 0)
            glyphs = glyphs.copy(using: &shear) ?? glyphs
        }
        let bounds = glyphs.boundingBoxOfPath
        guard !bounds.isNull, !bounds.isEmpty else { return nil }

        // Effect sizes, in pixels.
        let outline = CGFloat(clamp(style.outline)) * 0.15 * cap
        let depth = CGFloat(clamp(style.depth)) * 0.13 * cap
        let shadowBlur = CGFloat(clamp(style.shadow)) * 0.10 * cap
        let shadowDrop = CGFloat(clamp(style.shadow)) * 0.05 * cap
        let padding = 0.04 * cap

        let side = outline + shadowBlur + padding
        let top = outline + shadowBlur + padding
        let bottom = outline + depth + shadowDrop + shadowBlur + padding
        let width = Int(ceil(bounds.width + 2 * side))
        let height = Int(ceil(bounds.height + top + bottom))

        guard let space = CGColorSpace(name: CGColorSpace.sRGB),
              let ctx = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8,
                                  bytesPerRow: 0, space: space,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }

        ctx.interpolationQuality = .high
        ctx.setShouldAntialias(true)
        ctx.translateBy(x: side - bounds.minX, y: bottom - bounds.minY)

        let rim = outline > 0
            ? glyphs.copy(strokingWithWidth: outline * 2, lineCap: .round, lineJoin: .round, miterLimit: 10)
            : nil

        func fillSilhouette(_ color: CGColor) {
            ctx.setFillColor(color)
            ctx.addPath(glyphs)
            ctx.fillPath()
            if let rim {
                ctx.addPath(rim)
                ctx.fillPath()
            }
        }

        // Shadow falls under the whole tag, so the layers are drawn as one group.
        if style.shadow > 0 {
            ctx.setShadow(offset: CGSize(width: 0, height: -shadowDrop), blur: shadowBlur,
                          color: CGColor(srgbRed: 0, green: 0, blue: 0, alpha: 0.35 + 0.35 * clamp(style.shadow)))
        }
        ctx.beginTransparencyLayer(auxiliaryInfo: nil)

        // 3D depth: the silhouette stacked downwards, darkest at the back.
        if depth >= 1 {
            let steps = Int(depth.rounded(.up))
            for i in stride(from: steps, through: 1, by: -1) {
                ctx.saveGState()
                ctx.translateBy(x: 0, y: -CGFloat(i) * depth / CGFloat(steps))
                fillSilhouette(cgColor(palette.depth))
                ctx.restoreGState()
            }
        }

        // Outline.
        if rim != nil {
            fillSilhouette(cgColor(palette.outline))
        }

        // Gradient fill.
        ctx.saveGState()
        ctx.addPath(glyphs)
        ctx.clip()
        if let gradient = CGGradient(colorsSpace: space,
                                     colors: [cgColor(palette.top), cgColor(palette.bottom)] as CFArray,
                                     locations: [0, 1]) {
            ctx.drawLinearGradient(gradient, start: CGPoint(x: 0, y: bounds.maxY),
                                   end: CGPoint(x: 0, y: bounds.minY), options: [])
        }

        if style.shine {
            // A bright lip along the top edges and a soft shade along the bottom
            // edges, so the numbers read as rounded instead of flat.
            let lip = 0.04 * cap
            bevel(ctx, glyphs, shift: -lip, color: CGColor(srgbRed: 1, green: 1, blue: 1, alpha: 0.55))
            bevel(ctx, glyphs, shift: lip, color: CGColor(srgbRed: 0, green: 0, blue: 0, alpha: 0.18))
            // Glossy top half.
            ctx.setFillColor(CGColor(srgbRed: 1, green: 1, blue: 1, alpha: 0.14))
            ctx.fill(CGRect(x: bounds.minX - cap, y: bounds.midY + 0.06 * cap,
                            width: bounds.width + 2 * cap, height: bounds.height))
        }
        ctx.restoreGState()

        ctx.endTransparencyLayer()
        return ctx.makeImage()
    }

    /// The outlines of `text` in Mint Display, with the digits `capHeight` pixels tall.
    public static func glyphPath(for text: String, capHeight: CGFloat, spacing: CGFloat = 0) -> CGPath {
        let font = MintDisplay.font(capHeight: capHeight)
        let attributes: [NSAttributedString.Key: Any] = [
            NSAttributedString.Key(kCTFontAttributeName as String): font,
            NSAttributedString.Key(kCTKernAttributeName as String): spacing * capHeight,
        ]
        let line = CTLineCreateWithAttributedString(NSAttributedString(string: text, attributes: attributes))
        let path = CGMutablePath()
        let runs = CTLineGetGlyphRuns(line) as? [CTRun] ?? []
        for run in runs {
            let count = CTRunGetGlyphCount(run)
            guard count > 0 else { continue }
            var glyphs = [CGGlyph](repeating: 0, count: count)
            var positions = [CGPoint](repeating: .zero, count: count)
            CTRunGetGlyphs(run, CFRange(location: 0, length: 0), &glyphs)
            CTRunGetPositions(run, CFRange(location: 0, length: 0), &positions)

            var runFont = font
            let runAttributes = CTRunGetAttributes(run) as NSDictionary
            if let value = runAttributes[kCTFontAttributeName as String],
               CFGetTypeID(value as CFTypeRef) == CTFontGetTypeID() {
                runFont = value as! CTFont
            }
            for i in 0..<count {
                var transform = CGAffineTransform(translationX: positions[i].x, y: positions[i].y)
                if let outline = CTFontCreatePathForGlyph(runFont, glyphs[i], &transform) {
                    path.addPath(outline)
                }
            }
        }
        return path
    }

    /// PNG bytes for an image, keeping its transparency.
    public static func pngData(_ image: CGImage) -> Data? {
        let data = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(data as CFMutableData,
                                                                 "public.png" as CFString, 1, nil)
        else { return nil }
        CGImageDestinationAddImage(destination, image, nil)
        guard CGImageDestinationFinalize(destination) else { return nil }
        return data as Data
    }

    /// Paints the part of the glyphs not covered by the glyphs moved by `shift`.
    /// Must be called with the glyphs already set as the clip.
    private static func bevel(_ ctx: CGContext, _ glyphs: CGPath, shift: CGFloat, color: CGColor) {
        var move = CGAffineTransform(translationX: 0, y: shift)
        guard let moved = glyphs.copy(using: &move) else { return }
        ctx.saveGState()
        ctx.addPath(glyphs)
        ctx.addPath(moved)
        ctx.setFillColor(color)
        ctx.fillPath(using: .evenOdd)
        ctx.restoreGState()
    }

    private static func cgColor(_ hex: UInt32) -> CGColor {
        CGColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255,
                green: CGFloat((hex >> 8) & 0xFF) / 255,
                blue: CGFloat(hex & 0xFF) / 255, alpha: 1)
    }

    private static func clamp(_ value: Double) -> Double { min(1, max(0, value)) }
}
