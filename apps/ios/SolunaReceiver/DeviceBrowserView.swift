//
//  DeviceBrowserView.swift
//  Soluna
//
//  Discover nearby solunad devices via Bonjour and connect directly.
//

import SwiftUI
import Network

// MARK: - Model

struct SolunaLocalDevice: Identifiable, Equatable {
    let id: String       // Bonjour service name or "manual:<host>"
    let name: String     // display name
    let host: String     // resolved IPv4 or hostname
    let port: Int        // relay port (default 5099 for solunad)
    var isManual: Bool = false
}

// MARK: - Browser

@MainActor
final class DeviceBrowser: NSObject, ObservableObject {
    @Published var devices: [SolunaLocalDevice] = []
    @Published var isScanning = false

    private var browser: NWBrowser?
    private var resolvers: [String: DeviceResolver] = [:]

    func startScanning() {
        stopScanning()
        devices = []
        isScanning = true

        let params = NWParameters.tcp
        let browser = NWBrowser(for: .bonjour(type: "_soluna._tcp.", domain: "local."), using: params)
        self.browser = browser

        browser.browseResultsChangedHandler = { [weak self] _, changes in
            guard let self else { return }
            Task { @MainActor in
                for change in changes {
                    switch change {
                    case .added(let result):
                        if case .service(let name, _, _, _) = result.endpoint {
                            self.resolveService(name: name)
                        }
                    case .removed(let result):
                        if case .service(let name, _, _, _) = result.endpoint {
                            self.devices.removeAll { $0.id == name }
                            self.resolvers.removeValue(forKey: name)
                        }
                    default: break
                    }
                }
            }
        }

        browser.stateUpdateHandler = { [weak self] state in
            Task { @MainActor in
                if case .failed = state { self?.isScanning = false }
            }
        }

        browser.start(queue: .main)
    }

    func stopScanning() {
        browser?.cancel()
        browser = nil
        resolvers.removeAll()
        isScanning = false
    }

    private func resolveService(name: String) {
        let svc = NetService(domain: "local.", type: "_soluna._tcp.", name: name)
        let resolver = DeviceResolver(service: svc) { [weak self] host, port in
            Task { @MainActor in
                guard let self, let host else { return }
                let device = SolunaLocalDevice(id: name, name: name, host: host, port: port ?? 5099)
                if let idx = self.devices.firstIndex(where: { $0.id == name }) {
                    self.devices[idx] = device
                } else {
                    self.devices.append(device)
                }
            }
        }
        resolvers[name] = resolver
        resolver.start()
    }
}

// MARK: - NetService Resolver

private final class DeviceResolver: NSObject, NetServiceDelegate {
    private let service: NetService
    private let completion: (String?, Int?) -> Void
    private var done = false

    init(service: NetService, completion: @escaping (String?, Int?) -> Void) {
        self.service = service
        self.completion = completion
        super.init()
    }

    func start() {
        service.delegate = self
        service.resolve(withTimeout: 3.0)
    }

    func netServiceDidResolveAddress(_ sender: NetService) {
        guard !done, let addresses = sender.addresses else { return }
        for data in addresses {
            data.withUnsafeBytes { ptr in
                let sa = ptr.baseAddress!.assumingMemoryBound(to: sockaddr.self)
                guard sa.pointee.sa_family == UInt8(AF_INET) else { return }
                let sin = ptr.baseAddress!.assumingMemoryBound(to: sockaddr_in.self)
                var addr = sin.pointee.sin_addr
                var buf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
                inet_ntop(AF_INET, &addr, &buf, socklen_t(INET_ADDRSTRLEN))
                done = true
                completion(String(cString: buf), sender.port > 0 ? sender.port : 5099)
            }
            if done { break }
        }
    }

    func netService(_ sender: NetService, didNotResolve errorDict: [String: NSNumber]) {
        guard !done else { return }
        done = true
        completion(nil, nil)
    }
}

// MARK: - View

struct DeviceBrowserView: View {
    @ObservedObject var browser: DeviceBrowser
    let connectedDeviceHost: String?
    let onSelect: (SolunaLocalDevice) -> Void
    let onDisconnect: () -> Void

    @State private var manualHost = ""
    @State private var showManualEntry = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            // Header
            HStack {
                Image(systemName: "laptopcomputer.and.iphone")
                    .foregroundStyle(.secondary)
                Text("Devices")
                    .font(.headline)
                    .foregroundStyle(.primary)
                Spacer()
                // Hide scan button when a device is connected
                if connectedDeviceHost == nil {
                    Button(action: {
                        browser.isScanning ? browser.stopScanning() : browser.startScanning()
                    }) {
                        HStack(spacing: 4) {
                            if browser.isScanning {
                                ProgressView().scaleEffect(0.7)
                            }
                            Text(browser.isScanning ? "Scanning..." : "Scan")
                                .font(.caption.weight(.medium))
                        }
                        .foregroundColor(.solunaGradientMid)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 5)
                        .background(Color.solunaGradientMid.opacity(0.12))
                        .clipShape(Capsule())
                    }
                } else {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundColor(.solunaLive)
                        .font(.system(size: 14))
                }
            }
            .padding(.horizontal, 4)

            // Connected device banner
            if let host = connectedDeviceHost {
                let devName = browser.devices.first(where: { $0.host == host })?.name ?? host
                HStack(spacing: 8) {
                    Circle().fill(Color.solunaLive).frame(width: 7, height: 7)
                        .shadow(color: .solunaLive, radius: 3)
                    Text("Connected to \(devName)")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundColor(.solunaLive)
                    Spacer()
                    Button("Disconnect") { onDisconnect() }
                        .font(.system(size: 11, weight: .medium))
                        .foregroundColor(.solunaMic)
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 8)
                .background(Color.solunaLive.opacity(0.08))
                .clipShape(RoundedRectangle(cornerRadius: 10))
            }

            // Device list
            if browser.devices.isEmpty && !browser.isScanning {
                HStack {
                    Image(systemName: "antenna.radiowaves.left.and.right.slash")
                        .foregroundColor(.white.opacity(0.2))
                    Text("No devices found nearby")
                        .font(.caption)
                        .foregroundColor(.white.opacity(0.3))
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 8)
            } else {
                ForEach(browser.devices) { device in
                    DeviceRow(
                        device: device,
                        isConnected: connectedDeviceHost == device.host,
                        onConnect: { onSelect(device) }
                    )
                }
            }

            // Manual IP entry
            if showManualEntry {
                HStack(spacing: 8) {
                    Image(systemName: "network")
                        .font(.system(size: 13))
                        .foregroundColor(.white.opacity(0.4))
                    TextField("IP Address (e.g. 192.168.1.5)", text: $manualHost)
                        .font(.system(size: 13))
                        .foregroundColor(.white)
                        .autocorrectionDisabled()
                        .autocapitalization(.none)
                        .keyboardType(.decimalPad)
                    Button("Connect") {
                        let h = manualHost.trimmingCharacters(in: .whitespaces)
                        guard !h.isEmpty else { return }
                        let device = SolunaLocalDevice(
                            id: "manual:\(h)", name: h, host: h, port: 5099, isManual: true
                        )
                        onSelect(device)
                        showManualEntry = false
                        manualHost = ""
                    }
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundColor(.solunaGradientMid)
                    .disabled(manualHost.trimmingCharacters(in: .whitespaces).isEmpty)
                }
                .padding(10)
                .background(Color.white.opacity(0.04))
                .clipShape(RoundedRectangle(cornerRadius: 10))
            }

            // Manual IP toggle
            Button(action: { showManualEntry.toggle() }) {
                HStack(spacing: 4) {
                    Image(systemName: showManualEntry ? "xmark" : "plus")
                        .font(.system(size: 10, weight: .bold))
                    Text(showManualEntry ? "Cancel" : "Enter IP manually")
                        .font(.system(size: 11, weight: .medium))
                }
                .foregroundColor(.white.opacity(0.35))
            }
        }
    }
}

// MARK: - Device Row

private struct DeviceRow: View {
    let device: SolunaLocalDevice
    let isConnected: Bool
    let onConnect: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            // Icon
            ZStack {
                Circle()
                    .fill(isConnected ? Color.solunaLive.opacity(0.15) : Color.white.opacity(0.06))
                    .frame(width: 40, height: 40)
                Image(systemName: device.isManual ? "network" : "laptopcomputer")
                    .font(.system(size: 17))
                    .foregroundColor(isConnected ? .solunaLive : .white.opacity(0.5))
            }

            // Info
            VStack(alignment: .leading, spacing: 2) {
                Text(device.name)
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundColor(isConnected ? .white : .white.opacity(0.8))
                Text("\(device.host):\(device.port)")
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundColor(.white.opacity(0.35))
            }

            Spacer()

            // Connect / Connected
            if isConnected {
                HStack(spacing: 4) {
                    Circle().fill(Color.solunaLive).frame(width: 5, height: 5)
                    Text("Live")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(.solunaLive)
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 5)
                .background(Color.solunaLive.opacity(0.1))
                .clipShape(Capsule())
            } else {
                Button("Connect", action: onConnect)
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundColor(.white)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
                    .background(Color.solunaGradientMid.opacity(0.25))
                    .clipShape(Capsule())
            }
        }
        .padding(.vertical, 4)
    }
}

#if DEBUG
#Preview {
    DeviceBrowserView(
        browser: DeviceBrowser(),
        connectedDeviceHost: nil,
        onSelect: { _ in },
        onDisconnect: {}
    )
    .padding()
    .background(Color.black)
    .preferredColorScheme(.dark)
}
#endif
