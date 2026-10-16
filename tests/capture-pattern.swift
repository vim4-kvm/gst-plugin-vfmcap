// 4K HDMI test fixture: swiftc capture-pattern.swift -o capture-pattern
// Run on the Mac supplying the physical HDMI signal. Exits after 240 seconds.
import Cocoa
let app = NSApplication.shared
app.setActivationPolicy(.accessory)
guard let screen = NSScreen.screens.first(where: {
    $0.frame.width == 1920 && $0.frame.height == 1080 && $0.backingScaleFactor == 2
}) else {
    fputs("Requires a 3840x2160 display at 1920x1080 HiDPI scale\n", stderr)
    exit(1)
}
setbuf(stdout, nil)
class Pattern: NSView {
    var frameID: UInt32 = 0
    override func draw(_ dirtyRect: NSRect) {
        frameID += 1
        NSGraphicsContext.current?.shouldAntialias = false
        NSColor.black.setFill(); bounds.fill()
        let colors: [NSColor] = [.red, .green, .blue, .cyan, .magenta, .yellow, .white, .black]
        for (i, color) in colors.enumerated() {
            color.setFill()
            NSRect(x: CGFloat(i) * 240, y: 850, width: 240, height: 120).fill()
        }
        // One physical pixel per stripe on the 2x backing display.
        for x in 0..<3840 {
            (x % 2 == 0 ? NSColor.red : NSColor.cyan).setFill()
            NSRect(x: CGFloat(x) / 2, y: 650, width: 0.5, height: 100).fill()
        }
        for y in 0..<200 {
            (y % 2 == 0 ? NSColor.red : NSColor.cyan).setFill()
            NSRect(x: 0, y: 300 + CGFloat(y) / 2, width: 1920, height: 0.5).fill()
        }
        let id = frameID & 65535
        let word = UInt32(0xa5000000) | (id << 8) | (((id >> 8) ^ (id & 255) ^ 0xa5) & 255)
        // Same frame ID at top/middle/bottom detects torn or repeated frames.
        for center in [85.0, 540.0, 995.0] {
            for bit in 0..<32 {
                ((word & (1 << (31 - bit))) != 0 ? NSColor.white : NSColor.black).setFill()
                NSRect(x: 20 + CGFloat(bit) * 58.75, y: 1080 - center - 30,
                       width: 55, height: 60).fill()
            }
        }
        print("\(frameID),\(Date().timeIntervalSince1970)")
    }
}
let window = NSWindow(contentRect: screen.frame, styleMask: .borderless, backing: .buffered, defer: false, screen: screen)
window.level = .screenSaver
window.isReleasedWhenClosed = false
let view = Pattern(frame: NSRect(origin: .zero, size: screen.frame.size))
window.contentView = view
window.orderFrontRegardless()
let timer = Timer.scheduledTimer(withTimeInterval: 1.0/60, repeats: true) { _ in view.needsDisplay = true }
Timer.scheduledTimer(withTimeInterval: 240, repeats: false) { _ in timer.invalidate(); window.close(); app.terminate(nil) }
app.run()
