#!/bin/zsh

# ═══════════════════════════════════════════════════════════════════════════════
# SCRIPT DE DÉPLOIEMENT AUTOMATISÉ - DISPOMESH
# ═══════════════════════════════════════════════════════════════════════════════

# Couleurs
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Chemins absolus (adaptés à ton workspace)
BASE_DIR="/Users/flavio/Documents/Cours/ADI 2/Projets/Beta/Display"
MASTER_DIR="$BASE_DIR/master_recepteur"
SLAVE_DIR="$BASE_DIR/slave_emetteur"

echo "${BLUE}
  _____  _                   __  __           _     
 |  __ \(_)                 |  \/  |         | |    
 | |  | |_ ___ _ __   ___   | \  / | ___  ___| |__  
 | |  | | / __| '_ \ / _ \  | |\/| |/ _ \/ __| '_ \ 
 | |__| | \__ \ |_) | (_) | | |  | |  __/\__ \ | | |
 |_____/|_|___/ .__/ \___/  |_|  |_|\___||___/_| |_|
              | |                                   
              |_|                                   
${NC}"
echo "${YELLOW}Script de déploiement v1.0${NC}\n"

# Fonction pour lister les ports
list_ports() {
    echo "${BLUE}Ports série disponibles :${NC}"
    ls /dev/cu.* | grep -v "Bluetooth" | grep -v "iPhone"
    echo ""
}

# 1. DÉPLOIEMENT MASTER
echo "${GREEN}══════════════════════════════════════════════════════════════${NC}"
echo "${GREEN} 1. DÉPLOIEMENT MASTER (Firmware + Site Web)${NC}"
echo "${GREEN}══════════════════════════════════════════════════════════════${NC}"
list_ports

echo -n "Port du MASTER (ex: /dev/cu.usbserial-0001) ou 'skip' : "
read master_port

if [[ "$master_port" != "skip" && -n "$master_port" ]]; then
    echo "\n${YELLOW}[MASTER] Upload du système de fichiers (SPIFFS)...${NC}"
    pio run -d "$MASTER_DIR" -e esp32dev -t uploadfs --upload-port "$master_port"
    
    if [ $? -eq 0 ]; then
        echo "\n${YELLOW}[MASTER] Upload du Firmware...${NC}"
        pio run -d "$MASTER_DIR" -e esp32dev -t upload --upload-port "$master_port"
        
        if [ $? -eq 0 ]; then
            echo "\n${GREEN}✅ MASTER déployé avec succès !${NC}"
        else
            echo "\n${RED}❌ Erreur lors de l'upload du Firmware Master.${NC}"
        fi
    else
        echo "\n${RED}❌ Erreur lors de l'upload SPIFFS. Vérifie que le moniteur série est fermé.${NC}"
    fi
else
    echo "⏭️  Master ignoré."
fi

# 2. DÉPLOIEMENT SLAVES
while true; do
    echo "\n${GREEN}══════════════════════════════════════════════════════════════${NC}"
    echo "${GREEN} 2. DÉPLOIEMENT SLAVE (Firmware uniquement)${NC}"
    echo "${GREEN}══════════════════════════════════════════════════════════════${NC}"
    list_ports

    echo -n "Port du SLAVE (ex: /dev/cu.usbserial-0002) ou 'fin' : "
    read slave_port

    if [[ "$slave_port" == "fin" || -z "$slave_port" ]]; then
        break
    fi

    echo "\n${YELLOW}[SLAVE] Upload du Firmware sur $slave_port...${NC}"
    pio run -d "$SLAVE_DIR" -e esp32dev -t upload --upload-port "$slave_port"
    
    if [ $? -eq 0 ]; then
        echo "\n${GREEN}✅ SLAVE déployé avec succès sur $slave_port !${NC}"
    else
        echo "\n${RED}❌ Erreur lors de l'upload Slave.${NC}"
    fi
done

echo "\n${BLUE}🚀 Déploiement terminé !${NC}"
