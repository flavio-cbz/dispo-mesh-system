#!/bin/zsh

# ═══════════════════════════════════════════════════════════════════════════════
# SCRIPT DE DÉPLOIEMENT AUTOMATISÉ - DISPOMESH
# ═══════════════════════════════════════════════════════════════════════════════

# Couleurs
GREEN=$'\033[0;32m'
BLUE=$'\033[0;34m'
YELLOW=$'\033[0;33m'
RED=$'\033[0;31m'
NC=$'\033[0m' # No Color

# Chemins absolus (adaptés à ton workspace)
BASE_DIR="/Users/flavio/Documents/Cours/ADI 2/Projets/Beta/Display"
MASTER_DIR="$BASE_DIR/master_recepteur"
SLAVE_DIR="$BASE_DIR/slave_emetteur"

# Fonction de sélection interactive
select_option() {
    local prompt="$1"
    shift
    local choices=("$@")
    local selected=1
    local num_choices=${#choices[@]}
    
    # Sauvegarder le curseur et le cacher (sur stderr)
    tput sc >&2
    tput civis >&2
    
    echo "$prompt" >&2
    # Réserver l'espace
    for ((i=1; i<=num_choices; i++)); do
        echo "" >&2
    done
    
    while true; do
        # Remonter
        tput cuu "$num_choices" >&2
        
        for ((i=1; i<=num_choices; i++)); do
            tput el >&2 # Effacer la ligne
            if [ $i -eq $selected ]; then
                echo "${GREEN}➜ ${choices[$i]}${NC}" >&2
            else
                echo "  ${choices[$i]}" >&2
            fi
        done
        
        read -s -k 1 key
        if [[ $key == $'\e' ]]; then
            read -s -k 2 key
            if [[ $key == "[A" ]]; then # Haut
                ((selected--))
                [ $selected -lt 1 ] && selected=$num_choices
            elif [[ $key == "[B" ]]; then # Bas
                ((selected++))
                [ $selected -gt $num_choices ] && selected=1
            fi
        elif [[ $key == $'\n' || $key == $'\r' ]]; then # Entrée
            break
        fi
    done
    
    tput cnorm >&2
    echo "${choices[$selected]}"
}

# Force auto-reset for ESP32
export ESPTOOL_BEFORE=default_reset
export ESPTOOL_AFTER=hard_reset

run_pio_command() {
    local message="$1"
    shift
    echo "${message}"
    "$@"
    local rc=$?
    if [ $rc -ne 0 ]; then
        echo "\n${RED}⛔ La commande a échoué.${NC}"
        # On ne demande plus le boot manuel, on laisse l'erreur remonter
        # print_boot_instructions
        # "$@"
        # rc=$?
    fi
    return $rc
}

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
echo "${YELLOW}Script de déploiement v1.0 (Interactive Mode)${NC}\n"

# 1. DÉPLOIEMENT MASTER
echo "${GREEN}══════════════════════════════════════════════════════════════${NC}"
echo "${GREEN} 1. DÉPLOIEMENT MASTER (Firmware + Site Web)${NC}"
echo "${GREEN}══════════════════════════════════════════════════════════════${NC}"

# Récupérer les ports
ports=($(ls /dev/cu.* 2>/dev/null | grep -v "Bluetooth" | grep -v "iPhone"))
if [ ${#ports[@]} -eq 0 ]; then
    ports=("Aucun port détecté")
fi
menu_choices=("${ports[@]}" "Skip")

echo "Sélectionnez le port du MASTER :"
master_port=$(select_option "Utilisez les flèches ↑/↓ et Entrée :" "${menu_choices[@]}")

if [[ "$master_port" != "Skip" && "$master_port" != "Aucun port détecté" ]]; then
    echo "\n${YELLOW}[MASTER] Sélectionné : $master_port${NC}"
    
    echo "\n${BLUE}ℹ️  Le Master nécessite 1 étape : Firmware (Code)${NC}"
    
    # if run_pio_command "${YELLOW}[MASTER] Étape 1/2 : Upload du système de fichiers (SPIFFS)...${NC}" \
    #     pio run -d "$MASTER_DIR" -e esp32dev -t uploadfs --upload-port "$master_port"; then

    if run_pio_command "\n${YELLOW}[MASTER] Upload du Firmware (Code)...${NC}" \
        pio run -d "$MASTER_DIR" -e esp32dev -t upload --upload-port "$master_port"; then
        echo "\n${GREEN}✅ MASTER déployé avec succès !${NC}"
    else
        echo "\n${RED}❌ Erreur lors de l'upload du Firmware Master.${NC}"
    fi

    # else
    #     echo "\n${RED}❌ Erreur lors de l'upload SPIFFS. Vérifie que le moniteur série est fermé.${NC}"
    # fi
else
    echo "⏭️  Master ignoré."
fi

# 2. DÉPLOIEMENT SLAVES
while true; do
    echo "\n${GREEN}══════════════════════════════════════════════════════════════${NC}"
    echo "${GREEN} 2. DÉPLOIEMENT SLAVE (Firmware uniquement)${NC}"
    echo "${GREEN}══════════════════════════════════════════════════════════════${NC}"

    ports=($(ls /dev/cu.* 2>/dev/null | grep -v "Bluetooth" | grep -v "iPhone"))
    if [ ${#ports[@]} -eq 0 ]; then
        ports=("Aucun port détecté")
    fi
    menu_choices=("${ports[@]}" "Fin")

    echo "Sélectionnez le port du SLAVE :"
    slave_port=$(select_option "Utilisez les flèches ↑/↓ et Entrée :" "${menu_choices[@]}")

    if [[ "$slave_port" == "Fin" || "$slave_port" == "Aucun port détecté" ]]; then
        break
    fi

    echo "\n${YELLOW}[SLAVE] Sélectionné : $slave_port${NC}"
    if run_pio_command "${YELLOW}[SLAVE] Upload du Firmware sur $slave_port...${NC}" \
        pio run -d "$SLAVE_DIR" -e esp32dev -t upload --upload-port "$slave_port"; then
        echo "\n${GREEN}✅ SLAVE déployé avec succès sur $slave_port !${NC}"
    else
        echo "\n${RED}❌ Erreur lors de l'upload Slave.${NC}"
    fi
done

echo "\n${BLUE}🚀 Déploiement terminé !${NC}"
