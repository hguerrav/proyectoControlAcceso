/**
 * Webhook de registro de eventos -> Google Sheets
 * Hoja de cálculo > Extensiones > Apps Script > pegar este código.
 * Implementar > Aplicación web (Ejecutar como: Yo | Acceso: Cualquier usuario).
 * Tras cada cambio: Administrar implementaciones > Editar > Nueva versión.
 */
const API_TOKEN = 'CAMBIA_ESTE_TOKEN';   // igual que en el ESP32
const SHEET_NAME = 'Eventos';
const HEADERS = ['Fecha/Hora (dispositivo)', 'Recibido (servidor)', 'Dispositivo',
                 'Usuario', 'Evento', 'Detalle', 'Uptime (s)', 'RSSI (dBm)'];

function doGet(e) {
  const p = (e && e.parameter) ? e.parameter : {};   // e es undefined si se ejecuta desde el editor
  if (p.token !== API_TOKEN) return json({ ok: false, error: 'unauthorized' });

  const lock = LockService.getScriptLock();
  lock.waitLock(10000);
  try {
    const ss = SpreadsheetApp.getActiveSpreadsheet();
    let sheet = ss.getSheetByName(SHEET_NAME);
    if (!sheet) {
      sheet = ss.insertSheet(SHEET_NAME);
      sheet.appendRow(HEADERS);
      sheet.setFrozenRows(1);
    }
    // Hora del dispositivo (NTP) si llegó; si no, queda vacía y se usa la del servidor
    const tsDispositivo = p.ts ? new Date(p.ts) : '';
    sheet.appendRow([tsDispositivo, new Date(), p.device || '', p.user || '',
                     p.event || '', p.detail || '', p.uptime || '', p.rssi || '']);
    return json({ ok: true });
  } finally {
    lock.releaseLock();
  }
}

function json(obj) {
  return ContentService.createTextOutput(JSON.stringify(obj)).setMimeType(ContentService.MimeType.JSON);
}

// Prueba manual desde el editor (botón Ejecutar)
function pruebaManual() {
  const r = doGet({ parameter: { token: API_TOKEN, device: 'TEST', user: 'USR-TEST',
    event: 'PRUEBA_MANUAL', ts: new Date().toISOString(), detail: 'desde editor' } });
  Logger.log(r.getContent());
}
