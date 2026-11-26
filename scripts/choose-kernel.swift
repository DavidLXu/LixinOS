import AppKit
import UniformTypeIdentifiers

struct KernelProfile: Decodable {
    let name: String
    let disk: String
    let loader: String
    let initial: Int
    let minimum: Int
}

func memoryLabel(_ mib: Int) -> String {
    mib.isMultiple(of: 1024) ? "\(mib / 1024) GiB" : "\(mib) MiB"
}

struct KernelOption {
    let profile: KernelProfile
    let sizes: [Int]
    var selected: Int

    init(_ profile: KernelProfile, explicitRAM: Bool) {
        self.profile = profile
        let range = (2...16).map { $0 * 1024 }
        let fixed = profile.minimum > range.last!
        selected = max(profile.initial, explicitRAM ? profile.minimum : range[0])
        sizes = fixed ? Array(Set([profile.minimum, selected])).sorted()
                      : Array(Set(range + [selected])).sorted()
    }
}

struct DiskChoice {
    var options: [KernelOption]
    let fallbackNote: String
}

final class KernelPicker: NSObject {
    let titles: [String: String]
    let daily: String
    let release: String
    let ram: String
    let helper: URL
    let start: NSButton
    var disks: [DiskChoice]
    var diskIndex = 0
    let diskMenu = NSPopUpButton()
    let menu = NSPopUpButton()
    let slider = NSSlider()
    let value = NSTextField(labelWithString: "")
    let minimum = NSTextField(labelWithString: "")
    let maximum = NSTextField(labelWithString: "")
    let note = NSTextField(labelWithString: "")
    let view = NSView(frame: NSRect(x: 0, y: 0, width: 340, height: 156))

    init(_ args: [String], profiles: [KernelProfile], start: NSButton) {
        titles = ["rust": args[0], "tcc": args[1]]
        daily = args[4]
        release = args[5]
        ram = args[6]
        helper = URL(fileURLWithPath: args[8])
        self.start = start
        disks = [DiskChoice(options: profiles.map { KernelOption($0, explicitRAM: !args[6].isEmpty) },
                            fallbackNote: args[7].isEmpty ? "" : "共享盘在 iCloud 中，已选本地桌面盘。")]
        super.init()
        for (label, y, control, action) in [
            ("工作盘", 132, diskMenu, #selector(changeDisk)),
            ("内核", 100, menu, #selector(changeKernel))
        ] {
            let text = NSTextField(labelWithString: label)
            text.frame = NSRect(x: 0, y: y, width: 52, height: 20)
            view.addSubview(text)
            control.frame = NSRect(x: 54, y: y - 4, width: 286, height: 28)
            control.target = self
            control.action = action
            control.setAccessibilityLabel(label)
            view.addSubview(control)
        }
        let ramLabel = NSTextField(labelWithString: "内存")
        ramLabel.frame = NSRect(x: 0, y: 68, width: 50, height: 20)
        view.addSubview(ramLabel)
        value.frame = NSRect(x: 180, y: 68, width: 160, height: 20)
        value.alignment = .right
        value.font = .monospacedDigitSystemFont(ofSize: 14, weight: .medium)
        view.addSubview(value)
        slider.frame = NSRect(x: 0, y: 40, width: 340, height: 26)
        slider.allowsTickMarkValuesOnly = true
        slider.isContinuous = true
        slider.target = self
        slider.action = #selector(changeMemory)
        slider.setAccessibilityLabel("虚拟机内存")
        view.addSubview(slider)
        for label in [minimum, maximum, note] {
            label.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
            label.textColor = .secondaryLabelColor
            view.addSubview(label)
        }
        minimum.frame = NSRect(x: 0, y: 22, width: 120, height: 17)
        maximum.frame = NSRect(x: 220, y: 22, width: 120, height: 17)
        maximum.alignment = .right
        note.frame = NSRect(x: 0, y: 0, width: 340, height: 18)
        showDisk(preferred: args[3])
    }

    var current: KernelOption { disks[diskIndex].options[menu.indexOfSelectedItem] }

    func diskTitle(_ path: String) -> String {
        path == daily ? "Daily" : URL(fileURLWithPath: path).lastPathComponent
    }

    func showDisk(preferred: String) {
        let options = disks[diskIndex].options
        menu.removeAllItems()
        menu.addItems(withTitles: options.map { titles[$0.profile.name]! })
        menu.selectItem(at: options.firstIndex { $0.profile.name == preferred } ?? 0)
        diskMenu.removeAllItems()
        for disk in disks {
            // Identical basenames can belong to different directories.
            let path = disk.options[0].profile.disk
            let item = NSMenuItem(title: diskTitle(path), action: nil, keyEquivalent: "")
            item.toolTip = path
            diskMenu.menu?.addItem(item)
        }
        if !disks.contains(where: { $0.options[0].profile.disk == daily }),
           FileManager.default.fileExists(atPath: daily) {
            diskMenu.addItem(withTitle: "Daily")
            diskMenu.lastItem?.representedObject = daily
        }
        diskMenu.menu?.addItem(.separator())
        diskMenu.addItem(withTitle: "选择其他镜像…")
        diskMenu.selectItem(at: diskIndex)
        changeKernel()
    }

    func loadDisk(_ path: String) throws -> DiskChoice {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        process.arguments = ["python3", helper.path, "--phase", "picker", "--picked-image",
                             "--disk=\(path)", "--release=\(release)", "--ram=\(ram)"]
        let output = Pipe()
        process.standardOutput = output
        process.standardError = output
        try process.run()
        let data = output.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        guard process.terminationStatus == 0 else {
            let message = String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
            throw NSError(domain: "LixinOS", code: 1, userInfo: [NSLocalizedDescriptionKey: message])
        }
        let profiles = try JSONDecoder().decode([KernelProfile].self, from: data)
        return DiskChoice(options: profiles.map { KernelOption($0, explicitRAM: !ram.isEmpty) }, fallbackNote: "")
    }

    @objc func changeDisk() {
        let preferred = current.profile.name
        let index = diskMenu.indexOfSelectedItem
        if disks.indices.contains(index) {
            diskIndex = index
        } else {
            var path = diskMenu.selectedItem?.representedObject as? String
            if path == nil {
                let panel = NSOpenPanel()
                panel.title = "选择 LixinOS 工作盘"
                panel.allowedContentTypes = [UTType(filenameExtension: "img") ?? .data]
                panel.directoryURL = URL(fileURLWithPath: current.profile.disk).deletingLastPathComponent()
                if panel.runModal() == .OK { path = panel.url?.path }
            }
            if let path = path {
                if let known = disks.firstIndex(where: { $0.options[0].profile.disk == path }) {
                    diskIndex = known
                } else {
                    do {
                        disks.append(try loadDisk(path))
                        diskIndex = disks.count - 1
                    } catch {
                        let alert = NSAlert()
                        alert.messageText = "无法使用这块工作盘"
                        alert.informativeText = error.localizedDescription
                        alert.runModal()
                    }
                }
            }
        }
        showDisk(preferred: preferred)
    }

    @objc func changeKernel() {
        let option = current
        diskMenu.selectedItem?.title = diskTitle(option.profile.disk)
        diskMenu.toolTip = option.profile.disk
        slider.minValue = 0
        slider.maxValue = Double(max(1, option.sizes.count - 1))
        slider.numberOfTickMarks = option.sizes.count
        slider.integerValue = option.sizes.firstIndex(of: option.selected)!
        slider.isEnabled = option.sizes.count > 1
        minimum.stringValue = slider.isEnabled ? memoryLabel(option.sizes.first!) : ""
        maximum.stringValue = slider.isEnabled ? memoryLabel(option.sizes.last!) : ""
        changeMemory()
    }

    @objc func changeMemory() {
        let mib = current.sizes[slider.integerValue]
        disks[diskIndex].options[menu.indexOfSelectedItem].selected = mib
        value.stringValue = memoryLabel(mib)
        slider.setAccessibilityValueDescription(value.stringValue)
        let floor = current.profile.minimum
        start.isEnabled = mib >= floor
        if current.sizes.count == 1 {
            note.stringValue = "此内核使用这块工作盘需要 \(memoryLabel(floor))。"
        } else if floor > current.sizes[0] {
            note.stringValue = "此内核使用这块工作盘至少需要 \(memoryLabel(floor))。"
        } else {
            note.stringValue = disks[diskIndex].fallbackNote
        }
    }
}

let args = Array(CommandLine.arguments.dropFirst())
guard args.count == 9,
      let profiles = try? JSONDecoder().decode([KernelProfile].self, from: Data(args[2].utf8)),
      !profiles.isEmpty else { exit(2) }
let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let alert = NSAlert()
alert.messageText = "启动 LixinOS"
alert.informativeText = args[7].isEmpty ? "" : "共享盘尚未下载，已预选本地桌面盘。"
let picker = KernelPicker(args, profiles: profiles, start: alert.addButton(withTitle: "启动"))
alert.accessoryView = picker.view
alert.addButton(withTitle: "取消").keyEquivalent = "\u{1b}"
app.activate(ignoringOtherApps: true)
let result: [String]
if alert.runModal() == .alertFirstButtonReturn {
    let option = picker.current
    result = [option.profile.name, "\(option.selected)M", option.profile.disk, option.profile.loader]
} else {
    result = ["cancel"]
}
for field in result {
    FileHandle.standardOutput.write(Data(field.utf8) + Data([0]))
}
