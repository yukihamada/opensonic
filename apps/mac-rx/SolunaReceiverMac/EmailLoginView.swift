//
//  EmailLoginView.swift
//  SolunaReceiverMac
//
//  Passwordless email login flow (request code → verify)
//

import SwiftUI

struct EmailLoginView: View {
    @ObservedObject var auth: AuthManager
    @Environment(\.dismiss) private var dismiss

    @State private var email = ""
    @State private var code = ""

    var body: some View {
        VStack(spacing: 20) {
            // Icon
            Image(systemName: "envelope.badge")
                .font(.system(size: 40))
                .foregroundColor(.solunaGradientStart)

            Text("Login with Email")
                .font(.title2.bold())

            Text("No password needed. Enter your email\nand verify with a 6-digit code.")
                .font(.subheadline)
                .foregroundColor(.secondary)
                .multilineTextAlignment(.center)

            switch auth.state {
            case .unauthenticated:
                emailInputSection
            case .codeSent(let sentEmail):
                codeInputSection(email: sentEmail)
            case .authenticated(let email):
                authenticatedSection(email: email)
            }

            if let error = auth.error {
                Text(error)
                    .font(.caption)
                    .foregroundColor(.red)
            }
        }
        .padding(32)
        .frame(width: 360)
    }

    private var emailInputSection: some View {
        VStack(spacing: 12) {
            TextField("Email address", text: $email)
                .textFieldStyle(.roundedBorder)
                .disableAutocorrection(true)

            Button {
                Task { await auth.requestCode(email: email) }
            } label: {
                HStack {
                    if auth.isLoading { ProgressView().controlSize(.small) }
                    Text("Send Code")
                }
                .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .tint(.solunaGradientStart)
            .disabled(email.isEmpty || !email.contains("@") || auth.isLoading)
        }
    }

    private func codeInputSection(email: String) -> some View {
        VStack(spacing: 12) {
            Text("Code sent to \(email)")
                .font(.subheadline)
                .foregroundColor(.secondary)

            TextField("6-digit code", text: $code)
                .textFieldStyle(.roundedBorder)
                .font(.system(size: 24, weight: .bold, design: .monospaced))
                .multilineTextAlignment(.center)
                .onChange(of: code) { newValue in
                    code = String(newValue.prefix(6).filter { $0.isNumber })
                    if code.count == 6 {
                        Task { await auth.verifyCode(email: email, code: code) }
                    }
                }

            Button {
                Task { await auth.verifyCode(email: email, code: code) }
            } label: {
                HStack {
                    if auth.isLoading { ProgressView().controlSize(.small) }
                    Text("Verify")
                }
                .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .tint(.solunaGradientStart)
            .disabled(code.count != 6 || auth.isLoading)

            Button("Change email") {
                auth.state = .unauthenticated
                code = ""
            }
            .font(.caption)
        }
    }

    private func authenticatedSection(email: String) -> some View {
        VStack(spacing: 12) {
            Image(systemName: "checkmark.circle.fill")
                .font(.system(size: 40))
                .foregroundColor(.solunaLive)

            Text("Verified!")
                .font(.title3.bold())

            Text(email)
                .foregroundColor(.secondary)

            Button("Done") { dismiss() }
                .buttonStyle(.borderedProminent)
                .tint(.solunaGradientStart)
        }
    }
}
