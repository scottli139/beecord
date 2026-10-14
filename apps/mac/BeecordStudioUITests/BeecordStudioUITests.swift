import XCTest

/// UI 冒烟测试（TESTING.md §4：AX 树驱动，禁坐标/截图）。
final class BeecordStudioUITests: XCTestCase {

    override func setUpWithError() throws {
        continueAfterFailure = false
    }

    @MainActor
    func testAppLaunchesShowsPlaceholder() throws {
        let app = XCUIApplication()
        app.launch()
        XCTAssertTrue(app.staticTexts["Beecord Studio"].waitForExistence(timeout: 10))
    }
}
