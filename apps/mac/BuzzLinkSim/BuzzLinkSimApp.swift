import SwiftUI

@main
struct BuzzLinkSimApp: App {
    @State private var model = SimDeviceModel()

    var body: some Scene {
        WindowGroup {
            SimContentView(model: model)
                .onAppear { model.start() }
        }
    }
}
