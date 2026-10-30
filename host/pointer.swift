// AgentTally pointer: move the Mac mouse off the bottom edge of the screen and
// onto the display, like a second screen. Started by agenttally.py, which
// forwards the JSON lines printed here to the display over USB.
//
// No Accessibility or Input Monitoring permission is needed: a thin strip
// window at the screen edge notices the mouse, the helper becomes the active
// app, freezes and hides the cursor (as games do) and reads the movement from
// events delivered to its own window.
//
//   agenttally-pointer --w 240 --h 320 --start 0 --length 0.25 --speed 0.8
//
// stdout: {"t":"enter"} {"t":"m","x":..,"y":..,"b":0|1} {"t":"ms","dy":..} {"t":"rb"} {"t":"leave"}
// It exits (and releases the mouse) when stdin closes, i.e. when the bridge stops.
import AppKit

var opts: [String: Double] = ["w": 240, "h": 320, "start": 0, "length": 0.25, "speed": 0.8]
var argv = CommandLine.arguments.dropFirst().makeIterator()
while let a = argv.next() {
  if a.hasPrefix("--"), let v = argv.next(), let d = Double(v) { opts[String(a.dropFirst(2))] = d }
}
let W = CGFloat(opts["w"]!), H = CGFloat(opts["h"]!), SPEED = CGFloat(opts["speed"]!)

func emit(_ obj: [String: Any]) {
  if let d = try? JSONSerialization.data(withJSONObject: obj), let s = String(data: d, encoding: .utf8) {
    print(s)
    fflush(stdout)
  }
}

final class KeyPanel: NSPanel {
  override var canBecomeKey: Bool { true }
  override var canBecomeMain: Bool { true }
}

final class StripView: NSView {
  weak var ctl: Pointer?
  override func updateTrackingAreas() {
    trackingAreas.forEach(removeTrackingArea)
    addTrackingArea(NSTrackingArea(rect: bounds, options: [.mouseEnteredAndExited, .activeAlways, .inVisibleRect], owner: self))
  }
  override func mouseEntered(with e: NSEvent) { ctl?.enter() }
}

// Catches the frozen cursor's events; also shows a small "Esc to return" hint.
final class CatcherView: NSView {
  weak var ctl: Pointer?
  var hint = "Mouse on AgentTally \u{00B7} Esc or move up to return"
  override var acceptsFirstResponder: Bool { true }
  override func acceptsFirstMouse(for e: NSEvent?) -> Bool { true }
  override func updateTrackingAreas() {
    trackingAreas.forEach(removeTrackingArea)
    addTrackingArea(NSTrackingArea(rect: bounds, options: [.mouseMoved, .activeAlways, .inVisibleRect], owner: self))
  }
  override func draw(_ r: NSRect) {
    NSColor.clear.setFill(); bounds.fill()
    let attrs: [NSAttributedString.Key: Any] = [.font: NSFont.systemFont(ofSize: 12, weight: .medium), .foregroundColor: NSColor.white]
    let size = (hint as NSString).size(withAttributes: attrs)
    let pill = NSRect(x: 12, y: 12, width: size.width + 24, height: size.height + 12)
    NSColor(calibratedRed: 0.85, green: 0.47, blue: 0.34, alpha: 0.92).setFill()
    NSBezierPath(roundedRect: pill, xRadius: pill.height / 2, yRadius: pill.height / 2).fill()
    (hint as NSString).draw(at: NSPoint(x: pill.minX + 12, y: pill.minY + 6), withAttributes: attrs)
  }
  override func mouseMoved(with e: NSEvent) { ctl?.move(e) }
  override func mouseDragged(with e: NSEvent) { ctl?.move(e) }
  override func mouseDown(with e: NSEvent) { ctl?.button(true) }
  override func mouseUp(with e: NSEvent) { ctl?.button(false) }
  override func rightMouseDown(with e: NSEvent) { if ctl?.grabbed == true { emit(["t": "rb"]) } }
  override func scrollWheel(with e: NSEvent) { ctl?.scroll(e) }
  override func keyDown(with e: NSEvent) { if e.keyCode == 53 { ctl?.leave() } }  // Esc
}

final class Pointer: NSObject {
  let strip: NSPanel, catcher: KeyPanel, catcherView: CatcherView
  var active = false, grabbed = false, down = false
  var x = W / 2, y: CGFloat = 0
  var previous: NSRunningApplication?
  var lastEmit = Date.distantPast, pending = false

  override init() {
    let screen = NSScreen.screens[0].frame  // the screen with the menu bar
    let sx = screen.minX + screen.width * CGFloat(opts["start"]!)
    let sw = screen.width * CGFloat(opts["length"]!)
    strip = NSPanel(contentRect: NSRect(x: sx, y: screen.minY, width: sw, height: 2),
                    styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
    catcher = KeyPanel(contentRect: NSRect(x: sx, y: screen.minY, width: min(sw, 420), height: 160),
                       styleMask: [.borderless], backing: .buffered, defer: false)
    catcherView = CatcherView(frame: NSRect(x: 0, y: 0, width: min(sw, 420), height: 160))
    super.init()
    for p in [strip as NSPanel, catcher as NSPanel] {
      p.level = .screenSaver
      p.collectionBehavior = [.canJoinAllSpaces, .stationary, .fullScreenAuxiliary]
      p.isOpaque = false
      p.hasShadow = false
      p.acceptsMouseMovedEvents = true
    }
    strip.backgroundColor = NSColor.black.withAlphaComponent(0.01)  // invisible, still gets events
    catcher.backgroundColor = NSColor.black.withAlphaComponent(0.01)
    let sv = StripView(frame: strip.contentLayoutRect); sv.ctl = self
    strip.contentView = sv
    catcherView.ctl = self
    catcher.contentView = catcherView
    strip.orderFrontRegardless()
    NotificationCenter.default.addObserver(forName: NSApplication.didResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
      self?.leave()  // Cmd-Tab or a click elsewhere
    }
  }

  func cgY(_ cocoaY: CGFloat) -> CGFloat { NSScreen.screens[0].frame.height - cocoaY }

  func enter() {
    guard !active else { return }
    active = true
    previous = NSWorkspace.shared.frontmostApplication
    x = min(max(x, 0), W - 1); y = 0
    catcher.orderFrontRegardless()
    let f = catcher.frame
    CGWarpMouseCursorPosition(CGPoint(x: f.midX, y: cgY(f.midY)))
    NSApp.activate(ignoringOtherApps: true)
    catcher.makeKey()
    emit(["t": "enter"])
    send()
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { [weak self] in
      guard let self = self, self.active else { return }
      if NSApp.isActive { self.grab() } else { self.catcherView.hint = "Click to move the mouse onto AgentTally"; self.catcherView.needsDisplay = true }
    }
  }

  func grab() {
    guard active, !grabbed else { return }
    grabbed = true
    CGAssociateMouseAndMouseCursorPosition(0)
    NSCursor.hide()
    catcherView.hint = "Mouse on AgentTally \u{00B7} Esc or move up to return"
    catcherView.needsDisplay = true
  }

  func move(_ e: NSEvent) {
    guard grabbed else { return }
    x = min(max(x + e.deltaX * SPEED, 0), W - 1)
    y += e.deltaY * SPEED
    if y < -6 && !down { leave(); return }  // pushed back up past the top edge
    y = min(max(y, 0), H - 1)
    send()
  }

  func button(_ isDown: Bool) {
    if !grabbed { if isDown { grab() }; return }
    down = isDown
    send(force: true)
  }

  func scroll(_ e: NSEvent) {
    guard grabbed else { return }
    let dy = e.scrollingDeltaY * (e.hasPreciseScrollingDeltas ? 1 : 12)
    if abs(dy) >= 0.5 { emit(["t": "ms", "dy": Int(dy.rounded())]) }
  }

  // ~60 updates per second at most; the last position is always sent.
  func send(force: Bool = false) {
    let now = Date()
    if force || now.timeIntervalSince(lastEmit) > 0.016 {
      lastEmit = now
      pending = false
      emit(["t": "m", "x": Int(x), "y": Int(y), "b": down ? 1 : 0])
    } else if !pending {
      pending = true
      DispatchQueue.main.asyncAfter(deadline: .now() + 0.02) { [weak self] in
        if self?.pending == true { self?.send(force: true) }
      }
    }
  }

  func leave() {
    guard active else { return }
    active = false
    if grabbed { CGAssociateMouseAndMouseCursorPosition(1); NSCursor.unhide() }
    grabbed = false
    down = false
    catcher.orderOut(nil)
    let f = strip.frame
    CGWarpMouseCursorPosition(CGPoint(x: f.minX + min(f.width, 420) / 2, y: cgY(f.minY + 80)))
    previous?.activate()
    emit(["t": "leave"])
  }
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let pointer = Pointer()
// stop (and give the mouse back) when the bridge goes away
FileHandle.standardInput.readabilityHandler = { h in
  if h.availableData.isEmpty {
    DispatchQueue.main.async { pointer.leave(); exit(0) }
  }
}
signal(SIGTERM, SIG_IGN)
let term = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
term.setEventHandler { pointer.leave(); exit(0) }
term.resume()
app.run()
