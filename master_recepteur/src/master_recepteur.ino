/*
 * ═══════════════════════════════════════════════════════════════════════════════
 * ESP32 MASTER RÉCEPTEUR - DispoMesh v6.1 - EMBEDDED WEB
 * ═══════════════════════════════════════════════════════════════════════════════
 * NOUVELLES FONCTIONNALITÉS:
 *   ✓ Anti-clignotement (cache displays)
 *   ✓ Detection écrans détachables I2C
 *   ✓ Animations de transition fluides
 *   ✓ Historique des statuts
 *   ✓ API REST complète
 *   ✓ Dashboard web amélioré avec QR Code
 * ═══════════════════════════════════════════════════════════════════════════════
 */

#include <Adafruit_GFX.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_task_wdt.h>
#include <painlessMesh.h>

// MODULES CRITIQUES
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

// Config CONFIGURATION MESH
#define MESH_PREFIX "DispoMesh"
#define MESH_PASSWORD "meshpass2025"
#define MESH_PORT 5555
#define MESH_CHANNEL 6

// Anim CONFIGURATION ANIMATIONS
#define SCROLL_SPEED 2.5f
#define ANIMATION_FPS 30
#define FRAME_DELAY (1000 / ANIMATION_FPS)
#define SCREEN_CHECK_MS 5000
#define HISTORY_MAX 20
#define HEARTBEAT_TIMEOUT_MS 60000 // 60 secondes sans heartbeat = déconnexion

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_ADDR 0x3C
#define TCA_ADDR 0x70

#define PIN_NEOPIXEL 27
#define PIXELS_PER_SLOT 1
constexpr uint8_t MAX_SLOTS = 5;
#define TOTAL_PIXELS (MAX_SLOTS * PIXELS_PER_SLOT)

Adafruit_NeoPixel strip(TOTAL_PIXELS, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
const uint8_t canaux[MAX_SLOTS] = {0, 1, 2, 6, 7};

const uint32_t C_DISPO = strip.Color(0, 255, 0);
const uint32_t C_BUSY = strip.Color(255, 0, 0);
const uint32_t C_AWAY = strip.Color(255, 140, 0);
const uint32_t C_OFF = strip.Color(10, 10, 10);

// Screen Cache displays (anti-clignotement)
Adafruit_SSD1306 *displays[MAX_SLOTS] = {nullptr};
U8G2_FOR_ADAFRUIT_GFX *u8g2Cache[MAX_SLOTS] = {nullptr};
bool screenPresent[MAX_SLOTS] = {false};

// History Structure historique
struct HistoryEntry {
  unsigned long timestamp;
  int oldState;
  int newState;
};

struct PersonInfo {
  uint32_t nodeId = 0;
  String prenom = "---";
  String nom = "---";
  int etat = 2;
  bool connecte = false;

  // Heartbeat tracking
  unsigned long lastHeartbeat = 0;
  bool ecoMode = false;

  // Animation scrolling
  float scrollPos = 0;
  bool needsScroll = false;
  int textWidth = 0;
  bool dirty = true;

  // Animation transition
  int prevEtat = 2;
  uint8_t transitionFrame = 0;
  bool inTransition = false;

  // Historique circulaire
  HistoryEntry history[HISTORY_MAX];
  uint8_t historyIdx = 0;
  uint8_t historyCount = 0;
};

PersonInfo persons[MAX_SLOTS];
SemaphoreHandle_t xMutex;
Scheduler userScheduler;
painlessMesh mesh;
WebServer server(80);

unsigned long lastScreenCheck = 0;
unsigned long bootTime = 0;

// Historique global pour l'API
struct GlobalHistoryEntry {
  unsigned long timestamp;
  uint8_t slot;
  int oldState;
  int newState;
};
#define GLOBAL_HISTORY_MAX 30
GlobalHistoryEntry globalHistory[GLOBAL_HISTORY_MAX];
uint8_t globalHistoryIdx = 0;
uint8_t globalHistoryCount = 0;

// Icônes
const unsigned char icon_wifi[] PROGMEM = {0x3C, 0x42, 0x81, 0x3C,
                                           0x42, 0x18, 0x00, 0x18};
const unsigned char icon_disconn[] PROGMEM = {0x00, 0x42, 0x24, 0x18,
                                              0x18, 0x24, 0x42, 0x00};

// ═══════════════════════════════════════════════════════════════════════
// WEB RESOURCES (EMBEDDED)
// ═══════════════════════════════════════════════════════════════════════
const char HTML_INDEX[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>DispoMesh Dashboard</title>
    <link rel="stylesheet" href="style.css">
</head>
<body>
    <div class="container">
        <header>
            <div class="logo-area">
                <div class="pulse-dot"></div>
                <h1>DISPOMESH</h1>
            </div>
            <p class="subtitle">Système de Disponibilité Distribué</p>
        </header>

        <div class="dashboard-grid">
            <!-- Stats Cards -->
            <div class="stats-container">
                <div class="stat-card available">
                    <span class="stat-value" id="cd">-</span>
                    <span class="stat-label">Disponibles</span>
                </div>
                <div class="stat-card busy">
                    <span class="stat-value" id="cb">-</span>
                    <span class="stat-label">Occupés</span>
                </div>
                <div class="stat-card away">
                    <span class="stat-value" id="ca">-</span>
                    <span class="stat-label">Absents</span>
                </div>
                <div class="stat-card total">
                    <span class="stat-value" id="ct">-</span>
                    <span class="stat-label">Connectés</span>
                </div>
            </div>

            <!-- Slots Grid -->
            <div id="slots" class="slots-grid">
                <!-- Slots injected by JS -->
            </div>

            <!-- Bottom Section -->
            <div class="bottom-section">
                <div class="panel qr-panel">
                    <h3>Rejoindre le Mesh</h3>
                    <div class="qr-code">
                        <svg viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg">
                            <path d="M10,10 h30 v30 h-30 z M50,10 h30 v30 h-30 z M10,50 h30 v30 h-30 z M50,50 h10 v10 h-10 z M70,50 h10 v10 h-10 z M50,70 h10 v10 h-10 z M70,70 h10 v10 h-10 z" fill="#333"/>
                        </svg>
                        <div class="wifi-creds">
                            <strong>SSID:</strong> DispoMesh<br>
                            <strong>PASS:</strong> meshpass2025
                        </div>
                    </div>
                </div>

                <div class="panel history-panel">
                    <h3>Historique Récent</h3>
                    <div id="hist" class="history-list">
                        <!-- History injected by JS -->
                    </div>
                </div>
            </div>
        </div>

        <footer>
            <div class="status-bar">
                <span>DispoMesh v6.1 Embedded</span>
                <span class="separator">•</span>
                <span>Uptime: <span id="up">-</span></span>
            </div>
        </footer>
    </div>
    <script src="script.js"></script>
</body>
</html>
)rawliteral";

const char CSS_STYLE[] PROGMEM = R"rawliteral(
:root {
    --bg-dark: #0f172a;
    --bg-card: rgba(30, 41, 59, 0.7);
    --text-primary: #f8fafc;
    --text-secondary: #94a3b8;
    --accent-green: #4ade80;
    --accent-red: #f87171;
    --accent-orange: #fbbf24;
    --accent-blue: #38bdf8;
    --glass-border: 1px solid rgba(255, 255, 255, 0.08);
}

* { margin: 0; padding: 0; box-sizing: border-box; }

body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    background-color: var(--bg-dark);
    background-image: radial-gradient(circle at top right, #1e293b 0%, transparent 40%),
                      radial-gradient(circle at bottom left, #1e1b4b 0%, transparent 40%);
    color: var(--text-primary);
    min-height: 100vh;
    padding: 2rem;
    line-height: 1.5;
}

.container {
    max-width: 1200px;
    margin: 0 auto;
}

/* Header */
header {
    margin-bottom: 3rem;
    text-align: center;
}

.logo-area {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 1rem;
    margin-bottom: 0.5rem;
}

.pulse-dot {
    width: 12px;
    height: 12px;
    background-color: var(--accent-green);
    border-radius: 50%;
    box-shadow: 0 0 0 0 rgba(74, 222, 128, 0.7);
    animation: pulse 2s infinite;
}

@keyframes pulse {
    0% { transform: scale(0.95); box-shadow: 0 0 0 0 rgba(74, 222, 128, 0.7); }
    70% { transform: scale(1); box-shadow: 0 0 0 10px rgba(74, 222, 128, 0); }
    100% { transform: scale(0.95); box-shadow: 0 0 0 0 rgba(74, 222, 128, 0); }
}

h1 {
    font-weight: 200;
    letter-spacing: 4px;
    font-size: 2.5rem;
}

.subtitle {
    color: var(--text-secondary);
    font-weight: 300;
    letter-spacing: 1px;
}

/* Stats */
.stats-container {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
    gap: 1.5rem;
    margin-bottom: 3rem;
}

.stat-card {
    background: var(--bg-card);
    backdrop-filter: blur(10px);
    border: var(--glass-border);
    border-radius: 16px;
    padding: 1.5rem;
    text-align: center;
    transition: transform 0.2s;
}

.stat-card:hover { transform: translateY(-2px); }

.stat-value { display: block; font-size: 2.5rem; font-weight: 600; line-height: 1.2; }
.stat-label { color: var(--text-secondary); font-size: 0.875rem; text-transform: uppercase; letter-spacing: 1px; }

.available .stat-value { color: var(--accent-green); }
.busy .stat-value { color: var(--accent-red); }
.away .stat-value { color: var(--accent-orange); }

/* Slots */
.slots-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
    gap: 1.5rem;
    margin-bottom: 3rem;
}

.slot {
    background: var(--bg-card);
    border: var(--glass-border);
    border-radius: 16px;
    padding: 1.5rem;
    position: relative;
    overflow: hidden;
    transition: all 0.3s ease;
}

.slot:hover {
    border-color: rgba(255,255,255,0.2);
    transform: translateY(-4px);
    box-shadow: 0 10px 30px -10px rgba(0,0,0,0.5);
}

.slot.offline { opacity: 0.6; filter: grayscale(0.8); }

.slot-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 1rem;
}

.slot-badge {
    font-size: 0.75rem;
    font-weight: 600;
    padding: 0.25rem 0.75rem;
    border-radius: 999px;
    text-transform: uppercase;
}

.badge-dispo { background: rgba(74, 222, 128, 0.15); color: var(--accent-green); }
.badge-busy { background: rgba(248, 113, 113, 0.15); color: var(--accent-red); }
.badge-away { background: rgba(251, 191, 36, 0.15); color: var(--accent-orange); }
.badge-offline { background: rgba(148, 163, 184, 0.15); color: var(--text-secondary); }

.slot-name {
    font-size: 1.5rem;
    font-weight: 500;
    margin-bottom: 0.5rem;
}

.slot-meta {
    display: flex;
    justify-content: space-between;
    align-items: center;
    font-size: 0.875rem;
    color: var(--text-secondary);
}

.eco-indicator {
    color: var(--accent-blue);
    display: flex;
    align-items: center;
    gap: 4px;
    font-size: 0.75rem;
}

/* Bottom Section */
.bottom-section {
    display: grid;
    grid-template-columns: 1fr 2fr;
    gap: 1.5rem;
    margin-bottom: 3rem;
}

.panel {
    background: var(--bg-card);
    border: var(--glass-border);
    border-radius: 16px;
    padding: 1.5rem;
}

.panel h3 {
    font-size: 1rem;
    font-weight: 500;
    margin-bottom: 1rem;
    color: var(--text-secondary);
    text-transform: uppercase;
}

.qr-panel { text-align: center; }
.qr-code {
    background: white;
    padding: 1rem;
    border-radius: 12px;
    display: inline-block;
    color: #333;
}
.qr-code svg { width: 100px; height: 100px; display: block; margin: 0 auto 1rem; }
.wifi-creds { font-size: 0.875rem; line-height: 1.4; }

.history-list {
    max-height: 200px;
    overflow-y: auto;
}

.hist-item {
    display: flex;
    justify-content: space-between;
    padding: 0.75rem 0;
    border-bottom: 1px solid rgba(255,255,255,0.05);
    font-size: 0.9rem;
}
.hist-item:last-child { border-bottom: none; }
.hist-time { color: var(--text-secondary); font-variant-numeric: tabular-nums; }

/* Footer */
footer {
    text-align: center;
    color: var(--text-secondary);
    font-size: 0.875rem;
    border-top: 1px solid rgba(255,255,255,0.05);
    padding-top: 2rem;
}

@media (max-width: 768px) {
    .bottom-section { grid-template-columns: 1fr; }
}
)rawliteral";

const char JS_SCRIPT[] PROGMEM = R"rawliteral(
const STATUS_LABELS = ['DISPONIBLE', 'OCCUPÉ', 'ABSENT'];
const STATUS_CLASSES = ['badge-dispo', 'badge-busy', 'badge-away'];

function formatTime(ms) {
    const s = Math.floor(ms / 1000);
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    return `${h}h ${m}m`;
}

function updateDashboard() {
    fetch('/api/data')
        .then(response => response.json())
        .then(data => {
            let dispo = 0, busy = 0, away = 0, total = 0;
            const slotsContainer = document.getElementById('slots');
            let slotsHtml = '';

            data.slots.forEach((slot, index) => {
                if (slot.connecte) {
                    total++;
                    if (slot.etat === 0) dispo++;
                    else if (slot.etat === 1) busy++;
                    else away++;
                }

                const statusClass = slot.connecte ? STATUS_CLASSES[slot.etat] : 'badge-offline';
                const statusText = slot.connecte ? STATUS_LABELS[slot.etat] : 'HORS LIGNE';
                const cardClass = slot.connecte ? '' : 'offline';
                const ecoBadge = slot.ecoMode ? '<span class="eco-indicator">⚡ ÉCO</span>' : '';
                const lastSeen = slot.connecte && slot.lastSeen > 0 ? `Seen: ${slot.lastSeen}s ago` : '';

                slotsHtml += `
                    <div class="slot ${cardClass}">
                        <div class="slot-header">
                            <span class="slot-id">POSTE ${index + 1}</span>
                            <span class="slot-badge ${statusClass}">${statusText}</span>
                        </div>
                        <div class="slot-name">${slot.prenom} ${slot.nom}</div>
                        <div class="slot-meta">
                            <span>Node: ${slot.nodeId || '-'}</span>
                            ${ecoBadge}
                        </div>
                        <div class="slot-meta" style="margin-top:4px; font-size:0.7rem">
                            ${lastSeen}
                        </div>
                    </div>
                `;
            });

            slotsContainer.innerHTML = slotsHtml;
            document.getElementById('cd').textContent = dispo;
            document.getElementById('cb').textContent = busy;
            document.getElementById('ca').textContent = away;
            document.getElementById('ct').textContent = total;
            document.getElementById('up').textContent = formatTime(data.uptime || 0);

            // History
            if (data.history && data.history.length > 0) {
                let histHtml = '';
                // Show last 8 entries
                const recentHistory = data.history.slice(Math.max(data.history.length - 8, 0)).reverse();
                
                recentHistory.forEach(h => {
                    const statusText = h.newState === -1 ? 'DÉCONNEXION' : STATUS_LABELS[h.newState];
                    const date = new Date(h.timestamp);
                    const timeStr = date.toLocaleTimeString('fr-FR', {hour: '2-digit', minute: '2-digit'});
                    
                    histHtml += `
                        <div class="hist-item">
                            <span>Poste ${h.slot + 1}: ${statusText}</span>
                            <span class="hist-time">${timeStr}</span>
                        </div>
                    `;
                });
                document.getElementById('hist').innerHTML = histHtml;
            }
        })
        .catch(err => console.error('Error fetching data:', err));
}

// Initial load
updateDashboard();
// Refresh every 2 seconds
setInterval(updateDashboard, 2000);
)rawliteral";

// Prototypes pour éviter les erreurs de compilation PlatformIO
void recoverI2C();
bool tcaSelect(uint8_t i);
bool isScreenPresent(uint8_t channel);
bool initScreen(uint8_t idx);
void checkScreens();
void addHistory(uint8_t idx, int oldState, int newState);
void updateLEDs();
void drawScreen(uint8_t idx);
void saveDb(uint32_t id, String p, String n);
int findSlot(uint32_t id);
int findFree();
void receivedCallback(uint32_t from, String &msg);
void changed();
void taskRun(void *p);
void playStartupAnimation();
void checkScreenSaver();

void recoverI2C() {
  Wire.end();
  delay(10);
  Wire.begin();
  Wire.setClock(400000);
  Wire.setTimeOut(1000);
}
bool tcaSelect(uint8_t i) {
  if (i > 7)
    return false;
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(1 << i);
  if (Wire.endTransmission() != 0) {
    recoverI2C();
    return false;
  }
  return true;
}

// Screen Detection d'écran I2C sur un canal
bool isScreenPresent(uint8_t channel) {
  if (!tcaSelect(channel))
    return false;
  Wire.beginTransmission(OLED_ADDR);
  return (Wire.endTransmission() == 0);
}

// Screen Initialisation d'un écran (cache anti-flicker)
bool initScreen(uint8_t idx) {
  if (!tcaSelect(canaux[idx]))
    return false;
  if (displays[idx] != nullptr) {
    delete u8g2Cache[idx];
    delete displays[idx];
  }
  displays[idx] = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
  if (!displays[idx]->begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    delete displays[idx];
    displays[idx] = nullptr;
    return false;
  }
  u8g2Cache[idx] = new U8G2_FOR_ADAFRUIT_GFX();
  u8g2Cache[idx]->begin(*displays[idx]);
  u8g2Cache[idx]->setFont(u8g2_font_unifont_t_symbols);
  u8g2Cache[idx]->setForegroundColor(SSD1306_WHITE);
  screenPresent[idx] = true;
  Serial.printf("[SCREEN] Ecran %d initialisé\n", idx);
  return true;
}

// Screen Vérification périodique des écrans (hot-plug)
void checkScreens() {
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    bool present = isScreenPresent(canaux[i]);
    if (present && !screenPresent[i]) {
      Serial.printf("[SCREEN] Ecran %d detecté!\n", i);
      if (initScreen(i))
        persons[i].dirty = true;
    } else if (!present && screenPresent[i]) {
      Serial.printf("[SCREEN] Ecran %d deconnecté\n", i);
      screenPresent[i] = false;
      if (displays[i] != nullptr) {
        delete u8g2Cache[i];
        delete displays[i];
        displays[i] = nullptr;
        u8g2Cache[i] = nullptr;
      }
    }
  }
}

// History Ajout historique (local par personne + global pour API)
void addHistory(uint8_t idx, int oldState, int newState) {
  if (idx >= MAX_SLOTS)
    return;
  PersonInfo &p = persons[idx];
  p.history[p.historyIdx].timestamp = millis();
  p.history[p.historyIdx].oldState = oldState;
  p.history[p.historyIdx].newState = newState;
  p.historyIdx = (p.historyIdx + 1) % HISTORY_MAX;
  if (p.historyCount < HISTORY_MAX)
    p.historyCount++;

  // Ajout à l'historique global
  globalHistory[globalHistoryIdx].timestamp = millis();
  globalHistory[globalHistoryIdx].slot = idx;
  globalHistory[globalHistoryIdx].oldState = oldState;
  globalHistory[globalHistoryIdx].newState = newState;
  globalHistoryIdx = (globalHistoryIdx + 1) % GLOBAL_HISTORY_MAX;
  if (globalHistoryCount < GLOBAL_HISTORY_MAX)
    globalHistoryCount++;
}

void updateLEDs() {
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    uint32_t c = C_OFF;
    if (persons[i].connecte) {
      if (persons[i].etat == 0)
        c = C_DISPO;
      else if (persons[i].etat == 1)
        c = C_BUSY;
      else
        c = C_AWAY;
    }
    for (int k = 0; k < PIXELS_PER_SLOT; k++)
      strip.setPixelColor(i * PIXELS_PER_SLOT + k, c);
  }
  strip.show();
}

// Anim Affichage avec cache (anti-flicker)
void drawScreen(uint8_t idx) {
  if (!screenPresent[idx] || displays[idx] == nullptr)
    return;
  if (!tcaSelect(canaux[idx]))
    return;

  Adafruit_SSD1306 *d = displays[idx];
  U8G2_FOR_ADAFRUIT_GFX *u = u8g2Cache[idx];
  PersonInfo &p = persons[idx];

  d->clearDisplay();

  // Header
  d->fillRect(0, 0, SCREEN_WIDTH, 10, SSD1306_WHITE);
  d->setTextColor(SSD1306_BLACK);
  d->setTextSize(1);
  d->setCursor(2, 1);
  d->print("POSTE ");
  d->print(idx + 1);
  d->drawBitmap(118, 1, p.connecte ? icon_wifi : icon_disconn, 8, 8,
                SSD1306_BLACK);

  if (!p.connecte) {
    d->setTextColor(SSD1306_WHITE);
    d->setCursor(20, 18);
    d->print("EN ATTENTE...");
    d->display();
    return;
  }

  // Animation transition (flash)
  if (p.inTransition && p.transitionFrame < 8) {
    if (p.transitionFrame % 2 == 0)
      d->fillRect(0, 10, SCREEN_WIDTH, 22, SSD1306_WHITE);
    p.transitionFrame++;
    if (p.transitionFrame >= 8)
      p.inTransition = false;
  }

  // Nom avec scrolling fluide
  String txt = p.prenom + " " + p.nom;
  p.textWidth = u->getUTF8Width(txt.c_str());
  p.needsScroll = (p.textWidth > SCREEN_WIDTH - 4);
  int y = 24;

  if (p.needsScroll) {
    p.scrollPos += SCROLL_SPEED;
    if (p.scrollPos > p.textWidth + 30)
      p.scrollPos = 0;
    int xPos = (int)(SCREEN_WIDTH - p.scrollPos);
    u->drawUTF8(xPos, y, txt.c_str());
    if (xPos + p.textWidth + 30 < SCREEN_WIDTH)
      u->drawUTF8(xPos + p.textWidth + 30, y, txt.c_str());
  } else {
    u->drawUTF8((SCREEN_WIDTH - p.textWidth) / 2, y, txt.c_str());
  }

  // Barre de statut en bas
  uint8_t barW = SCREEN_WIDTH / 3;
  uint8_t barX = (SCREEN_WIDTH - barW) / 2;
  d->drawRect(barX, 29, barW, 3, SSD1306_WHITE);
  if (p.etat == 0)
    d->fillRect(barX, 29, barW, 3, SSD1306_WHITE);
  else if (p.etat == 1)
    d->fillRect(barX, 29, barW / 2, 3, SSD1306_WHITE);

  d->display();
}

// Screen Saver
unsigned long lastActivityTime = 0;
bool screenSaverActive = false;
#define SCREEN_SAVER_TIMEOUT 600000 // 10 minutes

void playStartupAnimation() {
  // Knight Rider effect
  for (int j = 0; j < 2; j++) {
    for (int i = 0; i < TOTAL_PIXELS; i++) {
      strip.clear();
      strip.setPixelColor(i, strip.Color(0, 150, 255));
      if (i > 0)
        strip.setPixelColor(i - 1, strip.Color(0, 50, 100));
      if (i > 1)
        strip.setPixelColor(i - 2, strip.Color(0, 10, 20));
      strip.show();
      delay(30);
    }
    for (int i = TOTAL_PIXELS - 1; i >= 0; i--) {
      strip.clear();
      strip.setPixelColor(i, strip.Color(0, 150, 255));
      if (i < TOTAL_PIXELS - 1)
        strip.setPixelColor(i + 1, strip.Color(0, 50, 100));
      if (i < TOTAL_PIXELS - 2)
        strip.setPixelColor(i + 2, strip.Color(0, 10, 20));
      strip.show();
      delay(30);
    }
  }
  strip.clear();
  strip.show();
}

void checkScreenSaver() {
  if (millis() - lastActivityTime > SCREEN_SAVER_TIMEOUT) {
    if (!screenSaverActive) {
      screenSaverActive = true;
      Serial.println("[SAVER] Activation économiseur d'écran");
      for (int i = 0; i < MAX_SLOTS; i++) {
        if (displays[i] != nullptr && screenPresent[i]) {
          displays[i]->ssd1306_command(SSD1306_DISPLAYOFF);
        }
      }
      strip.setBrightness(5); // Dim LEDs
      strip.show();
    }
  } else {
    if (screenSaverActive) {
      screenSaverActive = false;
      Serial.println("[SAVER] Désactivation économiseur");
      for (int i = 0; i < MAX_SLOTS; i++) {
        if (displays[i] != nullptr && screenPresent[i]) {
          displays[i]->ssd1306_command(SSD1306_DISPLAYON);
        }
      }
      strip.setBrightness(40); // Restore brightness
      strip.show();
    }
  }
}

void saveDb(uint32_t id, String p, String n) {
  File f = SPIFFS.open("/n_" + String(id), "w");
  if (f) {
    f.print(p + "\n" + n);
    f.close();
  }
}

int findSlot(uint32_t id) {
  for (int i = 0; i < MAX_SLOTS; i++)
    if (persons[i].connecte && persons[i].nodeId == id)
      return i;
  return -1;
}
int findFree() {
  for (int i = 0; i < MAX_SLOTS; i++)
    if (!persons[i].connecte)
      return i;
  return -1;
}

void receivedCallback(uint32_t from, String &msg) {
  lastActivityTime = millis(); // Reset Screen Saver timer
  Serial.printf("[MESH] Recu de %u: %s\n", from, msg.c_str());

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, msg) != DeserializationError::Ok) {
    Serial.println("[MESH] Erreur JSON");
    return;
  }

  String t = doc["type"];

  if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
    if (t == "register") {
      int idx = findSlot(from);
      if (idx == -1)
        idx = findFree();

      if (idx != -1) {
        int oldEtat = persons[idx].etat;
        persons[idx].nodeId = from;
        persons[idx].prenom = doc["prenom"].as<String>();
        persons[idx].nom = doc["nom"].as<String>();
        persons[idx].etat = doc["etat"];
        persons[idx].connecte = true;
        persons[idx].dirty = true;
        persons[idx].scrollPos = 0;
        persons[idx].lastHeartbeat = millis(); // Initialiser heartbeat
        persons[idx].ecoMode = false;

        // Historique + animation si changement
        if (oldEtat != persons[idx].etat) {
          addHistory(idx, oldEtat, persons[idx].etat);
          persons[idx].inTransition = true;
          persons[idx].transitionFrame = 0;
        }

        Serial.printf("[MESH] Enregistre: %s %s slot %d\n",
                      persons[idx].prenom.c_str(), persons[idx].nom.c_str(),
                      idx);

        saveDb(from, persons[idx].prenom, persons[idx].nom);
        String response =
            "{\"type\":\"assigned\",\"slotId\":" + String(idx) + "}";
        mesh.sendSingle(from, response);
      }
    } else if (t == "status") {
      int idx = findSlot(from);
      if (idx != -1) {
        int oldEtat = persons[idx].etat;
        int newEtat = doc["etat"];

        if (oldEtat != newEtat) {
          persons[idx].etat = newEtat;
          persons[idx].dirty = true;
          persons[idx].inTransition = true;
          persons[idx].transitionFrame = 0;
          addHistory(idx, oldEtat, newEtat);
          Serial.printf("[MESH] Status slot %d: %d -> %d\n", idx, oldEtat,
                        newEtat);
        }
        // Mise à jour heartbeat sur changement de status
        persons[idx].lastHeartbeat = millis();
      }
    } else if (t == "heartbeat") {
      // 💓 Traitement du heartbeat
      int slotFromMsg = doc["slotId"];
      if (slotFromMsg >= 0 && slotFromMsg < MAX_SLOTS &&
          persons[slotFromMsg].nodeId == from) {
        persons[slotFromMsg].lastHeartbeat = millis();
        persons[slotFromMsg].ecoMode = doc["eco"] | false;

        // Mise à jour état si différent
        int newEtat = doc["etat"];
        if (persons[slotFromMsg].etat != newEtat) {
          int oldEtat = persons[slotFromMsg].etat;
          persons[slotFromMsg].etat = newEtat;
          persons[slotFromMsg].dirty = true;
          addHistory(slotFromMsg, oldEtat, newEtat);
        }
        Serial.printf("[HEARTBEAT] Slot %d OK (eco: %s)\n", slotFromMsg,
                      persons[slotFromMsg].ecoMode ? "ON" : "OFF");
      }
    }
    xSemaphoreGive(xMutex);
  }
}

void changed() {
  SimpleList<uint32_t> n = mesh.getNodeList();
  Serial.printf("[MESH] Connexions: %d nodes\n", n.size());

  if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
    for (int i = 0; i < MAX_SLOTS; i++) {
      if (persons[i].connecte) {
        bool found = false;
        for (auto id : n)
          if (id == persons[i].nodeId)
            found = true;
        if (!found) {
          Serial.printf("[MESH] Node %u deconnecte\n", persons[i].nodeId);
          addHistory(i, persons[i].etat, -1); // -1 = deconnexion
          persons[i].connecte = false;
          persons[i].dirty = true;
        }
      }
    }
    xSemaphoreGive(xMutex);
  }
}

void taskRun(void *p) {
  Wire.begin();
  Wire.setClock(400000);
  Wire.setTimeOut(1000);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(
      false); // DÉSACTIVER le mode économie d'énergie pour la stabilité WiFi

  // Configuration Mesh - MASTER comme ROOT
  mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT, WIFI_AP_STA,
            6);               // Canal fixe 6
  mesh.setRoot(true);         // IMPORTANT: Ce nœud est la RACINE du mesh
  mesh.setContainsRoot(true); // Le mesh contient un root

  mesh.onReceive(&receivedCallback);
  mesh.onChangedConnections(&changed);
  mesh.onNewConnection([](uint32_t id) {
    Serial.printf("[MASTER] Nouveau noeud connecté: %u\n", id);
    mesh.sendSingle(id, "{\"type\":\"identify\"}");
  });

  Serial.printf("[MASTER] NodeID: %u\n", mesh.getNodeId());
  Serial.println("[MASTER] Mesh initialisé en mode ROOT");

  // Note: Ne PAS appeler WiFi.softAP() ici - cela interfère avec painlessMesh

  // Serve static files from SPIFFS
  // Serve static files from PROGMEM (Embedded)
  server.on("/", HTTP_GET,
            []() { server.send_P(200, "text/html", HTML_INDEX); });
  server.on("/style.css", HTTP_GET,
            []() { server.send_P(200, "text/css", CSS_STYLE); });
  server.on("/script.js", HTTP_GET,
            []() { server.send_P(200, "application/javascript", JS_SCRIPT); });

  server.on("/api/data", HTTP_GET, []() {
    lastActivityTime = millis(); // Reset Screen Saver timer
    StaticJsonDocument<2048> doc;
    JsonArray slots = doc.createNestedArray("slots");
    if (xSemaphoreTake(xMutex, 100)) {
      for (int i = 0; i < MAX_SLOTS; i++) {
        JsonObject s = slots.createNestedObject();
        s["prenom"] = persons[i].prenom;
        s["nom"] = persons[i].nom;
        s["etat"] = persons[i].etat;
        s["connecte"] = persons[i].connecte;
        s["nodeId"] = persons[i].nodeId;
        s["ecoMode"] = persons[i].ecoMode;
        s["lastSeen"] = persons[i].connecte
                            ? (millis() - persons[i].lastHeartbeat) / 1000
                            : 0;
      }

      // Ajout de l'uptime
      doc["uptime"] = millis() - bootTime;

      // Ajout de l'historique global
      JsonArray hist = doc.createNestedArray("history");
      uint8_t count = min((uint8_t)10, globalHistoryCount); // 10 derniers max
      for (uint8_t i = 0; i < count; i++) {
        uint8_t idx = (globalHistoryIdx - 1 - i + GLOBAL_HISTORY_MAX) %
                      GLOBAL_HISTORY_MAX;
        JsonObject h = hist.createNestedObject();
        h["timestamp"] = globalHistory[idx].timestamp;
        h["slot"] = globalHistory[idx].slot;
        h["oldState"] = globalHistory[idx].oldState;
        h["newState"] = globalHistory[idx].newState;
      }

      xSemaphoreGive(xMutex);
    }
    String r;
    serializeJson(doc, r);
    server.send(200, "application/json", r);
  });
  server.begin();

  while (1) {
    // esp_task_wdt_reset(); // Watchdog désactivé
    mesh.update();
    server.handleClient();
    checkScreenSaver(); // Check Screen Saver logic

    // Vérification périodique des écrans (Hot-plug)
    if (millis() - lastScreenCheck > SCREEN_CHECK_MS) {
      checkScreens();
      lastScreenCheck = millis();
    }

    // 💓 Vérification heartbeat timeout (toutes les 10 secondes)
    static unsigned long lastHeartbeatCheck = 0;
    if (millis() - lastHeartbeatCheck > 10000) {
      if (xSemaphoreTake(xMutex, 100)) {
        for (int i = 0; i < MAX_SLOTS; i++) {
          if (persons[i].connecte && persons[i].lastHeartbeat > 0) {
            if (millis() - persons[i].lastHeartbeat > HEARTBEAT_TIMEOUT_MS) {
              Serial.printf("[HEARTBEAT] Timeout slot %d - déconnexion\n", i);
              addHistory(i, persons[i].etat, -1); // -1 = déconnexion
              persons[i].connecte = false;
              persons[i].dirty = true;
              persons[i].lastHeartbeat = 0;
            }
          }
        }
        xSemaphoreGive(xMutex);
      }
      lastHeartbeatCheck = millis();
    }

    static unsigned long l = 0;
    if (millis() - l > 60) {
      if (xSemaphoreTake(xMutex,
                         portMAX_DELAY)) { // Timeout augmenté pour fiabilité
        updateLEDs();
        for (int i = 0; i < MAX_SLOTS; i++)
          if (persons[i].dirty || persons[i].needsScroll ||
              persons[i].inTransition) {
            drawScreen(i);
            persons[i].dirty = false;
          }
        xSemaphoreGive(xMutex);
      }
      l = millis();
    }
    vTaskDelay(1);
  }
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable Brownout Detector
  Serial.begin(115200);
  bootTime = millis(); // Initialisation du temps de démarrage

  // Watchdog DÉSACTIVÉ pour stabilité maximale
  // esp_task_wdt_init(60, true);
  // esp_task_wdt_add(NULL);

  SPIFFS.begin(true);
  xMutex = xSemaphoreCreateMutex();
  strip.begin();
  strip.setBrightness(40);
  strip.show();

  xTaskCreatePinnedToCore(taskRun, "M", 16000, NULL, 1, NULL, 1);
}

void loop() { vTaskDelay(1000); }