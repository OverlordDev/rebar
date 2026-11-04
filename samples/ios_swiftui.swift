// An iOS SwiftUI app (platform 2) run from an IPA by tools/ipa.py: state, a button action, a List and a NavigationStack, exits by itself.
import SwiftUI
import UIKit

struct ContentView: View {
    @State private var count = 0
    var body: some View {
        NavigationStack {
            VStack(spacing: 16) {
                Text("Count: \(count)").font(.largeTitle)
                Button("Add") { count += 1 }.buttonStyle(.borderedProminent)
                List(["alpha", "beta", "gamma"], id: \.self) { Text($0) }
            }
            .padding()
            .navigationTitle("SwiftUI under rebar")
        }
        .onAppear {
            print("appeared count \(count)"); fflush(stdout)
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { print("bye"); fflush(stdout); exit(0) }
        }
    }
}

@main struct DemoApp: App {
    var body: some Scene { WindowGroup { ContentView() } }
}
