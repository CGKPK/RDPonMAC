import SwiftUI

struct MonitorLayoutView: View {
    let monitors = MonitorInfo.enumerate()

    var body: some View {
        VStack {
            Text("Display Layout")
                .font(.headline)

            if monitors.isEmpty {
                Text("No displays detected")
                    .foregroundColor(.secondary)
            } else {
                GeometryReader { geometry in
                    let scale = calculateScale(containerSize: geometry.size)

                    ZStack {
                        ForEach(monitors) { monitor in
                            RoundedRectangle(cornerRadius: 4)
                                .stroke(monitor.isPrimary ? Color.accentColor : Color.secondary, lineWidth: 2)
                                .background(
                                    RoundedRectangle(cornerRadius: 4)
                                        .fill(monitor.isPrimary ? Color.accentColor.opacity(0.1) : Color.secondary.opacity(0.1))
                                )
                                .overlay(
                                    VStack {
                                        Text("Display \(monitor.id + 1)")
                                            .font(.caption.bold())
                                        Text("\(monitor.width) x \(monitor.height)")
                                            .font(.caption2)
                                        if monitor.isPrimary {
                                            Text("Primary")
                                                .font(.caption2)
                                                .foregroundColor(.accentColor)
                                        }
                                    }
                                )
                                .frame(
                                    width: monitor.frame.width * scale,
                                    height: monitor.frame.height * scale
                                )
                                .offset(
                                    x: monitor.frame.origin.x * scale,
                                    y: monitor.frame.origin.y * scale
                                )
                        }
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
        }
        .padding()
    }

    private func calculateScale(containerSize: CGSize) -> CGFloat {
        guard !monitors.isEmpty else { return 1 }

        let totalWidth = monitors.map { $0.frame.maxX }.max()! - monitors.map { $0.frame.minX }.min()!
        let totalHeight = monitors.map { $0.frame.maxY }.max()! - monitors.map { $0.frame.minY }.min()!

        let scaleX = (containerSize.width - 40) / totalWidth
        let scaleY = (containerSize.height - 40) / totalHeight

        return min(scaleX, scaleY, 0.2)
    }
}
