import AppKit

enum MenuIcon: CaseIterable, Hashable {
    case sensor, connected, screenLevel, receiver, pair, forget, address

    private static let images = Dictionary(uniqueKeysWithValues: allCases.map { ($0, $0.draw()) })

    var image: NSImage { Self.images[self]! }

    private func draw() -> NSImage {
        let image = NSImage(size: NSSize(width: 18, height: 18), flipped: false) { _ in
            guard let pen = NSGraphicsContext.current?.cgContext else { return false }
            pen.setStrokeColor(CGColor(gray: 0, alpha: 1))
            pen.setFillColor(CGColor(gray: 0, alpha: 1))
            pen.setLineWidth(1.5)
            pen.setLineCap(.round)
            pen.setLineJoin(.round)

            func line(_ points: [CGPoint]) {
                pen.addLines(between: points)
                pen.strokePath()
            }
            func spark(_ x: CGFloat, _ y: CGFloat, _ radius: CGFloat, filled: Bool) {
                pen.move(to: CGPoint(x: x, y: y + radius))
                pen.addQuadCurve(to: CGPoint(x: x + radius, y: y), control: CGPoint(x: x + 1.2, y: y + 1.2))
                pen.addQuadCurve(to: CGPoint(x: x, y: y - radius), control: CGPoint(x: x + 1.2, y: y - 1.2))
                pen.addQuadCurve(to: CGPoint(x: x - radius, y: y), control: CGPoint(x: x - 1.2, y: y - 1.2))
                pen.addQuadCurve(to: CGPoint(x: x, y: y + radius), control: CGPoint(x: x - 1.2, y: y + 1.2))
                pen.closePath()
                filled ? pen.fillPath() : pen.strokePath()
            }
            func box(_ x: CGFloat, _ y: CGFloat, _ width: CGFloat, _ height: CGFloat) {
                pen.addPath(CGPath(roundedRect: CGRect(x: x, y: y, width: width, height: height),
                                   cornerWidth: 1.5, cornerHeight: 1.5, transform: nil))
                pen.strokePath()
            }
            func dot(_ x: CGFloat, _ y: CGFloat) {
                pen.fillEllipse(in: CGRect(x: x - 1, y: y - 1, width: 2, height: 2))
            }

            switch self {
            case .sensor, .connected:
                spark(6.5, 11, 5, filled: self == .connected)
                line([CGPoint(x: 8.5, y: 4), CGPoint(x: 12, y: 4), CGPoint(x: 15.5, y: 7.5)])
                dot(15.5, 7.5)
            case .screenLevel:
                spark(9, 9, 6, filled: false)
                dot(14.5, 14.5)
            case .receiver:
                box(2, 5, 14, 10)
                line([CGPoint(x: 9, y: 5), CGPoint(x: 9, y: 2), CGPoint(x: 6, y: 2)])
                line([CGPoint(x: 9, y: 2), CGPoint(x: 12, y: 2)])
                spark(9, 10, 2.8, filled: true)
            case .pair, .forget:
                box(2, 4, 8, 5.5)
                box(8, 8.5, 8, 5.5)
                if self == .forget {
                    line([CGPoint(x: 3, y: 15), CGPoint(x: 15, y: 3)])
                } else {
                    dot(5.5, 6.75)
                    dot(12.5, 11.25)
                }
            case .address:
                box(2, 4, 14, 10)
                for x in [5.0, 9.0, 13.0] { dot(x, 9) }
            }
            return true
        }
        image.isTemplate = true
        return image
    }
}
