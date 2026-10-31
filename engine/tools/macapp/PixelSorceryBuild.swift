// PixelSorceryBuild.swift - the PixelSorcery app (engine/tools/macapp/make_app.sh packages it for
// this checkout, make_dmg.sh as a standalone download): a native window over build_ui.py --engine,
// which finds the games in a ROM folder by content, puts them in ../roms and builds the chosen
// titles. The app shows and sends; the engine does the work.
// Standalone (Resources/payload in the bundle): the engine's tree is copied to
// ~/Library/Application Support/PixelSorcery/workspace by Resources/setup_workspace.sh (Python
// tools from the bundled wheels, SDL2 bundled); games are built and ROMs kept beside it.
// Engine protocol (build_ui.py): commands "scan FOLDER", "start CORES PROCS CHECK TITLE...",
// "play TITLE", "stop", "quit"; state lines S|folder|running|scanning|cpus, T|...per title..., N|note, E.
import SwiftUI
import AppKit
import Combine

struct Title: Identifiable, Equatable {
    var id: String            // games/ folder
    var code = "", name = "", rom = "missing", romDetail = "", stage = "", result = ""
    var done = 0, total = 0, seconds = 0, built = 0   // built: the native build's time (0: none)
    var usable: Bool { ["ready", "link", "make"].contains(rom) }
}

@MainActor
final class Engine: ObservableObject {
    @Published var titles: [Title] = []
    @Published var folder = ""
    @Published var running = false
    @Published var scanning = true
    @Published var cpus = ProcessInfo.processInfo.activeProcessorCount
    @Published var notes: [String] = []
    @Published var engineError = ""
    @Published var nativeOnly = false     // no emulator core: native ports only, no self-check
    @Published var setup = ""             // first-run setup step, empty once ready
    var repo: String
    private var proc: Process?
    private var input: FileHandle?
    private var pending: [String] = [], buffer = Data()

    init() {
        repo = Bundle.main.object(forInfoDictionaryKey: "PSRepo") as? String
            ?? ProcessInfo.processInfo.environment["PS_REPO"] ?? FileManager.default.currentDirectoryPath
        if let res = Bundle.main.resourcePath, FileManager.default.fileExists(atPath: res + "/payload") {
            prepareWorkspace(res)
        } else {
            start()
        }
    }

    var env: [String: String] = ProcessInfo.processInfo.environment

    // standalone: copy the engine out of the bundle and install its Python tools, then start
    private func prepareWorkspace(_ res: String) {
        let ws = NSHomeDirectory() + "/Library/Application Support/PixelSorcery/workspace"
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0"
        repo = ws
        env["PATH"] = ws + "/engine/.venv/bin:/usr/bin:/bin:/usr/sbin:/sbin"
        env["CMAKE_PREFIX_PATH"] = ws + "/engine/ext/sdl2"
        env["PS_VERSION"] = version
        setup = "Setting up…"
        let p = Process(), out = Pipe()
        p.executableURL = URL(fileURLWithPath: "/bin/zsh")
        p.arguments = [res + "/setup_workspace.sh", res + "/payload", res + "/wheels", ws, version]
        p.standardOutput = out; p.standardError = out
        out.fileHandleForReading.readabilityHandler = { [weak self] h in
            let lines = String(decoding: h.availableData, as: UTF8.self).split(separator: "\n")
            Task { @MainActor in for l in lines { self?.setupLine(String(l)) } }
        }
        p.terminationHandler = { [weak self] t in
            Task { @MainActor in
                guard let self else { return }
                if t.terminationStatus == 0 { self.setup = ""; self.start() }
                else if self.engineError.isEmpty { self.engineError = "Setup failed (see \(ws)/setup.log)" }
            }
        }
        do { try p.run() } catch { engineError = "cannot run the setup: \(error.localizedDescription)" }
    }

    private func setupLine(_ l: String) {
        if l.hasPrefix("STEP ") { setup = String(l.dropFirst(5)) + "…" }
        else if l == "NEEDTOOLS" {
            engineError = "PixelSorcery compiles the games on your Mac: install Apple's Command Line Tools (the dialog just opened), then reopen the app."
        }
    }

    func start() {
        let p = Process(), inp = Pipe(), out = Pipe()
        p.executableURL = URL(fileURLWithPath: repo + "/engine/.venv/bin/python")
        p.arguments = [repo + "/engine/tools/build_ui.py", "--engine"]
        p.currentDirectoryURL = URL(fileURLWithPath: repo)
        p.environment = env
        p.standardInput = inp; p.standardOutput = out; p.standardError = FileHandle.nullDevice
        out.fileHandleForReading.readabilityHandler = { [weak self] h in
            let d = h.availableData
            Task { @MainActor in self?.receive(d) }
        }
        do { try p.run() } catch { engineError = "cannot start the engine: \(error.localizedDescription) (\(repo))"; return }
        proc = p; input = inp.fileHandleForWriting
    }

    func send(_ line: String) { input?.write((line + "\n").data(using: .utf8)!) }
    func quit() { send("quit"); proc?.waitUntilExit() }

    private func receive(_ d: Data) {
        buffer.append(d)
        while let nl = buffer.firstIndex(of: 0x0A) {
            let line = String(decoding: buffer[buffer.startIndex..<nl], as: UTF8.self)
            buffer.removeSubrange(buffer.startIndex...nl)
            if line == "E" { apply(pending); pending.removeAll() } else { pending.append(line) }
        }
    }

    private func apply(_ lines: [String]) {
        var ts: [Title] = [], ns: [String] = []
        for l in lines {
            let f = l.components(separatedBy: "|")
            switch f.first {
            case "S" where f.count >= 5:
                folder = f[1]; running = f[2] == "1"; scanning = f[3] == "1"; cpus = Int(f[4]) ?? cpus
                nativeOnly = f.count > 5 && f[5] == "1"
            case "T" where f.count >= 11:
                ts.append(Title(id: f[1], code: f[2], name: f[3], rom: f[4], romDetail: f[5], stage: f[6],
                                result: f[10], done: Int(f[7]) ?? 0, total: Int(f[8]) ?? 0, seconds: Int(f[9]) ?? 0,
                                built: f.count > 11 ? Int(f[11]) ?? 0 : 0))
            case "N": ns.append(f.dropFirst().joined(separator: "|"))
            default: break
            }
        }
        if ts != titles { titles = ts }
        notes = ns
    }
}

struct ContentView: View {
    @StateObject var engine = Engine()
    @AppStorage("processes") var processes = 2
    @AppStorage("cores") var cores = 0
    @AppStorage("check") var check = false
    @State var chosen: Set<String> = []
    @State var chosenInit = false
    @State var selection: String?
    @State var log = ""
    let timer = Timer.publish(every: 1, on: .main, in: .common).autoconnect()

    var available: [Title] { engine.titles.filter(\.usable) }
    var suggested: Int { check ? max(1, effCores / 2) : 2 }
    var effCores: Int { cores == 0 ? engine.cpus : min(cores, engine.cpus) }
    var perProcess: Int { max(1, effCores / max(1, min(processes, max(1, chosen.count)))) }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if !engine.engineError.isEmpty { Text(engine.engineError).foregroundStyle(.red) }
            if !engine.setup.isEmpty { HStack { ProgressView().controlSize(.small); Text(engine.setup) } }
            // ROM folder
            HStack {
                Text("ROM folder").bold().frame(width: 90, alignment: .leading)
                Text(engine.folder.isEmpty ? "…" : engine.folder).lineLimit(1).truncationMode(.middle)
                    .foregroundStyle(.secondary).frame(maxWidth: .infinity, alignment: .leading)
                if engine.scanning { ProgressView().controlSize(.small); Text("scanning…").foregroundStyle(.secondary) }
                Button("Choose…") { chooseFolder() }.disabled(engine.running)
                Button("Rescan") { engine.send("scan " + engine.folder) }.disabled(engine.running || engine.scanning)
            }
            // settings
            HStack(spacing: 28) {
                Stepper(value: $processes, in: 1...max(1, engine.cpus)) {
                    Text("Processes  \(processes)").monospacedDigit()
                }.help("Titles built at the same time")
                Stepper(value: Binding(get: { effCores }, set: { cores = $0 }), in: 1...max(1, engine.cpus)) {
                    Text("Cores  \(effCores) of \(engine.cpus)").monospacedDigit()
                }
                if !engine.nativeOnly {     // the self-check replays against the emulator core
                    Picker("", selection: $check) {
                        Text("Build").tag(false); Text("Build + self-check").tag(true)
                    }.pickerStyle(.segmented).frame(width: 260)
                        .onChange(of: check) { _, _ in processes = suggested }
                }
            }.disabled(engine.running)
            Text("\(perProcess) cores per process · suggested \(suggested) processes for \(check ? "self-check (mostly one core per title)" : "a build (one generating while one compiles)")")
                .font(.caption).foregroundStyle(.secondary)
            Divider()
            Toggle(isOn: Binding(get: { !available.isEmpty && available.allSatisfy { chosen.contains($0.id) } },
                                 set: { on in chosen = on ? Set(available.map(\.id)) : [] })) {
                Text("All found in the ROM folder (\(available.count) of \(engine.titles.count) titles, \(chosen.count) chosen)").bold()
            }.disabled(engine.running)
            Table(engine.titles, selection: $selection) {
                TableColumn("") { t in
                    Toggle("", isOn: Binding(get: { chosen.contains(t.id) },
                                             set: { on in if on { chosen.insert(t.id) } else { chosen.remove(t.id) } }))
                        .labelsHidden().disabled(!t.usable || engine.running)
                }.width(24)
                TableColumn("Code") { t in Text(t.code).font(.body.monospaced()) }.width(60)
                TableColumn("Game") { t in Text(t.name) }.width(min: 170, ideal: 220)
                TableColumn("ROM") { t in romLabel(t) }.width(min: 120, ideal: 160)
                TableColumn("Progress") { t in progress(t) }.width(min: 160, ideal: 220)
                TableColumn("Time") { t in Text(t.seconds > 0 ? String(format: "%d:%02d", t.seconds / 60, t.seconds % 60) : "").monospacedDigit() }.width(min: 50, ideal: 56)
                TableColumn("") { t in playButtons(t) }.width(min: 150, ideal: 160)
            }
            .frame(minHeight: 330)
            // selected title: its log
            GroupBox(selection.flatMap { s in engine.titles.first { $0.id == s }?.name } ?? "Log (select a title)") {
                ScrollView { Text(log).font(.system(size: 11, design: .monospaced)).frame(maxWidth: .infinity, alignment: .leading).textSelection(.enabled) }
                    .frame(height: 110)
            }
            HStack {
                ForEach(engine.notes, id: \.self) { Text($0).font(.caption).foregroundStyle(.secondary) }
                Spacer()
                if engine.running {
                    Button("Stop") { engine.send("stop") }.keyboardShortcut(.cancelAction)
                } else {
                    Button("Start") {
                        let order = engine.titles.map(\.id).filter { chosen.contains($0) }
                        engine.send("start \(effCores) \(processes) \(check ? 1 : 0) " + order.joined(separator: " "))
                    }.keyboardShortcut(.defaultAction).disabled(chosen.isEmpty || engine.scanning)
                }
            }
        }
        .padding(16)
        .frame(minWidth: 900, minHeight: 680)
        .onChange(of: engine.titles) { _, ts in        // after each scan: everything found, chosen
            let avail = Set(ts.filter(\.usable).map(\.id))
            if !chosenInit || !chosen.isSubset(of: avail) { chosen = avail; chosenInit = !avail.isEmpty }
        }
        .onReceive(timer) { _ in readLog() }
        .onChange(of: selection) { _, _ in readLog() }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.willTerminateNotification)) { _ in engine.quit() }
    }

    // Play: the last build (play.sh rebuilds it first only if the code changed); Rebuild: this title alone
    @ViewBuilder func playButtons(_ t: Title) -> some View {
        HStack(spacing: 6) {
            Button(t.built > 0 ? "Play" : "Build & play") { engine.send("play " + t.id) }
                .help(t.built > 0 ? "Last build: " + Date(timeIntervalSince1970: Double(t.built)).formatted(date: .abbreviated, time: .shortened) : "Not built yet")
            if t.built > 0 {
                Button("Rebuild") { engine.send("start \(effCores) 1 \(check ? 1 : 0) " + t.id) }
            }
        }.controlSize(.small).disabled(!t.usable || engine.running)
    }

    func romLabel(_ t: Title) -> some View {
        let (text, color): (String, Color) = switch t.rom {
            case "ready": ("ROM ok", .green)
            case "link": ("found: " + t.romDetail, .green)
            case "make": ("made from " + t.romDetail, .green)
            case "other dump": ("other dump (not verified)", .orange)
            default: ("no ROM", .red)
        }
        return Label(text, systemImage: "circle.fill").labelStyle(DotLabel(color: color)).lineLimit(1).help(t.romDetail)
    }

    @ViewBuilder func progress(_ t: Title) -> some View {
        if t.stage == "done" || (!t.result.isEmpty && t.stage.isEmpty) {
            let ok = t.result == "OK" || t.result == "SELF-CHECK OK"
            Label(t.result, systemImage: ok ? "checkmark.circle.fill" : "xmark.octagon.fill").foregroundStyle(ok ? .green : .red)
        } else if t.stage == "waiting" {
            Text("waiting").foregroundStyle(.secondary)
        } else if t.total > 0 {
            ProgressView(value: Double(t.done), total: Double(t.total)) { EmptyView() } currentValueLabel: {
                Text("\(t.stage) \(t.done * 100 / max(1, t.total))%").font(.caption)
            }
        } else if !t.stage.isEmpty {
            HStack { ProgressView().controlSize(.small); Text(t.stage).font(.caption) }
        }
    }

    func readLog() {
        guard let s = selection else { log = ""; return }
        let path = engine.repo + "/games/\(s)/build_all.log"
        guard let d = FileManager.default.contents(atPath: path) else { log = "(no log yet)"; return }
        log = String(decoding: d.suffix(6000), as: UTF8.self).components(separatedBy: "\n").suffix(40).joined(separator: "\n")
    }

    func chooseFolder() {
        let p = NSOpenPanel()
        p.canChooseDirectories = true; p.canChooseFiles = false; p.allowsMultipleSelection = false
        p.message = "The folder holding your game dumps (any names, zips too)"
        if p.runModal() == .OK, let url = p.url { engine.send("scan " + url.path) }
    }
}

struct DotLabel: LabelStyle {
    var color: Color
    func makeBody(configuration: Configuration) -> some View {
        HStack(spacing: 6) { configuration.icon.font(.system(size: 7)).foregroundStyle(color); configuration.title }
    }
}

@main
struct PixelSorceryBuildApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    var body: some Scene {
        WindowGroup("PixelSorcery") { ContentView() }
            .windowResizability(.contentMinSize)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}
