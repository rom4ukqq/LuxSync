// Run on macOS from the repository root to regenerate both platform icons.
import AppKit

func icon(_ size: Int, receiver: Bool) -> Data {
    let space = CGColorSpace(name: CGColorSpace.sRGB)!
    let context = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: 0,
                            space: space, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    context.scaleBy(x: Double(size) / 512, y: Double(size) / 512)
    func color(_ rgb: UInt32) -> CGColor {
        CGColor(red: Double((rgb >> 16) & 255) / 255, green: Double((rgb >> 8) & 255) / 255,
                blue: Double(rgb & 255) / 255, alpha: 1)
    }
    func gradient(_ light: UInt32, _ dark: UInt32, _ top: CGFloat, _ bottom: CGFloat) {
        let gradient = CGGradient(colorsSpace: space, colors: [color(light), color(dark)] as CFArray,
                                  locations: [0, 1])!
        context.drawLinearGradient(gradient, start: CGPoint(x: 0, y: top), end: CGPoint(x: 0, y: bottom),
                                   options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
    }
    let tile = CGPath(roundedRect: CGRect(x: 24, y: 24, width: 464, height: 464),
                      cornerWidth: 112, cornerHeight: 112, transform: nil)
    context.saveGState()
    context.addPath(tile)
    context.clip()
    gradient(receiver ? 0x17454a : 0x34355c, receiver ? 0x081b23 : 0x181a34, 490, 24)
    context.restoreGState()
    context.addPath(tile)
    context.setStrokeColor(color(receiver ? 0x37817e : 0x7474ab))
    context.setLineWidth(8)
    context.strokePath()

    func line(_ points: [CGPoint], color tint: UInt32, width: CGFloat) {
        context.setStrokeColor(color(tint))
        context.setLineWidth(width)
        context.addLines(between: points)
        context.strokePath()
    }

    func spark(_ x: CGFloat, _ y: CGFloat, _ radius: CGFloat, color tint: UInt32) {
        let bend = radius * 0.28
        context.setFillColor(color(tint))
        context.move(to: CGPoint(x: x, y: y + radius))
        context.addQuadCurve(to: CGPoint(x: x + radius, y: y), control: CGPoint(x: x + bend, y: y + bend))
        context.addQuadCurve(to: CGPoint(x: x, y: y - radius), control: CGPoint(x: x + bend, y: y - bend))
        context.addQuadCurve(to: CGPoint(x: x - radius, y: y), control: CGPoint(x: x - bend, y: y - bend))
        context.addQuadCurve(to: CGPoint(x: x, y: y + radius), control: CGPoint(x: x - bend, y: y + bend))
        context.closePath()
        context.fillPath()
    }

    context.setLineCap(.round)
    context.setLineJoin(.round)
    if receiver {
        context.setStrokeColor(color(0x83f2d8))
        context.setLineWidth(27)
        context.addPath(CGPath(roundedRect: CGRect(x: 109, y: 169, width: 294, height: 199),
                               cornerWidth: 32, cornerHeight: 32, transform: nil))
        context.strokePath()
        line([CGPoint(x: 256, y: 166), CGPoint(x: 256, y: 113)], color: 0x83f2d8, width: 27)
        line([CGPoint(x: 202, y: 109), CGPoint(x: 310, y: 109)], color: 0x83f2d8, width: 27)
        spark(256, 268, 69, color: 0xffcb75)
    } else {
        spark(204, 301, 119, color: 0xffcb75)
        line([CGPoint(x: 237, y: 145), CGPoint(x: 318, y: 145), CGPoint(x: 378, y: 205)],
             color: 0xbbafff, width: 29)
        context.setFillColor(color(0xbbafff))
        context.fillEllipse(in: CGRect(x: 348, y: 175, width: 60, height: 60))
    }
    return NSBitmapImageRep(cgImage: context.makeImage()!).representation(using: .png, properties: [:])!
}

func little<T: FixedWidthInteger>(_ value: T) -> Data {
    var encoded = value.littleEndian
    return withUnsafeBytes(of: &encoded) { Data($0) }
}

let icons = [16, 20, 24, 32, 48, 64, 128, 256]
var ico = Data([0, 0, 1, 0]) + little(UInt16(icons.count))
var chunks = Data()
for size in icons {
    let bytes = icon(size, receiver: true)
    ico.append(contentsOf: [UInt8(truncatingIfNeeded: size), UInt8(truncatingIfNeeded: size), 0, 0])
    ico.append(little(UInt16(1)))
    ico.append(little(UInt16(32)))
    ico.append(little(UInt32(bytes.count)))
    ico.append(little(UInt32(6 + 16 * icons.count + chunks.count)))
    chunks.append(bytes)
}
try (ico + chunks).write(to: URL(fileURLWithPath: "assets/BrightSync.ico"))

let temporary = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
let iconset = temporary.appendingPathComponent("BrightSync.iconset")
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
defer { try? FileManager.default.removeItem(at: temporary) }
for size in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let suffix = scale == 1 ? "" : "@2x"
        try icon(size * scale, receiver: false).write(to: iconset.appendingPathComponent("icon_\(size)x\(size)\(suffix).png"))
    }
}
let process = Process()
process.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
process.arguments = ["-c", "icns", iconset.path, "-o", "assets/BrightSync.icns"]
try process.run()
process.waitUntilExit()
guard process.terminationStatus == 0 else { exit(1) }
