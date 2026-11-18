import SwiftUI

extension RootView {
    /// The old macOS screens still name their destination through this enum.
    /// The Windows-only RootView never switches on it; it exists so those views
    /// keep compiling while they are phased out.
    enum Tab: Hashable {
        case machines
        case install
        case session
    }
}
