import SwiftUI

/// Beecord Studio 入口：主窗口（侧栏三页）+ 标准 Settings 窗口（⌘,）+ 页面快捷键 ⌘1/2/3。
@main
struct BeecordStudioApp: App {
    @State private var appModel = AppModel()
    @State private var settingsModel = SettingsModel()

    var body: some Scene {
        WindowGroup {
            MainWindow(appModel: appModel, settingsModel: settingsModel)
        }
        .commands {
            CommandMenu("视图") {
                Button("设备") {
                    appModel.selectPage(.device)
                }
                .keyboardShortcut("1", modifiers: .command)
                Button("录音库") {
                    appModel.selectPage(.library)
                }
                .keyboardShortcut("2", modifiers: .command)
                Button("控制台") {
                    appModel.selectPage(.console)
                }
                .keyboardShortcut("3", modifiers: .command)
                .disabled(!settingsModel.developerMode)
            }
        }
        Settings {
            SettingsView(settingsModel: settingsModel)
        }
    }
}
