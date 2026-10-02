import CoreGraphics
import CoreText
import XCTest
@testable import PriceTagKit

final class TagRendererTests: XCTestCase {
    func testEmbeddedFontLoads() {
        XCTAssertTrue(MintDisplay.isAvailable)
        let font = MintDisplay.font(capHeight: 100)
        XCTAssertEqual(CTFontCopyPostScriptName(font) as String, MintDisplay.postScriptName)
    }

    func testEveryCharacterHasAGlyph() {
        for ch in "0123456789$.,-+" {
            let path = TagRenderer.glyphPath(for: String(ch), capHeight: 100)
            XCTAssertFalse(path.boundingBoxOfPath.isEmpty, "no glyph for \(ch)")
        }
    }

    func testDigitsAreTheRequestedHeight() {
        let box = TagRenderer.glyphPath(for: "0", capHeight: 200).boundingBoxOfPath
        XCTAssertEqual(box.height, 200, accuracy: 2)
    }

    func testRendersTransparentPNG() throws {
        var style = TagStyle()
        style.textHeight = 120
        let image = try XCTUnwrap(TagRenderer.render("$12.50", color: .green, style: style))
        XCTAssertGreaterThan(image.width, image.height)
        XCTAssertEqual(image.alphaInfo, .premultipliedLast)

        let png = try XCTUnwrap(TagRenderer.pngData(image))
        XCTAssertEqual(Array(png.prefix(4)), [0x89, 0x50, 0x4E, 0x47])

        // The corner pixel is fully transparent.
        let data = try XCTUnwrap(image.dataProvider?.data as Data?)
        XCTAssertEqual(data[3], 0)
    }

    func testEveryLookRenders() {
        var style = TagStyle()
        style.textHeight = 80
        for color in TagColor.allCases {
            for slant in [false, true] {
                style.slant = slant
                style.outline = slant ? 0 : 1
                style.depth = slant ? 0 : 1
                style.shadow = slant ? 0 : 1
                XCTAssertNotNil(TagRenderer.render("-$1,234.56", color: color, style: style))
            }
        }
    }
}
