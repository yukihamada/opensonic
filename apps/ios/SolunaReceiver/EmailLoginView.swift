//
//  EmailLoginView.swift
//  Soluna
//
//  Passwordless email login flow (request code → verify)
//

import SwiftUI

struct EmailLoginView: View {
    @ObservedObject var auth: AuthManager
    @Environment(\.dismiss) private var dismiss

    @State private var email = ""
    @State private var code = ""
    @State private var usernameInput = ""
    @State private var usernameAvailable: Bool? = nil
    @State private var isCheckingUsername = false
    @State private var usernameSaved = false
    @FocusState private var codeFocused: Bool

    var body: some View {
        NavigationView {
            ZStack {
                LinearGradient.solunaBg.ignoresSafeArea()

                VStack(spacing: 24) {
                    // Icon
                    Image(systemName: "envelope.badge")
                        .font(.system(size: 48))
                        .foregroundColor(.solunaGradientStart)
                        .padding(.top, 32)

                    Text("メールで登録")
                        .font(.title2.bold())
                        .foregroundColor(.white)

                    Text("パスワード不要。メールアドレスに届く\n6桁のコードで認証します。")
                        .font(.subheadline)
                        .foregroundColor(.white.opacity(0.7))
                        .multilineTextAlignment(.center)

                    switch auth.state {
                    case .unauthenticated:
                        emailInputSection
                    case .codeSent(let sentEmail):
                        codeInputSection(email: sentEmail)
                    case .authenticated(let email):
                        if auth.username == nil && !usernameSaved {
                            usernameSetupSection(email: email)
                        } else {
                            authenticatedSection(email: email)
                        }
                    }

                    if let error = auth.error {
                        Text(error)
                            .font(.caption)
                            .foregroundColor(.red)
                            .padding(.horizontal)
                    }

                    Spacer()
                }
                .padding()
            }
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button("閉じる") { dismiss() }
                        .foregroundColor(.solunaGradientStart)
                }
            }
        }
    }

    // MARK: - Email input

    private var emailInputSection: some View {
        VStack(spacing: 16) {
            TextField("メールアドレス", text: $email)
                .textContentType(.emailAddress)
                .keyboardType(.emailAddress)
                .autocapitalization(.none)
                .disableAutocorrection(true)
                .padding()
                .background(Color.white.opacity(0.1))
                .cornerRadius(12)
                .foregroundColor(.white)

            Button {
                Task { await auth.requestCode(email: email) }
            } label: {
                HStack {
                    if auth.isLoading {
                        ProgressView().tint(.white)
                    }
                    Text("コードを送信")
                        .fontWeight(.semibold)
                }
                .frame(maxWidth: .infinity)
                .padding()
                .background(LinearGradient.solunaAccent)
                .foregroundColor(.white)
                .cornerRadius(12)
            }
            .disabled(email.isEmpty || !email.contains("@") || auth.isLoading)
            .opacity(email.isEmpty || !email.contains("@") ? 0.5 : 1)
        }
    }

    // MARK: - Code input

    private func codeInputSection(email: String) -> some View {
        VStack(spacing: 16) {
            Text("\(email) に送信しました")
                .font(.subheadline)
                .foregroundColor(.white.opacity(0.7))

            TextField("6桁のコード", text: $code)
                .keyboardType(.numberPad)
                .multilineTextAlignment(.center)
                .font(.system(size: 32, weight: .bold, design: .monospaced))
                .padding()
                .background(Color.white.opacity(0.1))
                .cornerRadius(12)
                .foregroundColor(.white)
                .focused($codeFocused)
                .onChange(of: code) { newValue in
                    // Limit to 6 digits
                    code = String(newValue.prefix(6).filter { $0.isNumber })
                    if code.count == 6 {
                        Task { await auth.verifyCode(email: email, code: code) }
                    }
                }
                .onAppear { codeFocused = true }

            Button {
                Task { await auth.verifyCode(email: email, code: code) }
            } label: {
                HStack {
                    if auth.isLoading {
                        ProgressView().tint(.white)
                    }
                    Text("認証する")
                        .fontWeight(.semibold)
                }
                .frame(maxWidth: .infinity)
                .padding()
                .background(LinearGradient.solunaAccent)
                .foregroundColor(.white)
                .cornerRadius(12)
            }
            .disabled(code.count != 6 || auth.isLoading)

            Button("メールアドレスを変更") {
                auth.state = .unauthenticated
                code = ""
            }
            .font(.caption)
            .foregroundColor(.solunaGradientStart)
        }
    }

    // MARK: - Username setup

    private func usernameSetupSection(email: String) -> some View {
        VStack(spacing: 16) {
            Image(systemName: "at")
                .font(.system(size: 40))
                .foregroundColor(.solunaGradientStart)

            Text("ユーザー名を設定")
                .font(.title3.bold())
                .foregroundColor(.white)

            Text("@ユーザー名で他の人に繋がることができます")
                .font(.caption)
                .foregroundColor(.white.opacity(0.6))
                .multilineTextAlignment(.center)

            HStack(spacing: 0) {
                Text("@")
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundColor(.solunaGradientStart)
                    .padding(.leading, 12)
                TextField("ユーザー名 (3〜30文字)", text: $usernameInput)
                    .autocapitalization(.none)
                    .autocorrectionDisabled()
                    .padding(.vertical, 14)
                    .padding(.trailing, 12)
                    .foregroundColor(.white)
                    .onChange(of: usernameInput) { _ in
                        usernameAvailable = nil
                    }
                if isCheckingUsername {
                    ProgressView().scaleEffect(0.8).padding(.trailing, 12)
                } else if let avail = usernameAvailable {
                    Image(systemName: avail ? "checkmark.circle.fill" : "xmark.circle.fill")
                        .foregroundColor(avail ? .solunaLive : .solunaMic)
                        .padding(.trailing, 12)
                }
            }
            .background(Color.white.opacity(0.1))
            .cornerRadius(12)

            Button {
                Task {
                    isCheckingUsername = true
                    let avail = await auth.checkUsername(usernameInput)
                    isCheckingUsername = false
                    usernameAvailable = avail
                    if avail {
                        let ok = await auth.setUsername(usernameInput)
                        if ok { usernameSaved = true }
                    }
                }
            } label: {
                HStack {
                    if auth.isLoading { ProgressView().tint(.white) }
                    Text(usernameAvailable == false ? "この名前は使用中です" : "設定する")
                        .fontWeight(.semibold)
                }
                .frame(maxWidth: .infinity)
                .padding()
                .background(LinearGradient.solunaAccent)
                .foregroundColor(.white)
                .cornerRadius(12)
            }
            .disabled(usernameInput.count < 3 || auth.isLoading)
            .opacity(usernameInput.count < 3 ? 0.5 : 1)

            Button("スキップ") { usernameSaved = true }
                .font(.caption)
                .foregroundColor(.white.opacity(0.4))
        }
    }

    // MARK: - Authenticated

    private func authenticatedSection(email: String) -> some View {
        VStack(spacing: 16) {
            Image(systemName: "checkmark.circle.fill")
                .font(.system(size: 48))
                .foregroundColor(.solunaLive)

            Text("認証完了")
                .font(.title3.bold())
                .foregroundColor(.white)

            Text(email)
                .font(.subheadline)
                .foregroundColor(.white.opacity(0.7))

            Button("OK") { dismiss() }
                .font(.headline)
                .frame(maxWidth: .infinity)
                .padding()
                .background(LinearGradient.solunaAccent)
                .foregroundColor(.white)
                .cornerRadius(12)
        }
    }
}
