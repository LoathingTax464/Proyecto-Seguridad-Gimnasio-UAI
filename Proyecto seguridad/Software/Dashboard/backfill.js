// backfill.js 
// -------------------------------------------------------------------
// Exporta todos los datos del Realtime Database a BigQuery
// SIN USAR STREAMING BUFFER (usa carga por archivos)
// Sobrescribe las tablas en cada ejecución
// -------------------------------------------------------------------

const admin = require('firebase-admin');
const { BigQuery } = require('@google-cloud/bigquery');
const fs = require('fs');
const path = require('path');

// Inicializa Firebase
admin.initializeApp({
  credential: admin.credential.cert(require('./serviceAccountKey.json')),
  databaseURL: 'https://proyecto-tdi-default-rtdb.firebaseio.com'
});

// 🔹 Ruta absoluta hacia la clave JSON
const keyPath = path.join(__dirname, 'serviceAccountKey.json');

// 🔹 Inicializa BigQuery
const bq = new BigQuery({
  projectId: 'proyecto-tdi',
  keyFilename: keyPath
});

const db = admin.database();
const DATASET = 'rtdb_export';

// -------------------------------------------------------------------
// Función auxiliar: reemplaza completamente una tabla
// -------------------------------------------------------------------

async function replaceTable(table, rows) {
  const dataset = bq.dataset(DATASET);
  const tableRef = dataset.table(table);

  // Si no hay filas nuevas, TRUNCAMOS la tabla (si existe) y salimos
  if (!rows || rows.length === 0) {
    try {
      const fullTableId = `\`${bq.projectId}.${DATASET}.${table}\``;
      console.log(`⚠️ Tabla ${table} sin datos, se truncará en BigQuery.`);
      await bq.query(`TRUNCATE TABLE ${fullTableId}`);
      console.log(`✅ Tabla ${table} truncada (0 filas).`);
    } catch (err) {
      console.error(`⚠️ No se pudo truncar la tabla ${table}:`, err);
    }
    return;
  }

  const tmpFile = path.join(__dirname, `${table}_tmp.json`);
  try {
    // Guardar datos en un archivo temporal (NDJSON)
    fs.writeFileSync(tmpFile, rows.map(r => JSON.stringify(r)).join('\n'));

    // Borrar tabla anterior (si existe)
    try {
      await tableRef.delete();
      console.log(`🧹 Tabla ${table} eliminada antes de recrear.`);
    } catch {
      console.log(`ℹ️ Tabla ${table} no existía, se creará nueva.`);
    }

    // Crear tabla vacía y luego cargar datos desde el archivo
    await dataset.createTable(table);
    await tableRef.load(tmpFile, {
      sourceFormat: 'NEWLINE_DELIMITED_JSON',
      autodetect: true,
      writeDisposition: 'WRITE_TRUNCATE' // sobrescribe
    });

    console.log(`✅ Cargados ${rows.length} registros en la tabla ${table}`);
  } catch (err) {
    console.error(`⚠️ Error en la tabla ${table}:`, err);
  } finally {
    if (fs.existsSync(tmpFile)) fs.unlinkSync(tmpFile);
  }
}

// -------------------------------------------------------------------
// Convierte timestamps (segundos o ms) a ISO
// -------------------------------------------------------------------
function secondsOrMsToIso(value) {
  if (value === null || value === undefined) return null;
  const n = Number(value);
  if (!Number.isFinite(n)) return null;
  // Heurística simple: < 1e12 => segundos, sino milisegundos
  const ms = n < 1e12 ? n * 1000 : n;
  try {
    const d = new Date(ms);
    if (isNaN(d.getTime())) return null;
    return d.toISOString();
  } catch {
    return null;
  }
}

// -------------------------------------------------------------------
// Parsea "YYYY-MM-DD HH:mm" a ISO
// -------------------------------------------------------------------
function parseDateTimeLocal(str) {
  if (!str) return null;
  try {
    // Ej: "2025-11-22 9:40" -> "2025-11-22T9:40"
    const isoLike = str.replace(' ', 'T');
    const d = new Date(isoLike);
    if (isNaN(d.getTime())) return null;
    return d.toISOString();
  } catch {
    return null;
  }
}

// -------------------------------------------------------------------
// Función principal
// -------------------------------------------------------------------
async function backfill() {
  console.log('⏳ Conectando a Firebase...');
  const snap = await db.ref('/').get();
  const root = snap.val() || {};
  console.log('✅ Datos descargados desde Realtime Database');
  console.log('⏳ Subiendo datos a BigQuery...');

  // ------------------- USERS -------------------
  const users = root.users || {};
  const usersRows = [];
  const rfidRows = [];

  for (const user_id of Object.keys(users)) {
    const u = users[user_id];
    usersRows.push({
      user_id,
      fullName: u.fullName || null,
      name: u.name || null,
      surname: u.surname || null,
      rfid: u.rfid || null,
      createdAt: secondsOrMsToIso(u.createdAt)
    });
    if (u.rfid) rfidRows.push({ rfid: u.rfid, user_id });
  }

  // ------------------- RFID INDEX -------------------
  const rfidIndex = root.RFIDIndex || {};
  for (const rfid of Object.keys(rfidIndex)) {
    rfidRows.push({ rfid, user_id: rfidIndex[rfid] });
  }

  // ------------------- ACTIVIDADES -------------------
  const actividades = root.actividades || [];
  const actRows = [];
  actividades.forEach((a, idx) => {
    if (!a) return; // saltar el null inicial
    actRows.push({
      actividad_id: idx, // mantiene el índice real del array (coincide con reservas)
      deporte: a.deporte || null,
      tipo: a.tipo || null,
      profesor: a.profesor || null,
      cupo: a.cupo ?? null,
      reservados: a.reservados ?? null,
      horaInicio: parseDateTimeLocal(a.horaInicio),
      horaFin: parseDateTimeLocal(a.horaFin)
    });
  });

  // ------------------- ERRORES SISTEMA -------------------
  const errores = root.erroresSistema || {};
  const errRows = Object.keys(errores).map(ts => {
    const e = errores[ts] || {};
    return {
      ts: Number(ts),
      fecha: e.fecha || null,
      hora: e.hora || null,
      error: e.error || null,
      explicacion: e.explicacion || null
    };
  });

  // ------------------- ERRORES SISTEMA CONTADOR -------------------
  const erroresCont = root.erroresSistemaContador || {};
  const errCountRows = Object.keys(erroresCont).map(tipo => ({
    tipoError: tipo,
    contador: erroresCont[tipo]
  }));

  // ------------------- HISTÓRICO ACCESOS -------------------
  const hist = root.historicoAccesos || {};
  const histRows = [];
  for (const uid of Object.keys(hist)) {
    for (const ts of Object.keys(hist[uid])) {
      const h = hist[uid][ts];
      histRows.push({
        user_id: uid,
        ts: Number(ts),
        fecha: h.fecha || null,
        hora: h.hora || null,
        motivo: h.motivo || null,
        resultado: h.resultado || null,
        tipoActividad: h.tipoActividad || null
      });
    }
  }

  // ------------------- RESERVAS -------------------
  const reservas = root.reservas || {};
  const resRows = [];
  for (const uid of Object.keys(reservas)) {
    for (const actividad_id of Object.keys(reservas[uid])) {
      const r = reservas[uid][actividad_id];
      resRows.push({
        user_id: uid,
        actividad_id: Number(actividad_id),
        intentos: r.intentos ?? null,
        timestamp: r.timestamp ?? null,
        timestampIso: secondsOrMsToIso(r.timestamp),
        ultimoIngreso: r.ultimoIngreso ?? null,
        ultimoIngresoIso: secondsOrMsToIso(r.ultimoIngreso)
      });
    }
  }

  // ------------------- Subida a BigQuery -------------------
  await replaceTable('users', usersRows);
  await replaceTable('rfid_index', rfidRows);
  await replaceTable('actividades', actRows);
  await replaceTable('errores_sistema', errRows);
  await replaceTable('errores_sistema_contador', errCountRows);
  await replaceTable('historico_accesos', histRows);
  await replaceTable('reservas', resRows);

  console.log('🎉 Backfill completo. Tablas sobrescritas en BigQuery (sin streaming buffer).');
}

// Ejecutar
backfill().catch(e => {
  console.error('💥 Error general:', e);
  process.exit(1);
});
