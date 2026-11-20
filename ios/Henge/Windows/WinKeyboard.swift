import UIKit

/// USB HID keyboard usages. These are what the xHCI boot keyboard takes, not
/// the PS/2 scan codes UTM uses, so every key here goes straight to the guest.
enum WinHID {
    static let leftControl: UInt8 = 0xE0
    static let leftShift: UInt8 = 0xE1
    static let leftOption: UInt8 = 0xE2
    static let leftCommand: UInt8 = 0xE3

    static let escape: UInt8 = 0x29
    static let tab: UInt8 = 0x2B
    static let deleteForward: UInt8 = 0x4C
    static let capsLock: UInt8 = 0x39
    static let scrollLock: UInt8 = 0x47
    static let numLock: UInt8 = 0x53
    static let printScreen: UInt8 = 0x46
    static let insert: UInt8 = 0x49
    static let home: UInt8 = 0x4A
    static let end: UInt8 = 0x4D
    static let pageUp: UInt8 = 0x4B
    static let pageDown: UInt8 = 0x4E
    static let up: UInt8 = 0x52
    static let down: UInt8 = 0x51
    static let left: UInt8 = 0x50
    static let right: UInt8 = 0x4F

    /// F1...F12 are consecutive usages 0x3A...0x45.
    static func function(_ n: Int) -> UInt8 { UInt8(0x3A + n - 1) }
}

/// ⌃, ⌥, ⌘ and ⇧ stay down from a tap until the next key, exactly like UTM's
/// accessory bar. Tapping Ctrl then C on the software keyboard therefore sends
/// a real Ctrl+C, and the modifier is released after the C.
final class WinStickyKeys {
    static let shared = WinStickyKeys()

    private(set) var down: Set<UInt8> = []
    var changed: (() -> Void)?

    func isDown(_ usage: UInt8) -> Bool { down.contains(usage) }

    func toggle(_ usage: UInt8) {
        if down.contains(usage) {
            WinEngine.shared.key(hid: usage, down: false)
            down.remove(usage)
        } else {
            WinEngine.shared.key(hid: usage, down: true)
            down.insert(usage)
        }
        changed?()
    }

    func releaseAll() {
        guard !down.isEmpty else { return }
        for usage in down { WinEngine.shared.key(hid: usage, down: false) }
        down.removeAll()
        changed?()
    }
}

/// The row above the software keyboard: fixed modifiers, a horizontally
/// scrolling set of keys the iOS keyboard does not have, then Ctrl+Alt+Del and
/// the hide button. Modelled on UTM's VMKeyboardAccessoryView.
final class WinKeyboardAccessoryView: UIInputView {
    private weak var display: WinDisplayUIView?
    private var modifierButtons: [WinKeyButton] = []

    init(display: WinDisplayUIView) {
        self.display = display
        super.init(frame: CGRect(x: 0, y: 0, width: 0, height: 50), inputViewStyle: .keyboard)
        autoresizingMask = .flexibleWidth
        allowsSelfSizing = true
        build()
        WinStickyKeys.shared.changed = { [weak self] in self?.refresh() }
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    private func build() {
        let modifiers = [
            WinKeyButton("⌃", usage: WinHID.leftControl, label: "Control"),
            WinKeyButton("⌥", usage: WinHID.leftOption, label: "Option"),
            WinKeyButton("⌘", usage: WinHID.leftCommand, label: "Command"),
            WinKeyButton("⇧", usage: WinHID.leftShift, label: "Shift"),
        ]
        modifierButtons = modifiers
        for button in modifiers {
            button.addTarget(self, action: #selector(modifierTapped(_:)), for: .touchUpInside)
        }

        var keys = [
            WinKeyButton("⎋", usage: WinHID.escape, label: "Escape"),
            WinKeyButton("⇥", usage: WinHID.tab, label: "Tab"),
            WinKeyButton("↑", usage: WinHID.up, label: "Up"),
            WinKeyButton("↓", usage: WinHID.down, label: "Down"),
            WinKeyButton("←", usage: WinHID.left, label: "Left"),
            WinKeyButton("→", usage: WinHID.right, label: "Right"),
            WinKeyButton("Del", usage: WinHID.deleteForward, label: "Forward Delete"),
        ]
        for n in 1...12 {
            keys.append(WinKeyButton("F\(n)", usage: WinHID.function(n), label: "F\(n)"))
        }
        keys += [
            WinKeyButton("Caps", usage: WinHID.capsLock, label: "Caps Lock"),
            WinKeyButton("Scroll", usage: WinHID.scrollLock, label: "Scroll Lock"),
            WinKeyButton("Num", usage: WinHID.numLock, label: "Num Lock"),
            WinKeyButton("Pr Scr", usage: WinHID.printScreen, label: "Print Screen"),
            WinKeyButton("Ins", usage: WinHID.insert, label: "Insert"),
            WinKeyButton("Home", usage: WinHID.home, label: "Home"),
            WinKeyButton("End", usage: WinHID.end, label: "End"),
            WinKeyButton("Pg Up", usage: WinHID.pageUp, label: "Page Up"),
            WinKeyButton("Pg Dn", usage: WinHID.pageDown, label: "Page Down"),
        ]
        for button in keys {
            button.addTarget(self, action: #selector(keyDown(_:)), for: .touchDown)
            button.addTarget(self, action: #selector(keyUp(_:)), for: [.touchUpInside, .touchUpOutside, .touchCancel])
        }

        let modifierStack = UIStackView(arrangedSubviews: modifiers)
        modifierStack.spacing = 6

        let keyStack = UIStackView(arrangedSubviews: keys)
        keyStack.spacing = 6
        keyStack.layoutMargins = UIEdgeInsets(top: 1, left: 0, bottom: 1, right: 0)
        keyStack.isLayoutMarginsRelativeArrangement = true

        let scroll = UIScrollView()
        scroll.showsHorizontalScrollIndicator = false
        scroll.addSubview(keyStack)
        keyStack.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            keyStack.topAnchor.constraint(equalTo: scroll.contentLayoutGuide.topAnchor),
            keyStack.bottomAnchor.constraint(equalTo: scroll.contentLayoutGuide.bottomAnchor),
            keyStack.leadingAnchor.constraint(equalTo: scroll.contentLayoutGuide.leadingAnchor),
            keyStack.trailingAnchor.constraint(equalTo: scroll.contentLayoutGuide.trailingAnchor),
            keyStack.heightAnchor.constraint(equalTo: scroll.frameLayoutGuide.heightAnchor),
        ])

        let cad = WinKeyButton("⌃⌥⌦", usage: 0, width: 48, label: "Control Alt Delete")
        cad.addTarget(self, action: #selector(sendControlAltDelete), for: .touchUpInside)

        let hide = UIButton(type: .system)
        hide.setImage(UIImage(systemName: "keyboard.chevron.compact.down"), for: .normal)
        hide.tintColor = .label
        hide.accessibilityLabel = "Hide keyboard"
        hide.widthAnchor.constraint(equalToConstant: 34).isActive = true
        hide.heightAnchor.constraint(equalToConstant: 34).isActive = true
        hide.addTarget(self, action: #selector(hideKeyboard), for: .touchUpInside)

        let content = UIStackView(arrangedSubviews: [modifierStack, scroll, cad, hide])
        content.spacing = 8
        content.alignment = .center
        addSubview(content)
        content.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            content.topAnchor.constraint(equalTo: topAnchor, constant: 6),
            content.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -6),
            content.leadingAnchor.constraint(equalTo: safeAreaLayoutGuide.leadingAnchor, constant: 8),
            content.trailingAnchor.constraint(equalTo: safeAreaLayoutGuide.trailingAnchor, constant: -8),
        ])
        refresh()
    }

    private func refresh() {
        for button in modifierButtons {
            let on = WinStickyKeys.shared.isDown(button.usage)
            button.backgroundColor = on ? .systemBlue : .secondarySystemBackground
            button.setTitleColor(on ? .white : .label, for: .normal)
        }
    }

    @objc private func modifierTapped(_ sender: WinKeyButton) {
        WinStickyKeys.shared.toggle(sender.usage)
        UIImpactFeedbackGenerator(style: .light).impactOccurred()
    }

    @objc private func keyDown(_ sender: WinKeyButton) {
        guard !sender.isKeyDown else { return }
        sender.isKeyDown = true
        WinEngine.shared.key(hid: sender.usage, down: true)
    }

    @objc private func keyUp(_ sender: WinKeyButton) {
        guard sender.isKeyDown else { return }
        sender.isKeyDown = false
        WinEngine.shared.key(hid: sender.usage, down: false)
        WinStickyKeys.shared.releaseAll()
    }

    @objc private func sendControlAltDelete() {
        let sticky = WinStickyKeys.shared
        if !sticky.isDown(WinHID.leftControl) { WinEngine.shared.key(hid: WinHID.leftControl, down: true) }
        if !sticky.isDown(WinHID.leftOption) { WinEngine.shared.key(hid: WinHID.leftOption, down: true) }
        WinEngine.shared.key(hid: WinHID.deleteForward, down: true)
        WinEngine.shared.key(hid: WinHID.deleteForward, down: false)
        if !sticky.isDown(WinHID.leftControl) { WinEngine.shared.key(hid: WinHID.leftControl, down: false) }
        if !sticky.isDown(WinHID.leftOption) { WinEngine.shared.key(hid: WinHID.leftOption, down: false) }
        sticky.releaseAll()
    }

    @objc private func hideKeyboard() {
        WinStickyKeys.shared.releaseAll()
        display?.toggleKeyboard()
    }
}

/// A key that goes down on touch and up on release, so holding an arrow repeats
/// the way it does on a hardware keyboard.
@objc private final class WinKeyButton: UIButton {
    let usage: UInt8
    var isKeyDown = false

    init(_ title: String, usage: UInt8, width: CGFloat = 34, label: String) {
        self.usage = usage
        super.init(frame: .zero)
        setTitle(title, for: .normal)
        titleLabel?.font = .systemFont(ofSize: 13, weight: .semibold)
        setTitleColor(.label, for: .normal)
        backgroundColor = .secondarySystemBackground
        layer.cornerRadius = 8
        layer.cornerCurve = .continuous
        accessibilityLabel = label
        translatesAutoresizingMaskIntoConstraints = false
        widthAnchor.constraint(equalToConstant: width).isActive = true
        heightAnchor.constraint(equalToConstant: 34).isActive = true
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
}
