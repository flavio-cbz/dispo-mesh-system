/*
 * ═══════════════════════════════════════════════════════════════════════
 * ESP32 SLAVE ÉMETTEUR - DispoMesh v5.6 FINAL
 * ═══════════════════════════════════════════════════════════════════════
 * - BROWNOUT : DÉSACTIVÉ (Stabilité)
 * - WIFI : MASQUÉ (Variable HIDE_NETWORKS = 1)
 * - UI : Badge iOS + Icônes
 * - RESEAU : Dual Stack (Mesh + Config AP) sur même canal
 * ═══════════════════════════════════════════════════════════════════════
 */

#include <painlessMesh.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SPIFFS.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <U8g2_for_Adafruit_GFX.h>

// MODULES CRITIQUES (Anti-Brownout)
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// MODULES ÉCONOMIE D'ÉNERGIE
#include "esp_wifi.h"
#include <esp_pm.h>
#include <esp_task_wdt.h>

// 🔒 CONFIGURATION SÉCURITÉ
#define HIDE_NETWORKS   1  // 1 = Masqué, 0 = Visible

// ⚡ CONFIGURATION ÉCONOMIE D'ÉNERGIE
#define ECO_MODE_DELAY_MS     300000  // 5 minutes en ABSENT avant mode éco
#define HEARTBEAT_INTERVAL_MS 30000   // Heartbeat toutes les 30 secondes
#define LED_BRIGHTNESS_NORMAL 40
#define LED_BRIGHTNESS_ECO    8
#define CPU_FREQ_NORMAL       240     // MHz
#define CPU_FREQ_ECO          80      // MHz

// CONFIGURATION MATÉRIEL
#define BTN_PIN       32
#define PIN_NEOPIXEL  27
#define NUM_PIXELS    1

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_ADDR     0x3C

Adafruit_NeoPixel pixel(NUM_PIXELS, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
U8G2_FOR_ADAFRUIT_GFX u8g2;

const uint32_t COLOR_DISPO  = pixel.Color(0, 255, 0);
const uint32_t COLOR_BUSY   = pixel.Color(255, 0, 0);
const uint32_t COLOR_AWAY   = pixel.Color(255, 140, 0);

// VARIABLES GLOBALES
#define MESH_PREFIX     "DispoMesh"
#define MESH_PASSWORD   "meshpass2025"
#define MESH_PORT       5555

SemaphoreHandle_t xMutex;
char apSSID[32];
String monPrenom = "Nouveau";
String monNom = "Poste";
int etatActuel = 2;
int slotId = -1;
volatile bool configSauvegardee = false;
bool meshInitialized = false;
volatile bool needsRegister = false; // Flag pour re-registration différé

// ⚡ Variables mode économie d'énergie
unsigned long lastStateChange = 0;    // Timestamp du dernier changement d'état
unsigned long lastHeartbeat = 0;      // Timestamp du dernier heartbeat
bool ecoModeActive = false;           // Mode économie activé
uint8_t currentBrightness = LED_BRIGHTNESS_NORMAL;

Scheduler userScheduler;
painlessMesh mesh;
WebServer server(80);
DNSServer dnsServer;

// ═══════════════════════════════════════════════════════════════════════
// UI OLED
// ═══════════════════════════════════════════════════════════════════════
const unsigned char icon_wifi[] PROGMEM = { 0x3C, 0x42, 0x81, 0x3C, 0x42, 0x18, 0x00, 0x18 };

void updateLocalDisplay() {
  display.clearDisplay();
  u8g2.setForegroundColor(SSD1306_WHITE);
  
  // Header
  display.drawLine(0, 8, SCREEN_WIDTH, 8, SSD1306_WHITE);
  if (slotId != -1) display.drawBitmap(2, 0, icon_wifi, 8, 8, SSD1306_WHITE);
  
  u8g2.setFont(u8g2_font_4x6_tr);
  String idTxt = "ID:" + String(mesh.getNodeId() % 100);
  u8g2.drawUTF8(SCREEN_WIDTH - u8g2.getUTF8Width(idTxt.c_str()) - 2, 6, idTxt.c_str());

  // Nom
  u8g2.setFont(u8g2_font_helvB08_tf);
  String fullName = monPrenom + " " + monNom;
  int xName = (SCREEN_WIDTH - u8g2.getUTF8Width(fullName.c_str())) / 2;
  if (xName < 0) xName = 0;
  u8g2.drawUTF8(xName, 20, fullName.c_str());

  // Badge Statut
  String statusTxt;
  switch(etatActuel) {
    case 0: statusTxt = "DISPONIBLE"; break;
    case 1: statusTxt = "OCCUPE"; break;
    default: statusTxt = "ABSENT"; break;
  }
  
  u8g2.setFont(u8g2_font_5x7_tf);
  int wStatus = u8g2.getUTF8Width(statusTxt.c_str()) + 8;
  int xStatus = (SCREEN_WIDTH - wStatus) / 2;
  
  display.fillRoundRect(xStatus, 23, wStatus, 9, 4, SSD1306_WHITE);
  u8g2.setForegroundColor(SSD1306_BLACK);
  u8g2.setBackgroundColor(SSD1306_WHITE);
  u8g2.drawUTF8(xStatus + 4, 30, statusTxt.c_str());
  u8g2.setForegroundColor(SSD1306_WHITE);
  u8g2.setBackgroundColor(SSD1306_BLACK);

  display.display();
}

void updateLEDs() {
  pixel.clear();
  if (slotId == -1) {
    // Clignotement rouge quand non connecté
    if ((millis() / 500) % 2) {
      pixel.setPixelColor(0, COLOR_BUSY);
    } else {
      pixel.setPixelColor(0, 0); // OFF pour le clignotement
    }
  } else {
    switch (etatActuel) {
      case 0: pixel.setPixelColor(0, COLOR_DISPO); break;
      case 1: pixel.setPixelColor(0, COLOR_BUSY); break;
      default: pixel.setPixelColor(0, COLOR_AWAY); break;
    }
  }
  pixel.show();
}

// ═══════════════════════════════════════════════════════════════════════
// SYSTÈME
// ═══════════════════════════════════════════════════════════════════════
void initSSID() {
  uint8_t mac[6]; WiFi.macAddress(mac);
  snprintf(apSSID, sizeof(apSSID), "Config_Poste_%02X%02X", mac[4], mac[5]);
}

void sauvegarderNom(String p, String n) {
  File f = SPIFFS.open("/config.json", "w");
  if (f) {
    StaticJsonDocument<256> doc; doc["p"] = p; doc["n"] = n;
    serializeJson(doc, f); f.close();
  }
  updateLocalDisplay();
}

void chargerNom() {
  if (SPIFFS.exists("/config.json")) {
    File f = SPIFFS.open("/config.json", "r");
    if (f) {
      StaticJsonDocument<256> doc;
      if (deserializeJson(doc, f) == DeserializationError::Ok) {
        monPrenom = doc["p"] | String(apSSID);
        monNom = doc["n"] | "";
      }
      f.close();
    }
  } else { monPrenom = String(apSSID); }
}

// ═══════════════════════════════════════════════════════════════════════
// WEB SERVER
// ═══════════════════════════════════════════════════════════════════════
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Configuration Poste</title>
    <style>
        body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; background: #1a1a2e; color: #fff; display: flex; flex-direction: column; align-items: center; min-height: 100vh; margin: 0; padding: 20px; }
        .card { background: rgba(255, 255, 255, 0.05); padding: 30px; border-radius: 20px; width: 100%; max-width: 350px; backdrop-filter: blur(10px); border: 1px solid rgba(255, 255, 255, 0.1); box-shadow: 0 8px 32px 0 rgba(0, 0, 0, 0.37); }
        h2 { margin-top: 0; font-weight: 300; letter-spacing: 1px; text-align: center; margin-bottom: 20px; }
        .input-group { margin-bottom: 15px; }
        label { display: block; margin-bottom: 5px; font-size: 0.8rem; color: rgba(255, 255, 255, 0.6); }
        input { width: 100%; padding: 12px; background: rgba(0, 0, 0, 0.2); border: 1px solid rgba(255, 255, 255, 0.1); border-radius: 8px; color: #fff; font-size: 1rem; outline: none; transition: border-color 0.3s; box-sizing: border-box; }
        input:focus { border-color: #4ade80; }
        button { width: 100%; padding: 14px; background: linear-gradient(135deg, #4ade80 0%, #22c55e 100%); color: #000; border: none; border-radius: 8px; font-weight: 600; font-size: 1rem; cursor: pointer; transition: transform 0.2s, opacity 0.2s; margin-top: 10px; }
        button:active { transform: scale(0.98); }
        .status-box { margin-top: 25px; padding-top: 20px; border-top: 1px solid rgba(255, 255, 255, 0.1); }
        .status-item { display: flex; justify-content: space-between; margin-bottom: 8px; font-size: 0.9rem; }
        .status-val { font-weight: 500; }
        .connected { color: #4ade80; }
        .disconnected { color: #f87171; }
        .qr-panel { background: #fff; color: #000; padding: 15px; border-radius: 10px; margin-top: 20px; text-align: center; }
        .qr-title { font-size: 0.8rem; font-weight: 700; margin-bottom: 5px; opacity: 0.7; }
        .qr-ssid { font-family: monospace; font-size: 1.1rem; font-weight: bold; }
        .qr-pass { font-family: monospace; font-size: 0.9rem; color: #555; }
    </style>
</head>
<body>
    <div class="card">
        <h2>CONFIG POSTE</h2>
        <form id="f">
            <div class="input-group">
                <label>Prénom</label>
                <input name="prenom" placeholder="Ex: Jean" required>
            </div>
            <div class="input-group">
                <label>Nom</label>
                <input name="nom" placeholder="Ex: Dupont" required>
            </div>
            <button type="submit">ENREGISTRER</button>
        </form>

        <div class="status-box">
            <div class="status-item">
                <span>État Mesh</span>
                <span class="status-val" id="mesh-status">...</span>
            </div>
            <div class="status-item">
                <span>Slot ID</span>
                <span class="status-val" id="slot-id">...</span>
            </div>
        </div>

        <div class="qr-panel">
            <div class="qr-title">WIFI CONFIGURATION</div>
            <div class="qr-ssid" id="ssid-disp">Config_Poste_XX</div>
            <div class="qr-pass">Pass: config2025</div>
        </div>
    </div>

    <script>
        fetch('/api/get').then(r => r.json()).then(d => {
            document.querySelector('[name="prenom"]').value = d.prenom;
            document.querySelector('[name="nom"]').value = d.nom;
            document.getElementById('ssid-disp').textContent = d.ssid || 'Config_Poste_XX';
            
            const meshSt = document.getElementById('mesh-status');
            if(d.meshConnected) {
                meshSt.textContent = "CONNECTÉ";
                meshSt.className = "status-val connected";
            } else {
                meshSt.textContent = "DÉCONNECTÉ";
                meshSt.className = "status-val disconnected";
            }
            
            document.getElementById('slot-id').textContent = (d.slotId !== -1) ? ("POSTE " + (d.slotId + 1)) : "NON ASSIGNÉ";
        });
        
        document.getElementById('f').onsubmit = e => {
            e.preventDefault();
            const btn = document.querySelector('button');
            const orig = btn.innerText;
            btn.innerText = 'SAUVEGARDE...';
            fetch('/api/save', { method: 'POST', body: new URLSearchParams(new FormData(e.target)) })
                .then(() => { 
                    btn.innerText = 'OK !'; 
                    setTimeout(() => btn.innerText = orig, 2000);
                })
                .catch(() => btn.innerText = 'ERREUR');
        };
    </script>
</body>
</html>
)rawliteral";

void setupWebServer() {
  server.on("/", [](){ server.send_P(200, "text/html", HTML_PAGE); });
  server.on("/api/get", [](){ 
    StaticJsonDocument<256> d; 
    d["prenom"]=monPrenom; 
    d["nom"]=monNom; 
    d["ssid"]=String(apSSID);
    d["meshConnected"]=!mesh.getNodeList().empty();
    d["slotId"]=slotId;
    String s; serializeJson(d,s); server.send(200,"application/json",s); 
  });
  server.on("/api/save", [](){
    String p=server.arg("prenom"); String n=server.arg("nom"); p.trim(); n.trim();
    if(p.length()>0) { 
      if(xSemaphoreTake(xMutex,100)){ monPrenom=p; monNom=n; configSauvegardee=true; xSemaphoreGive(xMutex); }
      sauvegarderNom(p,n); server.send(200);
    } else server.send(400);
  });
  
  // Gestionnaire pour les pages non trouvées (Captive Portal)
  server.onNotFound([]() {
    server.send_P(200, "text/html", HTML_PAGE);
  });
  
  server.begin();
}

void sendRegister() {
  StaticJsonDocument<256> doc; doc["type"]="register"; 
  
  String p, n;
  // Tentative de prise de mutex avec timeout plus long (500ms)
  if(xSemaphoreTake(xMutex, 500)){ 
    p = monPrenom; 
    n = monNom; 
    xSemaphoreGive(xMutex); 
  } else {
    Serial.println("[SLAVE] ERREUR: Mutex occupé, envoi annulé pour éviter 'null'");
    return;
  }

  doc["prenom"] = p; 
  doc["nom"] = n; 
  doc["etat"] = etatActuel; 
  
  String msg; serializeJson(doc,msg); mesh.sendBroadcast(msg);
}

// 💓 Envoi heartbeat périodique au master
void sendHeartbeat() {
  if (slotId == -1) return; // Pas de heartbeat si non assigné
  
  StaticJsonDocument<128> doc;
  doc["type"] = "heartbeat";
  doc["slotId"] = slotId;
  doc["etat"] = etatActuel;
  doc["eco"] = ecoModeActive;
  
  String msg;
  serializeJson(doc, msg);
  mesh.sendBroadcast(msg);
  Serial.println("[HEARTBEAT] Envoyé");
}

// ⚡ Gestion du mode économie d'énergie
void checkEcoMode() {
  bool shouldBeEco = (etatActuel == 2) && 
                     (millis() - lastStateChange > ECO_MODE_DELAY_MS) &&
                     (slotId != -1); // Seulement si connecté
  
  if (shouldBeEco && !ecoModeActive) {
    // Activation mode éco
    ecoModeActive = true;
    Serial.println("[ECO] Mode économie ACTIVÉ");
    
    // Réduire luminosité LED
    currentBrightness = LED_BRIGHTNESS_ECO;
    pixel.setBrightness(currentBrightness);
    
    // Réduire fréquence CPU
    setCpuFrequencyMhz(CPU_FREQ_ECO);
    Serial.printf("[ECO] CPU: %d MHz\n", getCpuFrequencyMhz());
    
    // Activer WiFi power save
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    
  } else if (!shouldBeEco && ecoModeActive) {
    // Désactivation mode éco
    ecoModeActive = false;
    Serial.println("[ECO] Mode économie DÉSACTIVÉ");
    
    // Restaurer luminosité LED
    currentBrightness = LED_BRIGHTNESS_NORMAL;
    pixel.setBrightness(currentBrightness);
    
    // Restaurer fréquence CPU
    setCpuFrequencyMhz(CPU_FREQ_NORMAL);
    Serial.printf("[ECO] CPU: %d MHz\n", getCpuFrequencyMhz());
    
    // Désactiver WiFi power save pour réactivité
    esp_wifi_set_ps(WIFI_PS_NONE);
  }
}

// ═══════════════════════════════════════════════════════════════════════
// TÂCHES PRINCIPALES
// ═══════════════════════════════════════════════════════════════════════
void receivedCallback(uint32_t from, String &msg) {
  Serial.printf("[SLAVE] Message reçu de %u: %s\n", from, msg.c_str());
  
  StaticJsonDocument<512> doc; 
  if(deserializeJson(doc, msg) != DeserializationError::Ok) {
    Serial.println("[SLAVE] Erreur parsing JSON");
    return;
  }
  
  String t = doc["type"];
  
  if (t == "assigned") { 
    slotId = doc["slotId"]; 
    Serial.printf("[SLAVE] Slot assigné: %d\n", slotId);
    updateLEDs(); 
    updateLocalDisplay(); 
  }
  else if (t == "identify") {
    Serial.println("[SLAVE] Demande d'identification reçue");
    sendRegister();
  }
  else if (t == "update_info") {
    String p = doc["prenom"].as<String>();
    String n = doc["nom"].as<String>();
    
    if(xSemaphoreTake(xMutex, 500)){ 
      monPrenom = p; 
      monNom = n; 
      xSemaphoreGive(xMutex); 
      
      sauvegarderNom(p, n); 
      sendRegister();
    }
  }
}

void changedConnectionCallback() {
  auto nodes = mesh.getNodeList();
  Serial.printf("[SLAVE] Connexions changées! Nodes: %d\n", nodes.size());
  
  // Vérification plus robuste: utiliser isConnected() si disponible
  // ou vérifier si le mesh est toujours actif
  bool isConnected = mesh.isConnected(mesh.getNodeId()) || !nodes.empty();
  
  if (!isConnected && slotId != -1) { 
    Serial.println("[SLAVE] Déconnecté du mesh - Reset slotId");
    slotId = -1; 
    updateLEDs(); 
    updateLocalDisplay(); 
  } else if (isConnected || !nodes.empty()) {
    Serial.println("[SLAVE] Connecté au mesh - Envoi register différé");
    // Différer l'envoi pour éviter les problèmes dans le callback
    needsRegister = true; // Sera traité par la tâche principale
  }
}

void taskMesh(void *p) {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false); // STABILITÉ: Désactiver économie énergie WiFi
  
  // Configuration Mesh - SLAVE cherche le ROOT
  mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT, WIFI_AP_STA, 6); // Canal fixe 6
  mesh.setContainsRoot(true);   // IMPORTANT: Indique qu'il doit chercher un ROOT
  
  mesh.onReceive(&receivedCallback);
  mesh.onChangedConnections(&changedConnectionCallback);
  
  Serial.printf("[SLAVE] NodeID: %u\n", mesh.getNodeId());
  Serial.println("[SLAVE] Mesh initialisé - Recherche du ROOT...");
  
  // Note: Ne PAS appeler WiFi.softAP(MESH_PREFIX...) ici - cela interfère avec painlessMesh
  
  meshInitialized = true;
  
  while (true) {
    // esp_task_wdt_reset(); // Watchdog désactivé
    mesh.update();
    static unsigned long lbp = 0; static bool lbs = HIGH;
    bool s = digitalRead(BTN_PIN);
    if (s == LOW && lbs == HIGH && (millis() - lbp > 200)) {
      etatActuel = (etatActuel + 1) % 3; 
      lbp = millis();
      lastStateChange = millis(); // Tracker le changement d'état pour mode éco
      updateLEDs(); updateLocalDisplay();
      StaticJsonDocument<64> d; d["type"]="status"; d["etat"]=etatActuel; String m; serializeJson(d,m); mesh.sendBroadcast(m);
    }
    lbs = s;
    updateLEDs();
    
    bool triggerRegister = false;
    if (xSemaphoreTake(xMutex, 5)) {
      if (configSauvegardee) { configSauvegardee = false; triggerRegister = true; }
      if (needsRegister) { needsRegister = false; triggerRegister = true; } // Re-registration différée
      xSemaphoreGive(xMutex);
    }
    if (triggerRegister) sendRegister();
    
    // Re-registration périodique si non assigné
    static unsigned long lastRegisterAttempt = 0;
    if (slotId == -1 && !mesh.getNodeList().empty() && millis() - lastRegisterAttempt > 5000) {
      sendRegister();
      lastRegisterAttempt = millis();
    }
    
    // 💓 Heartbeat périodique
    if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL_MS) {
      sendHeartbeat();
      lastHeartbeat = millis();
    }
    
    // ⚡ Vérification mode économie
    checkEcoMode();
    
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void taskUI(void *p) {
  while(!meshInitialized) delay(100);
  
  // Attendre que le mesh soit stabilisé (2 secondes)
  delay(2000);
  
  // Démarrage AP Config sur canal 6 (même que mesh) - NE PAS changer dynamiquement
  // Note: L'AP de config utilise un SSID différent donc pas d'interférence
  WiFi.softAP(apSSID, "config2025", 6, HIDE_NETWORKS);
  Serial.printf("[SLAVE] AP Config démarré: %s\n", apSSID);
  Serial.printf("[SLAVE] IP: %s\n", WiFi.softAPIP().toString().c_str());
  
  dnsServer.start(53, "*", WiFi.softAPIP());
  setupWebServer();
  
  unsigned long lastDebug = 0;
  
  while(1) { 
    dnsServer.processNextRequest(); 
    server.handleClient(); 
    
    // Debug périodique
    if(millis() - lastDebug > 10000) {
      Serial.printf("[SLAVE] Connecté: %s, SlotID: %d, Nodes: %d\n", 
        slotId != -1 ? "OUI" : "NON", slotId, mesh.getNodeList().size());
      lastDebug = millis();
    }
    
    delay(10); 
  }
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable Brownout
  
  // Watchdog DÉSACTIVÉ
  // esp_task_wdt_init(60, true);
  // esp_task_wdt_add(NULL);
  
  Serial.begin(115200);
  initSSID(); SPIFFS.begin(true); xMutex = xSemaphoreCreateMutex();
  chargerNom();
  
  // Initialiser le tracker de changement d'état
  lastStateChange = millis();
  
  pinMode(BTN_PIN, INPUT_PULLUP);
  pixel.begin(); pixel.setBrightness(LED_BRIGHTNESS_NORMAL); pixel.show();
  Wire.begin(); 
  if(display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) { 
    u8g2.begin(display); 
    updateLocalDisplay(); 
    Serial.println("[SLAVE] Ecran OLED initialisé");
  } else {
    Serial.println("[SLAVE] ERREUR: Ecran OLED non detecté!");
  }
  
  Serial.printf("[SLAVE] CPU: %d MHz, Mode éco après %d min\n", 
    getCpuFrequencyMhz(), ECO_MODE_DELAY_MS / 60000);

  xTaskCreatePinnedToCore(taskMesh, "Mesh", 10000, NULL, 1, NULL, 1);
  delay(500);
  xTaskCreatePinnedToCore(taskUI, "UI", 6000, NULL, 1, NULL, 0);
}

void loop() { vTaskDelay(1000); }