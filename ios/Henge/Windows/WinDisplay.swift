import SwiftUI
import UIKit

/// The panel, as a Metal layer the engine blits into, plus the input devices the
/// design promised: the touchscreen pretends to be an absolute tablet, a hardware
/// keyboard passes its HID usages through, and a hidden text field brings up the
/// iOS keyboard for the guest when there is no hardware one.
final class WinDisplayUIView: UIView, UITextFieldDelegate {
    override class var layerClass: AnyClass { CAMetalLayer.self }
    var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    /// The one on screen, so the controls can toggle the software keyboard.
    static weak var current: WinDisplayUIView?

    private enum Mode { case none, leftDrag, scroll }
    private var mode: Mode = .none
    private var lastScrollY: CGFloat = 0
    private let keyField = UITextField()

    override var canBecomeFirstResponder: Bool { true }

    override init(frame: CGRect) {
        super.init(frame: frame)
        keyField.delegate = self
        keyField.keyboardType = .asciiCapable
        keyField.autocorrectionType = .no
        keyField.autocapitalizationType = .none
        keyField.spellCheckingType = .no
        keyField.smartQuotesType = .no
        keyField.smartDashesType = .no
        keyField.returnKeyType = .default
        keyField.text = ""
        keyField.alpha = 0.02
        keyField.frame = CGRect(x: 0, y: 0, width: 1, height: 1)
        addSubview(keyField)
        WinDisplayUIView.current = self
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    /// Show or hide the iOS keyboard. The field is invisible; every character it
    /// receives is translated into the USB HID usage the guest expects.
    func toggleKeyboard() {
        if keyField.isFirstResponder {
            keyField.resignFirstResponder()
        } else {
            becomeFirstResponder()
            keyField.becomeFirstResponder()
        }
    }

    private func normalized(_ p: CGPoint) -> CGPoint {
        CGPoint(x: max(0, min(1, p.x / bounds.width)), y: max(0, min(1, p.y / bounds.height)))
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        // Keep the software keyboard up while the finger moves the pointer.
        if !keyField.isFirstResponder { becomeFirstResponder() }
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

    // MARK: - Software keyboard

    func textField(_ textField: UITextField,
                   shouldChangeCharactersIn range: NSRange,
                   replacementString string: String) -> Bool {
        if string.isEmpty { tap(0x2A) } // backspace
        for ch in string { send(ch) }
        return false
    }

    func textFieldShouldReturn(_ textField: UITextField) -> Bool {
        tap(0x28) // Enter
        return false
    }

    private func send(_ ch: Character) {
        guard let a = ch.asciiValue, let hit = usage(for: a) else { return }
        if hit.1 {
            WinEngine.shared.key(hid: 0xE1, down: true) // left Shift
            tap(hit.0)
            WinEngine.shared.key(hid: 0xE1, down: false)
        } else {
            tap(hit.0)
        }
    }

    private func tap(_ usage: UInt8) {
        WinEngine.shared.key(hid: usage, down: true)
        WinEngine.shared.key(hid: usage, down: false)
    }

    /// US layout, ASCII to USB HID usage; the Bool is "needs Shift".
    private func usage(for a: UInt8) -> (UInt8, Bool)? {
        switch a {
        case 97...122: return (UInt8(a - 97) + 0x04, false) // a-z
        case 65...90:  return (UInt8(a - 65) + 0x04, true)  // A-Z
        case 49...57:  return (UInt8(a - 49) + 0x1E, false) // 1-9
        case 48:       return (0x27, false)                  // 0
        case 32:       return (0x2C, false)                  // space
        case 10, 13:   return (0x28, false)                  // Enter
        case 9:        return (0x2B, false)                  // Tab
        case 45:       return (0x2D, false)                  // -
        case 61:       return (0x2E, false)                  // =
        case 91:       return (0x2F, false)                  // [
        case 93:       return (0x30, false)                  // ]
        case 92:       return (0x31, false)                  // backslash
        case 59:       return (0x33, false)                  // ;
        case 39:       return (0x34, false)                  // '
        case 96:       return (0x35, false)                  // `
        case 44:       return (0x36, false)                  // ,
        case 46:       return (0x37, false)                  // .
        case 47:       return (0x38, false)                  // /
        case 33:       return (0x1E, true)                   // !
        case 64:       return (0x1F, true)                   // @
        case 35:       return (0x20, true)                   // #
        case 36:       return (0x21, true)                   // $
        case 37:       return (0x22, true)                   // %
        case 94:       return (0x23, true)                   // ^
        case 38:       return (0x24, true)                   // &
        case 42:       return (0x25, true)                   // *
        case 40:       return (0x26, true)                   // (
        case 41:       return (0x27, true)                   // )
        case 95:       return (0x2D, true)                   // _
        case 43:       return (0x2E, true)                   // +
        case 123:      return (0x2F, true)                   // {
        case 125:      return (0x30, true)                   // }
        case 124:      return (0x31, true)                   // |
        case 58:       return (0x33, true)                   // :
        case 34:       return (0x34, true)                   // "
        case 126:      return (0x35, true)                   // ~
        case 60:       return (0x36, true)                   // <
        case 62:       return (0x37, true)                   // >
        case 63:       return (0x38, true)                   // ?
        default:       return nil
        }
    }
}

/// SwiftUI wrapper. It reports its layer once, which is all the engine needs:
/// `fb_metal` keeps a reference and stops when the layer goes away.
struct WinDisplayView: UIViewRepresentable {
    /// Toggle the software keyboard from the controls below the panel.
    static func toggleKeyboard() { WinDisplayUIView.current?.toggleKeyboard() }

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
