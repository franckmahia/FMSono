import AppKit

let destination = CommandLine.arguments[1]
try FileManager.default.createDirectory(atPath: destination, withIntermediateDirectories: true)

func render(pixels: Int, path: String) throws {
    let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels,
        bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
        colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
    let side = CGFloat(pixels)
    let square = NSRect(x: side * 0.05, y: side * 0.05, width: side * 0.9, height: side * 0.9)
    let outline = NSBezierPath(roundedRect: square, xRadius: side * 0.21, yRadius: side * 0.21)
    let gradient = NSGradient(starting: NSColor(red: 0.04, green: 0.20, blue: 0.28, alpha: 1),
                              ending: NSColor(red: 0.06, green: 0.61, blue: 0.60, alpha: 1))!
    gradient.draw(in: outline, angle: 45)
    let heights: [CGFloat] = [0.15, 0.26, 0.43, 0.60, 0.34, 0.52, 0.27, 0.17]
    NSColor.white.setStroke()
    for (index, height) in heights.enumerated() {
        let x = side * (0.22 + CGFloat(index) * 0.08)
        let path = NSBezierPath()
        path.lineWidth = side * 0.043; path.lineCapStyle = .round
        path.move(to: NSPoint(x: x, y: side * (0.5 - height / 2)))
        path.line(to: NSPoint(x: x, y: side * (0.5 + height / 2)))
        path.stroke()
    }
    NSGraphicsContext.restoreGraphicsState()
    try bitmap.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: path))
}
for size in [16, 32, 128, 256, 512] {
    try render(pixels: size, path: "\(destination)/icon_\(size)x\(size).png")
    try render(pixels: size * 2, path: "\(destination)/icon_\(size)x\(size)@2x.png")
}

// PNG-backed ICNS container; avoids depending on iconutil's image services.
func bigEndian(_ number: UInt32) -> Data {
    var value = number.bigEndian
    return withUnsafeBytes(of: &value) { Data($0) }
}
var chunks = Data()
for (type, filename) in [("icp4", "icon_16x16.png"), ("icp5", "icon_32x32.png"),
                         ("icp6", "icon_32x32@2x.png"), ("ic07", "icon_128x128.png"),
                         ("ic08", "icon_256x256.png"), ("ic09", "icon_512x512.png"),
                         ("ic10", "icon_512x512@2x.png")] {
    let png = try Data(contentsOf: URL(fileURLWithPath: "\(destination)/\(filename)"))
    chunks.append(Data(type.utf8)); chunks.append(bigEndian(UInt32(png.count + 8))); chunks.append(png)
}
var container = Data("icns".utf8)
container.append(bigEndian(UInt32(chunks.count + 8))); container.append(chunks)
let iconURL = URL(fileURLWithPath: destination).deletingLastPathComponent().appendingPathComponent("AppIcon.icns")
try container.write(to: iconURL)
guard NSImage(contentsOf: iconURL) != nil else { fatalError("Icône illisible") }
