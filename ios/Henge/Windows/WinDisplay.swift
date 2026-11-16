import SwiftUI
import UIKit

/// The panel, as a Metal layer the engine blits into, plus the two input
/// devices the design promised: the touchscreen pretends to be an absolute
/// tablet, and a hardware keyboard passes its HID usages straight through.
final class WinDisplayUIView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }
    var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    private enum Mode { case none, leftDrag, scroll }
    private var mode: Mode = .none
    private var lastScrollY: CGFloat = 0

    override var canBecomeFirstResponder: Bool { true }

    /// Normalized point in the guest's framebuffer, which is what the tablet
    /// reports: (0,0) top-left, (1,1) bottom-right.
    private func normalized(_ p: CGPoint) -> CGPoint {
        CGPoint(x: max(0, min(1, p.x / bounds.width)), y: max(0, min(1, p.y / bounds.height)))
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        becomeFirstResponder()
        if (event?.allTouches?.count ?? 1) >= 2 {
            mode = .scroll
            lastScrollY = touches.first?.location(in: self).y ?? 0
            return
        }
        mode = .leftDrag
        if let t = touches.first {
            WinEngine.shared.pointer(normalized(t.location(in: self)), buttons: 1)
        }
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let t = touches.first else { return }
        switch mode {
        case .leftDrag:
            WinEngine.shared.pointer(normalized(t.location(in: self)), buttons: 1)
        case .scroll:
            let y = t.location(in: self).y
            let dy = y - lastScrollY
            lastScrollY = y
            if abs(dy) >= 8 {
                // Finger down = wheel up: the sign the HID wheel expects.
                WinEngine.shared.pointer(normalized(t.location(in: self)), buttons: 0, wheel: Int8(dy > 0 ? 1 : -1))
            }
        case .none:
            break
        }
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        if mode == .leftDrag, let t = touches.first {
            WinEngine.shared.pointer(normalized(t.location(in: self)), buttons: 0)
        }
        if (event?.allTouches?.count ?? 0) <= 1 { mode = .none }
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        WinEngine.shared.pointer(CGPoint(x: 0.5, y: 0.5), buttons: 0)
        mode = .none
    }

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        for p in presses {
            if let key = p.key { WinEngine.shared.key(hid: UInt8(key.keyCode.rawValue), down: true) }
        }
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        for p in presses {
            if let key = p.key { WinEngine.shared.key(hid: UInt8(key.keyCode.rawValue), down: false) }
        }
    }

    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        pressesEnded(presses, with: event)
    }
}

/// SwiftUI wrapper. It reports its layer once, which is all the engine needs:
/// `fb_metal` keeps a reference and stops when the layer goes away.
struct WinDisplayView: UIViewRepresentable {
    func makeUIView(context: Context) -> WinDisplayUIView {
        let v = WinDisplayUIView()
        v.backgroundColor = .black
        v.metalLayer.framebufferOnly = true
        v.metalLayer.isOpaque = true
        WinEngine.shared.attach(v.metalLayer)
        return v
    }

    func updateUIView(_ uiView: WinDisplayUIView, context: Context) {}
}
