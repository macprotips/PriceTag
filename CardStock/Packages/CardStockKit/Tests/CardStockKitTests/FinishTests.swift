@testable import CardStockKit
import XCTest

final class FinishTests: XCTestCase {
    func testNames() {
        XCTAssertEqual(Finish.nonfoil.name, "Normal")
        XCTAssertEqual(Finish.foil.name, "Foil")
        XCTAssertEqual(Finish.etched.name, "Etched Foil")
        XCTAssertEqual(Finish.holofoil.name, "Holo")
        XCTAssertEqual(Finish.reverseHolofoil.name, "Reverse Holo")
        XCTAssertEqual(Finish("1st-edition-holofoil").name, "1st Edition Holo")
        XCTAssertEqual(Finish("1stEditionNormal").name, "1st Edition Normal")
        XCTAssertEqual(Finish("unlimited-holofoil").name, "Unlimited Holo")
    }

    func testReverse() {
        XCTAssertTrue(Finish.reverseHolofoil.isReverse)
        XCTAssertTrue(Finish("reverseHolofoil").isReverse)
        XCTAssertFalse(Finish.holofoil.isReverse)
    }
}
