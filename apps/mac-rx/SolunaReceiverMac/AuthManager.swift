//
//  AuthManager.swift
//  SolunaReceiver
//
//  Passwordless email authentication manager
//

import Foundation
import Combine

@MainActor
class AuthManager: ObservableObject {
    static let shared = AuthManager()

    enum AuthState {
        case unauthenticated
        case codeSent(email: String)
        case authenticated(email: String)
    }

    @Published var state: AuthState = .unauthenticated
    @Published var isLoading = false
    @Published var error: String?

    @Published private(set) var userEmail: String?
    @Published private(set) var userId: String?
    @Published private(set) var devices: [String] = []

    private let relayBase = "https://relay.solun.art"
    private let tokenKey = "soluna_auth_token"
    private let emailKey = "soluna_user_email"

    private init() {
        // Restore session from keychain
        if let token = UserDefaults.standard.string(forKey: tokenKey),
           let email = UserDefaults.standard.string(forKey: emailKey),
           !token.isEmpty, !email.isEmpty {
            userEmail = email
            state = .authenticated(email: email)
            // Verify token is still valid
            Task { await fetchMe() }
        }
    }

    var isAuthenticated: Bool {
        if case .authenticated = state { return true }
        return false
    }

    var authToken: String? {
        UserDefaults.standard.string(forKey: tokenKey)
    }

    // MARK: - Request verification code

    func requestCode(email: String) async {
        isLoading = true
        error = nil
        defer { isLoading = false }

        guard let url = URL(string: "\(relayBase)/api/auth/request-code") else { return }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try? JSONSerialization.data(withJSONObject: ["email": email])

        do {
            let (data, response) = try await URLSession.shared.data(for: request)
            guard let http = response as? HTTPURLResponse else { return }

            if http.statusCode == 200 {
                state = .codeSent(email: email)
            } else if http.statusCode == 429 {
                error = "しばらく待ってから再試行してください"
            } else {
                let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
                error = json?["error"] as? String ?? "送信に失敗しました"
            }
        } catch {
            self.error = "ネットワークエラー: \(error.localizedDescription)"
        }
    }

    // MARK: - Verify code

    func verifyCode(email: String, code: String) async {
        isLoading = true
        error = nil
        defer { isLoading = false }

        // Read device ID from UserDefaults (same as AudioReceiver.deviceId)
        let key = "soluna_device_id"
        let deviceId: String
        if let existing = UserDefaults.standard.string(forKey: key), !existing.isEmpty {
            deviceId = existing
        } else {
            let newId = UUID().uuidString.lowercased()
            UserDefaults.standard.set(newId, forKey: key)
            deviceId = newId
        }

        guard let url = URL(string: "\(relayBase)/api/auth/verify") else { return }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try? JSONSerialization.data(withJSONObject: [
            "email": email,
            "code": code,
            "device_id": deviceId
        ])

        do {
            let (data, response) = try await URLSession.shared.data(for: request)
            guard let http = response as? HTTPURLResponse else { return }

            if http.statusCode == 200,
               let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
               let token = json["token"] as? String {
                // Save token and email
                UserDefaults.standard.set(token, forKey: tokenKey)
                UserDefaults.standard.set(email, forKey: emailKey)
                userEmail = email
                userId = json["user_id"] as? String
                devices = json["devices"] as? [String] ?? []
                state = .authenticated(email: email)
            } else {
                let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
                error = json?["error"] as? String ?? "認証に失敗しました"
            }
        } catch {
            self.error = "ネットワークエラー: \(error.localizedDescription)"
        }
    }

    // MARK: - Fetch user info

    func fetchMe() async {
        guard let token = authToken else { return }
        guard let url = URL(string: "\(relayBase)/api/auth/me") else { return }
        var request = URLRequest(url: url)
        request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")

        do {
            let (data, response) = try await URLSession.shared.data(for: request)
            guard let http = response as? HTTPURLResponse else { return }

            if http.statusCode == 200,
               let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                userEmail = json["email"] as? String
                userId = json["user_id"] as? String
                devices = json["devices"] as? [String] ?? []
            } else if http.statusCode == 401 {
                // Token expired
                logout()
            }
        } catch {
            // Network error — keep existing session
        }
    }

    // MARK: - Link device

    func linkDevice(_ deviceId: String) async {
        guard let token = authToken else { return }
        guard let url = URL(string: "\(relayBase)/api/auth/link-device") else { return }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        request.httpBody = try? JSONSerialization.data(withJSONObject: ["device_id": deviceId])

        do {
            let (_, response) = try await URLSession.shared.data(for: request)
            if let http = response as? HTTPURLResponse, http.statusCode == 200 {
                if !devices.contains(deviceId) {
                    devices.append(deviceId)
                }
            }
        } catch {}
    }

    // MARK: - Short Device ID

    /// Deterministic 5-char short ID from a UUID string, e.g. "#A3F2K"
    static func shortId(from uuid: String) -> String {
        let clean = uuid.replacingOccurrences(of: "-", with: "").uppercased()
        // Simple FNV-1a-like hash over the hex bytes for determinism
        var h: UInt64 = 0xcbf29ce484222325
        for ch in clean.utf8 {
            h ^= UInt64(ch)
            h &*= 0x100000001b3
        }
        // Encode lowest 25 bits as 5 base-36 characters (0-9A-Z)
        let chars = Array("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ")
        var val = h & 0x1FFFFFF // 25 bits → enough for 5 base-36 digits
        var result = ""
        for _ in 0..<5 {
            result.append(chars[Int(val % 36)])
            val /= 36
        }
        return "#\(result)"
    }

    /// Short device ID for the current device
    var shortDeviceId: String {
        let key = "soluna_device_id"
        if let uuid = UserDefaults.standard.string(forKey: key), !uuid.isEmpty {
            return Self.shortId(from: uuid)
        }
        return "#-----"
    }

    // MARK: - Logout

    func logout() {
        UserDefaults.standard.removeObject(forKey: tokenKey)
        UserDefaults.standard.removeObject(forKey: emailKey)
        userEmail = nil
        userId = nil
        devices = []
        state = .unauthenticated
    }
}
