/*
  ESP32 Captive Portal + Kontrol Relay + Deep Sleep + Ganti Sandi WiFi
  ---------------------------------------------------------------------
  Board   : ESP32 DevKit (core Arduino-ESP32 2.x atau 3.x)
  Relay   : GPIO26
  Tombol bangun (opsional): GPIO33 ke GND -> membangunkan dari deep sleep
  Reset sandi (darurat)   : GPIO27 ke GND saat boot, tahan 3 detik -> WiFi terbuka

  Keunggulan ESP32 dibanding ESP8266:
  - Bangun dari deep sleep lewat timer internal (tidak perlu kabel ke RST)
  - Relay bisa DIPERTAHANKAN tetap menyala saat deep sleep (GPIO hold)
  - Sandi disimpan di NVS (Preferences), lebih awet dari EEPROM
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"

// ===== Konfigurasi =====
const char* AP_SSID = "Relay-Portal";

#define RELAY_PIN            26
#define RELAY_AKTIF_LOW      false   // true jika modul relay aktif saat sinyal LOW
#define RELAY_TAHAN_SAAT_TIDUR true  // true: relay tetap pada kondisi terakhir saat deep sleep
#define RESET_PIN            27
#define WAKE_PIN             33      // harus pin RTC (mis. 32,33,34,35,36,39,25,26,27,4,2,0,12,13,14,15)
#define PAKAI_TOMBOL_BANGUN  true

const unsigned long IDLE_TIMEOUT_MS = 5UL * 60UL * 1000UL;
const uint32_t TIDUR_DEFAULT_MENIT  = 10;
const uint32_t TIDUR_MAKS_MENIT     = 24UL * 60UL;

const int SANDI_MIN  = 8;
const int SANDI_MAKS = 63;

const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);

DNSServer dnsServer;
WebServer server(80);
Preferences prefs;

String sandiWifi = "";                 // kosong = WiFi terbuka
RTC_DATA_ATTR bool relayOn = false;    // bertahan selama deep sleep
unsigned long aktivitasTerakhir = 0;

bool terapkanSandi = false;
unsigned long waktuTerapkan = 0;

// ===== Sandi (NVS) =====
bool adaSandi() { return sandiWifi.length() >= SANDI_MIN; }

void muatSandi() {
  prefs.begin("wifi", false);
  sandiWifi = prefs.getString("sandi", "");
  prefs.end();
}

void simpanSandi(const String& s) {
  prefs.begin("wifi", false);
  prefs.putString("sandi", s);
  prefs.end();
  sandiWifi = s;
}

void mulaiAP() {
  WiFi.softAPdisconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, adaSandi() ? sandiWifi.c_str() : nullptr);
}

// ===== Relay =====
void setRelay(bool on) {
  relayOn = on;
  bool level = RELAY_AKTIF_LOW ? !on : on;
  digitalWrite(RELAY_PIN, level ? HIGH : LOW);
}

// ===== Deep sleep =====
void masukTidur(uint32_t menit) {
  if (menit < 1) menit = 1;
  if (menit > TIDUR_MAKS_MENIT) menit = TIDUR_MAKS_MENIT;
  Serial.printf("Masuk deep sleep %u menit...\n", menit);

  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

#if RELAY_TAHAN_SAAT_TIDUR
  gpio_hold_en((gpio_num_t)RELAY_PIN);   // kunci level pin relay
  gpio_deep_sleep_hold_en();
#endif

  esp_sleep_enable_timer_wakeup((uint64_t)menit * 60ULL * 1000000ULL);
#if PAKAI_TOMBOL_BANGUN
  rtc_gpio_pullup_en((gpio_num_t)WAKE_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)WAKE_PIN);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)WAKE_PIN, 0);  // bangun saat LOW
#endif
  delay(100);
  esp_deep_sleep_start();
}

// ===== Halaman web =====
const char HALAMAN[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Kontrol Relay</title>
<style>
  body{font-family:system-ui,sans-serif;background:#10141a;color:#eee;margin:0;padding:20px;text-align:center}
  h1{font-size:1.4rem;margin-bottom:4px}
  p.sub{color:#8a94a3;margin-top:0}
  .kartu{background:#1a2029;border-radius:16px;padding:20px;margin:16px auto;max-width:360px}
  .status{font-size:1.1rem;margin-bottom:14px}
  .titik{display:inline-block;width:12px;height:12px;border-radius:50%;background:#555;margin-right:8px}
  .on .titik{background:#2ecc71}
  button{width:100%;padding:16px;border:0;border-radius:12px;font-size:1.1rem;font-weight:600;cursor:pointer}
  #btnRelay{background:#2ecc71;color:#06260f}
  #btnRelay.mati{background:#e74c3c;color:#fff}
  select,input,#btnTidur,#btnSandi{margin-top:10px}
  select,input{width:100%;box-sizing:border-box;padding:12px;border-radius:10px;border:0;background:#262e3a;color:#eee;font-size:1rem}
  #btnTidur{background:#3b82f6;color:#fff}
  #btnSandi{background:#f59e0b;color:#1f1400}
  label.cek{display:block;text-align:left;margin-top:10px;color:#8a94a3;font-size:.9rem}
  label.cek input{width:auto;margin-right:6px}
  small{color:#8a94a3;display:block;margin-top:10px}
  #pesan{margin-top:10px;font-weight:600}
  .ok{color:#2ecc71}.err{color:#ff6b6b}
</style>
</head>
<body>
  <h1>Kontrol Relay</h1>
  <p class="sub">ESP32 &bull; Captive Portal</p>

  <div class="kartu">
    <div class="status" id="status"><span class="titik"></span><span id="teks">Memuat...</span></div>
    <button id="btnRelay" onclick="toggle()">...</button>
  </div>

  <div class="kartu">
    <b>Mode Tidur (Deep Sleep)</b>
    <select id="menit">
      <option value="1">1 menit</option>
      <option value="5">5 menit</option>
      <option value="10" selected>10 menit</option>
      <option value="30">30 menit</option>
      <option value="60">60 menit</option>
      <option value="180">3 jam</option>
      <option value="720">12 jam</option>
    </select>
    <button id="btnTidur" onclick="tidur()">Tidur Sekarang</button>
    <small id="infoTidur">WiFi akan mati dan perangkat bangun otomatis. Sambungkan ulang setelah bangun.</small>
  </div>

  <div class="kartu">
    <b>Ganti Sandi WiFi</b>
    <input type="password" id="lama" placeholder="Sandi saat ini" style="display:none" autocomplete="off">
    <input type="password" id="baru" placeholder="Sandi baru (8-63 karakter)" autocomplete="off">
    <input type="password" id="ulang" placeholder="Ulangi sandi baru" autocomplete="off">
    <label class="cek"><input type="checkbox" onchange="lihat(this.checked)">Tampilkan sandi</label>
    <button id="btnSandi" onclick="gantiSandi()">Simpan Sandi</button>
    <div id="pesan"></div>
    <small id="infoSandi"></small>
  </div>

<script>
let on=false, terkunci=false;
function tampil(s){
  on=s.relay; terkunci=s.terkunci;
  document.getElementById('teks').textContent='Relay: '+(on?'MENYALA':'MATI');
  document.getElementById('status').className='status '+(on?'on':'');
  const b=document.getElementById('btnRelay');
  b.textContent=on?'Matikan Relay':'Nyalakan Relay';
  b.className=on?'mati':'';
  document.getElementById('lama').style.display=terkunci?'block':'none';
  document.getElementById('infoSandi').textContent=terkunci
    ?'WiFi saat ini dilindungi sandi. Kosongkan sandi baru untuk membuat WiFi terbuka.'
    :'WiFi saat ini TERBUKA (tanpa sandi). Siapa pun dapat mengontrol relay.';
  if(s.tahan) document.getElementById('infoTidur').textContent='Relay tetap pada kondisi terakhir selama tidur. WiFi mati dan perangkat bangun otomatis.';
}
function muat(){fetch('/status').then(r=>r.json()).then(tampil).catch(()=>{});}
function toggle(){fetch('/relay?s='+(on?0:1)).then(r=>r.json()).then(tampil);}
function tidur(){
  const m=document.getElementById('menit').value;
  if(!confirm('Masuk mode tidur '+m+' menit?'))return;
  fetch('/tidur?m='+m);
  document.body.innerHTML='<h1>Perangkat tidur...</h1><p class="sub">Bangun otomatis dalam '+m+' menit.</p>';
}
function lihat(v){['lama','baru','ulang'].forEach(i=>document.getElementById(i).type=v?'text':'password');}
function pesan(t,ok){const p=document.getElementById('pesan');p.textContent=t;p.className=ok?'ok':'err';}
function gantiSandi(){
  const lama=document.getElementById('lama').value;
  const baru=document.getElementById('baru').value;
  const ulang=document.getElementById('ulang').value;
  if(baru!==ulang){pesan('Sandi baru dan pengulangan tidak sama.',false);return;}
  if(baru.length>0&&baru.length<8){pesan('Sandi minimal 8 karakter.',false);return;}
  if(baru.length===0&&!confirm('Sandi kosong akan membuat WiFi TERBUKA. Lanjutkan?'))return;
  fetch('/sandi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'lama='+encodeURIComponent(lama)+'&baru='+encodeURIComponent(baru)})
  .then(r=>r.json()).then(j=>{
    pesan(j.pesan,j.ok);
    if(j.ok){
      document.body.innerHTML='<h1>Sandi diubah</h1><p class="sub">WiFi akan restart. Sambungkan ulang dengan sandi baru, lalu buka http://192.168.4.1</p>';
    }
  }).catch(()=>pesan('Gagal menghubungi perangkat.',false));
}
muat(); setInterval(muat,5000);
</script>
</body>
</html>
)rawliteral";

// ===== Handler =====
void tandaiAktif() { aktivitasTerakhir = millis(); }

void kirimStatus() {
  String j = String("{\"relay\":") + (relayOn ? "true" : "false") +
             ",\"terkunci\":" + (adaSandi() ? "true" : "false") +
             ",\"tahan\":" + (RELAY_TAHAN_SAAT_TIDUR ? "true" : "false") + "}";
  server.send(200, "application/json", j);
}

void handleRoot() {
  tandaiAktif();
  server.send_P(200, "text/html; charset=utf-8", HALAMAN);
}

void handleStatus() { tandaiAktif(); kirimStatus(); }

void handleRelay() {
  tandaiAktif();
  if (server.hasArg("s")) setRelay(server.arg("s") == "1");
  kirimStatus();
}

void handleTidur() {
  uint32_t m = server.hasArg("m") ? server.arg("m").toInt() : TIDUR_DEFAULT_MENIT;
  server.send(200, "text/plain", "OK");
  delay(300);
  masukTidur(m);
}

void kirimHasilSandi(bool ok, const char* pesan) {
  String j = String("{\"ok\":") + (ok ? "true" : "false") + ",\"pesan\":\"" + pesan + "\"}";
  server.send(200, "application/json", j);
}

void handleSandi() {
  tandaiAktif();
  String lama = server.arg("lama");
  String baru = server.arg("baru");

  if (adaSandi() && lama != sandiWifi) {
    kirimHasilSandi(false, "Sandi saat ini salah.");
    return;
  }
  if (baru.length() > 0 && baru.length() < SANDI_MIN) {
    kirimHasilSandi(false, "Sandi minimal 8 karakter.");
    return;
  }
  if (baru.length() > SANDI_MAKS) {
    kirimHasilSandi(false, "Sandi maksimal 63 karakter.");
    return;
  }

  simpanSandi(baru);
  kirimHasilSandi(true, "Sandi berhasil disimpan. WiFi akan restart.");
  terapkanSandi = true;
  waktuTerapkan = millis();
}

void redirectKePortal() {
  tandaiAktif();
  server.sendHeader("Location", String("http://") + apIP.toString(), true);
  server.send(302, "text/plain", "");
}

void cekResetSandi() {
  pinMode(RESET_PIN, INPUT_PULLUP);
  if (digitalRead(RESET_PIN) != LOW) return;
  unsigned long t = millis();
  while (digitalRead(RESET_PIN) == LOW) {
    if (millis() - t > 3000) {
      simpanSandi("");
      Serial.println("Sandi direset: WiFi kini terbuka.");
      return;
    }
    delay(10);
  }
}

void setup() {
  Serial.begin(115200);

  // Lepas kunci pin setelah bangun dari deep sleep, lalu pulihkan level relay
  bool bangunDariTidur = (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED);
  if (bangunDariTidur) {
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis((gpio_num_t)RELAY_PIN);
  } else {
    relayOn = false;   // nyala pertama kali: relay mati
  }
  pinMode(RELAY_PIN, OUTPUT);
  setRelay(relayOn);

  muatSandi();
  cekResetSandi();

  mulaiAP();
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/relay", handleRelay);
  server.on("/tidur", handleTidur);
  server.on("/sandi", HTTP_POST, handleSandi);
  server.on("/generate_204", redirectKePortal);
  server.on("/gen_204", redirectKePortal);
  server.on("/hotspot-detect.html", redirectKePortal);
  server.on("/fwlink", redirectKePortal);
  server.on("/connecttest.txt", redirectKePortal);
  server.onNotFound(redirectKePortal);
  server.begin();

  tandaiAktif();
  Serial.printf("\nAP '%s' aktif di %s | sandi: %s | relay: %s\n",
                AP_SSID, apIP.toString().c_str(),
                adaSandi() ? "ya" : "terbuka", relayOn ? "ON" : "OFF");
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  if (terapkanSandi && millis() - waktuTerapkan > 1500) {
    terapkanSandi = false;
    mulaiAP();
    tandaiAktif();
  }

  if (millis() - aktivitasTerakhir > IDLE_TIMEOUT_MS) {
    masukTidur(TIDUR_DEFAULT_MENIT);
  }
}
