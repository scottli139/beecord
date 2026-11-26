import XCTest

/// 消除系统蓝牙授权弹窗（"…would like to use Bluetooth."）。
/// macOS 无 addUIInterruptionMonitor（iOS 专属），且 Xcode 默认中断处理只匹配
/// would like to access/find，认不出 use Bluetooth：全新 runner 切 BLE 传输层
/// 触发弹窗后无人消除，滞留盖住后续用例控件致 Not hittable（CI run 45628371516）。
/// 弹窗宿主为 UserNotificationCenter（失败 run 日志实录），直接查询点按 Allow。
@MainActor
func dismissSystemBluetoothAlert(timeout: TimeInterval = 0) {
    let center = XCUIApplication(bundleIdentifier: "com.apple.UserNotificationCenter")
    let deadline = Date().addingTimeInterval(timeout)
    while true {
        for title in ["Allow", "允许"] {
            let button = center.buttons[title]
            if button.exists {
                button.click()
                return
            }
        }
        if Date() >= deadline { return }
        Thread.sleep(forTimeInterval: 0.3)
    }
}