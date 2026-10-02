import PriceTagKit
import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var model: AppModel
    @FocusState private var inputFocused: Bool

    var body: some View {
        HStack(spacing: 0) {
            VStack(spacing: 16) {
                priceField
                PreviewCanvas()
                ActionBar()
            }
            .padding(20)
            .frame(maxWidth: .infinity, maxHeight: .infinity)

            Divider()

            StylePanel()
                .frame(width: 290)
        }
        .overlay(alignment: .bottom) { ToastView().padding(.bottom, 84) }
        .sheet(isPresented: $model.showingBatch) { BatchExportView().environmentObject(model) }
        .onAppear { inputFocused = true }
    }

    private var priceField: some View {
        HStack(spacing: 12) {
            Image(systemName: "dollarsign.circle.fill")
                .font(.system(size: 26))
                .foregroundStyle(.green)
            TextField("Type a price, like 12.50", text: $model.input)
                .textFieldStyle(.plain)
                .font(.system(size: 30, weight: .bold, design: .rounded))
                .focused($inputFocused)
                .onSubmit { model.copyToClipboard() }
            if !model.input.isEmpty {
                Button {
                    model.input = ""
                    inputFocused = true
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.system(size: 18))
                        .foregroundStyle(.tertiary)
                }
                .buttonStyle(.plain)
                .help("Clear")
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
        .background(RoundedRectangle(cornerRadius: 14).fill(Color(nsColor: .textBackgroundColor)))
        .overlay(
            RoundedRectangle(cornerRadius: 14)
                .strokeBorder(inputFocused ? Color.accentColor.opacity(0.7) : Color.primary.opacity(0.12),
                              lineWidth: inputFocused ? 2 : 1)
        )
    }
}

/// The buttons under the preview.
struct ActionBar: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        HStack(spacing: 10) {
            Group {
                Button {
                    model.copyToClipboard()
                } label: {
                    Label("Copy", systemImage: "doc.on.doc")
                }
                .help("Copy the PNG to the clipboard (Return)")

                Button {
                    model.saveToExportFolder()
                } label: {
                    Label("Save to Folder", systemImage: "square.and.arrow.down")
                }
                .buttonStyle(.borderedProminent)
                .help("Save into \(model.exportFolder.lastPathComponent) (⌘Return)")

                Button {
                    model.saveAs()
                } label: {
                    Label("Save As…", systemImage: "square.and.arrow.down.on.square")
                }
                .help("Choose where to save (⌘S)")
            }
            .disabled(model.price == nil)

            Spacer()

            Button {
                model.showingBatch = true
            } label: {
                Label("Batch…", systemImage: "square.stack.3d.up")
            }
            .help("Export a whole list of prices at once (⌘B)")
        }
        .controlSize(.large)
    }
}

struct ToastView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        if let toast = model.toast {
            HStack(spacing: 8) {
                Image(systemName: toast.symbol)
                Text(toast.message).lineLimit(1)
                if let action = toast.action {
                    Button("Show", action: action)
                        .buttonStyle(.link)
                }
            }
            .font(.system(size: 13, weight: .medium))
            .padding(.horizontal, 16)
            .padding(.vertical, 10)
            .background(.regularMaterial, in: Capsule())
            .shadow(color: .black.opacity(0.18), radius: 10, y: 4)
            .transition(.move(edge: .bottom).combined(with: .opacity))
            .id(toast.id)
        }
    }
}
