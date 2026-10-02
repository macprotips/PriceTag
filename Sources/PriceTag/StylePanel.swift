import PriceTagKit
import SwiftUI

/// The right-hand sidebar: color, effects, number format and export size.
struct StylePanel: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 22) {
                section("Color") {
                    HStack(spacing: 14) {
                        ForEach(TagColor.allCases) { color in
                            ColorSwatch(color: color, selected: model.style.color == color) {
                                model.style.color = color
                            }
                        }
                    }
                    Toggle("Negative prices are always red", isOn: $model.style.autoRedForNegative)
                }

                section("Effects") {
                    EffectSlider(title: "Outline", value: $model.style.outline)
                    EffectSlider(title: "3D depth", value: $model.style.depth)
                    EffectSlider(title: "Shadow", value: $model.style.shadow)
                    EffectSlider(title: "Spacing", value: $model.style.spacing, range: -0.06...0.16)
                    Toggle("Shine", isOn: $model.style.shine)
                    Toggle("Slant", isOn: $model.style.slant)
                }

                section("Numbers") {
                    Picker("Cents", selection: $model.format.cents) {
                        Text("As typed").tag(PriceFormat.Cents.auto)
                        Text("Always").tag(PriceFormat.Cents.always)
                        Text("Never").tag(PriceFormat.Cents.never)
                    }
                    .pickerStyle(.segmented)
                    Toggle("Thousands separators (1,000)", isOn: $model.format.groupThousands)
                }

                section("Export size") {
                    Picker("Text height", selection: $model.style.textHeight) {
                        ForEach(TagStyle.textHeights, id: \.self) { height in
                            Text("\(Int(height)) px").tag(height)
                        }
                    }
                    .pickerStyle(.segmented)
                    .labelsHidden()
                    Text("Height of the numbers in the PNG. 250 px suits 1080p, 400 px or more suits 4K.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }

                section("Export folder") {
                    HStack(spacing: 8) {
                        Image(systemName: "folder.fill").foregroundStyle(.secondary)
                        Text(model.exportFolder.path.replacingOccurrences(of: NSHomeDirectory(), with: "~"))
                            .lineLimit(1)
                            .truncationMode(.middle)
                            .font(.callout)
                            .help(model.exportFolder.path)
                    }
                    HStack {
                        Button("Change…") { model.chooseExportFolder() }
                        Button("Open") { model.revealExportFolder() }
                    }
                    .controlSize(.small)
                }

                Button("Reset Look to Defaults") { model.resetStyle() }
                    .controlSize(.small)
            }
            .padding(20)
            .toggleStyle(.switch)
            .controlSize(.regular)
        }
        .background(Color(nsColor: .windowBackgroundColor))
    }

    private func section<Content: View>(_ title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(title.uppercased())
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
            content()
        }
    }
}

private struct ColorSwatch: View {
    let color: TagColor
    let selected: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            VStack(spacing: 5) {
                Circle()
                    .fill(LinearGradient(colors: [Color(hex: color.palette.top), Color(hex: color.palette.bottom)],
                                         startPoint: .top, endPoint: .bottom))
                    .overlay(Circle().strokeBorder(Color(hex: color.palette.outline), lineWidth: 3))
                    .frame(width: 38, height: 38)
                    .padding(3)
                    .overlay(Circle().strokeBorder(selected ? Color.accentColor : .clear, lineWidth: 2.5))
                Text(color.name)
                    .font(.caption)
                    .foregroundStyle(selected ? .primary : .secondary)
            }
        }
        .buttonStyle(.plain)
        .help(color.name)
    }
}

private struct EffectSlider: View {
    let title: String
    @Binding var value: Double
    var range: ClosedRange<Double> = 0...1

    var body: some View {
        HStack(spacing: 10) {
            Text(title)
                .frame(width: 70, alignment: .leading)
            Slider(value: $value, in: range)
                .controlSize(.small)
        }
    }
}

extension Color {
    init(hex: UInt32) {
        self.init(.sRGB,
                  red: Double((hex >> 16) & 0xFF) / 255,
                  green: Double((hex >> 8) & 0xFF) / 255,
                  blue: Double(hex & 0xFF) / 255)
    }
}
