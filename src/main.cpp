#include <Arduino.h>
#include <EEPROM.h>
#include <IWatchdog.h>

// ============================================================
// Pins
// ============================================================
#define HALL_1_PIN PB6
#define HALL_2_PIN PB7
#define HALL_3_PIN PB8

#define AH_PIN PA8
#define AL_PIN PA7
#define BH_PIN PA9
#define BL_PIN PB0
#define CH_PIN PA10
#define CL_PIN PB1

#define LED_PIN PC13

HardwareSerial CMD(PA3, PA2); // RX=PA3, TX=PA2

// ============================================================
// Tunables — Ayarlanabilir Parametreler
// ============================================================

// Motor kontrol döngüsü periyodu — 60µs = ~16.6kHz tick hızı
static constexpr uint32_t CONTROL_PERIOD_US          = 60;
// Gecikme durumunda telafi edilecek maksimum tick sayısı
static constexpr uint8_t  MAX_CONTROL_CATCHUP_TICKS  = 4;

// PWM çözünürlüğü (bit) — 8 bit = 0-255 arası duty
static constexpr uint8_t  PWM_RESOLUTION_BITS        = 8;

// Not: analogWriteFrequency her STM32 core'da garanti değil.
// BLDC_USE_ANALOGWRITE_FREQUENCY tanımlarsan deneyecek.
// Hedef PWM frekansı — MOSFET anahtarlama hızı
static constexpr uint32_t PWM_TARGET_FREQ_HZ         = 15000;

// Motor çalışırken izin verilen minimum hedef duty (kalkış torku kickDuty ile verilir)
static constexpr uint8_t  STARTUP_MIN_DUTY           = 0;
// Kalkışta hall geçişi gelmezse timeout — düşük hız/kalkış için biraz uzatıldı.
static constexpr uint32_t START_NO_HALL_TIMEOUT_MS   = 700;
// Yön değiştirirken nötr bekleme süresi — akım sıfırlanması için
static constexpr uint32_t DIRECTION_NEUTRAL_MS       = 80;

// Hall sinyali stabil kabul için gereken örnek sayısı (debounce)
static constexpr uint8_t  HALL_STABLE_SAMPLES        = 2;
// Eski sürümde hall geçişi 6-8ms içinde gelmezse sürüş kesiliyordu.
// Düşük hızda hall geçişi doğal olarak daha geç gelir; bu yüzden geçerli stabil hall
// artık motorControlTick içinde sürekli geçerli kabul edilir. Bu sabit yalnızca geriye
// dönük referans olarak bırakıldı.
static constexpr uint32_t HALL_VALID_STALE_US        = 6000;
// Hall ham değeri geçersiz olursa son geçerli state kısa süre tutulur.
static constexpr uint32_t INVALID_HALL_HOLD_US       = 15000;
// Hall geçersiz kalma süresi — sonrasında motor durdurulur
static constexpr uint32_t INVALID_HALL_STOP_US       = 25000;

// Geçersiz hall geçiş sayısı fault eşiği
static constexpr uint8_t  INVALID_TRANSITION_THRESHOLD = 20;

// CLI komut satırı boş kalma timeout — otomatik gönderme
static constexpr uint32_t CMD_IDLE_TIMEOUT_MS        = 150;

// Motor pole çifti sayısı — RPM hesaplamasında kullanılır
static constexpr uint8_t  POLE_PAIRS                 = 15;
// Telemetri gönderim aralığı (UART üzerinden)
static constexpr uint32_t TELEMETRY_INTERVAL_MS      = 100;
// Komut gelmezse motor durdurma süresi (güvenlik)
static constexpr uint32_t CMD_WATCHDOG_MS             = 800;
// Komut kuyrugunda stale kabul süresi — watchdog'tan küçük olmalı
static constexpr uint32_t CMD_STALE_MS                = 600;

// Brake timeout — motor süresiz brake'te kalmasın (ms)
static constexpr uint16_t DEFAULT_BRAKE_HOLD_MS       = 3000;

// Donanımsal watchdog timeout (µs) — 500ms
static constexpr uint32_t IWDG_TIMEOUT_US             = 500000;

// === PID RPM Control Ayarları ===

// PID döngüsü periyodu (ms) — 20ms = 50Hz
static constexpr uint32_t PID_INTERVAL_MS             = 20;
// RPM düşük geçiren filtre alfa katsayısı (0-1 arası, yüksek = hızlı yanıt)
static constexpr float    RPM_FILTER_ALPHA            = 0.25f;
// RPM feedback timeout — bu süre boyunca hall geçişi gelmezse fault (ms)
static constexpr uint32_t RPM_FEEDBACK_TIMEOUT_MS     = 1000;
// Varsayılan PID kazançları — artık DEFAULT_SPEED_KP/KI kullanılıyor
// (geriye uyumluluk için kaldırıldı)
// Integral anti-windup sınırı
static constexpr float    PID_INTEGRAL_LIMIT          = 500.0f;

// === Speed PI Control Ayarları ===

// Base PWM/feed-forward — statik sürtünmeyi aşmak için
static constexpr uint8_t  DEFAULT_BASE_PWM_LOW         = 55;   // hedef RPM <= 30
static constexpr uint8_t  DEFAULT_BASE_PWM_MID         = 45;   // hedef RPM <= 150
static constexpr uint8_t  DEFAULT_BASE_PWM_HIGH        = 35;   // hedef RPM > 150

// Start boost PWM değerleri
static constexpr uint8_t  DEFAULT_BOOST_LOW_PWM        = 65;
static constexpr uint8_t  DEFAULT_BOOST_MID_PWM        = 65;
static constexpr uint8_t  DEFAULT_BOOST_HIGH_PWM       = 65;
static constexpr uint16_t DEFAULT_BOOST_TIME_MS        = 150;
static constexpr uint8_t  DEFAULT_BOOST_EDGE_THRESH    = 3;

// RPM ramp hızları (RPM/saniye)
static constexpr float    DEFAULT_RAMP_UP_RPM_SEC      = 100.0f;
static constexpr float    DEFAULT_RAMP_DOWN_RPM_SEC    = 200.0f;

// Varsayılan Speed PI kazançları
static constexpr float    DEFAULT_SPEED_KP             = 0.80f;
static constexpr float    DEFAULT_SPEED_KI             = 0.10f;

// PI çıkış PWM sınırları (speed mode)
static constexpr uint8_t  SPEED_PI_MAX_PWM             = 180;
static constexpr uint8_t  SPEED_PI_MIN_PWM             = 0;

// Start boost geçiş RPM eşiği
static constexpr float    BOOST_RPM_THRESHOLD          = 3.0f;

// === Servis Modu Ayarları ===

// Identify modunda her step için toggle sayısı — hall tespiti için
static constexpr uint16_t IDENTIFY_STEP_TOGGLES      = 100;
// Identify modunda uygulanan duty değeri
static constexpr uint8_t  IDENTIFY_DUTY              = 35;
static constexpr uint16_t IDENTIFY_TOGGLE_MS         = 5;
// Toggle sonrası hall okuma için bekleme A (ms)
static constexpr uint16_t IDENTIFY_SETTLE_A_MS       = 12;
// Hall okuma için bekleme B (ms)
static constexpr uint16_t IDENTIFY_SETTLE_B_MS       = 6;
// Test modunda uygulanan duty değeri
static constexpr uint8_t  TEST_DUTY                  = 60;
// Test modunda her step süresi (ms)
static constexpr uint16_t TEST_STEP_MS               = 2000;
// Test modunda hall raporlama gecikmesi (ms)
static constexpr uint16_t TEST_REPORT_MS             = 1500;

// Tarama modu toplam süresi (ms)
static constexpr uint32_t SCAN_DURATION_MS           = 10000;
// Tarama modunda hall okuma aralığı (ms)
static constexpr uint16_t SCAN_POLL_MS               = 5;

// === CLI / UART Ayarları ===

// Maksimum komut satırı uzunluğu (karakter)
static constexpr uint8_t  UART_LINE_MAX              = 64;
// Komut kuyruğu kapasitesi (adet)
static constexpr uint8_t  CMD_QUEUE_LEN              = 8;
// UART RX ring buffer boyutu (karakter)
static constexpr uint16_t RX_RING_LEN                = 128;

// ============================================================
// Hall Map — Hall sinyali → motor faz eşleme
// Kalıcı yapılandırma — EEPROM'da saklanır
// Varsayılan: {255, 1, 3, 2, 5, 0, 4, 255} (255 = geçersiz durum)
// ============================================================
const uint8_t DEFAULT_HALL_MAP[8] = {255, 1, 3, 2, 5, 0, 4, 255};
uint8_t hallToMotor[8]            = {255, 1, 3, 2, 5, 0, 4, 255};

// EEPROM'da saklanan hall map yapısı — bütünlük kontrolü ile
struct SavedHallMap {
  uint32_t magic;
  uint8_t version;
  uint8_t map[8];
  uint8_t checksum;
};

// Motor parametreleri — kick, ramp, default PWM ayarları
struct SavedConfig {
  uint32_t magic;
  uint8_t version;

  uint8_t kickEnabled;
  uint8_t rampEnabled;

  uint8_t kickDuty;
  uint16_t kickMs;

  uint8_t rampStep;
  uint16_t rampIntervalMs;

  uint8_t defaultPwm;  // Control modu varsayılan PWM (EEPROM'dan okunur)
  uint16_t brakeHoldMs; // Brake timeout süresi (ms)

  uint8_t checksum;
};

// Hall map EEPROM adresi ve doğrulama sabitleri
static constexpr uint32_t HALLMAP_MAGIC   = 0x484D4150UL; // "HMAP"
static constexpr uint8_t  HALLMAP_VERSION = 1;
static constexpr int      EEPROM_ADDR_MAP = 0;

// Config EEPROM adresi ve doğrulama sabitleri
static constexpr uint32_t CFG_MAGIC       = 0x43464731UL; // "CFG1"
static constexpr uint8_t  CFG_VERSION     = 4;  // v4: brakeHoldMs eklendi
static constexpr int      EEPROM_ADDR_CFG = 64;

// Çalışma modu (Normal/Control/Settings) EEPROM adresi
static constexpr uint32_t MODE_MAGIC      = 0x4D4F4445UL; // "MODE"
static constexpr uint8_t  MODE_VERSION    = 1;
static constexpr int      EEPROM_ADDR_MODE = 128;

// ============================================================
// Enums
// ============================================================
enum class OperatingMode : uint8_t {
  Normal = 0,
  Control = 1,
  Settings = 2
};

enum class MotorPhase : uint8_t {
  Stopped = 0,
  Kick,
  Running,
  NeutralWait,
  Fault,
  Brake  // Aktif fren - low-side ON, high-side OFF
};

enum class MotorFaultCode : uint8_t {
  None = 0,
  StartupNoHall,
  InvalidHallPersist,
  IllegalTransitionSpam,
  QueueOverflow
};

enum class ServiceTask : uint8_t {
  None = 0,
  Scan,    // Hall sinyallerini tara
  Test,    // Her fazı tek tek test et
  Identify // Hall eşlemesini otomatik bul
};

enum class IdentifyPhase : uint8_t {
  Idle = 0,
  Toggle,   // İki faz arasında toggle yap
  SettleA,  // İlk hall okuma bekleme
  SettleB   // İkinci hall okuma bekleme
};

enum class SpeedControlMode : uint8_t {
  Duty = 0,   // Manuel PWM/duty modu
  Speed = 1   // RPM kapalı döngü hız kontrolü
};

enum class SpeedPhase : uint8_t {
  Idle = 0,       // Hedef RPM yok
  StartBoost,     // Kalkış boost aktif
  SpeedPI         // PI kontrol aktif
};

enum class CommandSource : uint8_t {
  USB = 0,
  UART    // CMD serial port
};

// Çalışma modu EEPROM yapısı
struct SavedMode {
  uint32_t magic;
  uint8_t version;
  uint8_t mode;      // 0=Normal, 1=Control, 2=Settings
  uint8_t checksum;
};

// ============================================================
// Runtime Structs — Çalışma Zamanı Değişkenleri
// ============================================================

// Hall sensör runtime — debounce, validasyon, zaman damgaları
struct HallRuntime {
  uint8_t rawCandidate = 0;         // Aday ham hall değeri
  uint8_t rawCandidateCount = 0;    // Aday tekrar sayısı (debounce)
  uint8_t stableRaw = 0;            // Stabil kabul edilen ham değer

  uint8_t lastValidRaw = 0;         // Son geçerli ham hall
  uint8_t lastValidState = 255;     // Son geçerli mapped durum (0-5)

  uint32_t lastStableChangeUs = 0;  // Son stabil değişim zamanı
  uint32_t lastValidUs = 0;         // Son geçerli hall zamanı
  uint32_t invalidSinceUs = 0;      // Hall geçersiz kabul başlangıcı

  uint32_t invalidRawCount = 0;         // Geçersiz ham hall sayısı
  uint32_t invalidTransitionCount = 0;  // Geçersiz geçiş sayısı

  uint32_t prevTransitionUs = 0;  // Önceki hall geçiş zamanı (RPM için)
  uint32_t hallPeriodUs = 0;      // Hall periyodu (RPM hesaplaması)
  uint32_t validTransitionCount = 0; // Geçerli hall geçiş sayısı (speed mode)
};

// Motor runtime — faz, duty, ramp, fault yönetimi
struct MotorRuntime {
  MotorPhase phase = MotorPhase::Stopped;
  MotorFaultCode faultCode = MotorFaultCode::None;

  int8_t direction = 1;           // 1 = ileri, -1 = geri
  uint8_t currentDuty = 0;        // Anlık uygulanan duty
  uint8_t targetDuty  = 0;        // Hedeflenen duty

  uint32_t phaseStartMs = 0;          // Mevcut faz başlangıç zamanı
  uint32_t lastRampUpdateMs = 0;      // Son ramp güncelleme zamanı
  uint32_t startHallDeadlineMs = 0;   // Kalkış hall timeout süresi

  bool restartPending = false;        // Yeniden başlatma bekleniyor mu
  int8_t pendingDirection = 1;        // Bekleyen yön
  uint32_t neutralReleaseMs = 0;      // Nötr bekleme bitiş zamanı

  uint8_t lastAppliedDriveState = 255;        // Son uygulanan faz durumu
  uint8_t lastAppliedDuty = 0;                // Son uygulanan duty
  uint8_t lastDrivenElectricalState = 255;    // Son sürülen elektriksel durum
};

// Servis görevleri runtime — scan, test, identify
struct ServiceRuntime {
  ServiceTask task = ServiceTask::None;
  uint32_t startMs = 0;
  uint32_t nextActionMs = 0;

  // scan — hall sinyallerini canlı izleme
  uint8_t scanLastHall = 255;

  // test — her fazı sırayla uygula, hall oku
  uint8_t testStep = 0;
  bool testStepActive = false;
  bool testStepReported = false;
  uint32_t testStepStartMs = 0;

  // identify — hall eşlemesini otomatik çıkar
  uint8_t identifyStep = 0;
  IdentifyPhase identifyPhase = IdentifyPhase::Idle;
  uint16_t identifyToggleCounter = 0;
  bool identifyToggleFlip = false;
  uint8_t identifyHallA = 255;
  uint8_t identifyCandidateMap[8] = {255,255,255,255,255,255,255,255};
};

// CLI satır durumu — tampon ve zaman damgası
struct CliLineState {
  char line[UART_LINE_MAX];
  uint8_t index = 0;
  uint32_t lastInputMs = 0;
};

// UART RX ring buffer — kesme-güvenli dairesel tampon
struct RxRing {
  volatile uint16_t head = 0;
  volatile uint16_t tail = 0;
  char data[RX_RING_LEN];
};

// Komut kuyruğu elemanı — timestamp ile stale detection
struct CommandItem {
  char text[UART_LINE_MAX];
  CommandSource src;
  uint32_t timestampMs;  // [architecture] Komut gelis zamani
};

// Bekleyen komut istekleri — deferred apply
struct CommandRequest {
  bool hasRunRequest = false;
  int8_t runDirection = 1;

  bool hasStopRequest = false;
  bool hasTargetDutyUpdate = false;
  uint8_t requestedTargetDuty = 0;
};

// ============================================================
// Globals — Küresel Değişkenler
// ============================================================

// Kick (kalkış darbesi) ayarları
bool kickEnabled = true;
bool rampEnabled = true;

uint8_t  kickDuty       = 225;  // düşük PWM komutunda bile güçlü ilk kalkış darbesi
uint16_t kickMs         = 50;   // hub motor kalkışı için yeterli süre
uint8_t  rampStep       = 32;   // kick sonrası yumuşak düşüş
uint16_t rampIntervalMs = 2;    // ramp aralığı

// Debug ve hata bayrakları
bool verboseDebug = false;
bool queueOverflowFlag = false;

// Aktif çalışma modu ve servis yanıt portu
OperatingMode activeMode = OperatingMode::Normal;
CommandSource serviceReplySrc = CommandSource::USB;

// Control modu runtime
uint8_t  controlPwmValue = 150;  // varsayılan sürüş PWM
uint16_t brakeHoldMs = DEFAULT_BRAKE_HOLD_MS; // brake timeout
int8_t   controlDirection = 0;   // 0=durdu, 1=ileri, -1=geri

// Runtime nesneleri — hall, motor, servis
HallRuntime hallRt;
MotorRuntime motorRt;
ServiceRuntime serviceRt;

// Kontrol döngüsü zamanlaması
uint32_t nextControlTickUs = 0;
uint32_t lastTelemetryMs = 0;
uint32_t lastMotorCommandMs = 0;
uint32_t lastUartActivityMs = 0;  // [safety] Host connection monitor

// CLI durumları — USB ve UART için ayrı
CliLineState usbCli;
CliLineState cmdCli;

// UART RX ring buffer'ları
RxRing usbRx;
RxRing cmdRx;

// Komut kuyruğu — başlangıç ve bitiş işaretçileri
CommandItem cmdQueue[CMD_QUEUE_LEN];
uint8_t cmdQueueHead = 0;
uint8_t cmdQueueTail = 0;

// Bekleyen motor komutları
CommandRequest pendingReq;

// Hall ISR önbellek — kesme içinden hızlı yazma
volatile uint8_t  isr_rawHall = 0;
volatile uint32_t isr_hallTimeUs = 0;
volatile bool     isr_hallEvent = false;

// === PID/Speed PI RPM Control Runtime ===

struct PidRuntime {
  bool enabled = false;                    // Speed PI modu aktif mi
  SpeedControlMode controlMode = SpeedControlMode::Duty;
  SpeedPhase speedPhase = SpeedPhase::Idle;

  int32_t targetRpmCmd = 0;               // Komut edilen hedef RPM
  float targetRpmRamped = 0.0f;           // Ramp edilmiş hedef RPM (PI girdisi)
  float kp = DEFAULT_SPEED_KP;
  float ki = DEFAULT_SPEED_KI;

  float filteredRpm = 0.0f;               // Düşük geçiren filtrelenmiş RPM
  float errorIntegral = 0.0f;             // Integral toplamı
  float prevError = 0.0f;                 // Önceki hata
  uint32_t lastPidTickMs = 0;             // Son PI döngüsü zamanı
  uint32_t lastHallEventMs = 0;           // Son hall geçiş zamanı
  int8_t pidDirection = 0;                // PI yönü: 0=durdu, 1=ileri, -1=geri
  bool firstRun = true;                   // İlk çalıştırma bayrağı

  // Base PWM/feed-forward
  uint8_t basePwmLow = DEFAULT_BASE_PWM_LOW;
  uint8_t basePwmMid = DEFAULT_BASE_PWM_MID;
  uint8_t basePwmHigh = DEFAULT_BASE_PWM_HIGH;

  // Start boost
  uint8_t boostLowPwm = DEFAULT_BOOST_LOW_PWM;
  uint8_t boostMidPwm = DEFAULT_BOOST_MID_PWM;
  uint8_t boostHighPwm = DEFAULT_BOOST_HIGH_PWM;
  uint16_t boostTimeMs = DEFAULT_BOOST_TIME_MS;
  uint8_t boostEdgeThreshold = DEFAULT_BOOST_EDGE_THRESH;

  // RPM ramp
  float rampUpPerSec = DEFAULT_RAMP_UP_RPM_SEC;
  float rampDownPerSec = DEFAULT_RAMP_DOWN_RPM_SEC;

  // Start boost runtime
  uint32_t boostStartMs = 0;
  uint32_t boostStartEdgeCount = 0;
  uint32_t hallEdgeCounter = 0;           // Toplam hall kenar sayacı

  // Telemetri için son hesaplama değerleri
  float piOutput = 0.0f;
  float basePwm = 0.0f;
  float rawRpm = 0.0f;
};

PidRuntime pidRt;

// Telemetri debug modu — detaylı çıktı
bool telemetryDebug = false;

// Telemetri gönderim aralığı (ayarlanabilir)
uint32_t telemetryIntervalMs = TELEMETRY_INTERVAL_MS;

// Forward declarations
uint32_t calculateRPM();
void processCommand(char* cmd, CommandSource src);
void stopMotorImmediate();
void cancelServiceTask();
void triggerFault(MotorFaultCode fault);
void beginRunRequest(int8_t requestedDirection);
void beginNeutralDirectionSwitch(int8_t requestedDirection);

// ============================================================
// Helpers — Yardımcı Fonksiyonlar
// ============================================================

// Değer aralık sınırlama (min/max clamp)
template <typename T>
static inline T clampValue(T v, T lo, T hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// Motor aktif sürüşte mi? (Kick veya Running — Brake aktif sürüş değil)
bool isMotorDriveActive() {
  return motorRt.phase == MotorPhase::Kick || motorRt.phase == MotorPhase::Running;
}

// PID integral ve durum sıfırla — güvenli durum geçişlerinde çağrılır
void resetPidState() {
  pidRt.errorIntegral = 0.0f;
  pidRt.prevError = 0.0f;
  pidRt.filteredRpm = 0.0f;
  pidRt.firstRun = true;
  pidRt.lastHallEventMs = 0;
  pidRt.hallEdgeCounter = 0;
  pidRt.speedPhase = SpeedPhase::Idle;
  pidRt.targetRpmRamped = 0.0f;
  pidRt.piOutput = 0.0f;
  pidRt.basePwm = 0.0f;
  pidRt.rawRpm = 0.0f;
  pidRt.boostStartMs = 0;
  pidRt.boostStartEdgeCount = 0;
}

// PID modunu devre dışı bırak — tüm durumu sıfırla
void disablePidMode() {
  pidRt.enabled = false;
  pidRt.controlMode = SpeedControlMode::Duty;
  pidRt.targetRpmCmd = 0;
  pidRt.targetRpmRamped = 0.0f;
  pidRt.pidDirection = 0;
  pidRt.speedPhase = SpeedPhase::Idle;
  resetPidState();
}

// RPM hesapla — hall periodundan mekanik RPM
// 6 geçiş/elektriksel tur × POLE_PAIRS elektriksel tur/mekanik tur
uint32_t calculateRPM() {
  if (hallRt.hallPeriodUs == 0) return 0;
  if (!isMotorDriveActive()) return 0;

  // Son hall geçişi 500ms'den eski ise RPM = 0 (stale guard)
  uint32_t sinceLastTransition = micros() - hallRt.prevTransitionUs;
  if (sinceLastTransition > 500000UL) return 0;

  // 6 hall geçişi/elektriksel tur × POLE_PAIRS = geçiş/mekanik tur
  uint32_t denominator = (uint32_t)hallRt.hallPeriodUs * (6UL * POLE_PAIRS);
  if (denominator == 0) return 0;
  return 60000000UL / denominator;
}

// Düşük geçiren filtre uygula — ham RPM'den gürültüsüz değer
float applyRpmFilter(float rawRpm) {
  pidRt.filteredRpm = RPM_FILTER_ALPHA * rawRpm + (1.0f - RPM_FILTER_ALPHA) * pidRt.filteredRpm;
  return pidRt.filteredRpm;
}

// Speed PI kontrol döngüsü — 50Hz (20ms) çalışır
// MotorRt.targetDuty ve currentDuty'yi günceller
// NOT: Doğrudan MOSFET yazmaz, mevcut motorControlTick'e bırakır
void runSpeedControlLoop(uint32_t nowMs) {
  if (!pidRt.enabled) return;
  if ((uint32_t)(nowMs - pidRt.lastPidTickMs) < PID_INTERVAL_MS) return;
  pidRt.lastPidTickMs = nowMs;

  float dt = (float)PID_INTERVAL_MS / 1000.0f;

  // Hedef RPM 0 → motor durdur
  if (pidRt.targetRpmCmd == 0) {
    if (isMotorDriveActive()) {
      stopMotorImmediate();
    }
    pidRt.targetRpmRamped = 0.0f;
    pidRt.speedPhase = SpeedPhase::Idle;
    resetPidState();
    motorRt.targetDuty = 0;
    return;
  }

  // RPM ramp hesapla
  float targetAbs = (float)abs(pidRt.targetRpmCmd);
  if (pidRt.targetRpmRamped < targetAbs) {
    pidRt.targetRpmRamped += pidRt.rampUpPerSec * dt;
    if (pidRt.targetRpmRamped > targetAbs) pidRt.targetRpmRamped = targetAbs;
  } else if (pidRt.targetRpmRamped > targetAbs) {
    pidRt.targetRpmRamped -= pidRt.rampDownPerSec * dt;
    if (pidRt.targetRpmRamped < targetAbs) pidRt.targetRpmRamped = targetAbs;
  }

  // Yön belirle
  int8_t dir = (pidRt.targetRpmCmd > 0) ? 1 : -1;

  // Motor duruyorsa başlat
  if (!isMotorDriveActive() && motorRt.phase != MotorPhase::NeutralWait) {
    // Yön değişimi kontrolü
    if (motorRt.direction != 0 && motorRt.direction != dir) {
      beginNeutralDirectionSwitch(dir);
      return;
    }
    // Motor başlat
    motorRt.targetDuty = SPEED_PI_MIN_PWM;
    beginRunRequest(dir);
    pidRt.pidDirection = dir;
    pidRt.lastHallEventMs = nowMs;
    // Start boost fazına geç
    pidRt.speedPhase = SpeedPhase::StartBoost;
    pidRt.boostStartMs = nowMs;
    pidRt.boostStartEdgeCount = pidRt.hallEdgeCounter;
    pidRt.errorIntegral = 0.0f;
    pidRt.firstRun = true;
    return;
  }

  // Hall feedback timeout
  if (pidRt.lastHallEventMs != 0 && (uint32_t)(nowMs - pidRt.lastHallEventMs) > RPM_FEEDBACK_TIMEOUT_MS) {
    triggerFault(MotorFaultCode::StartupNoHall);
    disablePidMode();
    return;
  }

  // Ham RPM hesapla ve filtrele
  float rawRpm = (float)calculateRPM();
  pidRt.rawRpm = rawRpm;
  float filteredRpm = applyRpmFilter(rawRpm);

  // Base PWM hesapla (feed-forward)
  float basePwm;
  if (pidRt.targetRpmRamped <= 30.0f) {
    basePwm = (float)pidRt.basePwmLow;
  } else if (pidRt.targetRpmRamped <= 150.0f) {
    basePwm = (float)pidRt.basePwmMid;
  } else {
    basePwm = (float)pidRt.basePwmHigh;
  }
  pidRt.basePwm = basePwm;

  // ── Start Boost fazı ──
  if (pidRt.speedPhase == SpeedPhase::StartBoost) {
    // Boost PWM hesapla
    uint8_t boostPwm;
    if (pidRt.targetRpmRamped <= 30.0f) {
      boostPwm = pidRt.boostLowPwm;
    } else if (pidRt.targetRpmRamped <= 150.0f) {
      boostPwm = pidRt.boostMidPwm;
    } else {
      boostPwm = pidRt.boostHighPwm;
    }

    // Hall kenar sayacı
    uint32_t edgesSinceBoost = pidRt.hallEdgeCounter - pidRt.boostStartEdgeCount;

    // Geçiş koşulları
    bool edgesOk = (edgesSinceBoost >= pidRt.boostEdgeThreshold);
    bool rpmOk = (filteredRpm > BOOST_RPM_THRESHOLD);
    bool timeout = ((uint32_t)(nowMs - pidRt.boostStartMs) >= pidRt.boostTimeMs);

    if (edgesOk || rpmOk) {
      // SpeedPI fazına geç
      pidRt.speedPhase = SpeedPhase::SpeedPI;
      pidRt.errorIntegral = 0.0f;
      pidRt.firstRun = true;
    } else if (timeout) {
      // Timeout — boost süresi doldu, PI'a geç (retry)
      pidRt.speedPhase = SpeedPhase::SpeedPI;
      pidRt.errorIntegral = 0.0f;
      pidRt.firstRun = true;
    }

    // Boost duty uygula (PI bypass)
    motorRt.currentDuty = boostPwm;
    motorRt.targetDuty = boostPwm;
    return;
  }

  // ── Speed PI fazı ──
  if (pidRt.speedPhase == SpeedPhase::SpeedPI) {
    float error = pidRt.targetRpmRamped - filteredRpm;

    // P terimi
    float pTerm = pidRt.kp * error;

    // Conditional anti-windup: sadece çıkış doymamışsa integral güncelle
    float tentativeOutput = basePwm + pTerm + pidRt.ki * (pidRt.errorIntegral + error * dt);
    bool saturatedHigh = (tentativeOutput >= (float)SPEED_PI_MAX_PWM);
    bool saturatedLow = (tentativeOutput <= (float)SPEED_PI_MIN_PWM);
    bool errorReducesSat = (saturatedHigh && error < 0) || (saturatedLow && error > 0);

    if ((!saturatedHigh && !saturatedLow) || errorReducesSat) {
      pidRt.errorIntegral += error * dt;
    }
    pidRt.errorIntegral = clampValue<float>(pidRt.errorIntegral, -PID_INTEGRAL_LIMIT, PID_INTEGRAL_LIMIT);

    // I terimi
    float iTerm = pidRt.ki * pidRt.errorIntegral;

    // PI çıkışı (D yok)
    float piOutput = basePwm + pTerm + iTerm;
    pidRt.piOutput = piOutput;

    // Çıkışı sınırla
    uint8_t dutyCmd = (uint8_t)clampValue<float>(piOutput, (float)SPEED_PI_MIN_PWM, (float)SPEED_PI_MAX_PWM);

    // Minimum duty — hedef RPM != 0 iken motor dönmeli
    if (dutyCmd == 0 && pidRt.targetRpmCmd != 0) {
      dutyCmd = 1;
    }

    // Duty uygula (ramp bypass — PI doğrudan kontrol eder)
    if (isMotorDriveActive()) {
      motorRt.targetDuty = dutyCmd;
      motorRt.currentDuty = dutyCmd;
    }

    pidRt.prevError = error;
    pidRt.firstRun = false;
  }

  // Speed mode'da PI aktifken watchdog'u yenile
  if (isMotorDriveActive()) {
    lastMotorCommandMs = nowMs;
  }
}

// Motor meşgul mü? (Stopped ve Fault hariç her şey)
bool isMotorBusy() {
  return motorRt.phase != MotorPhase::Stopped && motorRt.phase != MotorPhase::Fault;
}

// Servis görevi aktif mi?
bool isServiceActive() {
  return serviceRt.task != ServiceTask::None;
}

// Mapped durum geçerli mi? (0-5 arası)
bool isValidMappedState(uint8_t s) {
  return s <= 5;
}

// ============================================================
// Port-aware Reply Helpers — Yanıt Gönderme Yardımcıları
// USB veya UART portuna göre yanıt yönlendirme
// ============================================================
Print* getReplyPort(CommandSource src) {
  return (src == CommandSource::UART) ? static_cast<Print*>(&CMD)
                                      : static_cast<Print*>(&Serial);
}

void replyStr(CommandSource src, const char* s) {
  getReplyPort(src)->print(s);
}

void replyFC(CommandSource src, const __FlashStringHelper* s) {
  getReplyPort(src)->print(s);
}

template <typename T>
void reply(CommandSource src, const T& v) {
  getReplyPort(src)->print(v);
}

template <typename T>
void replyLn(CommandSource src, const T& v) {
  getReplyPort(src)->println(v);
}

void replyLn(CommandSource src) {
  getReplyPort(src)->println();
}

// system log: yalnızca USB'ye bas, UART akışını tıkamasın
void sysLn(const __FlashStringHelper* s) {
  Serial.println(s);
}

void sysPrint(const __FlashStringHelper* s) {
  Serial.print(s);
}

template <typename T>
void sysPrintV(const T& v) {
  Serial.print(v);
}

template <typename T>
void sysLnV(const T& v) {
  Serial.println(v);
}

// ============================================================
// Low-level Hall — Hall Sensör İşlemleri
// ============================================================

// Hall pinlerini doğrudan oku — 3 bit (H1=bit2, H2=bit1, H3=bit0)
uint8_t forceReadRawHall() {
  uint8_t h = 0;
  h |= digitalRead(HALL_1_PIN) ? 4 : 0;
  h |= digitalRead(HALL_2_PIN) ? 2 : 0;
  h |= digitalRead(HALL_3_PIN) ? 1 : 0;
  return h;
}

// Hall ISR — kesme ile hall değişimini yakala, zaman damgası kaydet
void hallISR() {
  isr_rawHall = forceReadRawHall();
  isr_hallTimeUs = micros();
  isr_hallEvent = true;
}

// Ham hall değerini mapped motor durumuna çevir (0-5 arası)
uint8_t hallToState(uint8_t hallRaw) {
  if (hallRaw > 7) return 255;
  return hallToMotor[hallRaw];
}

// Hall geçişi geçerli mi? — İleri/geri 1 adım olmalı (atlama yok)
bool isTransitionValid(uint8_t prevState, uint8_t nextState) {
  if (!isValidMappedState(prevState) || !isValidMappedState(nextState)) return true;
  const uint8_t delta = (uint8_t)((nextState + 6 - prevState) % 6);
  return (delta == 1 || delta == 5);
}

// Hall runtime'ı başlat — ilk okuma, validasyon
void initHallRuntime() {
  const uint8_t raw = forceReadRawHall();
  const uint32_t nowUs = micros();

  hallRt.rawCandidate = raw;
  hallRt.rawCandidateCount = 1;
  hallRt.stableRaw = raw;
  hallRt.lastStableChangeUs = nowUs;

  const uint8_t mapped = hallToState(raw);
  hallRt.lastValidRaw = raw;
  hallRt.lastValidUs = nowUs;

  if (isValidMappedState(mapped)) {
    hallRt.lastValidState = mapped;
    hallRt.invalidSinceUs = 0;
  } else {
    hallRt.lastValidState = 255;
    hallRt.invalidSinceUs = nowUs;
  }
}

// Runtime tanılayıcılarını sıfırla — motor yeniden başlatma öncesi
void clearRuntimeDiagnostics() {
  hallRt.invalidRawCount = 0;
  hallRt.invalidTransitionCount = 0;
  hallRt.invalidSinceUs = 0;
  hallRt.prevTransitionUs = 0;
  hallRt.hallPeriodUs = 0;

  initHallRuntime();

  motorRt.lastDrivenElectricalState = 255;
  motorRt.lastAppliedDriveState = 255;
  motorRt.lastAppliedDuty = 0;
}

// Hall runtime güncelleme — debounce, validasyon, RPM periodu
// ISR'den gelen veriyi işler, yoksa doğrudan okur
void updateHallRuntime(uint32_t mainUs) {
  uint8_t tRaw;
  uint32_t tTime;
  bool hadIsrEvent = false;

  noInterrupts();
  if (isr_hallEvent) {
    tRaw = isr_rawHall;
    tTime = isr_hallTimeUs;
    isr_hallEvent = false;
    hadIsrEvent = true;
  }
  interrupts();

  if (!hadIsrEvent) {
    tRaw = forceReadRawHall();
    tTime = mainUs;
  }

  if (tRaw == hallRt.rawCandidate) {
    if (hallRt.rawCandidateCount < 255) hallRt.rawCandidateCount++;
  } else {
    hallRt.rawCandidate = tRaw;
    hallRt.rawCandidateCount = 1;
  }

  if (hallRt.rawCandidateCount < HALL_STABLE_SAMPLES) return;
  if (tRaw == hallRt.stableRaw) return;

  const uint8_t prevState = hallToState(hallRt.stableRaw);
  const uint8_t newState  = hallToState(tRaw);

  hallRt.stableRaw = tRaw;
  hallRt.lastStableChangeUs = tTime;

  if (!isValidMappedState(newState)) {
    if (hallRt.invalidSinceUs == 0) hallRt.invalidSinceUs = tTime;
    hallRt.invalidRawCount++;
    return;
  }

  if (!isTransitionValid(prevState, newState)) {
    if (hallRt.invalidSinceUs == 0) hallRt.invalidSinceUs = tTime;
    hallRt.invalidTransitionCount++;
    return;
  }

  // RPM: hall geçiş periodu hesapla — sadece ISR event'inden (keskin zaman damgası)
  if (hadIsrEvent && hallRt.prevTransitionUs != 0) {
    uint32_t period = tTime - hallRt.prevTransitionUs;
    if (period > 0 && period < 1000000UL) {
      hallRt.hallPeriodUs = period;
    }
  }
  if (hadIsrEvent) hallRt.prevTransitionUs = tTime;

  // PID feedback timeout güncelle — geçerli hall geçişi
  if (hadIsrEvent && pidRt.enabled) {
    pidRt.lastHallEventMs = millis();
    pidRt.hallEdgeCounter++;
  }

  hallRt.lastValidRaw = tRaw;
  hallRt.lastValidState = newState;
  hallRt.lastValidUs = tTime;
  hallRt.invalidSinceUs = 0;
  hallRt.validTransitionCount++;
}

// ============================================================
// Power Stage — Güç Katı Sürme (6-step BLDC komütasyon)
// PH_HIGH: Yüksek taraf PWM pinleri, PH_LOW: Düşük taraf dijital pinler
// ============================================================
static const uint8_t PH_HIGH[6] = {BH_PIN, CH_PIN, CH_PIN, AH_PIN, AH_PIN, BH_PIN};
static const uint8_t PH_LOW [6] = {AL_PIN, AL_PIN, BL_PIN, BL_PIN, CL_PIN, CL_PIN};

// Tüm fazları kapat — güvenli durum (coast)
void allOff() {
  analogWrite(AH_PIN, 0);
  analogWrite(BH_PIN, 0);
  analogWrite(CH_PIN, 0);

  digitalWrite(AL_PIN, LOW);
  digitalWrite(BL_PIN, LOW);
  digitalWrite(CL_PIN, LOW);

  motorRt.lastAppliedDriveState = 255;
  motorRt.lastAppliedDuty = 0;
}

// Aktif fren — tüm low-side ON, high-side OFF (sargı kısa devre)
void allBrake() {
  analogWrite(AH_PIN, 0);
  analogWrite(BH_PIN, 0);
  analogWrite(CH_PIN, 0);

  digitalWrite(AL_PIN, HIGH);
  digitalWrite(BL_PIN, HIGH);
  digitalWrite(CL_PIN, HIGH);

  motorRt.lastAppliedDriveState = 255;
  motorRt.lastAppliedDuty = 0;
}

// Brake durumuna geç — yön ve restart sıfırla
void enterBrake() {
  memset(&pendingReq, 0, sizeof(pendingReq)); // pending temizle — restart önle
  motorRt.phase = MotorPhase::Brake;
  motorRt.phaseStartMs = millis();
  motorRt.direction = 0;
  motorRt.currentDuty = 0;
  motorRt.targetDuty = 0;
  motorRt.restartPending = false;
  allBrake();
  resetPidState();
}

// Faz durumunu ve duty uygula — 6-step komütasyon
// Gereksiz pin değişimlerini önler (cache kontrolü)
void applyDriveState(uint8_t driveState, uint8_t dutyCycle) {
  dutyCycle = clampValue<uint8_t>(dutyCycle, 0, 255);

  if (dutyCycle == 0 || driveState > 5) {
    allOff();
    return;
  }

  if (motorRt.lastAppliedDriveState == driveState &&
      motorRt.lastAppliedDuty == dutyCycle) {
    return;
  }

  if (motorRt.lastAppliedDriveState <= 5 &&
      motorRt.lastAppliedDriveState != driveState) {
    const uint8_t oldH = PH_HIGH[motorRt.lastAppliedDriveState];
    const uint8_t oldL = PH_LOW[motorRt.lastAppliedDriveState];
    const uint8_t newH = PH_HIGH[driveState];
    const uint8_t newL = PH_LOW[driveState];

    if (oldH != newH) analogWrite(oldH, 0);
    if (oldL != newL) digitalWrite(oldL, LOW);

    if (oldL != newL) digitalWrite(newL, HIGH);
    analogWrite(newH, dutyCycle);
  } else if (motorRt.lastAppliedDriveState == driveState) {
    analogWrite(PH_HIGH[driveState], dutyCycle);
  } else {
    allOff();
    digitalWrite(PH_LOW[driveState], HIGH);
    analogWrite(PH_HIGH[driveState], dutyCycle);
  }

  motorRt.lastAppliedDriveState = driveState;
  motorRt.lastAppliedDuty = dutyCycle;
}

// ============================================================
// Storage — EEPROM Kalıcı Depolama
// Hall map ve config okuma/yazma, doğrulama
// ============================================================

// Varsayılan hall map'i yükle
void loadDefaultHallMap() {
  for (uint8_t i = 0; i < 8; i++) hallToMotor[i] = DEFAULT_HALL_MAP[i];
}

// Hall map geçerli mi? — 6 benzersiz durum (0-5) olmalı, 255'ler hariç
bool validateHallMap(const uint8_t mapIn[8]) {
  bool seen[6] = {false, false, false, false, false, false};
  uint8_t validCount = 0;

  for (uint8_t hall = 0; hall < 8; hall++) {
    const uint8_t state = mapIn[hall];
    if (state == 255) continue;
    if (state > 5) return false;
    if (seen[state]) return false;
    seen[state] = true;
    validCount++;
  }

  return validCount == 6;
}

// Aktif hall map'i doğrula
bool validateCurrentHallMap() {
  return validateHallMap(hallToMotor);
}

// Hall map checksum — XOR tabanlı bütünlük kontrolü
uint8_t calcMapChecksum(const uint8_t mapIn[8]) {
  uint8_t x = 0x5A;
  for (uint8_t i = 0; i < 8; i++) x ^= (uint8_t)(mapIn[i] + (i * 17));
  return x;
}

// Hall map EEPROM'a kaydet — doğrulama ile
bool saveHallMapToStorage() {
  SavedHallMap data;
  data.magic = HALLMAP_MAGIC;
  data.version = HALLMAP_VERSION;
  for (uint8_t i = 0; i < 8; i++) data.map[i] = hallToMotor[i];
  data.checksum = calcMapChecksum(data.map);

  EEPROM.put(EEPROM_ADDR_MAP, data);

  SavedHallMap check;
  EEPROM.get(EEPROM_ADDR_MAP, check);

  if (check.magic != HALLMAP_MAGIC) return false;
  if (check.version != HALLMAP_VERSION) return false;
  if (check.checksum != calcMapChecksum(check.map)) return false;
  if (!validateHallMap(check.map)) return false;
  return true;
}

// Hall map EEPROM'dan yükle — doğrulama ile
bool loadHallMapFromStorage() {
  SavedHallMap data;
  EEPROM.get(EEPROM_ADDR_MAP, data);

  if (data.magic != HALLMAP_MAGIC) return false;
  if (data.version != HALLMAP_VERSION) return false;
  if (data.checksum != calcMapChecksum(data.map)) return false;
  if (!validateHallMap(data.map)) return false;

  for (uint8_t i = 0; i < 8; i++) hallToMotor[i] = data.map[i];
  return true;
}

// Hall map yeniden yükle — EEPROM'dan, yoksa varsayılan
void reloadHallMap() {
  if (!loadHallMapFromStorage()) {
    loadDefaultHallMap();
  }
}

// Config değerlerini sınırla — geçerli aralık kontrolü
void clampConfig() {
  kickDuty = clampValue<uint8_t>(kickDuty, 1, 255);
  rampStep = clampValue<uint8_t>(rampStep, 1, 64);
  rampIntervalMs = clampValue<uint16_t>(rampIntervalMs, 1, 1000);
  kickMs = clampValue<uint16_t>(kickMs, 0, 3000);
  brakeHoldMs = clampValue<uint16_t>(brakeHoldMs, 500, 10000);
}

// Varsayılan config değerlerini yükle
void loadDefaultConfig() {
  kickEnabled = true;
  rampEnabled = true;
  kickDuty = 225;
  kickMs = 50;
  rampStep = 32;
  rampIntervalMs = 2;
  controlPwmValue = 150;
  brakeHoldMs = DEFAULT_BRAKE_HOLD_MS;
  clampConfig();
}

// Config checksum — XOR tabanlı
uint8_t calcCfgChecksum(const SavedConfig& cfg) {
  uint8_t x = 0x33;
  x ^= cfg.kickEnabled;
  x ^= cfg.rampEnabled;
  x ^= cfg.kickDuty;
  x ^= (uint8_t)(cfg.kickMs & 0xFF);
  x ^= (uint8_t)(cfg.kickMs >> 8);
  x ^= cfg.rampStep;
  x ^= (uint8_t)(cfg.rampIntervalMs & 0xFF);
  x ^= (uint8_t)(cfg.rampIntervalMs >> 8);
  x ^= cfg.defaultPwm;
  x ^= (uint8_t)(cfg.brakeHoldMs & 0xFF);
  x ^= (uint8_t)(cfg.brakeHoldMs >> 8);
  return x;
}

// Config EEPROM'a kaydet — doğrulama ile
bool saveConfigToStorage() {
  SavedConfig cfg;
  cfg.magic = CFG_MAGIC;
  cfg.version = CFG_VERSION;
  cfg.kickEnabled = kickEnabled ? 1 : 0;
  cfg.rampEnabled = rampEnabled ? 1 : 0;
  cfg.kickDuty = kickDuty;
  cfg.kickMs = kickMs;
  cfg.rampStep = rampStep;
  cfg.rampIntervalMs = rampIntervalMs;
  cfg.defaultPwm = controlPwmValue;
  cfg.brakeHoldMs = brakeHoldMs;
  cfg.checksum = calcCfgChecksum(cfg);

  EEPROM.put(EEPROM_ADDR_CFG, cfg);

  SavedConfig check;
  EEPROM.get(EEPROM_ADDR_CFG, check);

  if (check.magic != CFG_MAGIC) return false;
  if (check.version != CFG_VERSION) return false;
  if (check.checksum != calcCfgChecksum(check)) return false;
  return true;
}

bool loadConfigFromStorage() {
  SavedConfig cfg;
  EEPROM.get(EEPROM_ADDR_CFG, cfg);

  if (cfg.magic != CFG_MAGIC) return false;
  if (cfg.version != CFG_VERSION) return false;
  if (cfg.checksum != calcCfgChecksum(cfg)) return false;

  kickEnabled = cfg.kickEnabled ? true : false;
  rampEnabled = cfg.rampEnabled ? true : false;
  kickDuty = cfg.kickDuty;
  kickMs = cfg.kickMs;
  rampStep = cfg.rampStep;
  rampIntervalMs = cfg.rampIntervalMs;
  controlPwmValue = cfg.defaultPwm;
  brakeHoldMs = cfg.brakeHoldMs;
  clampConfig();
  return true;
}

// ============================================================
// Mode Persistence — Çalışma Modu Kalıcılığı (EEPROM)
// Normal / Control / Settings modlarını kaydet/yükle
// ============================================================

// Mod checksum — basit XOR
uint8_t calcModeChecksum(uint8_t mode) {
  return (uint8_t)(0xA5 ^ mode ^ 0x3C);
}

// Mod EEPROM'a kaydet
bool saveModeToStorage() {
  SavedMode data;
  data.magic = MODE_MAGIC;
  data.version = MODE_VERSION;
  data.mode = (uint8_t)activeMode;
  data.checksum = calcModeChecksum(data.mode);

  EEPROM.put(EEPROM_ADDR_MODE, data);

  SavedMode check;
  EEPROM.get(EEPROM_ADDR_MODE, check);

  if (check.magic != MODE_MAGIC) return false;
  if (check.version != MODE_VERSION) return false;
  if (check.checksum != calcModeChecksum(check.mode)) return false;
  return true;
}

bool loadModeFromStorage() {
  SavedMode data;
  EEPROM.get(EEPROM_ADDR_MODE, data);

  if (data.magic != MODE_MAGIC) return false;
  if (data.version != MODE_VERSION) return false;
  if (data.checksum != calcModeChecksum(data.mode)) return false;
  if (data.mode > 2) return false;

  activeMode = (OperatingMode)data.mode;
  return true;
}

// ============================================================
// Status / help
// ============================================================
// ============================================================
// Status / Help — Durum ve Yardım Fonksiyonları
// ============================================================

// Hall map'i serial porta yazdır
void printHallMap(CommandSource src) {
  replyFC(src, F("[MAP] "));
  for (uint8_t i = 0; i < 8; i++) {
    reply(src, hallToMotor[i]);
    replyFC(src, F(" "));
  }
  replyLn(src);
}

void printHelp(CommandSource src) {
  replyLn(src, F("============================="));
  replyLn(src, F(" f/forward  |  f<0-255>"));
  replyLn(src, F(" b/backward |  b<0-255>"));
  replyLn(src, F(" s/stop       (coast)"));
  replyLn(src, F(" x/brake      (active brake)"));
  replyLn(src, F(" pwm"));
  replyLn(src, F(" pwm <0-255>"));
  replyLn(src, F(" kick on/off"));
  replyLn(src, F(" ramp on/off"));
  replyLn(src, F(" kickduty <n>"));
  replyLn(src, F(" kickms <n>"));
  replyLn(src, F(" ramprate <n>"));
  replyLn(src, F(" rampms <n>"));
  replyLn(src, F(" defpwm <n>"));
  replyLn(src, F(" savecfg / loadcfg / defaults"));
  replyLn(src, F(" saveall"));
  replyLn(src, F(" hall"));
  replyLn(src, F(" map / save / reload / mapreset"));
  replyLn(src, F(" scan / test / identify"));
  replyLn(src, F(" status"));
  replyLn(src, F(" debug on/off"));
  replyLn(src, F(" clrerr         (clear error flags)"));
  replyLn(src, F(" mode          (show current)"));
  replyLn(src, F(" mode duty     (manual PWM)"));
  replyLn(src, F(" mode speed    (RPM PI control)"));
  replyLn(src, F(" mode control   (WASD+RPM)"));
  replyLn(src, F(" mode settings (temiz monitör)"));
  replyLn(src, F(" mode normal   (CLI+telemetri)"));
  replyLn(src, F("--- Speed PI Control ---"));
  replyLn(src, F(" rpm <signed>   (set target RPM)"));
  replyLn(src, F(" kp/ki <float>  (PI gains)"));
  replyLn(src, F(" base <lo> <mid> <hi>"));
  replyLn(src, F(" boost <lo> <mid> <hi> <ms>"));
  replyLn(src, F(" ramp <up> <down> (RPM/sec)"));
  replyLn(src, F(" spstat         (speed PI status)"));
  replyLn(src, F(" dbg on/off    (telemetry debug)"));
  replyLn(src, F(" telper <ms>   (telemetry period)"));
  replyLn(src, F("============================="));
}

void printStatus(CommandSource src) {
  replyLn(src, F("--- STATUS ---"));

  replyFC(src, F("Mode: "));
  if (activeMode == OperatingMode::Control) replyLn(src, F("CONTROL"));
  else if (activeMode == OperatingMode::Settings) replyLn(src, F("SETTINGS"));
  else replyLn(src, F("NORMAL"));

  replyFC(src, F("Phase: "));
  switch (motorRt.phase) {
    case MotorPhase::Stopped: replyLn(src, F("STOPPED")); break;
    case MotorPhase::Kick: replyLn(src, F("KICK")); break;
    case MotorPhase::Running: replyLn(src, F("RUNNING")); break;
    case MotorPhase::NeutralWait: replyLn(src, F("NEUTRAL_WAIT")); break;
    case MotorPhase::Fault: replyLn(src, F("FAULT")); break;
    case MotorPhase::Brake: replyLn(src, F("BRAKE")); break;
  }

  replyFC(src, F("FaultCode: ")); replyLn(src, (uint8_t)motorRt.faultCode);
  replyFC(src, F("Dir: "));
  if (motorRt.direction > 0) replyLn(src, F("FWD"));
  else if (motorRt.direction < 0) replyLn(src, F("REV"));
  else replyLn(src, F("STOP"));
  replyFC(src, F("TargetDuty: ")); replyLn(src, motorRt.targetDuty);
  replyFC(src, F("CurrentDuty: ")); replyLn(src, motorRt.currentDuty);
  replyFC(src, F("Kick: ")); reply(src, kickEnabled ? F("ON") : F("OFF"));
  replyFC(src, F(" Duty=")); reply(src, kickDuty);
  replyFC(src, F(" Ms=")); replyLn(src, kickMs);
  replyFC(src, F("Ramp: ")); reply(src, rampEnabled ? F("ON") : F("OFF"));
  replyFC(src, F(" Step=")); reply(src, rampStep);
  replyFC(src, F(" IntervalMs=")); replyLn(src, rampIntervalMs);

  replyFC(src, F("Hall stableRaw: ")); replyLn(src, hallRt.stableRaw);
  replyFC(src, F("Hall validState: ")); replyLn(src, hallRt.lastValidState);
  replyFC(src, F("Hall invalidRawCount: ")); replyLn(src, hallRt.invalidRawCount);
  replyFC(src, F("Hall invalidTransitionCount: ")); replyLn(src, hallRt.invalidTransitionCount);

  replyFC(src, F("Service: ")); replyLn(src, (uint8_t)serviceRt.task);
  replyFC(src, F("QueueOverflowFlag: ")); replyLn(src, queueOverflowFlag ? F("YES") : F("NO"));

  replyFC(src, F("PID: ")); reply(src, pidRt.enabled ? F("ON") : F("OFF"));
  replyFC(src, F(" Mode=")); reply(src, (uint8_t)pidRt.controlMode == 0 ? F("DUTY") : F("SPEED"));
  replyFC(src, F(" Phase=")); reply(src, (uint8_t)pidRt.speedPhase);
  replyFC(src, F(" Tcmd=")); reply(src, pidRt.targetRpmCmd);
  replyFC(src, F(" Tamp=")); reply(src, (int32_t)pidRt.targetRpmRamped);
  replyFC(src, F(" F=")); reply(src, (int32_t)pidRt.filteredRpm);
  replyFC(src, F(" Kp=")); reply(src, pidRt.kp);
  replyFC(src, F(" Ki=")); replyLn(src, pidRt.ki);

  if (activeMode == OperatingMode::Control) {
    replyFC(src, F("ControlPWM: ")); replyLn(src, controlPwmValue);
    replyFC(src, F("ControlDir: ")); replyLn(src, controlDirection);
  }
}

// ============================================================
// RX Ring / CLI — Komut Satırı ve Kuyruk Yönetimi
// Ring buffer, komut kuyruğu, satır işleme
// ============================================================

// Ring buffer'a karakter ekle
bool rxPush(RxRing& rb, char c) {
  uint16_t next = (uint16_t)((rb.head + 1) % RX_RING_LEN);
  if (next == rb.tail) return false;
  rb.data[rb.head] = c;
  rb.head = next;
  return true;
}

// [safety] UART aktivite zaman damgasını güncelle
void updateUartActivity() {
  lastUartActivityMs = millis();
}

bool rxPop(RxRing& rb, char* out) {
  if (rb.head == rb.tail) return false;
  *out = rb.data[rb.tail];
  rb.tail = (uint16_t)((rb.tail + 1) % RX_RING_LEN);
  return true;
}

// [architecture] Latest-command-wins: ayni kaynaktan gelen motion komutlari
// kuyrukta birikmez, son komut oncekinin ustune yazar
static bool isMotionCommand(const char* cmd); // tanim asagida, startsWith sonrasi

bool enqueueCommand(const char* cmd, CommandSource src) {
  // Ayni kaynaktan motion komutu varsa, uzerine yaz (latest-wins)
  if (isMotionCommand(cmd)) {
    uint8_t idx = cmdQueueTail;
    while (idx != cmdQueueHead) {
      if (cmdQueue[idx].src == src && isMotionCommand(cmdQueue[idx].text)) {
        strncpy(cmdQueue[idx].text, cmd, UART_LINE_MAX - 1);
        cmdQueue[idx].text[UART_LINE_MAX - 1] = '\0';
        cmdQueue[idx].timestampMs = millis();
        return true; // mevcut entry uzerine yazildi
      }
      idx = (uint8_t)((idx + 1) % CMD_QUEUE_LEN);
    }
  }

  uint8_t next = (uint8_t)((cmdQueueHead + 1) % CMD_QUEUE_LEN);
  if (next == cmdQueueTail) return false;

  strncpy(cmdQueue[cmdQueueHead].text, cmd, UART_LINE_MAX - 1);
  cmdQueue[cmdQueueHead].text[UART_LINE_MAX - 1] = '\0';
  cmdQueue[cmdQueueHead].src = src;
  cmdQueue[cmdQueueHead].timestampMs = millis();
  cmdQueueHead = next;
  return true;
}

// Komut kuyrugundan dequeue — stale command discard
bool dequeueCommand(CommandItem* out) {
  while (cmdQueueHead != cmdQueueTail) {
    *out = cmdQueue[cmdQueueTail];
    cmdQueueTail = (uint8_t)((cmdQueueTail + 1) % CMD_QUEUE_LEN);
    // Stale command discard
    uint32_t now = millis();
    if ((uint32_t)(now - out->timestampMs) > CMD_STALE_MS) {
      sysLn(F("[STALE] cmd discarded"));
      continue; // stale — sonraki elemana geç
    }
    return true;
  }
  return false;
}

// Ring buffer'dan UART portunu oku — gelen karakterleri topla
void uartDrainToRing(Stream& port, RxRing& rb) {
  uint8_t budget = 32;
  while (port.available() && budget--) {
    char c = (char)port.read();
    if (!rxPush(rb, c)) {
      queueOverflowFlag = true;
    }
  }
}

// Ring buffer'dan satırlara ayır — '\r' veya '\n' ile komut oluştur
void processRxRingToLines(RxRing& rb, CliLineState& st, CommandSource src) {
  char ch;
  uint8_t budget = 32;

  while (budget-- && rxPop(rb, &ch)) {
    st.lastInputMs = millis();

    if (ch == '\r' || ch == '\n') {
      if (st.index > 0) {
        st.line[st.index] = '\0';
        if (!enqueueCommand(st.line, src)) queueOverflowFlag = true;
        st.index = 0;
      }
    } else {
      if (st.index < (UART_LINE_MAX - 1)) {
        st.line[st.index++] = ch;
      } else {
        queueOverflowFlag = true;
        st.index = 0;
      }
    }
  }

  if (st.index > 0 && (millis() - st.lastInputMs) > CMD_IDLE_TIMEOUT_MS) {
    st.line[st.index] = '\0';
    if (!enqueueCommand(st.line, src)) queueOverflowFlag = true;
    st.index = 0;
  }
}

// String'i parçala — baş ve sondan boşluk karakterlerini temizle
void trimInPlace(char* s) {
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n' || s[len - 1] == ' ' || s[len - 1] == '\t')) {
    s[--len] = '\0';
  }

  size_t start = 0;
  while (s[start] == ' ' || s[start] == '\t') start++;

  if (start > 0) memmove(s, s + start, strlen(s + start) + 1);
}

// String'i küçük harfe çevir
void toLowerInPlace(char* s) {
  while (*s) {
    if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
    s++;
  }
}

bool startsWith(const char* s, const char* prefix) {
  while (*prefix) {
    if (*s++ != *prefix++) return false;
  }
  return true;
}

// [architecture] Latest-command-wins helper
static bool isMotionCommand(const char* cmd) {
  if (cmd[0] == '\0') return false;
  // Tek karakter: f, b, s, x
  if (cmd[1] == '\0') {
    char c = cmd[0];
    return (c == 'f' || c == 'b' || c == 's' || c == 'x');
  }
  // f/b ile duty: "f123", "b200"
  if ((cmd[0] == 'f' || cmd[0] == 'b') && cmd[1] >= '0' && cmd[1] <= '9') return true;
  // Tam string komutlar
  if (strcmp(cmd, "stop") == 0 || strcmp(cmd, "forward") == 0 || strcmp(cmd, "backward") == 0) return true;
  if (strcmp(cmd, "brake") == 0) return true;
  // pwm/rpm: sadece "pwm" (query) veya "pwm <val>" (set)
  if (startsWith(cmd, "pwm")) return true;
  if (startsWith(cmd, "rpm")) return true;
  return false;
}

bool parseLongAfterPrefix(const char* s, const char* prefix, long* out) {
  if (!startsWith(s, prefix)) return false;
  s += strlen(prefix);
  while (*s == ' ') s++;
  if (*s == '\0') return false;

  char* endptr = nullptr;
  long v = strtol(s, &endptr, 10);
  if (endptr == s) return false;
  *out = v;
  return true;
}

bool parseFloatAfterPrefix(const char* s, const char* prefix, float* out) {
  if (!startsWith(s, prefix)) return false;
  s += strlen(prefix);
  while (*s == ' ') s++;
  if (*s == '\0') return false;

  char* endptr = nullptr;
  float v = strtof(s, &endptr);
  if (endptr == s) return false;
  *out = v;
  return true;
}

// ============================================================
// Motor Actions — Motor Kontrol İşlemleri
// Dur, kick, çalış, yön değiştir, ramp uygula
// ============================================================

// Motoru anında durdur — tüm fazlar kapatılır
void stopMotorImmediate() {
  motorRt.phase = MotorPhase::Stopped;
  motorRt.direction = 0;
  motorRt.restartPending = false;
  motorRt.currentDuty = 0;
  allOff();
  resetPidState();
}

// Hata durumunu tetikle — motor durur, fault kodu kaydedilir
void triggerFault(MotorFaultCode fault) {
  motorRt.phase = MotorPhase::Fault;
  motorRt.faultCode = fault;
  motorRt.direction = 0;
  motorRt.restartPending = false;
  motorRt.currentDuty = 0;
  allOff();
  resetPidState();
}

// Kick veya Running fazını başlat — yön ve duty ayarlanır
// Kick: kısa yüksek tork darbesi, Running: normal sürüş
void beginKickOrRun(int8_t requestedDirection) {
  clearRuntimeDiagnostics();

  motorRt.direction = (requestedDirection >= 0) ? 1 : -1;

  motorRt.targetDuty = clampValue<uint8_t>(motorRt.targetDuty, 0, 255);

  motorRt.phaseStartMs = millis();
  motorRt.lastRampUpdateMs = motorRt.phaseStartMs;
  motorRt.startHallDeadlineMs = motorRt.phaseStartMs + START_NO_HALL_TIMEOUT_MS;
  motorRt.faultCode = MotorFaultCode::None;

  if (kickEnabled && kickMs > 0 && !pidRt.enabled) {
    const uint8_t ksDuty = (kickDuty > motorRt.targetDuty) ? kickDuty : motorRt.targetDuty;
    motorRt.currentDuty = ksDuty;
    motorRt.phase = MotorPhase::Kick;
  } else {
    motorRt.phase = MotorPhase::Running;
    if (rampEnabled && !pidRt.enabled) {
      if (motorRt.currentDuty == 0) {
        motorRt.currentDuty = 0;
      }
    } else {
      motorRt.currentDuty = motorRt.targetDuty;
    }
  }
}

// Yön değiştirme — önce nötr bekleme, sonra yeni yönde başlat
void beginNeutralDirectionSwitch(int8_t requestedDirection) {
  allOff();
  motorRt.currentDuty = 0;
  motorRt.phase = MotorPhase::NeutralWait;
  motorRt.restartPending = true;
  motorRt.pendingDirection = (requestedDirection >= 0) ? 1 : -1;
  motorRt.neutralReleaseMs = millis() + DIRECTION_NEUTRAL_MS;
}

// Çalıştırma isteği — servis aktifse reddet, Brake'ten çıkış, yön değişimi
void beginRunRequest(int8_t requestedDirection) {
  if (isServiceActive()) {
    return;
  }

  // Brake'ten çıkış — önce stop, sonra normal start
  if (motorRt.phase == MotorPhase::Brake) {
    motorRt.phase = MotorPhase::Stopped;
    motorRt.direction = 0;
    motorRt.currentDuty = 0;
    motorRt.restartPending = false;
    allOff();
  }

  if (isMotorDriveActive() || motorRt.phase == MotorPhase::Fault) {
    if (motorRt.direction != ((requestedDirection >= 0) ? 1 : -1) ||
        motorRt.phase == MotorPhase::Fault) {
      beginNeutralDirectionSwitch(requestedDirection);
      return;
    }
    // Aynı yön — duty güncellemesi applyPendingRequests() içinde yapıldı
    return;
  }

  if (motorRt.phase == MotorPhase::NeutralWait) {
    motorRt.restartPending = true;
    motorRt.pendingDirection = (requestedDirection >= 0) ? 1 : -1;
    return;
  }

  beginKickOrRun(requestedDirection);
}

// Duty state güncelleme — ramp, kick, hedef duty yönetimi
void updateDutyState(uint32_t nowMs) {
  // Speed mode: PI kontrol duty'yi doğrudan ayarlar, ramp/kick bypass
  if (pidRt.enabled) return;

  motorRt.targetDuty = clampValue<uint8_t>(motorRt.targetDuty, 0, 255);

  if (motorRt.phase == MotorPhase::Kick) {
    const uint8_t req = (kickDuty > motorRt.targetDuty) ? kickDuty : motorRt.targetDuty;
    motorRt.currentDuty = req;
    return;
  }

  if (!rampEnabled) {
    motorRt.currentDuty = motorRt.targetDuty;
    return;
  }

  if ((uint32_t)(nowMs - motorRt.lastRampUpdateMs) < rampIntervalMs) return;
  motorRt.lastRampUpdateMs = nowMs;

  if (motorRt.currentDuty < motorRt.targetDuty) {
    uint16_t next = motorRt.currentDuty + rampStep;
    motorRt.currentDuty = (next > motorRt.targetDuty) ? motorRt.targetDuty : (uint8_t)next;
  } else if (motorRt.currentDuty > motorRt.targetDuty) {
    int16_t next = (int16_t)motorRt.currentDuty - (int16_t)rampStep;
    motorRt.currentDuty = (next < motorRt.targetDuty) ? motorRt.targetDuty : (uint8_t)next;
  }
}

// ============================================================
// Deferred Request Apply — Bekleyen Komutları Uygula
// Duty güncelleme, durdurma, çalıştırma sıralı işlenir
// ============================================================
void applyPendingRequests() {
  if (pendingReq.hasTargetDutyUpdate) {
    motorRt.targetDuty = pendingReq.requestedTargetDuty;
    pendingReq.hasTargetDutyUpdate = false;
  }

  if (pendingReq.hasStopRequest) {
    stopMotorImmediate();
    pendingReq.hasStopRequest = false;
    pendingReq.hasRunRequest = false;
    return;
  }

  if (pendingReq.hasRunRequest) {
    beginRunRequest(pendingReq.runDirection);
    pendingReq.hasRunRequest = false;
  }
}

// ============================================================
// Service Tasks — Servis Görevleri (Scan, Test, Identify)
// ============================================================

// Servis görevini bitir — temizle, motoru durdur
void finishServiceTask() {
  serviceRt.task = ServiceTask::None;
  serviceRt.startMs = 0;
  serviceRt.nextActionMs = 0;
  allOff();
  clearRuntimeDiagnostics();
}

// Servis görevini iptal et
void cancelServiceTask() {
  finishServiceTask();
}

// Servis görevi başlat — motor durmalı, aksi halde reddet
void requestServiceTask(ServiceTask task, CommandSource src) {
  if (isMotorBusy()) {
    replyLn(src, F("[ERR] Stop motor first"));
    return;
  }

  serviceRt.task = task;
  serviceRt.startMs = millis();
  serviceRt.nextActionMs = millis();
  serviceReplySrc = src;

  if (task == ServiceTask::Scan) {
    serviceRt.scanLastHall = 255;
    replyLn(src, F("[INFO] Scan start"));
  } else if (task == ServiceTask::Test) {
    serviceRt.testStep = 0;
    serviceRt.testStepActive = false;
    serviceRt.testStepReported = false;
    replyLn(src, F("[INFO] Test start"));
  } else if (task == ServiceTask::Identify) {
    serviceRt.identifyStep = 0;
    serviceRt.identifyPhase = IdentifyPhase::Toggle;
    serviceRt.identifyToggleCounter = 0;
    serviceRt.identifyToggleFlip = false;
    for (uint8_t i = 0; i < 8; i++) serviceRt.identifyCandidateMap[i] = 255;
    replyLn(src, F("[INFO] Identify start"));
  }
}

// Hall tarama — 10 saniye boyunca hall sinyallerini canlı izleyip raporla
void updateServiceScan() {
  uint32_t now = millis();
  if ((uint32_t)(now - serviceRt.startMs) >= SCAN_DURATION_MS) {
    replyLn(serviceReplySrc, F("[INFO] Scan done"));
    finishServiceTask();
    return;
  }

  if ((int32_t)(now - serviceRt.nextActionMs) < 0) return;
  serviceRt.nextActionMs = now + SCAN_POLL_MS;

  uint8_t h = forceReadRawHall();
  if (h != serviceRt.scanLastHall) {
    serviceRt.scanLastHall = h;
    replyFC(serviceReplySrc, F("Hall="));
    reply(serviceReplySrc, h);
    replyFC(serviceReplySrc, F(" bin="));
    reply(serviceReplySrc, (h >> 2) & 1);
    reply(serviceReplySrc, (h >> 1) & 1);
    replyLn(serviceReplySrc, h & 1);
  }
}

// Faz testi — her 6 fazı sırayla uygula, hall oku, raporla
void updateServiceTest() {
  uint32_t now = millis();

  if (serviceRt.testStep >= 6) {
    replyLn(serviceReplySrc, F("[INFO] Test done"));
    finishServiceTask();
    return;
  }

  if (!serviceRt.testStepActive) {
    serviceRt.testStepActive = true;
    serviceRt.testStepReported = false;
    serviceRt.testStepStartMs = now;
    applyDriveState(serviceRt.testStep, TEST_DUTY);
    return;
  }

  uint32_t elapsed = now - serviceRt.testStepStartMs;

  if (!serviceRt.testStepReported && elapsed >= TEST_REPORT_MS) {
    uint8_t hall = forceReadRawHall();
    replyFC(serviceReplySrc, F("[TEST] step="));
    reply(serviceReplySrc, serviceRt.testStep);
    replyFC(serviceReplySrc, F(" hall="));
    reply(serviceReplySrc, hall);
    replyFC(serviceReplySrc, F(" mapped="));
    replyLn(serviceReplySrc, hallToState(hall));
    serviceRt.testStepReported = true;
  }

  if (elapsed >= TEST_STEP_MS) {
    allOff();
    serviceRt.testStep++;
    serviceRt.testStepActive = false;
  }
}

// Hall eşleme tanımlama — her fazı toggle edip hall okuyarak eşlem çıkar
void updateServiceIdentify() {
  uint32_t now = millis();

  if (serviceRt.identifyStep >= 6) {
    allOff();

    if (validateHallMap(serviceRt.identifyCandidateMap)) {
      for (uint8_t i = 0; i < 8; i++) hallToMotor[i] = serviceRt.identifyCandidateMap[i];
      replyLn(serviceReplySrc, F("[OK] Identify updated RAM map"));
    } else {
      replyFC(serviceReplySrc, F("[INFO] Candidate map: "));
      for (uint8_t i = 0; i < 8; i++) {
        reply(serviceReplySrc, serviceRt.identifyCandidateMap[i]);
        replyStr(serviceReplySrc, " ");
      }
      replyLn(serviceReplySrc);
      replyLn(serviceReplySrc, F("[WARN] Identify produced invalid map"));
    }

    finishServiceTask();
    return;
  }

  switch (serviceRt.identifyPhase) {
    case IdentifyPhase::Toggle: {
      if ((int32_t)(now - serviceRt.nextActionMs) < 0) return;
      serviceRt.nextActionMs = now + IDENTIFY_TOGGLE_MS;

      uint8_t a = serviceRt.identifyStep;
      uint8_t b = (uint8_t)((a + 1) % 6);

      if (!serviceRt.identifyToggleFlip) applyDriveState(a, IDENTIFY_DUTY);
      else applyDriveState(b, IDENTIFY_DUTY);

      serviceRt.identifyToggleFlip = !serviceRt.identifyToggleFlip;
      serviceRt.identifyToggleCounter++;

      if (serviceRt.identifyToggleCounter >= IDENTIFY_STEP_TOGGLES) {
        allOff();
        serviceRt.identifyPhase = IdentifyPhase::SettleA;
        serviceRt.nextActionMs = now + IDENTIFY_SETTLE_A_MS;
      }
      break;
    }

    case IdentifyPhase::SettleA:
      if ((int32_t)(now - serviceRt.nextActionMs) < 0) return;
      serviceRt.identifyHallA = forceReadRawHall();
      serviceRt.identifyPhase = IdentifyPhase::SettleB;
      serviceRt.nextActionMs = now + IDENTIFY_SETTLE_B_MS;
      break;

    case IdentifyPhase::SettleB: {
      if ((int32_t)(now - serviceRt.nextActionMs) < 0) return;
      uint8_t hallB = forceReadRawHall();
      uint8_t hall = hallB;

      uint8_t mappedState = (uint8_t)((serviceRt.identifyStep + 2) % 6);
      if (hall < 8) serviceRt.identifyCandidateMap[hall] = mappedState;

      // her stepte spam log yok; sadece kısa bilgi
      replyFC(serviceReplySrc, F("[ID] step="));
      reply(serviceReplySrc, serviceRt.identifyStep);
      replyFC(serviceReplySrc, F(" hall="));
      reply(serviceReplySrc, hall);
      replyFC(serviceReplySrc, F(" -> state="));
      replyLn(serviceReplySrc, mappedState);

      serviceRt.identifyStep++;
      serviceRt.identifyPhase = IdentifyPhase::Toggle;
      serviceRt.identifyToggleCounter = 0;
      serviceRt.identifyToggleFlip = false;
      serviceRt.nextActionMs = now + 1;
      break;
    }

    case IdentifyPhase::Idle:
    default:
      break;
  }
}

// Servis görevi ana güncelleme — aktif göreve göre yönlendir
// [safety] IWDG her servis tick'ta yenilenir (identify ~66sn sürebilir)
void updateServiceTask() {
  if (!isServiceActive()) return;
  IWatchdog.reload();

  switch (serviceRt.task) {
    case ServiceTask::Scan: updateServiceScan(); break;
    case ServiceTask::Test: updateServiceTest(); break;
    case ServiceTask::Identify: updateServiceIdentify(); break;
    default: break;
  }
}

// ============================================================
// Command Parsing — Komut İşleme (CLI Parser)
// Tüm CLI komutlarını tanır, pendingReq'e yazar
// ============================================================
void processCommand(char* cmd, CommandSource src) {
  trimInPlace(cmd);
  toLowerInPlace(cmd);

  if (cmd[0] == '\0') return;

  if (strcmp(cmd, "f") == 0 || strcmp(cmd, "forward") == 0) {
    if (pidRt.enabled) disablePidMode();
    pendingReq.hasRunRequest = true;
    pendingReq.runDirection = 1;
    lastMotorCommandMs = millis();
    replyLn(src, F("[OK] Run request FWD"));
    return;
  }

  if (strcmp(cmd, "b") == 0 || strcmp(cmd, "backward") == 0) {
    if (pidRt.enabled) disablePidMode();
    pendingReq.hasRunRequest = true;
    pendingReq.runDirection = -1;
    lastMotorCommandMs = millis();
    replyLn(src, F("[OK] Run request REV"));
    return;
  }

  if (strcmp(cmd, "s") == 0 || strcmp(cmd, "stop") == 0) {
    if (isServiceActive()) cancelServiceTask();
    if (pidRt.enabled) disablePidMode();
    pendingReq.hasStopRequest = true;
    lastMotorCommandMs = 0;
    replyLn(src, F("[OK] Stop request"));
    return;
  }

  // x = brake (aktif fren) - low-side ON, high-side OFF
  if (strcmp(cmd, "x") == 0 || strcmp(cmd, "brake") == 0) {
    if (isServiceActive()) cancelServiceTask();
    if (pidRt.enabled) disablePidMode();
    enterBrake();
    lastMotorCommandMs = 0;
    replyLn(src, F("[OK] Brake active"));
    return;
  }

  // f<duty> — ileri + duty birleşik komut (araç modu)
  if (cmd[0] == 'f' && cmd[1] >= '0' && cmd[1] <= '9') {
    if (pidRt.enabled) disablePidMode();
    long duty = strtol(cmd + 1, nullptr, 10);
    duty = clampValue<long>(duty, 0, 255);
    pendingReq.hasTargetDutyUpdate = true;
    pendingReq.requestedTargetDuty = (uint8_t)duty;
    pendingReq.hasRunRequest = true;
    pendingReq.runDirection = 1;
    lastMotorCommandMs = millis();
    replyFC(src, F("[OK] FWD D="));
    replyLn(src, (uint8_t)duty);
    return;
  }

  // b<duty> — geri + duty birleşik komut (araç modu)
  if (cmd[0] == 'b' && cmd[1] >= '0' && cmd[1] <= '9') {
    if (pidRt.enabled) disablePidMode();
    long duty = strtol(cmd + 1, nullptr, 10);
    duty = clampValue<long>(duty, 0, 255);
    pendingReq.hasTargetDutyUpdate = true;
    pendingReq.requestedTargetDuty = (uint8_t)duty;
    pendingReq.hasRunRequest = true;
    pendingReq.runDirection = -1;
    lastMotorCommandMs = millis();
    replyFC(src, F("[OK] REV D="));
    replyLn(src, (uint8_t)duty);
    return;
  }

  if (strcmp(cmd, "pwm") == 0) {
    replyFC(src, F("[INFO] TargetDuty="));
    reply(src, motorRt.targetDuty);
    replyFC(src, F(" CurrentDuty="));
    replyLn(src, motorRt.currentDuty);
    return;
  }

  long v = 0;
  if (parseLongAfterPrefix(cmd, "pwm ", &v)) {
    if (pidRt.enabled) disablePidMode();
    v = clampValue<long>(v, 0, 255);
    pendingReq.hasTargetDutyUpdate = true;
    pendingReq.requestedTargetDuty = (uint8_t)v;
    lastMotorCommandMs = millis();
    replyFC(src, F("[OK] Requested TargetDuty="));
    replyLn(src, (uint8_t)v);
    return;
  }

  if (strcmp(cmd, "kick on") == 0) {
    kickEnabled = true;
    replyLn(src, F("[OK] Kick ON"));
    return;
  }
  if (strcmp(cmd, "kick off") == 0) {
    kickEnabled = false;
    replyLn(src, F("[OK] Kick OFF"));
    return;
  }
  if (strcmp(cmd, "ramp on") == 0) {
    rampEnabled = true;
    replyLn(src, F("[OK] Ramp ON"));
    return;
  }
  if (strcmp(cmd, "ramp off") == 0) {
    rampEnabled = false;
    replyLn(src, F("[OK] Ramp OFF"));
    return;
  }

  if (parseLongAfterPrefix(cmd, "kickduty ", &v)) {
    kickDuty = (uint8_t)clampValue<long>(v, 0, 255);
    replyLn(src, F("[OK] KickDuty updated"));
    return;
  }

  if (parseLongAfterPrefix(cmd, "kickms ", &v)) {
    kickMs = (uint16_t)clampValue<long>(v, 0, 3000);
    replyLn(src, F("[OK] KickMs updated"));
    return;
  }

  if (parseLongAfterPrefix(cmd, "ramprate ", &v)) {
    rampStep = (uint8_t)clampValue<long>(v, 1, 64);
    replyLn(src, F("[OK] RampStep updated"));
    return;
  }

  if (parseLongAfterPrefix(cmd, "rampms ", &v)) {
    rampIntervalMs = (uint16_t)clampValue<long>(v, 1, 1000);
    replyLn(src, F("[OK] RampMs updated"));
    return;
  }

  // [bugfix] Default PWM ayarlama komutu
  if (parseLongAfterPrefix(cmd, "defpwm ", &v)) {
    controlPwmValue = (uint8_t)clampValue<long>(v, 0, 255);
    replyFC(src, F("[OK] DefaultPWM="));
    replyLn(src, controlPwmValue);
    return;
  }

  if (strcmp(cmd, "savecfg") == 0) {
    clampConfig();
    bool ok = saveConfigToStorage();
    replyLn(src, ok ? F("[OK] Config saved") : F("[ERR] Config save failed"));
    return;
  }

  if (strcmp(cmd, "loadcfg") == 0) {
    if (loadConfigFromStorage()) replyLn(src, F("[OK] Config loaded"));
    else {
      loadDefaultConfig();
      replyLn(src, F("[INFO] Defaults loaded"));
    }
    return;
  }

  if (strcmp(cmd, "defaults") == 0) {
    loadDefaultConfig();
    replyLn(src, F("[OK] Defaults loaded into RAM"));
    return;
  }

  // [bugfix] saveall: hall map + config + mode persistence
  if (strcmp(cmd, "saveall") == 0) {
    bool ok1 = false;
    bool ok2 = false;
    bool ok3 = false;
    if (validateCurrentHallMap()) ok1 = saveHallMapToStorage();
    clampConfig();
    ok2 = saveConfigToStorage();
    ok3 = saveModeToStorage();

    replyFC(src, F("[INFO] saveall map="));
    reply(src, ok1 ? F("OK") : F("FAIL"));
    replyFC(src, F(" cfg="));
    reply(src, ok2 ? F("OK") : F("FAIL"));
    replyFC(src, F(" mode="));
    replyLn(src, ok3 ? F("OK") : F("FAIL"));
    return;
  }

  if (strcmp(cmd, "hall") == 0 || strcmp(cmd, "h") == 0) {
    uint8_t h = forceReadRawHall();
    replyFC(src, F("[INFO] Hall="));
    reply(src, h);
    replyFC(src, F(" State="));
    replyLn(src, hallToState(h));
    return;
  }

  if (strcmp(cmd, "map") == 0) {
    printHallMap(src);
    return;
  }

  if (strcmp(cmd, "save") == 0) {
    if (!validateCurrentHallMap()) {
      replyLn(src, F("[ERR] Current hall map invalid"));
      return;
    }
    bool ok = saveHallMapToStorage();
    replyLn(src, ok ? F("[OK] Hall map saved") : F("[ERR] Save failed"));
    return;
  }

  if (strcmp(cmd, "reload") == 0) {
    reloadHallMap();
    printHallMap(src);
    return;
  }

  if (strcmp(cmd, "mapreset") == 0) {
    loadDefaultHallMap();
    replyLn(src, F("[OK] Default hall map loaded into RAM"));
    printHallMap(src);
    return;
  }

  if (strcmp(cmd, "scan") == 0) {
    requestServiceTask(ServiceTask::Scan, src);
    return;
  }

  if (strcmp(cmd, "test") == 0) {
    requestServiceTask(ServiceTask::Test, src);
    return;
  }

  if (strcmp(cmd, "identify") == 0) {
    requestServiceTask(ServiceTask::Identify, src);
    return;
  }

  if (strcmp(cmd, "status") == 0) {
    printStatus(src);
    queueOverflowFlag = false;
    return;
  }

  if (strcmp(cmd, "clrerr") == 0) {
    queueOverflowFlag = false;
    if (motorRt.phase == MotorPhase::Fault) {
      motorRt.phase = MotorPhase::Stopped;
      motorRt.faultCode = MotorFaultCode::None;
      replyLn(src, F("[OK] Fault cleared, motor stopped"));
    } else {
      replyLn(src, F("[OK] Error flags cleared"));
    }
    return;
  }

  if (strcmp(cmd, "debug on") == 0) {
    verboseDebug = true;
    replyLn(src, F("[OK] Debug ON"));
    return;
  }

  if (strcmp(cmd, "debug off") == 0) {
    verboseDebug = false;
    replyLn(src, F("[OK] Debug OFF"));
    return;
  }

  // ── Speed PI komutları ──
  if (strcmp(cmd, "pid on") == 0 || strcmp(cmd, "mode speed") == 0) {
    pidRt.enabled = true;
    pidRt.controlMode = SpeedControlMode::Speed;
    resetPidState();
    replyLn(src, F("[OK] Speed PI ON"));
    return;
  }

  if (strcmp(cmd, "pid off") == 0 || strcmp(cmd, "mode duty") == 0) {
    disablePidMode();
    replyLn(src, F("[OK] Speed PI OFF (Duty mode)"));
    return;
  }

  if (strcmp(cmd, "rpm") == 0) {
    replyFC(src, F("[INFO] TargetRPM_Cmd="));
    reply(src, pidRt.targetRpmCmd);
    replyFC(src, F(" Ramped="));
    reply(src, (int32_t)pidRt.targetRpmRamped);
    replyFC(src, F(" Measured="));
    replyLn(src, calculateRPM());
    return;
  }

  if (parseLongAfterPrefix(cmd, "rpm ", &v)) {
    if (!pidRt.enabled) {
      replyLn(src, F("[ERR] Not in speed mode (use: mode speed)"));
      return;
    }
    pidRt.targetRpmCmd = (int32_t)v;
    pidRt.lastHallEventMs = millis();
    lastMotorCommandMs = millis();
    // Yön değişimi kontrolü
    int8_t newDir = (v > 0) ? 1 : ((v < 0) ? -1 : 0);
    if (v == 0) {
      stopMotorImmediate();
      resetPidState();
      replyLn(src, F("[OK] RPM=0 stop"));
      return;
    }
    if (newDir != 0 && pidRt.pidDirection != 0 && newDir != pidRt.pidDirection) {
      beginNeutralDirectionSwitch(newDir);
      pidRt.pidDirection = newDir;
      resetPidState();
    } else if (pidRt.pidDirection == 0) {
      pidRt.pidDirection = newDir;
    }
    replyFC(src, F("[OK] RPM="));
    replyLn(src, (int32_t)v);
    return;
  }

  float fv = 0.0f;
  if (parseFloatAfterPrefix(cmd, "kp ", &fv)) {
    pidRt.kp = clampValue<float>(fv, 0.0f, 10.0f);
    replyFC(src, F("[OK] Kp="));
    replyLn(src, pidRt.kp);
    return;
  }

  if (parseFloatAfterPrefix(cmd, "ki ", &fv)) {
    pidRt.ki = clampValue<float>(fv, 0.0f, 10.0f);
    replyFC(src, F("[OK] Ki="));
    replyLn(src, pidRt.ki);
    return;
  }

  // kd artık kullanılmıyor — geriye uyumluluk için kabul et ama etkisi yok
  if (parseFloatAfterPrefix(cmd, "kd ", &fv)) {
    replyLn(src, F("[INFO] Kd not used (PI only)"));
    return;
  }

  // pi <kp> <ki> — her iki kazancı aynı anda ayarla
  if (startsWith(cmd, "pi ")) {
    const char* p = cmd + 3;
    while (*p == ' ') p++;
    char* end1 = nullptr;
    float newKp = strtof(p, &end1);
    if (end1 != p) {
      while (*end1 == ' ') end1++;
      char* end2 = nullptr;
      float newKi = strtof(end1, &end2);
      if (end2 != end1) {
        pidRt.kp = clampValue<float>(newKp, 0.0f, 10.0f);
        pidRt.ki = clampValue<float>(newKi, 0.0f, 10.0f);
        replyFC(src, F("[OK] Kp="));
        reply(src, pidRt.kp);
        replyFC(src, F(" Ki="));
        replyLn(src, pidRt.ki);
        return;
      }
    }
    replyLn(src, F("[ERR] Usage: pi <kp> <ki>"));
    return;
  }

  // base <low> <mid> <high> — base PWM değerleri
  if (startsWith(cmd, "base ")) {
    const char* p = cmd + 5;
    while (*p == ' ') p++;
    long lo = strtol(p, nullptr, 10);
    const char* p2 = p;
    while (*p2 && *p2 != ' ') p2++;
    while (*p2 == ' ') p2++;
    long mid = strtol(p2, nullptr, 10);
    const char* p3 = p2;
    while (*p3 && *p3 != ' ') p3++;
    while (*p3 == ' ') p3++;
    long hi = strtol(p3, nullptr, 10);
    pidRt.basePwmLow = (uint8_t)clampValue<long>(lo, 0, 255);
    pidRt.basePwmMid = (uint8_t)clampValue<long>(mid, 0, 255);
    pidRt.basePwmHigh = (uint8_t)clampValue<long>(hi, 0, 255);
    replyFC(src, F("[OK] BasePWM Low="));
    reply(src, pidRt.basePwmLow);
    replyFC(src, F(" Mid="));
    reply(src, pidRt.basePwmMid);
    replyFC(src, F(" High="));
    replyLn(src, pidRt.basePwmHigh);
    return;
  }

  // boost <lo> <mid> <hi> <ms> — start boost değerleri
  if (startsWith(cmd, "boost ")) {
    const char* p = cmd + 6;
    while (*p == ' ') p++;
    long lo = strtol(p, nullptr, 10);
    while (*p && *p != ' ') p++; while (*p == ' ') p++;
    long mid = strtol(p, nullptr, 10);
    while (*p && *p != ' ') p++; while (*p == ' ') p++;
    long hi = strtol(p, nullptr, 10);
    while (*p && *p != ' ') p++; while (*p == ' ') p++;
    long ms = strtol(p, nullptr, 10);
    pidRt.boostLowPwm = (uint8_t)clampValue<long>(lo, 0, 255);
    pidRt.boostMidPwm = (uint8_t)clampValue<long>(mid, 0, 255);
    pidRt.boostHighPwm = (uint8_t)clampValue<long>(hi, 0, 255);
    pidRt.boostTimeMs = (uint16_t)clampValue<long>(ms, 10, 2000);
    replyFC(src, F("[OK] Boost Low="));
    reply(src, pidRt.boostLowPwm);
    replyFC(src, F(" Mid="));
    reply(src, pidRt.boostMidPwm);
    replyFC(src, F(" High="));
    reply(src, pidRt.boostHighPwm);
    replyFC(src, F(" Ms="));
    replyLn(src, pidRt.boostTimeMs);
    return;
  }

  // ramp <up> <down> — RPM ramp hızları
  if (startsWith(cmd, "ramp ") && !startsWith(cmd, "ramp on") && !startsWith(cmd, "ramp off") &&
      !startsWith(cmd, "ramprate") && !startsWith(cmd, "rampms")) {
    const char* p = cmd + 5;
    while (*p == ' ') p++;
    char* end1 = nullptr;
    float up = strtof(p, &end1);
    if (end1 != p) {
      while (*end1 == ' ') end1++;
      char* end2 = nullptr;
      float down = strtof(end1, &end2);
      if (end2 != end1) {
        pidRt.rampUpPerSec = clampValue<float>(up, 1.0f, 10000.0f);
        pidRt.rampDownPerSec = clampValue<float>(down, 1.0f, 10000.0f);
        replyFC(src, F("[OK] Ramp Up="));
        reply(src, (int32_t)pidRt.rampUpPerSec);
        replyFC(src, F(" Down="));
        replyLn(src, (int32_t)pidRt.rampDownPerSec);
        return;
      }
    }
    replyLn(src, F("[ERR] Usage: ramp <up_rpm_s> <down_rpm_s>"));
    return;
  }

  if (strcmp(cmd, "pidstatus") == 0 || strcmp(cmd, "spstat") == 0) {
    replyFC(src, F("--- SPEED PI STATUS ---"));
    replyLn(src);
    replyFC(src, F("ControlMode: "));
    replyLn(src, (uint8_t)pidRt.controlMode == 0 ? F("DUTY") : F("SPEED"));
    replyFC(src, F("SpeedPhase: "));
    switch (pidRt.speedPhase) {
      case SpeedPhase::Idle: replyLn(src, F("IDLE")); break;
      case SpeedPhase::StartBoost: replyLn(src, F("START_BOOST")); break;
      case SpeedPhase::SpeedPI: replyLn(src, F("SPEED_PI")); break;
    }
    replyFC(src, F("MotorPhase: "));
    switch (motorRt.phase) {
      case MotorPhase::Stopped: replyLn(src, F("STOPPED")); break;
      case MotorPhase::Kick: replyLn(src, F("KICK")); break;
      case MotorPhase::Running: replyLn(src, F("RUNNING")); break;
      case MotorPhase::NeutralWait: replyLn(src, F("NEUTRAL_WAIT")); break;
      case MotorPhase::Fault: replyLn(src, F("FAULT")); break;
      case MotorPhase::Brake: replyLn(src, F("BRAKE")); break;
    }
    replyFC(src, F("TargetRPM_Cmd: ")); replyLn(src, pidRt.targetRpmCmd);
    replyFC(src, F("TargetRPM_Ramped: ")); replyLn(src, (int32_t)pidRt.targetRpmRamped);
    replyFC(src, F("MeasuredRPM_Raw: ")); replyLn(src, (int32_t)pidRt.rawRpm);
    replyFC(src, F("MeasuredRPM_Filtered: ")); replyLn(src, (int32_t)pidRt.filteredRpm);
    float error = pidRt.targetRpmRamped - pidRt.filteredRpm;
    replyFC(src, F("Error: ")); replyLn(src, (int32_t)error);
    replyFC(src, F("Kp: ")); replyLn(src, pidRt.kp);
    replyFC(src, F("Ki: ")); replyLn(src, pidRt.ki);
    replyFC(src, F("Integral: ")); replyLn(src, (int32_t)pidRt.errorIntegral);
    replyFC(src, F("BasePWM: ")); replyLn(src, (int32_t)pidRt.basePwm);
    replyFC(src, F("PI_Output: ")); replyLn(src, (int32_t)pidRt.piOutput);
    replyFC(src, F("FinalDuty: ")); replyLn(src, motorRt.targetDuty);
    replyFC(src, F("AppliedDuty: ")); replyLn(src, motorRt.currentDuty);
    replyFC(src, F("HallState: ")); replyLn(src, hallRt.stableRaw);
    replyFC(src, F("HallPeriod_us: ")); replyLn(src, hallRt.hallPeriodUs);
    replyFC(src, F("FaultCode: ")); replyLn(src, (uint8_t)motorRt.faultCode);
    replyFC(src, F("BoostEdges: "));
    replyLn(src, pidRt.hallEdgeCounter - pidRt.boostStartEdgeCount);
    replyFC(src, F("BasePWM_L/M/H: "));
    reply(src, pidRt.basePwmLow); replyFC(src, F("/"));
    reply(src, pidRt.basePwmMid); replyFC(src, F("/"));
    replyLn(src, pidRt.basePwmHigh);
    replyFC(src, F("Boost_L/M/H/T: "));
    reply(src, pidRt.boostLowPwm); replyFC(src, F("/"));
    reply(src, pidRt.boostMidPwm); replyFC(src, F("/"));
    reply(src, pidRt.boostHighPwm); replyFC(src, F("/"));
    replyLn(src, pidRt.boostTimeMs);
    replyFC(src, F("Ramp_Up/Down: "));
    reply(src, (int32_t)pidRt.rampUpPerSec); replyFC(src, F("/"));
    replyLn(src, (int32_t)pidRt.rampDownPerSec);
    return;
  }

  // Telemetri debug modu
  if (strcmp(cmd, "dbg on") == 0) {
    telemetryDebug = true;
    replyLn(src, F("[OK] Telemetry DBG ON"));
    return;
  }

  if (strcmp(cmd, "dbg off") == 0) {
    telemetryDebug = false;
    replyLn(src, F("[OK] Telemetry DBG OFF"));
    return;
  }

  // Telemetri aralığı ayarlama
  if (parseLongAfterPrefix(cmd, "telper ", &v)) {
    telemetryIntervalMs = (uint32_t)clampValue<long>(v, 50, 5000);
    replyFC(src, F("[OK] TelemetryPeriod="));
    reply(src, telemetryIntervalMs);
    replyLn(src, F("ms"));
    return;
  }

  // ── Mode komutları ──
  if (strcmp(cmd, "mode") == 0) {
    replyFC(src, F("[INFO] Mode: "));
    if (activeMode == OperatingMode::Control) replyLn(src, F("CONTROL"));
    else if (activeMode == OperatingMode::Settings) replyLn(src, F("SETTINGS"));
    else replyLn(src, F("NORMAL"));
    return;
  }

  if (strcmp(cmd, "mode control") == 0 || strcmp(cmd, "control") == 0) {
    // Control moduna geçerken service task iptal et
    if (isServiceActive()) {
      cancelServiceTask();
    }
    // Control moduna geçerken pending istekleri temizle
    memset(&pendingReq, 0, sizeof(pendingReq));
    activeMode = OperatingMode::Control;
    controlDirection = 0;
    bool saved = saveModeToStorage();
    replyLn(src, saved ? F("[OK] Mode=CONTROL (saved)") : F("[OK] Mode=CONTROL (save fail)"));
    CMD.println(F("[MODE] CONTROL"));
    return;
  }

  if (strcmp(cmd, "mode normal") == 0) {
    if (isMotorBusy()) {
      stopMotorImmediate();
    }
    if (isServiceActive()) {
      cancelServiceTask();
    }
    activeMode = OperatingMode::Normal;
    bool saved = saveModeToStorage();
    replyLn(src, saved ? F("[OK] Mode=NORMAL (saved)") : F("[OK] Mode=NORMAL (save fail)"));
    CMD.println(F("[MODE] NORMAL"));
    return;
  }

  if (strcmp(cmd, "settings") == 0 || strcmp(cmd, "mode settings") == 0) {
    if (isMotorBusy()) stopMotorImmediate();
    if (isServiceActive()) cancelServiceTask();
    activeMode = OperatingMode::Settings;
    bool saved = saveModeToStorage();
    replyLn(src, saved ? F("[OK] Mode=SETTINGS (saved)") : F("[OK] Mode=SETTINGS (save fail)"));
    CMD.println(F("[MODE] SETTINGS"));
    return;
  }

  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
    printHelp(src);
    return;
  }

  replyLn(src, F("[ERR] Unknown command"));
}

// [bugfix] Kuyruk darboğazı: if→while, max 8 iterasyon (loop starvation önleme)
void processQueuedCommands() {
  CommandItem item;
  uint8_t budget = CMD_QUEUE_LEN;
  while (budget-- && dequeueCommand(&item)) {
    processCommand(item.text, item.src);
  }
}

// ============================================================
// Control Mode f/b/s Handler — Control Modu Komut İşleme
// f=ileri, b=geri, s=dur, f<duty>/b<duty>=direkt duty
// ============================================================
void processControlCommand(char* cmd, CommandSource src) {
  trimInPlace(cmd);
  toLowerInPlace(cmd);

  if (cmd[0] == '\0') return;

  // "mode normal" her zaman çalışsın — Control modundan çıkış
  if (strcmp(cmd, "mode normal") == 0) {
    if (isMotorBusy()) stopMotorImmediate();
    if (isServiceActive()) cancelServiceTask();
    activeMode = OperatingMode::Normal;
    saveModeToStorage();
    replyLn(src, F("[OK] Mode=NORMAL"));
    CMD.println(F("[MODE] NORMAL"));
    return;
  }

  if (strcmp(cmd, "mode") == 0) {
    replyFC(src, F("[INFO] Mode: CONTROL"));
    replyFC(src, F(" SpeedCtrl="));
    replyLn(src, pidRt.enabled ? F("SPEED") : F("DUTY"));
    return;
  }

  if (strcmp(cmd, "mode duty") == 0) {
    disablePidMode();
    replyLn(src, F("[OK] Speed PI OFF (Duty mode)"));
    return;
  }

  if (strcmp(cmd, "mode speed") == 0) {
    pidRt.enabled = true;
    pidRt.controlMode = SpeedControlMode::Speed;
    resetPidState();
    replyLn(src, F("[OK] Speed PI ON"));
    return;
  }

  if (strcmp(cmd, "spstat") == 0) {
    // processCommand'a yönlendir — spstat handler orada
    processCommand(cmd, src);
    return;
  }

  if (strcmp(cmd, "status") == 0) {
    printStatus(src);
    queueOverflowFlag = false;
    return;
  }

  // Identify komutu — Control modunda da çalışır
  if (strcmp(cmd, "identify") == 0) {
    requestServiceTask(ServiceTask::Identify, src);
    return;
  }

  // Hall sensörü değeri sorgulama
  if (strcmp(cmd, "h") == 0 || strcmp(cmd, "hall") == 0) {
    uint8_t h = forceReadRawHall();
    CMD.print(F("HALL:"));
    CMD.print(h);
    CMD.print(F(",STATE:"));
    CMD.println(hallToState(h));
    return;
  }

  // Settings moduna geç
  if (strcmp(cmd, "settings") == 0 || strcmp(cmd, "mode settings") == 0) {
    if (isMotorBusy()) stopMotorImmediate();
    if (isServiceActive()) cancelServiceTask();
    activeMode = OperatingMode::Settings;
    saveModeToStorage();
    replyLn(src, F("[OK] Mode=SETTINGS"));
    CMD.println(F("[MODE] SETTINGS"));
    return;
  }

  // rpm komutu — speed mode RPM ayarı (Control modunda da çalışır)
  if (startsWith(cmd, "rpm ")) {
    processCommand(cmd, src);
    return;
  }

  // spstat — speed PI durumu
  if (strcmp(cmd, "spstat") == 0 || strcmp(cmd, "pidstatus") == 0) {
    processCommand(cmd, src);
    return;
  }

  // f = ileri varsayılan PWM
  if (strcmp(cmd, "f") == 0) {
    if (pidRt.enabled) disablePidMode();
    lastMotorCommandMs = millis();
    if (controlDirection == 1 && isMotorDriveActive()) {
      if (motorRt.targetDuty != controlPwmValue) {
        pendingReq.hasTargetDutyUpdate = true;
        pendingReq.requestedTargetDuty = controlPwmValue;
      }
      CMD.print(F("OK:FWD,PWM:"));
      CMD.println(controlPwmValue);
      return;
    }
    controlDirection = 1;
    pendingReq.hasTargetDutyUpdate = true;
    pendingReq.requestedTargetDuty = controlPwmValue;
    pendingReq.hasRunRequest = true;
    pendingReq.runDirection = 1;
    CMD.print(F("OK:FWD,PWM:"));
    CMD.println(controlPwmValue);
    return;
  }

  // f<duty> = ileri + duty birleşik
  if (cmd[0] == 'f' && cmd[1] >= '0' && cmd[1] <= '9') {
    if (pidRt.enabled) disablePidMode();
    long duty = strtol(cmd + 1, nullptr, 10);
    duty = clampValue<long>(duty, 0, 255);
    lastMotorCommandMs = millis();
    controlDirection = 1;
    pendingReq.hasTargetDutyUpdate = true;
    pendingReq.requestedTargetDuty = (uint8_t)duty;
    pendingReq.hasRunRequest = true;
    pendingReq.runDirection = 1;
    CMD.print(F("OK:FWD,PWM:"));
    CMD.println(duty);
    return;
  }

  // b = geri varsayılan PWM
  if (strcmp(cmd, "b") == 0) {
    if (pidRt.enabled) disablePidMode();
    lastMotorCommandMs = millis();
    if (controlDirection == -1 && isMotorDriveActive()) {
      if (motorRt.targetDuty != controlPwmValue) {
        pendingReq.hasTargetDutyUpdate = true;
        pendingReq.requestedTargetDuty = controlPwmValue;
      }
      CMD.print(F("OK:REV,PWM:"));
      CMD.println(controlPwmValue);
      return;
    }
    controlDirection = -1;
    pendingReq.hasTargetDutyUpdate = true;
    pendingReq.requestedTargetDuty = controlPwmValue;
    pendingReq.hasRunRequest = true;
    pendingReq.runDirection = -1;
    CMD.print(F("OK:REV,PWM:"));
    CMD.println(controlPwmValue);
    return;
  }

  // b<duty> = geri + duty birleşik
  if (cmd[0] == 'b' && cmd[1] >= '0' && cmd[1] <= '9') {
    if (pidRt.enabled) disablePidMode();
    long duty = strtol(cmd + 1, nullptr, 10);
    duty = clampValue<long>(duty, 0, 255);
    lastMotorCommandMs = millis();
    controlDirection = -1;
    pendingReq.hasTargetDutyUpdate = true;
    pendingReq.requestedTargetDuty = (uint8_t)duty;
    pendingReq.hasRunRequest = true;
    pendingReq.runDirection = -1;
    CMD.print(F("OK:REV,PWM:"));
    CMD.println(duty);
    return;
  }

  // s = dur (coast stop)
  if (strcmp(cmd, "s") == 0 || strcmp(cmd, "stop") == 0) {
    if (pidRt.enabled) disablePidMode();
    controlDirection = 0;
    pendingReq.hasStopRequest = true;
    lastMotorCommandMs = 0;
    CMD.println(F("OK:STOP"));
    return;
  }

  // x = brake (aktif fren)
  if (strcmp(cmd, "x") == 0 || strcmp(cmd, "brake") == 0) {
    if (pidRt.enabled) disablePidMode();
    controlDirection = 0;
    enterBrake();
    lastMotorCommandMs = 0;
    CMD.println(F("OK:BRAKE"));
    return;
  }

  // pwm <değer> — direkt PWM set
  long pv = 0;
  if (parseLongAfterPrefix(cmd, "pwm ", &pv)) {
    if (pidRt.enabled) disablePidMode();
    controlPwmValue = (uint8_t)clampValue<long>(pv, 0, 255);
    if (controlDirection != 0) {
      pendingReq.hasTargetDutyUpdate = true;
      pendingReq.requestedTargetDuty = controlPwmValue;
      lastMotorCommandMs = millis();
    }
    CMD.print(F("PWM:"));
    CMD.println(controlPwmValue);
    return;
  }

  // Bilinmeyen komut — normal parser'a düşür
  processCommand(cmd, src);
}

// [bugfix] Kuyruk darboğazı: if→while, max 8 iterasyon
void processControlQueuedCommands() {
  CommandItem item;
  uint8_t budget = CMD_QUEUE_LEN;
  while (budget-- && dequeueCommand(&item)) {
    processControlCommand(item.text, item.src);
  }
}

// Control modu davranışları:
// - Command watchdog aktif (800ms lease timeout)
// - Host connection monitor aktif (2sn UART aktivite yoksa stop)
// - Heartbeat/lease mantığı: host periyodik f/b gönderir, timeout olursa motor durur
// - Bu sayede host çökerse/koptuğunda motor otomatik durur (güvenlik)

// ============================================================
// Motor Scheduler / Control — Ana Motor Kontrol Döngüsü
// Her tick: hall oku, pending komutları uygula, faz sür
// ============================================================

// Motor kontrol tick — hall güncelle, komut uygula, faz sür
void motorControlTick(uint32_t nowUs) {
  updateHallRuntime(nowUs);
  applyPendingRequests();

  // queueOverflowFlag sticky — sadece status/flush komutlarında sıfırlanır

  if (motorRt.phase == MotorPhase::Stopped || motorRt.phase == MotorPhase::Fault) {
    if (motorRt.lastAppliedDriveState != 255 || motorRt.lastAppliedDuty != 0) {
      allOff();
    }
    // Control modunda motor durduğunda controlDirection senkronize et
    if (activeMode == OperatingMode::Control && controlDirection != 0) {
      controlDirection = 0;
    }
    digitalWrite(LED_PIN, HIGH);
    return;
  }

  // Brake durumu - aktif fren, low-side ON, high-side OFF
  if (motorRt.phase == MotorPhase::Brake) {
    // Brake timeout — süresiz brake'te kalmasın
    if (brakeHoldMs > 0 && (uint32_t)(millis() - motorRt.phaseStartMs) >= brakeHoldMs) {
      stopMotorImmediate();
      CMD.println(F("[WARN] Brake timeout"));
      return;
    }
    allBrake();
    digitalWrite(LED_PIN, LOW);  // LED yanar (aktif durum)
    return;
  }

  if (motorRt.phase == MotorPhase::NeutralWait) {
    allOff();
    if (motorRt.restartPending && millis() >= motorRt.neutralReleaseMs) {
      beginKickOrRun(motorRt.pendingDirection);
      motorRt.restartPending = false;
    }
    digitalWrite(LED_PIN, HIGH);
    return;
  }

  uint32_t nowMs = millis();

  if ((int32_t)(nowMs - motorRt.startHallDeadlineMs) >= 0) {
    const uint32_t sinceLastValidUs = nowUs - hallRt.lastValidUs;
    if (sinceLastValidUs > (START_NO_HALL_TIMEOUT_MS * 1000UL)) {
      triggerFault(MotorFaultCode::StartupNoHall);
      return;
    }
  }

  if (hallRt.invalidTransitionCount > INVALID_TRANSITION_THRESHOLD) {
    triggerFault(MotorFaultCode::IllegalTransitionSpam);
    return;
  }

  if (motorRt.phase == MotorPhase::Kick) {
    if ((uint32_t)(nowMs - motorRt.phaseStartMs) >= kickMs) {
      motorRt.phase = MotorPhase::Running;
      if (!rampEnabled) motorRt.currentDuty = motorRt.targetDuty;
    }
  }

  updateDutyState(nowMs);

  bool haveState = false;
  uint8_t electricalState = 255;

  // Düşük hız fix'i:
  // Eski mantık son hall GEÇİŞ zamanına bakıp 6-8ms sonra fazları kapatıyordu.
  // Düşük devirde hall geçiş aralığı bundan uzun olduğu için motor, özellikle
  // kalkışta ve yavaş sürüşte torkunu kaybediyordu. Stabil hall değeri geçerliyse
  // aynı electrical state'i sürmeye devam etmek doğru davranıştır.
  const uint8_t stableState = hallToState(hallRt.stableRaw);

  if (isValidMappedState(stableState)) {
    electricalState = stableState;
    haveState = true;
    motorRt.lastDrivenElectricalState = electricalState;
  } else {
    // Sadece 000/111 gibi geçersiz hall glitch'lerinde kısa süre son state'i tut.
    bool canHoldLastState =
        isValidMappedState(motorRt.lastDrivenElectricalState) &&
        hallRt.invalidSinceUs != 0 &&
        ((uint32_t)(nowUs - hallRt.invalidSinceUs) <= INVALID_HALL_HOLD_US);

    if (canHoldLastState) {
      electricalState = motorRt.lastDrivenElectricalState;
      haveState = true;
    }
  }

  if (!haveState) {
    allOff();

    if (hallRt.invalidSinceUs != 0 &&
        (uint32_t)(nowUs - hallRt.invalidSinceUs) > INVALID_HALL_STOP_US) {
      triggerFault(MotorFaultCode::InvalidHallPersist);
      return;
    }

    digitalWrite(LED_PIN, HIGH);
    return;
  }

  uint8_t driveState = electricalState;
  if (motorRt.direction < 0) {
    driveState = (uint8_t)((electricalState + 3) % 6);
  }

  applyDriveState(driveState, motorRt.currentDuty);
  digitalWrite(LED_PIN, LOW);
}

// Motor kontrol zamanlayıcısı — periyodik tick, catch-up ile
void runMotorControlScheduler() {
  if (isServiceActive()) return;

  uint8_t catchup = 0;
  while ((int32_t)(micros() - nextControlTickUs) >= 0 &&
         catchup < MAX_CONTROL_CATCHUP_TICKS) {
    motorControlTick(nextControlTickUs);
    nextControlTickUs += CONTROL_PERIOD_US;
    catchup++;
  }

  if (catchup == MAX_CONTROL_CATCHUP_TICKS) {
    nextControlTickUs = micros() + CONTROL_PERIOD_US;
  }
}

// ============================================================
// Telemetri — Veri Gönderme
// ============================================================

// Birleşik telemetri — Normal ve Control modda aynı format
// Kompakt format: RPM:<measured>,T:<target>,D:<duty>,DIR:<F/R>,PH:<phase>,SP:<0/1>,BRAKE:<0/1>,FC:<code>,H:<hall>
// Debug format:   RPM:<measured>,RF:<filtered>,Tcmd:<target>,Trmp:<ramped>,ERR:<error>,OUT:<pi_out>,I:<integral>,D:<duty>,FC:<code>
void sendTelemetry(uint32_t nowMs) {
  if ((uint32_t)(nowMs - lastTelemetryMs) < telemetryIntervalMs) return;
  lastTelemetryMs = nowMs;

  uint32_t rpm = calculateRPM();

  if (telemetryDebug && pidRt.enabled) {
    // Detaylı Speed PI telemetri (debug modu)
    float targetAbs = (float)abs(pidRt.targetRpmCmd);
    float error = pidRt.targetRpmRamped - pidRt.filteredRpm;
    float piOut = pidRt.kp * error + pidRt.ki * pidRt.errorIntegral;

    CMD.print(F("RPM:"));
    CMD.print(rpm);
    CMD.print(F(",RF:"));
    CMD.print((int32_t)pidRt.filteredRpm);
    CMD.print(F(",Tcmd:"));
    CMD.print(pidRt.targetRpmCmd);
    CMD.print(F(",Trmp:"));
    CMD.print((int32_t)pidRt.targetRpmRamped);
    CMD.print(F(",ERR:"));
    CMD.print((int32_t)error);
    CMD.print(F(",OUT:"));
    CMD.print((int32_t)piOut);
    CMD.print(F(",I:"));
    CMD.print((int32_t)pidRt.errorIntegral);
    CMD.print(F(",D:"));
    CMD.print(motorRt.currentDuty);
    CMD.print(F(",PH:"));
    CMD.print((uint8_t)pidRt.speedPhase);
    CMD.print(F(",FC:"));
    CMD.println((uint8_t)motorRt.faultCode);
    return;
  }

  // Kompakt telemetri (varsayılan)
  uint32_t target = pidRt.enabled ? (uint32_t)abs(pidRt.targetRpmCmd) : 0;

  CMD.print(F("RPM:"));
  CMD.print(rpm);
  CMD.print(F(",T:"));
  CMD.print(target);
  CMD.print(F(",D:"));
  CMD.print(motorRt.currentDuty);
  CMD.print(F(",DIR:"));
  char dirChar;
  if (motorRt.direction > 0) dirChar = 'F';
  else if (motorRt.direction < 0) dirChar = 'R';
  else dirChar = 'N';
  CMD.print(dirChar);
  CMD.print(F(",PH:"));
  CMD.print((uint8_t)motorRt.phase);
  CMD.print(F(",SP:"));
  CMD.print(pidRt.enabled ? 1 : 0);
  CMD.print(F(",BRAKE:"));
  CMD.print(motorRt.phase == MotorPhase::Brake ? 1 : 0);
  CMD.print(F(",FC:"));
  CMD.print((uint8_t)motorRt.faultCode);
  CMD.print(F(",H:"));
  CMD.println(hallRt.stableRaw);
}

// Komut watchdog — belirli süre komut gelmezse motor durdur (güvenlik)
void checkCommandWatchdog(uint32_t nowMs) {
  if (!isMotorBusy()) return;
  if (lastMotorCommandMs == 0) return;

  if ((uint32_t)(nowMs - lastMotorCommandMs) > CMD_WATCHDOG_MS) {
    stopMotorImmediate();
    disablePidMode();
    CMD.println(F("[WARN] WD stop"));
    Serial.println(F("[WARN] Watchdog auto-stop"));
    lastMotorCommandMs = 0;
  }
}

// Host connection monitor — 2sn UART aktivite yoksa motor durdur
static constexpr uint32_t HOST_DISCONNECT_TIMEOUT_MS = 2000;
void checkHostConnection(uint32_t nowMs) {
  if (!isMotorBusy()) return;
  if (isServiceActive()) return;  // service task sırasında atla (identify vb.)
  if (lastUartActivityMs == 0) return;

  if ((uint32_t)(nowMs - lastUartActivityMs) > HOST_DISCONNECT_TIMEOUT_MS) {
    stopMotorImmediate();
    disablePidMode();
    CMD.println(F("[WARN] Host disconnect stop"));
    Serial.println(F("[WARN] Host disconnect auto-stop"));
    lastUartActivityMs = 0;
  }
}

// ============================================================
// Setup / Loop — Başlangıç ve Ana Döngü
// ============================================================

// Başlangıç — pinler, EEPROM, ISR, mod yüklemesi
void setup() {
  Serial.begin(115200);
  CMD.begin(115200);
  delay(100);

  // Pinler
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  // Donanımsal watchdog — IWDG reset kontrolü
  if (IWatchdog.isReset(true)) {
    sysLn(F("[WARN] IWDG reset detected"));
  }

  // IWDG başlat — 500ms timeout
  IWatchdog.begin(IWDG_TIMEOUT_US);
  if (!IWatchdog.isEnabled()) {
    sysLn(F("[ERR] IWDG init failed"));
  }

  pinMode(HALL_1_PIN, INPUT_PULLUP);
  pinMode(HALL_2_PIN, INPUT_PULLUP);
  pinMode(HALL_3_PIN, INPUT_PULLUP);

  pinMode(AL_PIN, OUTPUT);
  pinMode(BL_PIN, OUTPUT);
  pinMode(CL_PIN, OUTPUT);

  pinMode(AH_PIN, OUTPUT);
  pinMode(BH_PIN, OUTPUT);
  pinMode(CH_PIN, OUTPUT);

  // PWM ayarları
  analogWriteResolution(PWM_RESOLUTION_BITS);

#if defined(BLDC_USE_ANALOGWRITE_FREQUENCY)
  analogWriteFrequency(PWM_TARGET_FREQ_HZ);
  sysPrint(F("[OK] PWM freq set to "));
  sysPrintV(PWM_TARGET_FREQ_HZ);
  sysLn(F(" Hz"));
#else
  sysLn(F("[INFO] PWM freq left at core default"));
#endif

  allOff();

  // Hall ISR — 3 pin için kesme bağla
  attachInterrupt(digitalPinToInterrupt(HALL_1_PIN), hallISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(HALL_2_PIN), hallISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(HALL_3_PIN), hallISR, CHANGE);

  // Kalıcı verileri yükle
  if (loadHallMapFromStorage()) sysLn(F("[OK] Loaded saved hall map"));
  else {
    loadDefaultHallMap();
    sysLn(F("[INFO] Using default hall map"));
  }

  if (loadConfigFromStorage()) sysLn(F("[OK] Loaded saved config"));
  else {
    loadDefaultConfig();
    sysLn(F("[INFO] Using default config"));
  }

  // Mod yükle
  if (loadModeFromStorage()) {
    sysPrint(F("[OK] Loaded mode: "));
    if (activeMode == OperatingMode::Control) sysLn(F("CONTROL"));
    else if (activeMode == OperatingMode::Settings) sysLn(F("SETTINGS"));
    else sysLn(F("NORMAL"));
  } else {
    activeMode = OperatingMode::Normal;
    sysLn(F("[INFO] Default mode: NORMAL"));
  }

  initHallRuntime();

  memset(&usbCli, 0, sizeof(usbCli));
  memset(&cmdCli, 0, sizeof(cmdCli));

  nextControlTickUs = micros() + CONTROL_PERIOD_US;

  printHallMap(CommandSource::USB);
  printHelp(CommandSource::USB);

  if (activeMode == OperatingMode::Control) {
    sysLn(F("[INFO] Control mode active — WASD via UART"));
    CMD.println(F("[MODE] CONTROL"));
  } else if (activeMode == OperatingMode::Settings) {
    sysLn(F("[INFO] Settings mode active — clean monitor"));
  }
}

// Ana döngü — motor kontrol, UART okuma, komut işleme, telemetri
void loop() {
  uint32_t nowMs = millis();

  // Motor her zaman önce — mod fark etmez, motor sürücü çalışır
  runMotorControlScheduler();

  // Speed PI kontrol döngüsü — 50Hz (20ms)
  runSpeedControlLoop(nowMs);

  // RX toplama
  if (Serial.available()) {
    updateUartActivity();
    uartDrainToRing(Serial, usbRx);
  }
  if (CMD.available()) {
    updateUartActivity();
    uartDrainToRing(CMD, cmdRx);
  }

  // ring -> line -> command queue
  processRxRingToLines(usbRx, usbCli, CommandSource::USB);
  processRxRingToLines(cmdRx, cmdCli, CommandSource::UART);

  if (activeMode == OperatingMode::Control) {
    // Control modunda: WASD komut işleyici
    processControlQueuedCommands();

    // service mode
    updateServiceTask();

    // Birleşik telemetri (Normal/Control aynı format)
    sendTelemetry(nowMs);

    // Control modunda watchdog aktif — lease timeout ile failsafe
    checkCommandWatchdog(nowMs);

    // Host connection monitor — UART aktivite yoksa durdur
    checkHostConnection(nowMs);
  } else if (activeMode == OperatingMode::Settings) {
    // Settings modu — temiz monitör, tüm komutlar çalışır
    processQueuedCommands();
    updateServiceTask();
    // Telemetry YOK — temiz monitör
    // Watchdog AKTİF — güvenlik için
    checkCommandWatchdog(nowMs);
    checkHostConnection(nowMs);
  } else {
    // Normal mod — tam CLI
    processQueuedCommands();
    updateServiceTask();
    sendTelemetry(nowMs);
    checkCommandWatchdog(nowMs);
    checkHostConnection(nowMs);
  }

  // Donanımsal watchdog yenile — loop sağlıklı çalışıyorsa
  IWatchdog.reload();
}