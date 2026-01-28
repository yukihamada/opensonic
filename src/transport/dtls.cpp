/**
 * DTLS Socket — OpenSSL DTLS 1.2 implementation
 * SPDX-License-Identifier: MIT
 */

#include <soluna/transport/dtls.h>

#ifdef SOLUNA_HAS_DTLS

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/evp.h>

#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace soluna::transport {

struct DtlsSocket::Impl {
    DtlsConfig config;
    std::unique_ptr<soluna::pal::UdpSocket> udp;
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    BIO* rbio = nullptr;  // read BIO (network → OpenSSL)
    BIO* wbio = nullptr;  // write BIO (OpenSSL → network)
    bool connected = false;
    soluna::pal::SocketAddress peer_addr;

    ~Impl() {
        if (ssl) {
            SSL_free(ssl); // also frees BIOs
            ssl = nullptr;
        }
        if (ctx) {
            SSL_CTX_free(ctx);
            ctx = nullptr;
        }
    }

    bool init_context() {
        const SSL_METHOD* method = (config.role == DtlsRole::Server)
            ? DTLS_server_method()
            : DTLS_client_method();

        ctx = SSL_CTX_new(method);
        if (!ctx) return false;

        SSL_CTX_set_min_proto_version(ctx, DTLS1_2_VERSION);

        if (!config.cert_file.empty() && !config.key_file.empty()) {
            if (SSL_CTX_use_certificate_file(ctx, config.cert_file.c_str(),
                                              SSL_FILETYPE_PEM) != 1) {
                return false;
            }
            if (SSL_CTX_use_PrivateKey_file(ctx, config.key_file.c_str(),
                                             SSL_FILETYPE_PEM) != 1) {
                return false;
            }
        } else {
            // Generate self-signed certificate for testing
            if (!generate_self_signed()) return false;
        }

        return true;
    }

    bool generate_self_signed() {
        EVP_PKEY* pkey = EVP_PKEY_new();
        if (!pkey) return false;

        EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        if (!pctx) { EVP_PKEY_free(pkey); return false; }

        EVP_PKEY_keygen_init(pctx);
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1);

        EVP_PKEY* gen_pkey = nullptr;
        EVP_PKEY_keygen(pctx, &gen_pkey);
        EVP_PKEY_CTX_free(pctx);

        if (!gen_pkey) { EVP_PKEY_free(pkey); return false; }

        X509* x509 = X509_new();
        if (!x509) { EVP_PKEY_free(gen_pkey); EVP_PKEY_free(pkey); return false; }

        ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
        X509_gmtime_adj(X509_get_notBefore(x509), 0);
        X509_gmtime_adj(X509_get_notAfter(x509), 365 * 24 * 3600);
        X509_set_pubkey(x509, gen_pkey);

        X509_NAME* name = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("soluna-dtls"), -1, -1, 0);
        X509_set_issuer_name(x509, name);
        X509_sign(x509, gen_pkey, EVP_sha256());

        SSL_CTX_use_certificate(ctx, x509);
        SSL_CTX_use_PrivateKey(ctx, gen_pkey);

        X509_free(x509);
        EVP_PKEY_free(gen_pkey);
        EVP_PKEY_free(pkey);
        return true;
    }

    bool init_ssl() {
        ssl = SSL_new(ctx);
        if (!ssl) return false;

        rbio = BIO_new(BIO_s_mem());
        wbio = BIO_new(BIO_s_mem());
        SSL_set_bio(ssl, rbio, wbio);

        if (config.role == DtlsRole::Server) {
            SSL_set_accept_state(ssl);
        } else {
            SSL_set_connect_state(ssl);
        }

        return true;
    }

    // Flush pending DTLS output to UDP
    void flush_wbio() {
        uint8_t buf[4096];
        int pending;
        while ((pending = BIO_read(wbio, buf, sizeof(buf))) > 0) {
            udp->send_to(buf, pending, peer_addr);
        }
    }

    // Feed incoming UDP data into DTLS
    void feed_rbio(const void* data, size_t len) {
        BIO_write(rbio, data, static_cast<int>(len));
    }
};

DtlsSocket::DtlsSocket() : impl_(std::make_unique<Impl>()) {}
DtlsSocket::~DtlsSocket() = default;

std::unique_ptr<DtlsSocket> DtlsSocket::create(const DtlsConfig& config) {
    auto sock = std::unique_ptr<DtlsSocket>(new DtlsSocket());
    sock->impl_->config = config;
    sock->impl_->udp = soluna::pal::UdpSocket::create();
    if (!sock->impl_->udp) return nullptr;

    if (!sock->impl_->init_context()) return nullptr;
    if (!sock->impl_->init_ssl()) return nullptr;

    return sock;
}

bool DtlsSocket::bind(uint16_t port) {
    return impl_->udp->bind(port);
}

bool DtlsSocket::handshake(const soluna::pal::SocketAddress& peer) {
    impl_->peer_addr = peer;

    // Set timeout for handshake
    impl_->udp->set_recv_timeout_ms(impl_->config.handshake_timeout_ms);

    int ret;
    if (impl_->config.role == DtlsRole::Client) {
        ret = SSL_do_handshake(impl_->ssl);
    } else {
        ret = SSL_do_handshake(impl_->ssl);
    }

    impl_->flush_wbio();

    // Handshake loop
    uint8_t buf[4096];
    soluna::pal::SocketAddress src;

    int attempts = 0;
    while (!SSL_is_init_finished(impl_->ssl) && attempts < 50) {
        int n = impl_->udp->recv_from(buf, sizeof(buf), src);
        if (n > 0) {
            impl_->feed_rbio(buf, n);
            ret = SSL_do_handshake(impl_->ssl);
            impl_->flush_wbio();
        }
        attempts++;
    }

    impl_->connected = SSL_is_init_finished(impl_->ssl);
    // Restore reasonable timeout
    impl_->udp->set_recv_timeout_ms(10);
    return impl_->connected;
}

int DtlsSocket::send_to(const void* data, size_t len,
                         const soluna::pal::SocketAddress& dest) {
    if (!impl_->connected) return -1;

    int written = SSL_write(impl_->ssl, data, static_cast<int>(len));
    impl_->flush_wbio();
    return written;
}

int DtlsSocket::recv_from(void* data, size_t len,
                           soluna::pal::SocketAddress& src) {
    if (!impl_->connected) return -1;

    // Read from UDP and feed into DTLS
    uint8_t buf[4096];
    int n = impl_->udp->recv_from(buf, sizeof(buf), src);
    if (n <= 0) return n;

    impl_->feed_rbio(buf, n);

    int read = SSL_read(impl_->ssl, data, static_cast<int>(len));
    return (read > 0) ? read : 0;
}

bool DtlsSocket::is_connected() const {
    return impl_->connected;
}

int DtlsSocket::fd() const {
    return impl_->udp->fd();
}

void DtlsSocket::shutdown() {
    if (impl_->ssl && impl_->connected) {
        SSL_shutdown(impl_->ssl);
        impl_->flush_wbio();
        impl_->connected = false;
    }
}

} // namespace soluna::transport

#else // !SOLUNA_HAS_DTLS

// Stub implementation when OpenSSL is not available
namespace soluna::transport {

struct DtlsSocket::Impl {};

DtlsSocket::DtlsSocket() : impl_(std::make_unique<Impl>()) {}
DtlsSocket::~DtlsSocket() = default;

std::unique_ptr<DtlsSocket> DtlsSocket::create(const DtlsConfig&) {
    return nullptr; // DTLS not available
}

bool DtlsSocket::bind(uint16_t) { return false; }
bool DtlsSocket::handshake(const soluna::pal::SocketAddress&) { return false; }
int DtlsSocket::send_to(const void*, size_t, const soluna::pal::SocketAddress&) { return -1; }
int DtlsSocket::recv_from(void*, size_t, soluna::pal::SocketAddress&) { return -1; }
bool DtlsSocket::is_connected() const { return false; }
int DtlsSocket::fd() const { return -1; }
void DtlsSocket::shutdown() {}

} // namespace soluna::transport

#endif // SOLUNA_HAS_DTLS
