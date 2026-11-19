import AppKit
import ApplicationServices
import CoreGraphics
import Darwin

// Report QEMU's content area in backing pixels, excluding its title bar.
guard CommandLine.arguments.count == 2, let pid = Int32(CommandLine.arguments[1]) else {
    exit(2)
}
let windowStyle: NSWindow.StyleMask = [.titled, .closable, .miniaturizable, .resizable]
let titleHeight = NSWindow.contentRect(forFrameRect: CGRect(x: 0, y: 0, width: 800, height: 600),
                                       styleMask: windowStyle).height
let titleBar = 600 - titleHeight
var previous: [Int] = []
var reported: [Int] = []
var nativeWindowReady = false
var draggingEnabled = false
var nextDragAttempt = Date.distantPast

func accessibilityValue<T>(_ element: AXUIElement, _ attribute: String) -> T? {
    var value: CFTypeRef?
    guard AXUIElementCopyAttributeValue(element, attribute as CFString, &value) == .success
    else { return nil }
    return value as? T
}

func enableWindowDragging() -> Bool {
    guard AXIsProcessTrusted() else { return false }
    let app = AXUIElementCreateApplication(pid)
    guard let window: AXUIElement = accessibilityValue(app, kAXMainWindowAttribute),
          let bar: AXUIElement = accessibilityValue(app, kAXMenuBarAttribute),
          let menus: [AXUIElement] = accessibilityValue(bar, kAXChildrenAttribute),
          let view = menus.first(where: { accessibilityValue($0, kAXTitleAttribute) as String? == "View" }),
          let menu: AXUIElement = (accessibilityValue(view, kAXChildrenAttribute) as [AXUIElement]?)?.first,
          let items: [AXUIElement] = accessibilityValue(menu, kAXChildrenAttribute),
          let zoom = items.first(where: { accessibilityValue($0, kAXTitleAttribute) as String? == "Zoom To Fit" })
    else { return false }
    var resizable = DarwinBoolean(false)
    guard AXUIElementIsAttributeSettable(window, kAXSizeAttribute as CFString, &resizable) == .success
    else { return false }
    return resizable.boolValue || AXUIElementPerformAction(zoom, kAXPressAction as CFString) == .success
}

while kill(pid, 0) == 0 {
    autoreleasepool {
        let windows = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements],
                                                  kCGNullWindowID) as? [[String: Any]] ?? []
        let window = windows.compactMap { row -> CGRect? in
            guard row[kCGWindowOwnerPID as String] as? Int32 == pid,
                  row[kCGWindowLayer as String] as? Int == 0,
                  let bounds = row[kCGWindowBounds as String] as? NSDictionary,
                  let rect = CGRect(dictionaryRepresentation: bounds),
                  rect.width >= 100, rect.height >= 100 else { return nil }
            return rect
        }.max { $0.width * $0.height < $1.width * $1.height }

        guard let window else { previous = []; return }
        let screens = NSScreen.screens.compactMap { screen -> (CGRect, CGFloat)? in
            guard let number = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber
            else { return nil }
            return (CGDisplayBounds(number.uint32Value), screen.backingScaleFactor)
        }
        let screen = screens.max { a, b in
            func area(_ rectangle: CGRect) -> CGFloat {
                rectangle.isNull ? 0 : rectangle.width * rectangle.height
            }
            return area(a.0.intersection(window)) < area(b.0.intersection(window))
        }
        let backing = screen?.1 ?? 1
        let fullScreen = screen.map { abs($0.0.width - window.width) < 2
            && abs($0.0.height - window.height) < 2 } ?? false
        let size = [Int((window.width * backing).rounded()),
                    Int(((window.height - (fullScreen ? 0 : titleBar)) * backing).rounded())]
        // Cocoa opens on QEMU's 640-pixel placeholder before the guest configures ramfb.
        defer { previous = size }
        guard size == previous else { return }
        if !nativeWindowReady {
            guard size[0] > 640 else { return }
            nativeWindowReady = true
            reported = size
        }
        if !draggingEnabled && Date() >= nextDragAttempt {
            draggingEnabled = enableWindowDragging()
            nextDragAttempt = Date().addingTimeInterval(1)
        }
        // Ignore minor differences caused by Cocoa chrome and pixel rounding.
        if abs(size[0] - reported[0]) > 4 || abs(size[1] - reported[1]) > 4 {
            print("\(size[0])\t\(size[1])")
            fflush(stdout)
            reported = size
        }
    }
    Thread.sleep(forTimeInterval: 0.18)
}
