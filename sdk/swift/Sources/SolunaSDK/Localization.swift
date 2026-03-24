import Foundation

/// Information about a supported locale.
public struct LocaleInfo: Sendable {
    public let code: String
    public let name: String
    public let localizedName: String
    public let isRTL: Bool

    public init(code: String, name: String, localizedName: String, isRTL: Bool = false) {
        self.code = code
        self.name = name
        self.localizedName = localizedName
        self.isRTL = isRTL
    }
}

/// Multi-language support for the Soluna SDK.
///
/// Provides localized strings for connection states, channel names,
/// error messages, and UI labels across 30 languages. Uses a simple
/// dictionary-based lookup without requiring .strings resource files.
///
/// Usage:
/// ```swift
/// let l10n = SolunaLocalization()
/// let text = l10n.localized("connected")  // "Connected" or localized equivalent
/// l10n.setLanguage("ja")
/// let jaText = l10n.localized("connected")  // "接続済み"
/// ```
public final class SolunaLocalization: ObservableObject {

    // MARK: - Published State

    /// The current language code.
    @Published public private(set) var currentLanguage: String

    // MARK: - Public Properties

    /// Whether the current language is right-to-left.
    public var isRTL: Bool {
        rtlLanguages.contains(currentLanguage)
    }

    /// All supported languages.
    public var supportedLanguages: [LocaleInfo] {
        languageNames.map { code, name in
            let locale = Locale(identifier: code)
            let localizedName = locale.localizedString(forLanguageCode: code) ?? name
            return LocaleInfo(
                code: code,
                name: name,
                localizedName: localizedName,
                isRTL: rtlLanguages.contains(code)
            )
        }.sorted { $0.code < $1.code }
    }

    // MARK: - Private

    private let rtlLanguages: Set<String> = ["ar", "he"]

    private let languageNames: [String: String] = [
        "ja": "Japanese", "en": "English", "zh-Hans": "Chinese (Simplified)",
        "zh-Hant": "Chinese (Traditional)", "ko": "Korean", "es": "Spanish",
        "fr": "French", "de": "German", "it": "Italian", "pt": "Portuguese",
        "ru": "Russian", "ar": "Arabic", "hi": "Hindi", "th": "Thai",
        "vi": "Vietnamese", "id": "Indonesian", "ms": "Malay", "tr": "Turkish",
        "pl": "Polish", "nl": "Dutch", "sv": "Swedish", "da": "Danish",
        "no": "Norwegian", "fi": "Finnish", "cs": "Czech", "hu": "Hungarian",
        "ro": "Romanian", "uk": "Ukrainian", "he": "Hebrew", "el": "Greek"
    ]

    /// Localized string tables keyed by language code, then by string key.
    private let strings: [String: [String: String]]

    // MARK: - Init

    public init() {
        // Auto-detect language from system locale
        let preferred = Locale.preferredLanguages.first ?? "en"
        let languageCode: String
        if preferred.hasPrefix("zh-Hans") {
            languageCode = "zh-Hans"
        } else if preferred.hasPrefix("zh-Hant") || preferred.hasPrefix("zh-TW") || preferred.hasPrefix("zh-HK") {
            languageCode = "zh-Hant"
        } else {
            languageCode = String(preferred.prefix(2))
        }
        currentLanguage = languageCode

        strings = SolunaLocalization.buildStringTables()
    }

    // MARK: - Public API

    /// Set the active language.
    ///
    /// - Parameter code: BCP 47 language code (e.g., "ja", "en", "zh-Hans").
    public func setLanguage(_ code: String) {
        currentLanguage = code
    }

    /// Get a localized string for the given key.
    ///
    /// Falls back to English if the key is not found in the current language.
    ///
    /// - Parameter key: The string key to look up.
    /// - Returns: The localized string, or the key itself if not found.
    public func localized(_ key: String) -> String {
        // Try current language
        if let table = strings[currentLanguage], let value = table[key] {
            return value
        }
        // Fallback to English
        if let table = strings["en"], let value = table[key] {
            return value
        }
        // Return key as-is
        return key
    }

    // MARK: - String Tables

    private static func buildStringTables() -> [String: [String: String]] {
        var tables: [String: [String: String]] = [:]

        // English (base)
        tables["en"] = [
            "connected": "Connected",
            "connecting": "Connecting",
            "disconnected": "Disconnected",
            "error": "Error",
            "channel": "Channel",
            "listeners": "Listeners",
            "now_playing": "Now Playing",
            "mute": "Mute",
            "unmute": "Unmute",
            "play": "Play",
            "stop": "Stop",
            "next_channel": "Next Channel",
            "previous_channel": "Previous Channel",
            "settings": "Settings",
            "volume": "Volume",
            "quality": "Quality",
            "recording": "Recording",
            "transcription": "Transcription",
            "no_signal": "No Signal",
            "reconnecting": "Reconnecting",
            "low_quality": "Low Quality",
            "high_quality": "High Quality",
        ]

        // Japanese
        tables["ja"] = [
            "connected": "接続済み",
            "connecting": "接続中",
            "disconnected": "切断済み",
            "error": "エラー",
            "channel": "チャンネル",
            "listeners": "リスナー",
            "now_playing": "再生中",
            "mute": "ミュート",
            "unmute": "ミュート解除",
            "play": "再生",
            "stop": "停止",
            "next_channel": "次のチャンネル",
            "previous_channel": "前のチャンネル",
            "settings": "設定",
            "volume": "音量",
            "quality": "品質",
            "recording": "録音中",
            "transcription": "文字起こし",
            "no_signal": "信号なし",
            "reconnecting": "再接続中",
            "low_quality": "低品質",
            "high_quality": "高品質",
        ]

        // Chinese Simplified
        tables["zh-Hans"] = [
            "connected": "已连接",
            "connecting": "连接中",
            "disconnected": "已断开",
            "error": "错误",
            "channel": "频道",
            "listeners": "听众",
            "now_playing": "正在播放",
            "mute": "静音",
            "unmute": "取消静音",
            "play": "播放",
            "stop": "停止",
            "next_channel": "下一频道",
            "previous_channel": "上一频道",
            "settings": "设置",
            "volume": "音量",
            "quality": "质量",
            "recording": "录音中",
            "transcription": "转录",
            "no_signal": "无信号",
            "reconnecting": "重新连接中",
            "low_quality": "低质量",
            "high_quality": "高质量",
        ]

        // Chinese Traditional
        tables["zh-Hant"] = [
            "connected": "已連接",
            "connecting": "連接中",
            "disconnected": "已斷開",
            "error": "錯誤",
            "channel": "頻道",
            "listeners": "聽眾",
            "now_playing": "正在播放",
            "mute": "靜音",
            "unmute": "取消靜音",
            "play": "播放",
            "stop": "停止",
            "next_channel": "下一頻道",
            "previous_channel": "上一頻道",
            "settings": "設定",
            "volume": "音量",
            "quality": "品質",
            "recording": "錄音中",
            "transcription": "轉錄",
            "no_signal": "無信號",
            "reconnecting": "重新連接中",
            "low_quality": "低品質",
            "high_quality": "高品質",
        ]

        // Korean
        tables["ko"] = [
            "connected": "연결됨",
            "connecting": "연결 중",
            "disconnected": "연결 해제",
            "error": "오류",
            "channel": "채널",
            "listeners": "리스너",
            "now_playing": "재생 중",
            "mute": "음소거",
            "unmute": "음소거 해제",
            "play": "재생",
            "stop": "정지",
            "next_channel": "다음 채널",
            "previous_channel": "이전 채널",
            "settings": "설정",
            "volume": "볼륨",
            "quality": "품질",
            "recording": "녹음 중",
            "transcription": "전사",
            "no_signal": "신호 없음",
            "reconnecting": "재연결 중",
            "low_quality": "저품질",
            "high_quality": "고품질",
        ]

        // Spanish
        tables["es"] = [
            "connected": "Conectado",
            "connecting": "Conectando",
            "disconnected": "Desconectado",
            "error": "Error",
            "channel": "Canal",
            "listeners": "Oyentes",
            "now_playing": "Reproduciendo",
            "mute": "Silenciar",
            "unmute": "Activar sonido",
            "play": "Reproducir",
            "stop": "Detener",
            "next_channel": "Siguiente canal",
            "previous_channel": "Canal anterior",
            "settings": "Ajustes",
            "volume": "Volumen",
            "quality": "Calidad",
            "recording": "Grabando",
            "transcription": "Transcripción",
            "no_signal": "Sin señal",
            "reconnecting": "Reconectando",
            "low_quality": "Baja calidad",
            "high_quality": "Alta calidad",
        ]

        // French
        tables["fr"] = [
            "connected": "Connecté",
            "connecting": "Connexion",
            "disconnected": "Déconnecté",
            "error": "Erreur",
            "channel": "Chaîne",
            "listeners": "Auditeurs",
            "now_playing": "En lecture",
            "mute": "Muet",
            "unmute": "Rétablir le son",
            "play": "Lecture",
            "stop": "Arrêter",
            "next_channel": "Chaîne suivante",
            "previous_channel": "Chaîne précédente",
            "settings": "Paramètres",
            "volume": "Volume",
            "quality": "Qualité",
            "recording": "Enregistrement",
            "transcription": "Transcription",
            "no_signal": "Pas de signal",
            "reconnecting": "Reconnexion",
            "low_quality": "Basse qualité",
            "high_quality": "Haute qualité",
        ]

        // German
        tables["de"] = [
            "connected": "Verbunden",
            "connecting": "Verbindung wird hergestellt",
            "disconnected": "Getrennt",
            "error": "Fehler",
            "channel": "Kanal",
            "listeners": "Zuhörer",
            "now_playing": "Wird abgespielt",
            "mute": "Stumm",
            "unmute": "Stummschaltung aufheben",
            "play": "Abspielen",
            "stop": "Stopp",
            "next_channel": "Nächster Kanal",
            "previous_channel": "Vorheriger Kanal",
            "settings": "Einstellungen",
            "volume": "Lautstärke",
            "quality": "Qualität",
            "recording": "Aufnahme",
            "transcription": "Transkription",
            "no_signal": "Kein Signal",
            "reconnecting": "Erneut verbinden",
            "low_quality": "Niedrige Qualität",
            "high_quality": "Hohe Qualität",
        ]

        // Italian
        tables["it"] = [
            "connected": "Connesso",
            "connecting": "Connessione",
            "disconnected": "Disconnesso",
            "error": "Errore",
            "channel": "Canale",
            "listeners": "Ascoltatori",
            "now_playing": "In riproduzione",
            "mute": "Muto",
            "unmute": "Riattiva audio",
            "play": "Riproduci",
            "stop": "Ferma",
            "next_channel": "Canale successivo",
            "previous_channel": "Canale precedente",
            "settings": "Impostazioni",
            "volume": "Volume",
            "quality": "Qualità",
            "recording": "Registrazione",
            "transcription": "Trascrizione",
            "no_signal": "Nessun segnale",
            "reconnecting": "Riconnessione",
            "low_quality": "Bassa qualità",
            "high_quality": "Alta qualità",
        ]

        // Portuguese
        tables["pt"] = [
            "connected": "Conectado",
            "connecting": "Conectando",
            "disconnected": "Desconectado",
            "error": "Erro",
            "channel": "Canal",
            "listeners": "Ouvintes",
            "now_playing": "Tocando agora",
            "mute": "Mudo",
            "unmute": "Ativar som",
            "play": "Reproduzir",
            "stop": "Parar",
            "next_channel": "Próximo canal",
            "previous_channel": "Canal anterior",
            "settings": "Configurações",
            "volume": "Volume",
            "quality": "Qualidade",
            "recording": "Gravando",
            "transcription": "Transcrição",
            "no_signal": "Sem sinal",
            "reconnecting": "Reconectando",
            "low_quality": "Baixa qualidade",
            "high_quality": "Alta qualidade",
        ]

        // Russian
        tables["ru"] = [
            "connected": "Подключено",
            "connecting": "Подключение",
            "disconnected": "Отключено",
            "error": "Ошибка",
            "channel": "Канал",
            "listeners": "Слушатели",
            "now_playing": "Сейчас играет",
            "mute": "Без звука",
            "unmute": "Включить звук",
            "play": "Воспроизвести",
            "stop": "Остановить",
            "next_channel": "Следующий канал",
            "previous_channel": "Предыдущий канал",
            "settings": "Настройки",
            "volume": "Громкость",
            "quality": "Качество",
            "recording": "Запись",
            "transcription": "Транскрипция",
            "no_signal": "Нет сигнала",
            "reconnecting": "Переподключение",
            "low_quality": "Низкое качество",
            "high_quality": "Высокое качество",
        ]

        // Arabic
        tables["ar"] = [
            "connected": "متصل",
            "connecting": "جاري الاتصال",
            "disconnected": "غير متصل",
            "error": "خطأ",
            "channel": "قناة",
            "listeners": "المستمعون",
            "now_playing": "يتم التشغيل الآن",
            "mute": "كتم الصوت",
            "unmute": "إلغاء كتم الصوت",
            "play": "تشغيل",
            "stop": "إيقاف",
            "next_channel": "القناة التالية",
            "previous_channel": "القناة السابقة",
            "settings": "الإعدادات",
            "volume": "مستوى الصوت",
            "quality": "الجودة",
            "recording": "تسجيل",
            "transcription": "النسخ",
            "no_signal": "لا توجد إشارة",
            "reconnecting": "إعادة الاتصال",
            "low_quality": "جودة منخفضة",
            "high_quality": "جودة عالية",
        ]

        // Hindi
        tables["hi"] = [
            "connected": "कनेक्ट किया गया",
            "connecting": "कनेक्ट हो रहा है",
            "disconnected": "डिस्कनेक्ट किया गया",
            "error": "त्रुटि",
            "channel": "चैनल",
            "listeners": "श्रोता",
            "now_playing": "अभी चल रहा है",
            "mute": "म्यूट",
            "unmute": "अनम्यूट",
            "play": "चलाएं",
            "stop": "रोकें",
            "settings": "सेटिंग्स",
            "volume": "वॉल्यूम",
        ]

        // Thai
        tables["th"] = [
            "connected": "เชื่อมต่อแล้ว",
            "connecting": "กำลังเชื่อมต่อ",
            "disconnected": "ตัดการเชื่อมต่อ",
            "error": "ข้อผิดพลาด",
            "channel": "ช่อง",
            "listeners": "ผู้ฟัง",
            "now_playing": "กำลังเล่น",
            "mute": "ปิดเสียง",
            "unmute": "เปิดเสียง",
            "play": "เล่น",
            "stop": "หยุด",
            "settings": "การตั้งค่า",
            "volume": "ระดับเสียง",
        ]

        // Vietnamese
        tables["vi"] = [
            "connected": "Đã kết nối",
            "connecting": "Đang kết nối",
            "disconnected": "Đã ngắt kết nối",
            "error": "Lỗi",
            "channel": "Kênh",
            "listeners": "Người nghe",
            "now_playing": "Đang phát",
            "mute": "Tắt tiếng",
            "unmute": "Bật tiếng",
            "play": "Phát",
            "stop": "Dừng",
            "settings": "Cài đặt",
            "volume": "Âm lượng",
        ]

        // Indonesian
        tables["id"] = [
            "connected": "Terhubung",
            "connecting": "Menghubungkan",
            "disconnected": "Terputus",
            "error": "Kesalahan",
            "channel": "Saluran",
            "listeners": "Pendengar",
            "now_playing": "Sedang diputar",
            "mute": "Bisukan",
            "unmute": "Bunyikan",
            "play": "Putar",
            "stop": "Berhenti",
            "settings": "Pengaturan",
            "volume": "Volume",
        ]

        // Malay
        tables["ms"] = [
            "connected": "Disambungkan",
            "connecting": "Menyambung",
            "disconnected": "Diputuskan",
            "error": "Ralat",
            "channel": "Saluran",
            "listeners": "Pendengar",
            "play": "Main",
            "stop": "Berhenti",
            "settings": "Tetapan",
            "volume": "Kelantangan",
        ]

        // Turkish
        tables["tr"] = [
            "connected": "Bağlandı",
            "connecting": "Bağlanıyor",
            "disconnected": "Bağlantı kesildi",
            "error": "Hata",
            "channel": "Kanal",
            "listeners": "Dinleyiciler",
            "now_playing": "Şimdi çalıyor",
            "mute": "Sessiz",
            "unmute": "Sesi aç",
            "play": "Oynat",
            "stop": "Durdur",
            "settings": "Ayarlar",
            "volume": "Ses",
        ]

        // Polish
        tables["pl"] = [
            "connected": "Połączono",
            "connecting": "Łączenie",
            "disconnected": "Rozłączono",
            "error": "Błąd",
            "channel": "Kanał",
            "listeners": "Słuchacze",
            "play": "Odtwórz",
            "stop": "Zatrzymaj",
            "settings": "Ustawienia",
            "volume": "Głośność",
        ]

        // Dutch
        tables["nl"] = [
            "connected": "Verbonden",
            "connecting": "Verbinden",
            "disconnected": "Verbroken",
            "error": "Fout",
            "channel": "Kanaal",
            "listeners": "Luisteraars",
            "play": "Afspelen",
            "stop": "Stoppen",
            "settings": "Instellingen",
            "volume": "Volume",
        ]

        // Swedish
        tables["sv"] = [
            "connected": "Ansluten",
            "connecting": "Ansluter",
            "disconnected": "Frånkopplad",
            "error": "Fel",
            "channel": "Kanal",
            "listeners": "Lyssnare",
            "play": "Spela",
            "stop": "Stoppa",
            "settings": "Inställningar",
            "volume": "Volym",
        ]

        // Danish
        tables["da"] = [
            "connected": "Tilsluttet",
            "connecting": "Tilslutter",
            "disconnected": "Frakoblet",
            "error": "Fejl",
            "channel": "Kanal",
            "listeners": "Lyttere",
            "play": "Afspil",
            "stop": "Stop",
            "settings": "Indstillinger",
            "volume": "Lydstyrke",
        ]

        // Norwegian
        tables["no"] = [
            "connected": "Tilkoblet",
            "connecting": "Kobler til",
            "disconnected": "Frakoblet",
            "error": "Feil",
            "channel": "Kanal",
            "listeners": "Lyttere",
            "play": "Spill",
            "stop": "Stopp",
            "settings": "Innstillinger",
            "volume": "Volum",
        ]

        // Finnish
        tables["fi"] = [
            "connected": "Yhdistetty",
            "connecting": "Yhdistetään",
            "disconnected": "Yhteys katkaistu",
            "error": "Virhe",
            "channel": "Kanava",
            "listeners": "Kuuntelijat",
            "play": "Toista",
            "stop": "Pysäytä",
            "settings": "Asetukset",
            "volume": "Äänenvoimakkuus",
        ]

        // Czech
        tables["cs"] = [
            "connected": "Připojeno",
            "connecting": "Připojování",
            "disconnected": "Odpojeno",
            "error": "Chyba",
            "channel": "Kanál",
            "listeners": "Posluchači",
            "play": "Přehrát",
            "stop": "Zastavit",
            "settings": "Nastavení",
            "volume": "Hlasitost",
        ]

        // Hungarian
        tables["hu"] = [
            "connected": "Csatlakoztatva",
            "connecting": "Csatlakozás",
            "disconnected": "Leválasztva",
            "error": "Hiba",
            "channel": "Csatorna",
            "listeners": "Hallgatók",
            "play": "Lejátszás",
            "stop": "Leállítás",
            "settings": "Beállítások",
            "volume": "Hangerő",
        ]

        // Romanian
        tables["ro"] = [
            "connected": "Conectat",
            "connecting": "Se conectează",
            "disconnected": "Deconectat",
            "error": "Eroare",
            "channel": "Canal",
            "listeners": "Ascultători",
            "play": "Redare",
            "stop": "Oprire",
            "settings": "Setări",
            "volume": "Volum",
        ]

        // Ukrainian
        tables["uk"] = [
            "connected": "Підключено",
            "connecting": "Підключення",
            "disconnected": "Від'єднано",
            "error": "Помилка",
            "channel": "Канал",
            "listeners": "Слухачі",
            "play": "Відтворити",
            "stop": "Зупинити",
            "settings": "Налаштування",
            "volume": "Гучність",
        ]

        // Hebrew
        tables["he"] = [
            "connected": "מחובר",
            "connecting": "מתחבר",
            "disconnected": "מנותק",
            "error": "שגיאה",
            "channel": "ערוץ",
            "listeners": "מאזינים",
            "play": "נגן",
            "stop": "עצור",
            "settings": "הגדרות",
            "volume": "עוצמת קול",
        ]

        // Greek
        tables["el"] = [
            "connected": "Συνδεδεμένο",
            "connecting": "Σύνδεση",
            "disconnected": "Αποσυνδεδεμένο",
            "error": "Σφάλμα",
            "channel": "Κανάλι",
            "listeners": "Ακροατές",
            "play": "Αναπαραγωγή",
            "stop": "Διακοπή",
            "settings": "Ρυθμίσεις",
            "volume": "Ένταση",
        ]

        return tables
    }
}
