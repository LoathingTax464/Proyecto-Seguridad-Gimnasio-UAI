// index.js
// -------------------------------------------------------------------
// Cloud Function que ejecuta el backfill de Firebase → BigQuery
// usando carga por archivos (sin streaming buffer)
// -------------------------------------------------------------------

const { spawn } = require('child_process');
const path = require('path');

exports.syncFirebaseToBigQuery = async (req, res) => {
  try {
    console.log('🚀 Iniciando ejecución del backfill...');
    const scriptPath = path.join(__dirname, 'backfill.js');

    // Ejecutar el script como proceso hijo
    const process = spawn('node', [scriptPath], { stdio: 'inherit' });

    process.on('close', (code) => {
      if (code === 0) {
        console.log('✅ Backfill completado con éxito.');
        res.status(200).send('Backfill completado con éxito.');
      } else {
        console.error('❌ Backfill falló con código:', code);
        res.status(500).send(`Backfill falló con código ${code}`);
      }
    });
  } catch (err) {
    console.error('💥 Error al ejecutar backfill:', err);
    res.status(500).send('Error interno al ejecutar el backfill');
  }
};


