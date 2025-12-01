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