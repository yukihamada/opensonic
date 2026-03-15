/**
 * soluna-win — Soluna Windows GUI client (RX + TX)
 *
 * Single .exe, no external dependencies.
 * Dark UI with level meter, waveform, channel management.
 *
 * Build: cmake -B build -G "Visual Studio 17 2022" -A x64
 *        cmake --build build --config Release --target soluna-win
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _WIN32
#error "Windows only."
#endif

// Enable modern visual styles
#pragma comment(linker,"/manifestdependency:\"type='win32' "\
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "\
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <dwmapi.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <algorithm>

// ── OSTP / RTP structures ─────────────────────────────────────────────────────

#pragma pack(push,1)
struct RtpHeader   { uint8_t cc_x_p_v; uint8_t m_pt; uint16_t seq; uint32_t ts; uint32_t ssrc; };
struct OstpHeader  { uint8_t magic[4]; uint16_t stream_id; uint16_t seq_ext;
                     uint32_t device_id; uint32_t flags; uint32_t media_ts; };
#pragma pack(pop)

static constexpr uint32_t kOstpMagic   = 0x4F535450;
static constexpr uint8_t  kPtOstp      = 96;
static constexpr uint32_t kRate        = 48000;
static constexpr uint32_t kRingCap     = 192000;
static constexpr uint32_t kFramesPkt   = 480;
static constexpr float    kSampleScale = 8388608.0f;
static constexpr size_t   kMaxPkt      = 65536;

// ── Time helpers ──────────────────────────────────────────────────────────────

static uint64_t now_ns() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime<<32)|ft.dwLowDateTime;
    return (t - 116444736000000000ULL)*100;
}
static double now_sec() {
    LARGE_INTEGER f,c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart/(double)f.QuadPart;
}

// ── CRC-32 ────────────────────────────────────────────────────────────────────

static uint32_t crc_table[256];
static void crc_init() {
    for(uint32_t i=0;i<256;i++){uint32_t c=i;for(int j=0;j<8;j++)c=(c&1)?(0xEDB88320u^(c>>1)):(c>>1);crc_table[i]=c;}
}
static uint32_t crc32(const uint8_t*d,size_t n){uint32_t c=~0u;for(size_t i=0;i<n;i++)c=crc_table[(c^d[i])&0xFF]^(c>>8);return~c;}

// ── Ring buffer (SPSC) ────────────────────────────────────────────────────────

class Ring {
public:
    Ring(size_t cap,uint32_t ch):ch_(ch){size_t c=1;while(c<cap)c<<=1;cap_=c;mask_=c-1;buf_.resize(c*ch);}
    size_t write(const int32_t*s,size_t f){
        size_t wr=wp_.load(std::memory_order_relaxed),rd=rp_.load(std::memory_order_acquire);
        size_t n=(std::min)(f,cap_-(wr-rd));
        for(size_t i=0;i<n;i++)for(uint32_t c=0;c<ch_;c++)buf_[((wr+i)&mask_)*ch_+c]=s[i*ch_+c];
        wp_.store(wr+n,std::memory_order_release);return n;
    }
    size_t read(int32_t*d,size_t f){
        size_t rd=rp_.load(std::memory_order_relaxed),wr=wp_.load(std::memory_order_acquire);
        size_t n=(std::min)(f,wr-rd);
        for(size_t i=0;i<n;i++)for(uint32_t c=0;c<ch_;c++)d[i*ch_+c]=buf_[((rd+i)&mask_)*ch_+c];
        rp_.store(rd+n,std::memory_order_release);return n;
    }
    void discard(size_t f){size_t rd=rp_.load(std::memory_order_relaxed),wr=wp_.load(std::memory_order_acquire);rp_.store(rd+(std::min)(f,wr-rd),std::memory_order_release);}
    size_t avail()const{return wp_.load(std::memory_order_acquire)-rp_.load(std::memory_order_relaxed);}
    void reset(){wp_.store(0,std::memory_order_seq_cst);rp_.store(0,std::memory_order_seq_cst);}
private:
    uint32_t ch_;size_t cap_,mask_;std::vector<int32_t>buf_;
    std::atomic<size_t>wp_{0},rp_{0};
};

// ── WASAPI output ─────────────────────────────────────────────────────────────

struct WasapiOut {
    IMMDevice*device=nullptr;IAudioClient*ac=nullptr;IAudioRenderClient*rc=nullptr;
    UINT32 buf_frames=0;uint32_t ch=2;
    bool open(uint32_t channels){
        ch=channels;
        IMMDeviceEnumerator*en=nullptr;
        if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),(void**)&en)))return false;
        HRESULT hr=en->GetDefaultAudioEndpoint(eRender,eConsole,&device);en->Release();
        if(FAILED(hr)||!device)return false;
        if(FAILED(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,(void**)&ac))||!ac)return false;
        WAVEFORMATEX fmt{};fmt.wFormatTag=WAVE_FORMAT_PCM;fmt.nChannels=(WORD)ch;
        fmt.nSamplesPerSec=kRate;fmt.wBitsPerSample=16;fmt.nBlockAlign=fmt.nChannels*2;fmt.nAvgBytesPerSec=kRate*fmt.nBlockAlign;
        hr=ac->Initialize(AUDCLNT_SHAREMODE_SHARED,0,400000,0,&fmt,nullptr);
        if(hr==AUDCLNT_E_UNSUPPORTED_FORMAT){WAVEFORMATEX*mf=nullptr;ac->GetMixFormat(&mf);if(mf){hr=ac->Initialize(AUDCLNT_SHAREMODE_SHARED,0,400000,0,mf,nullptr);CoTaskMemFree(mf);}}
        if(FAILED(hr))return false;
        ac->GetBufferSize(&buf_frames);
        return SUCCEEDED(ac->GetService(__uuidof(IAudioRenderClient),(void**)&rc))&&rc;
    }
    bool start(){return SUCCEEDED(ac->Start());}
    void close(){if(ac){ac->Stop();ac->Release();ac=nullptr;}if(rc){rc->Release();rc=nullptr;}if(device){device->Release();device=nullptr;}}
};

// ── WASAPI capture (loopback or mic) ─────────────────────────────────────────

struct WasapiCap {
    IMMDevice*device=nullptr;IAudioClient*ac=nullptr;IAudioCaptureClient*cc=nullptr;
    WAVEFORMATEX*fmt=nullptr;uint32_t ch=2,rate=kRate;bool is_float=true;uint16_t bits=32;
    bool open(uint32_t channels,bool loopback){
        ch=channels;
        IMMDeviceEnumerator*en=nullptr;
        if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),(void**)&en)))return false;
        HRESULT hr=en->GetDefaultAudioEndpoint(loopback?eRender:eCapture,eConsole,&device);en->Release();
        if(FAILED(hr)||!device)return false;
        if(FAILED(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,(void**)&ac))||!ac)return false;
        if(FAILED(ac->GetMixFormat(&fmt))||!fmt)return false;
        rate=fmt->nSamplesPerSec;
        if(fmt->wFormatTag==WAVE_FORMAT_IEEE_FLOAT){is_float=true;bits=32;}
        else if(fmt->wFormatTag==WAVE_FORMAT_EXTENSIBLE){const WAVEFORMATEXTENSIBLE*ex=(const WAVEFORMATEXTENSIBLE*)fmt;is_float=(ex->SubFormat==KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);bits=fmt->wBitsPerSample;}
        else{is_float=false;bits=fmt->wBitsPerSample;}
        DWORD flags=loopback?AUDCLNT_STREAMFLAGS_LOOPBACK:0;
        if(FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED,flags,200000,0,fmt,nullptr)))return false;
        return SUCCEEDED(ac->GetService(__uuidof(IAudioCaptureClient),(void**)&cc))&&cc;
    }
    bool start(){return SUCCEEDED(ac->Start());}
    size_t read(std::vector<float>&out){
        UINT32 pkt=0;size_t total=0;
        while(SUCCEEDED(cc->GetNextPacketSize(&pkt))&&pkt>0){
            BYTE*data;UINT32 frames;DWORD flags;
            if(FAILED(cc->GetBuffer(&data,&frames,&flags,nullptr,nullptr))||frames==0)break;
            bool silent=(flags&AUDCLNT_BUFFERFLAGS_SILENT)!=0;
            uint32_t sc=fmt->nChannels;size_t base=out.size();out.resize(base+frames*ch);
            for(UINT32 f=0;f<frames;f++)for(uint32_t c=0;c<ch;c++){
                float s=0.f;uint32_t src=(c<sc)?c:0;
                if(!silent){
                    if(is_float)s=((const float*)data)[f*sc+src];
                    else if(bits==16)s=(float)((const int16_t*)data)[f*sc+src]/32768.f;
                    else if(bits==24){const uint8_t*b=data+(f*sc+src)*3;int32_t v=(int32_t)(((uint32_t)b[2]<<16)|((uint32_t)b[1]<<8)|(uint32_t)b[0]);if(v&0x800000)v|=0xFF000000;s=(float)v/8388608.f;}
                    else s=(float)((const int32_t*)data)[f*sc+src]/2147483648.f;
                }
                out[base+f*ch+c]=s;
            }
            total+=frames;cc->ReleaseBuffer(frames);
        }
        return total;
    }
    void close(){if(ac){ac->Stop();ac->Release();ac=nullptr;}if(cc){cc->Release();cc=nullptr;}if(device){device->Release();device=nullptr;}if(fmt){CoTaskMemFree(fmt);fmt=nullptr;}}
};

// ── Build OSTP packet ─────────────────────────────────────────────────────────

static size_t build_ostp(uint8_t*buf,size_t cap,uint16_t seq,uint32_t rts,uint32_t ssrc,uint32_t ch,const float*smp,uint32_t frames){
    size_t payload=(size_t)frames*ch*4,total=sizeof(RtpHeader)+sizeof(OstpHeader)+payload+4;
    if(total>cap)return 0;
    RtpHeader*rtp=(RtpHeader*)buf;rtp->cc_x_p_v=0x80;rtp->m_pt=kPtOstp;rtp->seq=htons(seq);rtp->ts=htonl(rts);rtp->ssrc=htonl(ssrc);
    OstpHeader*oh=(OstpHeader*)(buf+sizeof(RtpHeader));
    oh->magic[0]='O';oh->magic[1]='S';oh->magic[2]='T';oh->magic[3]='P';
    oh->stream_id=htons((uint16_t)(ch<<12));oh->seq_ext=0;oh->device_id=htonl(ssrc);oh->flags=0;oh->media_ts=htonl((uint32_t)(now_ns()&0xFFFFFFFFu));
    int32_t*pcm=(int32_t*)(buf+sizeof(RtpHeader)+sizeof(OstpHeader));
    size_t ns=(size_t)frames*ch;
    for(size_t i=0;i<ns;i++){float s=smp[i];if(s>1.f)s=1.f;if(s<-1.f)s=-1.f;pcm[i]=(int32_t)(s*kSampleScale);}
    uint32_t crc=crc32(buf,sizeof(RtpHeader)+sizeof(OstpHeader)+payload);
    memcpy(buf+sizeof(RtpHeader)+sizeof(OstpHeader)+payload,&crc,4);
    return total;
}

// ── Shared audio state ────────────────────────────────────────────────────────

enum class AppState { Stopped, Connecting, Connected, Error };

struct AudioState {
    Ring   ring{kRingCap, 2};
    std::atomic<uint32_t> target_fill{2880};
    std::atomic<uint32_t> underruns{0};
    std::atomic<uint64_t> pkts_rx{0};
    std::atomic<uint64_t> pkts_drop{0};
    std::atomic<float>    out_level{0.f};   // 0..1 output peak
    std::atomic<float>    tx_level{0.f};    // 0..1 mic/loopback input peak
    std::atomic<int>      volume_pct{100};  // 0..100
    std::atomic<bool>     muted{false};
    std::atomic<AppState> state{AppState::Stopped};
    std::atomic<bool>     tx_active{false};
    SOCKET  sock = INVALID_SOCKET;
    sockaddr_in dest{};
    uint32_t channels = 2;
    volatile bool running = false;
    volatile bool tx_running = false;
};

static AudioState g_audio;

// ── Playback thread ───────────────────────────────────────────────────────────

static void playback_thread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    WasapiOut out;
    if (!out.open(g_audio.channels) || !out.start()) {
        g_audio.state.store(AppState::Error);
        CoUninitialize(); return;
    }
    static int32_t tmp[8192*2];
    bool prefill = true;
    while (g_audio.running) {
        if (prefill) {
            if (g_audio.ring.avail() < g_audio.target_fill.load()) { Sleep(5); continue; }
            prefill = false;
        }
        UINT32 pad=0; if (FAILED(out.ac->GetCurrentPadding(&pad))) break;
        UINT32 want = out.buf_frames - pad;
        if (want == 0) { Sleep(2); continue; }
        want = (std::min)(want, (UINT32)4096);
        BYTE* data=nullptr; if (FAILED(out.rc->GetBuffer(want, &data))) break;
        size_t got = g_audio.ring.read(tmp, want);
        int16_t* dst = (int16_t*)data;
        float vol = g_audio.muted.load() ? 0.f : g_audio.volume_pct.load() / 100.f;
        float peak = 0.f;
        for (size_t i = 0; i < got*g_audio.channels; i++) {
            float s = (float)(tmp[i] >> 8) / 32768.f * vol;
            if (s > 1.f) s=1.f; if (s<-1.f) s=-1.f;
            if (s < 0 ? -s > peak : s > peak) peak = s < 0 ? -s : s;
            dst[i] = (int16_t)(s * 32767.f);
        }
        g_audio.out_level.store(peak);
        if (got < want) { memset(dst+got*g_audio.channels,0,(want-got)*g_audio.channels*2); prefill=true; }
        out.rc->ReleaseBuffer(want,0);
    }
    out.close(); CoUninitialize();
}

// ── TX thread ─────────────────────────────────────────────────────────────────

static bool g_use_mic = false;

static void tx_thread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    WasapiCap cap;
    if (!cap.open(g_audio.channels, !g_use_mic) || !cap.start()) {
        g_audio.tx_active.store(false);
        CoUninitialize(); return;
    }
    uint32_t fpkt = (uint32_t)((double)kFramesPkt*(double)cap.rate/(double)kRate);
    if (fpkt < 48) fpkt = 48;
    uint16_t seq=(uint16_t)(now_ns()&0xFFFF);
    uint32_t rts=(uint32_t)(now_ns()&0xFFFFFFFF);
    uint32_t ssrc=(uint32_t)(now_ns()^(now_ns()>>13));
    std::vector<float> accum; accum.reserve(fpkt*g_audio.channels*4);
    static uint8_t pkt[kMaxPkt];
    g_audio.tx_active.store(true);
    while (g_audio.tx_running) {
        cap.read(accum);
        if (accum.size() < fpkt*g_audio.channels) { Sleep(3); cap.read(accum); }
        while (accum.size() >= fpkt*g_audio.channels) {
            float peak = 0.f;
            for (size_t i = 0; i < fpkt*g_audio.channels; i++) { float a=accum[i]<0?-accum[i]:accum[i]; if(a>peak)peak=a; }
            g_audio.tx_level.store(peak);
            size_t len=build_ostp(pkt,sizeof(pkt),seq,rts,ssrc,g_audio.channels,accum.data(),fpkt);
            if (len>0) sendto(g_audio.sock,(const char*)pkt,(int)len,0,(const sockaddr*)&g_audio.dest,sizeof(g_audio.dest));
            seq++;rts+=kFramesPkt;
            accum.erase(accum.begin(),accum.begin()+fpkt*g_audio.channels);
        }
    }
    cap.close(); g_audio.tx_active.store(false); CoUninitialize();
}

// ── Network receive thread ────────────────────────────────────────────────────

static void recv_thread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    static uint8_t pkt[kMaxPkt]; static int32_t dec[8192*2];
    int32_t last_seq = -1;
    g_audio.state.store(AppState::Connecting);
    std::thread pb(playback_thread);
    while (g_audio.running) {
        int n = recv(g_audio.sock,(char*)pkt,(int)sizeof(pkt),0);
        if (n <= 0) continue;
        if ((size_t)n < sizeof(RtpHeader)) continue;
        RtpHeader*rtp=(RtpHeader*)pkt;
        uint8_t pt=rtp->m_pt&0x7F;
        if (pt==126||pt==127) continue;
        bool is_ostp=false; size_t off=sizeof(RtpHeader);
        if ((size_t)n >= sizeof(RtpHeader)+sizeof(OstpHeader)) {
            OstpHeader*oh=(OstpHeader*)(pkt+sizeof(RtpHeader));
            uint32_t magic; memcpy(&magic,oh->magic,4);
            if (magic==htonl(kOstpMagic)){is_ostp=true;off+=sizeof(OstpHeader);}
        }
        if (off>=(size_t)n) continue;
        const uint8_t*payload=pkt+off; size_t plen=(size_t)n-off; size_t frames;
        if (is_ostp) {
            frames=plen/(4*g_audio.channels);
            size_t samps=(std::min)(frames*g_audio.channels,sizeof(dec)/4);
            memcpy(dec,payload,samps*4); frames=samps/g_audio.channels;
        } else {
            frames=plen/(2*g_audio.channels);
            const int16_t*src=(const int16_t*)payload;
            size_t samps=(std::min)(frames*g_audio.channels,sizeof(dec)/4);
            for(size_t i=0;i<samps;i++) dec[i]=(int32_t)((int16_t)ntohs((uint16_t)src[i]))<<8;
            frames=samps/g_audio.channels;
        }
        if (frames==0) continue;
        uint16_t seq=ntohs(rtp->seq);
        if (last_seq>=0){int d=(int)(uint16_t)(seq-(uint16_t)last_seq);if(d==0)continue;if(d>1&&d<100)g_audio.pkts_drop.fetch_add(d-1);}
        last_seq=seq; g_audio.pkts_rx.fetch_add(1);
        size_t w=g_audio.ring.write(dec,frames);
        if (w<frames){g_audio.ring.discard(frames-w);g_audio.ring.write(dec+w*g_audio.channels,frames-w);}
        g_audio.state.store(AppState::Connected);
    }
    pb.join(); CoUninitialize();
}

// ── Win32 GUI ─────────────────────────────────────────────────────────────────

// Control IDs
#define ID_BTN_CONNECT   101
#define ID_BTN_MUTE      102
#define ID_SLD_VOLUME    103
#define ID_BTN_TX        104
#define ID_BTN_MIC       105
#define ID_EDT_CHANNEL   106
#define ID_EDT_PASSWORD  107
#define ID_EDT_RELAY     108
#define ID_TIMER_UI      200

// Dark theme colors
static const COLORREF kBg        = RGB(18,18,24);
static const COLORREF kSurface   = RGB(28,28,36);
static const COLORREF kBorder    = RGB(50,50,65);
static const COLORREF kAccent    = RGB(100,140,255);
static const COLORREF kAccentGrn = RGB(80,210,120);
static const COLORREF kAccentRed = RGB(255,80,80);
static const COLORREF kText      = RGB(230,230,240);
static const COLORREF kTextDim   = RGB(130,130,150);

static HBRUSH hBgBrush, hSurfBrush;
static HFONT  hFontSm, hFontMd, hFontBig;

// Child windows
static HWND hEdtChannel, hEdtPassword, hEdtRelay;
static HWND hBtnConnect, hBtnMute;
static HWND hSldVolume;
static HWND hBtnTx, hBtnMic;
static HWND hWnd;

// State
static std::thread g_recv_thr, g_tx_thr;
static bool g_is_connected = false;
static bool g_tx_on = false;

// Keepalive timer
static double g_last_hello = 0;

// Level meter history (8 bars)
static float g_rx_bars[8] = {};
static float g_tx_bars[8] = {};
static int g_bar_idx = 0;

static void start_connect() {
    if (g_is_connected) return;

    char relay_buf[256]={}, ch_buf[128]={}, pw_buf[128]={};
    GetWindowTextA(hEdtRelay,   relay_buf, sizeof(relay_buf));
    GetWindowTextA(hEdtChannel, ch_buf,    sizeof(ch_buf));
    GetWindowTextA(hEdtPassword,pw_buf,    sizeof(pw_buf));

    std::string relay_str = relay_buf[0] ? relay_buf : "relay.solun.art:5100";
    std::string ch        = ch_buf[0]    ? ch_buf    : "soluna";
    std::string pw        = pw_buf;

    // Parse host:port
    std::string host = relay_str; uint16_t port = 5100;
    auto c = relay_str.rfind(':');
    if (c != std::string::npos) { host = relay_str.substr(0,c); port = (uint16_t)atoi(relay_str.substr(c+1).c_str()); }

    // Create socket
    g_audio.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_audio.sock == INVALID_SOCKET) { MessageBoxA(hWnd,"Socket failed","Error",MB_OK|MB_ICONERROR); return; }

    DWORD tv = 100; setsockopt(g_audio.sock,SOL_SOCKET,SO_RCVTIMEO,(const char*)&tv,sizeof(tv));

    sockaddr_in ba{}; ba.sin_family=AF_INET; ba.sin_addr.s_addr=INADDR_ANY;
    bind(g_audio.sock,(sockaddr*)&ba,sizeof(ba));

    g_audio.dest.sin_family = AF_INET; g_audio.dest.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &g_audio.dest.sin_addr) <= 0) {
        ADDRINFOA hints{},*res=nullptr; hints.ai_family=AF_INET; hints.ai_socktype=SOCK_DGRAM;
        char ps[16]; snprintf(ps,sizeof(ps),"%u",port);
        if (getaddrinfo(host.c_str(),ps,&hints,&res)!=0||!res) {
            MessageBoxA(hWnd,"Cannot resolve relay host","Error",MB_OK|MB_ICONERROR);
            closesocket(g_audio.sock); g_audio.sock=INVALID_SOCKET; return;
        }
        g_audio.dest=*(sockaddr_in*)res->ai_addr; freeaddrinfo(res);
    }

    // JOIN
    std::string join = "JOIN:" + ch;
    if (!pw.empty()) join += ":" + pw;
    join += "\n";
    sendto(g_audio.sock, join.c_str(),(int)join.size(),0,(sockaddr*)&g_audio.dest,sizeof(g_audio.dest));
    g_last_hello = now_sec();

    g_audio.running = true;
    g_audio.pkts_rx.store(0); g_audio.pkts_drop.store(0);
    g_audio.state.store(AppState::Connecting);
    g_recv_thr = std::thread(recv_thread);
    g_is_connected = true;

    SetWindowTextA(hBtnConnect, "Disconnect");
}

static void stop_connect() {
    if (!g_is_connected) return;
    g_audio.running = false;
    if (g_tx_on) {
        g_audio.tx_running = false;
        if (g_tx_thr.joinable()) g_tx_thr.join();
        g_tx_on = false;
    }
    if (g_recv_thr.joinable()) g_recv_thr.join();
    if (g_audio.sock != INVALID_SOCKET) { closesocket(g_audio.sock); g_audio.sock=INVALID_SOCKET; }
    g_audio.state.store(AppState::Stopped);
    g_audio.ring.reset();
    g_is_connected = false;
    SetWindowTextA(hBtnConnect, "Connect");
}

static void toggle_tx(bool use_mic) {
    if (!g_is_connected) { MessageBoxA(hWnd,"Connect first","Info",MB_OK|MB_ICONINFORMATION); return; }
    if (g_tx_on && g_use_mic == use_mic) {
        // Stop TX
        g_audio.tx_running = false;
        if (g_tx_thr.joinable()) g_tx_thr.join();
        g_tx_on = false;
        SetWindowTextA(hBtnTx,  "TX: System Audio");
        SetWindowTextA(hBtnMic, "TX: Microphone");
    } else {
        if (g_tx_on) { g_audio.tx_running=false; if(g_tx_thr.joinable())g_tx_thr.join(); g_tx_on=false; }
        g_use_mic = use_mic;
        g_audio.tx_running = true;
        g_tx_thr = std::thread(tx_thread);
        g_tx_on = true;
        if (!use_mic) SetWindowTextA(hBtnTx, "■ Stop TX");
        else          SetWindowTextA(hBtnMic,"■ Stop Mic");
        if (!use_mic) SetWindowTextA(hBtnMic,"TX: Microphone");
        else          SetWindowTextA(hBtnTx, "TX: System Audio");
    }
}

// ── Custom control drawing ────────────────────────────────────────────────────

static void draw_rounded_rect(HDC hdc, RECT r, int rad, COLORREF fill, COLORREF border) {
    HBRUSH hb = CreateSolidBrush(fill);
    HPEN   hp = border ? CreatePen(PS_SOLID,1,border) : (HPEN)GetStockObject(NULL_PEN);
    SelectObject(hdc,hb); SelectObject(hdc,hp);
    RoundRect(hdc,r.left,r.top,r.right,r.bottom,rad,rad);
    DeleteObject(hb); if(border) DeleteObject(hp);
}

static void draw_level_bar(HDC hdc, RECT r, float level, COLORREF color) {
    draw_rounded_rect(hdc, r, 3, RGB(35,35,45), 0);
    RECT filled = r;
    filled.right = r.left + (LONG)((r.right-r.left)*min(1.f,level));
    if (filled.right > filled.left) draw_rounded_rect(hdc,filled,3,color,0);
}

static void draw_bars(HDC hdc, RECT r, float* bars, int count, COLORREF color) {
    int w = r.right - r.left, h = r.bottom - r.top;
    int bar_w = (w - (count-1)*2) / count;
    for (int i = 0; i < count; i++) {
        int x = r.left + i*(bar_w+2);
        int bar_h = (int)(h * min(1.f, bars[i]));
        RECT bg = {x, r.top, x+bar_w, r.bottom};
        draw_rounded_rect(hdc, bg, 2, RGB(35,35,45), 0);
        if (bar_h > 0) {
            RECT fill = {x, r.bottom-bar_h, x+bar_w, r.bottom};
            draw_rounded_rect(hdc,fill,2,color,0);
        }
    }
}

static void paint_window(HWND hwnd) {
    PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
    RECT cr; GetClientRect(hwnd, &cr);

    // Background
    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(hdc, &cr, bg); DeleteObject(bg);

    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, hFontBig);

    // ── Title ──────────────────────────────────────────────────
    SetTextColor(hdc, kAccent);
    TextOutA(hdc, 20, 20, "SOLUNA", 6);

    // Status dot + text
    AppState st = g_audio.state.load();
    COLORREF dot_color = st==AppState::Connected ? kAccentGrn : st==AppState::Connecting ? RGB(255,180,0) : st==AppState::Error ? kAccentRed : kTextDim;
    HBRUSH dot_br = CreateSolidBrush(dot_color);
    SelectObject(hdc, dot_br); SelectObject(hdc,(HPEN)GetStockObject(NULL_PEN));
    Ellipse(hdc, cr.right-120, 28, cr.right-108, 40); DeleteObject(dot_br);
    SelectObject(hdc, hFontSm); SetTextColor(hdc, dot_color);
    const char* st_txt = st==AppState::Connected?"Connected":st==AppState::Connecting?"Connecting...":st==AppState::Error?"Error":"Stopped";
    TextOutA(hdc, cr.right-105, 25, st_txt, (int)strlen(st_txt));

    // ── Section: Connection ────────────────────────────────────
    int y = 70;
    SelectObject(hdc, hFontSm); SetTextColor(hdc, kTextDim);
    TextOutA(hdc, 20, y, "RELAY HOST", 10); y += 18;
    // (input drawn by child controls)

    y = 115;
    TextOutA(hdc, 20, y, "CHANNEL", 7); y += 18;

    y = 160;
    TextOutA(hdc, 20, y, "PASSWORD (optional)", 19); y += 18;

    // ── Divider ────────────────────────────────────────────────
    HPEN div_pen = CreatePen(PS_SOLID,1,kBorder);
    SelectObject(hdc, div_pen);
    MoveToEx(hdc, 20, 220, nullptr); LineTo(hdc, cr.right-20, 220);
    DeleteObject(div_pen);

    // ── Level Meters ───────────────────────────────────────────
    y = 235;
    SelectObject(hdc, hFontSm); SetTextColor(hdc, kTextDim);
    TextOutA(hdc, 20, y, "OUTPUT", 6);
    TextOutA(hdc, cr.right/2+10, y, "TX INPUT", 8);
    y += 18;

    RECT rx_meter = {20, y, cr.right/2-10, y+12};
    RECT tx_meter = {cr.right/2+10, y, cr.right-20, y+12};
    float rx_lvl = g_audio.out_level.load();
    float tx_lvl = g_audio.tx_level.load();
    draw_level_bar(hdc, rx_meter, rx_lvl, kAccentGrn);
    draw_level_bar(hdc, tx_meter, tx_lvl, kAccent);
    y += 20;

    // Bar graph (history)
    RECT rx_bars_r = {20, y, cr.right/2-10, y+50};
    RECT tx_bars_r = {cr.right/2+10, y, cr.right-20, y+50};
    draw_bars(hdc, rx_bars_r, g_rx_bars, 8, kAccentGrn);
    draw_bars(hdc, tx_bars_r, g_tx_bars, 8, kAccent);
    y += 58;

    // ── Divider ────────────────────────────────────────────────
    div_pen = CreatePen(PS_SOLID,1,kBorder);
    SelectObject(hdc, div_pen);
    MoveToEx(hdc, 20, y, nullptr); LineTo(hdc, cr.right-20, y);
    DeleteObject(div_pen); y += 14;

    // ── Volume label ───────────────────────────────────────────
    SetTextColor(hdc, kTextDim); SelectObject(hdc, hFontSm);
    char vol_buf[32]; snprintf(vol_buf, sizeof(vol_buf), "VOLUME  %d%%", g_audio.volume_pct.load());
    TextOutA(hdc, 20, y, vol_buf, (int)strlen(vol_buf));
    y += 18;
    // (slider drawn by child control)

    y += 48;
    div_pen = CreatePen(PS_SOLID,1,kBorder);
    SelectObject(hdc, div_pen);
    MoveToEx(hdc, 20, y, nullptr); LineTo(hdc, cr.right-20, y);
    DeleteObject(div_pen); y += 14;

    // ── Stats ──────────────────────────────────────────────────
    SelectObject(hdc, hFontSm); SetTextColor(hdc, kTextDim);
    char stat_buf[128];
    uint64_t rx=g_audio.pkts_rx.load(), drop=g_audio.pkts_drop.load();
    double loss = (rx+drop)>0 ? 100.0*drop/(rx+drop) : 0.0;
    snprintf(stat_buf,sizeof(stat_buf),"Packets: %llu   Loss: %.1f%%   Buffer: %u ms",
        (unsigned long long)rx, loss,
        (uint32_t)(g_audio.ring.avail()*1000/kRate));
    TextOutA(hdc, 20, y, stat_buf, (int)strlen(stat_buf)); y += 16;

    char tx_stat[64];
    snprintf(tx_stat, sizeof(tx_stat), "TX: %s", g_tx_on ? (g_use_mic?"Microphone (active)":"System Audio (active)") : "Off");
    SetTextColor(hdc, g_tx_on ? kAccentGrn : kTextDim);
    TextOutA(hdc, 20, y, tx_stat, (int)strlen(tx_stat));

    EndPaint(hwnd, &ps);
}

// ── Subclass proc for dark edit boxes ────────────────────────────────────────

static WNDPROC g_orig_edit_proc = nullptr;
static LRESULT CALLBACK edit_subclass(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_ERASEBKGND){HDC hdc=(HDC)wp;RECT r;GetClientRect(h,&r);HBRUSH b=CreateSolidBrush(kSurface);FillRect(hdc,&r,b);DeleteObject(b);return 1;}
    return CallWindowProcA(g_orig_edit_proc,h,msg,wp,lp);
}

static HWND make_edit(HWND parent,int x,int y,int w,int h,int id,const char*hint="") {
    HWND e = CreateWindowA("EDIT","",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,x,y,w,h,parent,(HMENU)(UINT_PTR)id,GetModuleHandleA(nullptr),nullptr);
    SendMessageA(e,EM_SETCUEBANNERA,0,(LPARAM)hint);
    SendMessageA(e,WM_SETFONT,(WPARAM)hFontSm,TRUE);
    if(!g_orig_edit_proc)g_orig_edit_proc=(WNDPROC)GetWindowLongPtrA(e,GWLP_WNDPROC);
    SetWindowLongPtrA(e,GWLP_WNDPROC,(LONG_PTR)edit_subclass);
    return e;
}

static HWND make_btn(HWND parent,const char*txt,int x,int y,int w,int h,int id) {
    HWND b=CreateWindowA("BUTTON",txt,WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,x,y,w,h,parent,(HMENU)(UINT_PTR)id,GetModuleHandleA(nullptr),nullptr);
    SendMessageA(b,WM_SETFONT,(WPARAM)hFontSm,TRUE);
    return b;
}

// ── WndProc ───────────────────────────────────────────────────────────────────

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        int W = 480;
        hEdtRelay   = make_edit(hwnd,20,88,  W-130,26,ID_EDT_RELAY,   "relay.solun.art:5100");
        hEdtChannel = make_edit(hwnd,20,133, W-130,26,ID_EDT_CHANNEL, "soluna");
        hEdtPassword= make_edit(hwnd,20,178, W-130,26,ID_EDT_PASSWORD,"(optional)");
        hBtnConnect = make_btn(hwnd,"Connect",W-105,88,90,65,ID_BTN_CONNECT);
        // Volume slider
        hSldVolume = CreateWindowA(TRACKBAR_CLASSA,"",WS_CHILD|WS_VISIBLE|TBS_HORZ|TBS_NOTICKS,20,375,W-130,28,hwnd,(HMENU)ID_SLD_VOLUME,GetModuleHandleA(nullptr),nullptr);
        SendMessageA(hSldVolume,TBM_SETRANGE,0,MAKELONG(0,100));
        SendMessageA(hSldVolume,TBM_SETPOS,TRUE,100);
        // Mute
        hBtnMute = make_btn(hwnd,"Mute",W-105,375,90,28,ID_BTN_MUTE);
        // TX buttons
        hBtnTx  = make_btn(hwnd,"TX: System Audio",20, 425,220,32,ID_BTN_TX);
        hBtnMic = make_btn(hwnd,"TX: Microphone",  250,425,220,32,ID_BTN_MIC);
        SetTimer(hwnd, ID_TIMER_UI, 80, nullptr); // ~12fps
        // Dark title bar (Win10 1809+)
        BOOL dark=TRUE; DwmSetWindowAttribute(hwnd,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
        break;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc=(HDC)wp;
        SetTextColor(hdc,kText); SetBkColor(hdc,kSurface);
        return (LRESULT)hSurfBrush;
    }
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, kBg);
        return (LRESULT)hBgBrush;
    case WM_ERASEBKGND: {
        HDC hdc=(HDC)wp; RECT r; GetClientRect(hwnd,&r);
        FillRect(hdc,&r,hBgBrush); return 1;
    }
    case WM_PAINT:
        paint_window(hwnd); return 0;

    case WM_TIMER:
        if (wp == ID_TIMER_UI) {
            // Keepalive
            if (g_is_connected) {
                double t=now_sec();
                if (t-g_last_hello>=5.0) {
                    const char hello[]="HELLO\n";
                    sendto(g_audio.sock,hello,(int)strlen(hello),0,(sockaddr*)&g_audio.dest,sizeof(g_audio.dest));
                    g_last_hello=t;
                }
            }
            // Update bar history
            g_rx_bars[g_bar_idx % 8] = g_audio.out_level.load();
            g_tx_bars[g_bar_idx % 8] = g_audio.tx_level.load();
            g_bar_idx++;
            // Decay level
            g_audio.out_level.store(g_audio.out_level.load() * 0.85f);
            g_audio.tx_level.store(g_audio.tx_level.load() * 0.85f);
            // Volume from slider
            int vol = (int)SendMessageA(hSldVolume, TBM_GETPOS, 0, 0);
            g_audio.volume_pct.store(vol);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BTN_CONNECT:
            if (!g_is_connected) start_connect();
            else                 stop_connect();
            break;
        case ID_BTN_MUTE:
            g_audio.muted.store(!g_audio.muted.load());
            SetWindowTextA(hBtnMute, g_audio.muted.load() ? "Unmute" : "Mute");
            break;
        case ID_BTN_TX:  toggle_tx(false); break;
        case ID_BTN_MIC: toggle_tx(true);  break;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER_UI);
        stop_connect();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// ── WinMain ───────────────────────────────────────────────────────────────────

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    crc_init();

    WSADATA wsa; WSAStartup(MAKEWORD(2,2),&wsa);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Init common controls (for slider)
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    hBgBrush   = CreateSolidBrush(kBg);
    hSurfBrush = CreateSolidBrush(kSurface);
    hFontSm  = CreateFontA(14,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,"Segoe UI");
    hFontMd  = CreateFontA(16,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,"Segoe UI");
    hFontBig = CreateFontA(24,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,"Segoe UI");

    WNDCLASSEXA wc{}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=WndProc; wc.hInstance=hInst;
    wc.lpszClassName="SolunaWin"; wc.hCursor=LoadCursorA(nullptr,IDC_ARROW);
    wc.hbrBackground=hBgBrush; wc.hIcon=LoadIconA(nullptr,IDI_APPLICATION);
    RegisterClassExA(&wc);

    int W=480, H=490;
    hWnd = CreateWindowExA(0,"SolunaWin","Soluna",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        CW_USEDEFAULT,CW_USEDEFAULT,W,H,nullptr,nullptr,hInst,nullptr);

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageA(&msg,nullptr,0,0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    DeleteObject(hBgBrush); DeleteObject(hSurfBrush);
    DeleteObject(hFontSm);  DeleteObject(hFontMd); DeleteObject(hFontBig);
    CoUninitialize(); WSACleanup();
    return (int)msg.wParam;
}
