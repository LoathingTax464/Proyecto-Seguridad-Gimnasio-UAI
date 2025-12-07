/*
  SISTEMA DE CONTROL DE ACCESO CON RFID + ESP8266 + OLED + FIREBASE

  FUNCIONALIDADES PRINCIPALES:
  - Lee tarjetas RFID (credenciales de estudiantes) con el módulo MFRC522.
  - Consulta en Firebase si la tarjeta pertenece a un usuario registrado.
  - Verifica si el usuario tiene una reserva activa en /reservas.
  - Aplica lógica de horario (ventana de ingreso) según tipo de actividad (indoor / outdoor).
  - Cuenta el número de ingresos por reserva y aplica límite.
  - Muestra mensajes en pantalla OLED según el resultado:
      * "Bienvenido"
      * "Sin reservas"
      * "Fuera de horario"
      * "Max. ingresos"
      * "Por favor, acercarte a un profesor" (al 3er fallo).
  - Registra TODOS los intentos de acceso en /historicoAccesos.
  - Registra errores de sistema en /erroresSistema con fecha, hora, tipo y explicación.
  - Administra reconexiones de WiFi y Firebase de forma controlada para evitar inestabilidad.

  EXPLICACIÓN DE LAS LIBRERÍAS Y POR QUÉ ESTAS:

  #include <ESP8266WiFi.h>
    - Librería oficial para manejar WiFi específicamente en el ESP8266.
    - Se usa en vez de WiFi genérico porque:
      * Está optimizada para este chip.
      * Exponde constantes y funciones específicas (WL_CONNECTED, WiFi.mode, etc.).
      * Evita problemas de compatibilidad que tendríamos usando librerías pensadas para ESP32.

  #include <Firebase_ESP_Client.h>
    - Cliente especializado para Firebase en microcontroladores (ESP8266, ESP32).
    - Se usa en vez de hacer peticiones HTTP "a mano" porque:
      * Maneja autenticación, reconexión, timeouts y certificados internamente.
      * Proporciona métodos de alto nivel (getInt, getString, getJSON, setJSON).
      * Es más robusto y eficiente en RAM que implementar nuestro propio cliente HTTP+JSON.

  #include <Wire.h>
    - Librería estándar para comunicación I2C (TWI).
    - Necesaria para hablar con la pantalla OLED SSD1306.
    - Se usa en lugar de "bit banging" con digitalWrite, porque:
      * I2C nativo es más veloz y estable.
      * Reduce el código de bajo nivel, haciéndolo más legible.

  #include <SPI.h>
    - Librería estándar para comunicación SPI.
    - Necesaria para el lector RFID MFRC522 (usa bus SPI).
    - Se usa en lugar de simular SPI manualmente:
      * Permite mayor velocidad de lectura/escritura.
      * Minimiza errores de timing.

  #include <MFRC522.h>
    - Librería específica para el módulo RFID RC522.
    - Implementa todos los comandos de bajo nivel (anticolisión, lectura UID, etc.).
    - Se usa en vez de escribir nuestro propio driver porque:
      * Es una librería probada y usada masivamente.
      * Evita tener que implementar protocolos RFID (ISO14443) desde cero.

  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>
    - Adafruit_GFX: librería genérica de gráficos (texto, líneas, rectángulos, etc.).
    - Adafruit_SSD1306: driver específico para pantallas OLED con chip SSD1306.
    - Se usan en vez de otras alternativas porque:
      * Son muy estables, bien documentadas y populares.
      * Tienen buen soporte para ESP8266.
      * Facilitan manejar texto y gráficos sin escribir código de bajo nivel.

  #include <NTPClient.h>
  #include <WiFiUdp.h>
    - NTPClient: cliente sencillo para obtener la hora actual desde servidores NTP.
    - WiFiUdp: permite enviar/recibir paquetes UDP sobre WiFi.
    - Se usan en lugar de implementar NTP manualmente porque:
      * Abstraen el protocolo NTP (paquetes binarios, puertos, etc.).
      * Permiten obtener hora formateada y epoch de forma simple.
      * Facilitan aplicar offset horario (zona Chile: -3 horas).

  #include <time.h>
    - Librería estándar C para manejo de fechas y tiempos (struct tm).
    - Se usa aquí para convertir el epoch en fecha (AAAA-MM-DD).
    - Complementa a NTPClient para poder registrar "fecha" humana en Firebase.
*/

#include <ESP8266WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>  // para formatear fecha a partir del epoch

// ===== CONFIGURACIÓN WIFI =====
// Credenciales de la red WiFi a la cual se conectará el ESP8266.
#define WIFI_SSID      "Urticestau"
#define WIFI_PASSWORD  "Kw!$17*28F"

// ===== CONFIGURACIÓN FIREBASE =====
// API key y URL del proyecto Firebase (Realtime Database).
// Permite que la placa lea/escriba en la base de datos.
#define API_KEY      ""
#define DATABASE_URL "https://proyecto-tdi-default-rtdb.firebaseio.com/"

// Objetos principales para trabajar con Firebase:
// fbdo   -> conexión / transporte a la RTDB y almacenamiento de respuesta.
// auth   -> credenciales de usuario (email/contraseña).
// config -> configuración general (api_key, database_url, timeouts, etc.).
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ===== JSON GLOBALES (OPTIMIZACIÓN MEMORIA) =====
// Se reutilizan los mismos objetos FirebaseJson para muchas escrituras,
// en vez de crear y destruir nuevos en cada función (evita fragmentación de RAM).
FirebaseJson jsonErrores;      // se reutiliza en registrarErrorSistema
FirebaseJson jsonAccesos;      // se reutiliza en registrarAcceso

// ===== CONFIGURACIÓN HARDWARE =====
// Pines SPI entre el ESP8266 y el módulo RFID RC522.
#define SS_PIN  15  // D8 -> pin SS/SDA del lector RFID
#define RST_PIN 0   // D3 -> pin RST del lector RFID
MFRC522 mfrc522(SS_PIN, RST_PIN);

// Parámetros de la pantalla OLED.
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 32
#define OLED_RESET    -1   // Sin pin de reset dedicado, se usa el interno.
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ===== VARIABLES =====
// Cliente UDP usado por NTPClient para comunicarse con servidores NTP.
WiFiUDP ntpUDP;
// Cliente NTP para obtener hora actual (epoch y HH:MM:SS).
// Offset -3 horas (Chile) y actualización cada 60000 ms.
NTPClient timeClient(ntpUDP, "pool.ntp.org", -3 * 3600, 60000);

// Variables para manejo de estado de la pantalla y del sistema.
String uid = "";                         // UID de la tarjeta actual
bool mostrandoMensajeTemporal = false;   // si hay mensaje temporal mostrado
unsigned long tiempoUltimoMensaje = 0;   // cuándo se mostró el último mensaje
unsigned long duracionMensaje = 3000;    // tiempo que permanece el mensaje (ms)
int erroresFirebase = 0;                 // contador de errores consecutivos de Firebase
String resetReasonGlobal = "";           // motivo del último reset del ESP

// --- Antirrebote y control de presencia ---
// Evita procesar muchas veces seguidas la misma tarjeta.
String lastUID = "";                     // último UID leído
unsigned long lastRead = 0;              // momento de la última lectura
const unsigned long DEBOUNCE_MS = 3000;  // ventana mínima entre lecturas de la misma tarjeta (3 s)

// Estructura para guardar parámetros relacionados al acceso.
struct Acceso {
  String tipo;           // "indoor" o "outdoor"
  int vecesPermitidas;   // límite de ingresos por reserva
};
Acceso accesoActual;

// ===== CONTADORES ESPECIALES =====
// Controla cada cuánto se intenta reconectar WiFi.
// La placa no reconecta inmediatamente si se cae el WiFi, espera 5 intentos de tarjetas.
int contadorIntentosWiFi = 0;

// Controla los intentos fallidos de un mismo usuario.
// Al 3er fallo aparece el mensaje "Por favor, acercarte a un profesor".
String ultimoUserKeyProfesor = "";
int contadorProfesor = 0;

// OPTIMIZACIÓN: control de frecuencia de logs de error de sistema.
// Evita registrar el mismo tipo de error miles de veces en pocos segundos.
String ultimoErrorTipo = "";
unsigned long ultimoErrorMillis = 0;
const unsigned long INTERVALO_MIN_LOG_MS = 30000; // 30 s entre logs del mismo tipo

// OPTIMIZACIÓN: control de frecuencia de actualización NTP.
// No se llama a timeClient.update() en cada loop, solo cada cierto tiempo.
unsigned long ultimoUpdateNTP = 0;
const unsigned long INTERVALO_NTP_MS = 60000; // 60 s

// ===== OLED =====
// Función helper para mostrar un mensaje de una o dos líneas en la pantalla OLED.
// Además, administra el tiempo que el mensaje se muestra.
void mostrarOLED(const String &linea1, const String &linea2 = "", int tiempo = 3000) {
  display.clearDisplay();                 // limpia la pantalla
  display.setTextSize(1);                 // tamaño de fuente
  display.setTextColor(SSD1306_WHITE);    // color del texto (blanco)
  display.setCursor(0, 8);                // posición de la primera línea
  display.println(linea1);                // escribe la primera línea
  if (linea2.length() > 0) {              // si hay segunda línea
    display.setCursor(0, 20);             // posición de la segunda línea
    display.println(linea2);              // escribe la segunda línea
  }
  display.display();                      // actualiza la pantalla
  mostrandoMensajeTemporal = true;        // indica que hay un mensaje temporal
  tiempoUltimoMensaje = millis();         // guarda instante de inicio
  duracionMensaje = tiempo;               // duración que se mantendrá
}

// ===== FECHA FORMATEADA =====
// Devuelve la fecha actual en formato "AAAA-MM-DD" usando el epoch del NTPClient.
// Importante: aquí NO se llama timeClient.update(), eso se hace en loop()
// para no sobrecargar la red.
String obtenerFechaActual() {
  time_t raw = timeClient.getEpochTime();      // obtiene epoch actual
  struct tm * timeInfo = localtime(&raw);      // convierte a estructura de fecha/hora local
  if (!timeInfo) return "";                    // si algo falla, devuelve string vacío
  char buffer[11];                             // "YYYY-MM-DD" + '\0' = 11 chars
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d",
           timeInfo->tm_year + 1900,
           timeInfo->tm_mon + 1,
           timeInfo->tm_mday);
  return String(buffer);
}

// ===== CONTADOR DE ERRORES DE SISTEMA =====
// Incrementa un contador en Firebase para cada tipo de error de sistema.
// Útil para estadísticas (cuántas veces se cayó WiFi, Firebase, etc.).
void incrementarContadorErrorSistema(const String &tipo) {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
  String path = "/erroresSistemaContador/" + tipo;
  int valor = 0;
  if (Firebase.RTDB.getInt(&fbdo, path.c_str())) valor = fbdo.intData();
  Firebase.RTDB.setInt(&fbdo, path.c_str(), valor + 1);
}

// ===== REGISTRO DE ERRORES DE SISTEMA =====
// Registra un error detallado en /erroresSistema con:
// fecha, hora, tipo de error y explicación.
void registrarErrorSistema(const String &tipo, const String &detalle) {
  ESP.wdtFeed(); yield();

  // Evitar loguear el mismo tipo de error muchas veces seguidas (dentro de 30 s).
  unsigned long ahora = millis();
  if (tipo == ultimoErrorTipo && (ahora - ultimoErrorMillis) < INTERVALO_MIN_LOG_MS) {
    Serial.print(F("↩ Saltando log repetido de error: "));
    Serial.println(tipo);
    return;
  }
  ultimoErrorTipo = tipo;
  ultimoErrorMillis = ahora;

  // Si no hay WiFi o Firebase, solo se muestra por Serial (no se puede subir a la nube).
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) {
    Serial.println(F("⚠️ No se puede registrar error de sistema (sin WiFi/Firebase)"));
    Serial.print(F("Tipo: "));   Serial.println(tipo);
    Serial.print(F("Detalle: ")); Serial.println(detalle);
    return;
  }

  // Se usa el epoch como clave (nodo hijo) en erroresSistema.
  String ts = String(timeClient.getEpochTime());
  String path = "/erroresSistema/" + ts;

  // Se limpia el JSON global y se rellenan los campos.
  jsonErrores.clear();
  String fecha = obtenerFechaActual();
  String hora  = timeClient.getFormattedTime();

  // Formato solicitado: Fecha, Hora, error, explicación
  jsonErrores.set("fecha",       fecha);
  jsonErrores.set("hora",        hora);
  jsonErrores.set("error",       tipo);
  jsonErrores.set("explicacion", detalle);

  // Escritura en Firebase
  if (Firebase.RTDB.setJSON(&fbdo, path.c_str(), &jsonErrores)) {
    Serial.print(F("🧾 Error de sistema registrado en ")); Serial.println(path);
    incrementarContadorErrorSistema(tipo);
  } else {
    Serial.print(F("❌ Error al registrar en /erroresSistema: "));
    Serial.println(fbdo.errorReason());
  }
}

// ===== WIFI =====
// Conecta la placa a la red WiFi usando las credenciales definidas arriba.
void conectarWiFi() {
  Serial.println(F("\n--- Conectando WiFi ---"));
  mostrarOLED("Conectando WiFi...");
  WiFi.mode(WIFI_STA);            // modo estación (cliente)
  WiFi.setAutoReconnect(true);    // que intente reconectar automáticamente
  WiFi.persistent(false);         // no guardar credenciales en flash
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 30) {
    delay(400);
    Serial.print(".");
    intentos++;
    yield();
  }

  if (WiFi.status() == WL_CONNECTED) {
    String ipStr = WiFi.localIP().toString();
    mostrarOLED("WiFi conectado", ipStr, 1500);
    Serial.print(F("\n✅ WiFi conectado: ")); Serial.println(ipStr);
  } else {
    mostrarOLED("Error WiFi", "Reintentando...", 2000);
    Serial.println(F("\n⚠️ Error al conectar WiFi."));
    registrarErrorSistema("wifi_conexion", "No se pudo conectar al WiFi");
  }
  yield();
}

// ===== FIREBASE =====
// Inicializa Firebase con las credenciales y parámetros de configuración.
void conectarFirebase() {
  Serial.println(F("--- Conectando Firebase ---"));
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  // Timeouts algo más cortos para evitar que el ESP se quede "pegado".
  config.timeout.serverResponse      = 7000;   // antes 10000
  config.timeout.rtdbKeepAlive       = 45000;
  config.timeout.rtdbStreamReconnect = 1000;

  // Usuario y contraseña de Firebase Authentication.
  auth.user.email    = "geronimo.urti@gmail.com";
  auth.user.password = "Gero2005";

  // Buffers y tamaño de respuesta para la conexión segura.
  fbdo.setBSSLBufferSize(512, 256);
  fbdo.setResponseSize(1024);

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  delay(1000); yield();

  if (Firebase.ready()) {
    mostrarOLED("Firebase conectado", "", 1500);
    Serial.println(F("✅ Firebase conectado correctamente."));
    erroresFirebase = 0;
  } else {
    String reason = fbdo.errorReason();
    mostrarOLED("Error Firebase", "Intentando de nuevo...", 2000);
    Serial.print(F("⚠️ Error al conectar con Firebase: ")); Serial.println(reason);
    registrarErrorSistema("firebase_conexion", reason);
  }
}

// ===== CHEQUEO DE CONEXIÓN =====
// Función central para verificar si tanto WiFi como Firebase están OK.
// Aplica política de reconexión: WiFi se intenta después de 5 lecturas de tarjeta fallidas.
bool conexionFirebaseActiva() {
  ESP.wdtFeed(); yield();

  // --- WiFi ---
  if (WiFi.status() != WL_CONNECTED) {
    // NO reconectar inmediatamente, esperar 5 intentos de tarjeta
    contadorIntentosWiFi++;
    Serial.printf("⚠️ WiFi perdido, intento %d/5 antes de reconectar\n", contadorIntentosWiFi);
    mostrarOLED("WiFi perdido", "");

    if (contadorIntentosWiFi >= 5) {
      mostrarOLED("WiFi perdido", "Reconectando...");
      registrarErrorSistema("wifi_perdida", "Reconectando tras 5 intentos de tarjeta");
      conectarWiFi();
      contadorIntentosWiFi = 0;  // reset
    }
    return false;
  } else {
    // Si WiFi está OK, se resetea el contador de intentos.
    contadorIntentosWiFi = 0;
  }

  // --- Firebase ---
  if (!Firebase.ready()) {
    erroresFirebase++;
    Serial.printf("⚠️ Firebase no ready (%d)\n", erroresFirebase);
    registrarErrorSistema("firebase_no_ready", "Firebase.ready() == false");
    if (erroresFirebase >= 5) {
      mostrarOLED("Reconectando", "Firebase...");
      Serial.println(F("🔄 Reintentando conexión Firebase..."));
      conectarFirebase();
      erroresFirebase = 0;
    }
    return false;
  }
  erroresFirebase = 0;
  return true;
}

// ===== OBTENER UID =====
// Extrae el UID de la tarjeta RFID leída por el MFRC522 y lo devuelve como String HEX.
String getUID() {
  if (mfrc522.uid.size == 0 || mfrc522.uid.size > 10) return "";
  String uidString = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) uidString += "0";
    uidString += String(mfrc522.uid.uidByte[i], HEX);
  }
  uidString.toUpperCase();
  Serial.print(F("UID detectado: ")); Serial.println(uidString);
  return uidString;
}

// ===== VERIFICAR HORARIO =====
// Determina si el usuario puede ingresar en función de:
// - tipo de actividad (indoor/outdoor)
// - hora actual
// - horaInicio / horaFin de la reserva
// - número de veces que ya ha ingresado (vecesIngresadas).
bool verificarVentanaHorario(const String &horaInicioStr, const String &horaFinStr, const String &tipo, int vecesIngresadas) {
  // Hora actual a partir del NTPClient en formato "HH:MM:SS"
  String currentTime = timeClient.getFormattedTime();
  int horaActual   = currentTime.substring(0, 2).toInt();
  int minutoActual = currentTime.substring(3, 5).toInt();
  int minutosActual = horaActual * 60 + minutoActual;

  // Validación básica de formato de strings de hora.
  if (horaInicioStr.length() < 16 || horaFinStr.length() < 16) return false;

  // Se asume formato "YYYY-MM-DDTHH:MM:SS" o similar, se extraen HH y MM.
  int horaInicio   = horaInicioStr.substring(11, 13).toInt();
  int minutoInicio = horaInicioStr.substring(14, 16).toInt();
  int minutosInicio = horaInicio * 60 + minutoInicio;

  int horaFin   = horaFinStr.substring(11, 13).toInt();
  int minutoFin = horaFinStr.substring(14, 16).toInt();
  int minutosFin = horaFin * 60 + minutoFin;

  // Lógica según tipo de actividad:
  // indoor: solo un ingreso permitido alrededor de la hora de inicio (±10 min).
  if (tipo == "indoor") {
    return (vecesIngresadas == 0 &&
            minutosActual >= (minutosInicio - 10) &&
            minutosActual <= (minutosInicio + 10));
  }

  // outdoor: se permiten dos ventanas:
  // - 1er ingreso cerca de la hora de inicio.
  // - 2do ingreso cerca de la hora de fin.
  if (tipo == "outdoor") {
    if (vecesIngresadas == 0 &&
        minutosActual >= (minutosInicio - 10) &&
        minutosActual <= (minutosInicio + 10)) return true;
    if (vecesIngresadas == 1 &&
        minutosActual >= (minutosFin - 15) &&
        minutosActual <= (minutosFin + 15)) return true;
  }
  return false;
}

// ===== REGISTRO ACCESO =====
// Registra CADA intento de acceso (permitido o no) en Firebase en:
// /historicoAccesos/userID/epoch
void registrarAcceso(const String &userID, bool exito, const String &tipo, const String &motivo) {
  ESP.wdtFeed(); yield();
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) {
    Serial.println(F("⚠️ No se puede registrar acceso (sin WiFi/Firebase)"));
    return;
  }

  // Se usa el epoch como ID del registro, agrupado por usuario.
  String path = "/historicoAccesos/" + userID + "/" + String(timeClient.getEpochTime());

  jsonAccesos.clear();

  String fecha = obtenerFechaActual();
  String hora  = timeClient.getFormattedTime();

  jsonAccesos.set("resultado",     exito ? "permitido" : "denegado");
  jsonAccesos.set("tipoActividad", tipo);
  jsonAccesos.set("motivo",        motivo);
  jsonAccesos.set("fecha",         fecha);
  jsonAccesos.set("hora",          hora);

  if (!Firebase.RTDB.setJSON(&fbdo, path.c_str(), &jsonAccesos)) {
    Serial.print(F("❌ Error al registrar acceso en historicoAccesos: "));
    Serial.println(fbdo.errorReason());
    // Si quisieras, se podría llamar registrarErrorSistema aquí,
    // pero implicaría otra escritura más en Firebase por cada error.
  }
}

// ===== MANEJO CONTADOR "ACÉRCATE A UN PROFESOR" =====
// Lleva la cuenta de cuántos intentos fallidos seguidos tiene un mismo userKey.
// Devuelve true a partir del 3er fallo consecutivo.
bool debeMostrarProfesor(const String &userKey) {
  if (userKey == ultimoUserKeyProfesor) {
    contadorProfesor++;
  } else {
    ultimoUserKeyProfesor = userKey;
    contadorProfesor = 1;
  }
  Serial.printf("Intentos fallidos para %s: %d\n", userKey.c_str(), contadorProfesor);
  return (contadorProfesor >= 3);
}

// Reinicia el contador de intentos fallidos (por ejemplo, cuando el ingreso es exitoso).
void resetProfesorCounter() {
  ultimoUserKeyProfesor = "";
  contadorProfesor = 0;
}

// ===== VERIFICAR ACCESO (con manejo optimizado de "sin reservas") =====
// Función principal que conecta todo el flujo:
// - Lee userKey desde RFIDIndex.
// - Verifica si es bypass (u1/u9).
// - Busca nombre de usuario.
// - Busca reservas activas.
// - Revisa actividad y ventana horaria.
// - Aplica tope de ingresos.
// - Registra cada intento en historicoAccesos.
// - Gestiona mensaje "Por favor, acercarte a un profesor" al 3er fallo.
void verificarAcceso(const String &uid) {
  ESP.wdtFeed(); yield();

  // Verifica conexiones antes de hacer cualquier operación de red.
  if (!conexionFirebaseActiva()) return;

  mostrarOLED("Leyendo tarjeta...");
  Serial.print(F("🔍 Verificando UID: ")); Serial.println(uid);

  // === PASO 1: Buscar userKey en /RFIDIndex/UID ===
  String pathRFID = "/RFIDIndex/" + uid;
  if (!Firebase.RTDB.getString(&fbdo, pathRFID.c_str())) {
    String reason = fbdo.errorReason();
    registrarErrorSistema("firebase_get_RFIDIndex", reason);
    if (fbdo.httpCode() == 404 || reason.indexOf("path not exist") != -1) {
      mostrarOLED("Tarjeta no", "registrada");
      Serial.println(F("🟡 UID no registrado en la base de datos."));
    } else {
      mostrarOLED("Error lectura", "Intente otra vez");
      Serial.println(F("⚠️ Error genérico al leer Firebase (RFIDIndex)."));
    }
    return;
  }

  String userKey = fbdo.stringData();
  if (userKey.length() == 0 || userKey == "null") {
    mostrarOLED("Tarjeta no", "registrada");
    Serial.println(F("🟡 UID sin asignación válida en RFIDIndex."));
    return;
  }

  ESP.wdtFeed(); yield();

  // === BYPASS ===
  // Usuarios especiales (u1 y u9) tienen acceso liberado (por ejemplo, profesor/administrador).
  if (userKey == "u1" || userKey == "u9") {
    String nombre = "";
    String nombrePathEspecial = "/users/" + userKey + "/fullName";
    if (Firebase.RTDB.getString(&fbdo, nombrePathEspecial.c_str())) nombre = fbdo.stringData();
    else nombre = "Usuario " + userKey;

    mostrarOLED("Acceso liberado", nombre);
    registrarAcceso(userKey, true, "liberado", "Acceso libre (sin restricciones)");
    resetProfesorCounter();
    Serial.printf("✔ Acceso liberado para %s\n", userKey.c_str());
    return;
  }

  // === PASO 2: Obtener nombre del usuario ===
  String nombrePath = "/users/" + userKey + "/fullName";
  if (!Firebase.RTDB.getString(&fbdo, nombrePath.c_str())) {
    String reason = fbdo.errorReason();
    mostrarOLED("Error usuario");
    Serial.print(F("❌ No se pudo obtener el nombre del usuario: ")); Serial.println(reason);
    registrarErrorSistema("firebase_get_nombre", reason);
    return;
  }
  String nombre = fbdo.stringData();
  Serial.print(F("Nombre: ")); Serial.println(nombre);

  ESP.wdtFeed(); yield();

  // === PASO 3: Buscar reserva del usuario en /reservas/userKey ===
  String resPath = "/reservas/" + userKey;

  if (!Firebase.RTDB.getJSON(&fbdo, resPath.c_str())) {
    String reason = fbdo.errorReason();
    int code = fbdo.httpCode();

    // CASO 1: Nodo /reservas/userKey NO existe → usuario simplemente no tiene reservas.
    if (code == 404 || reason.indexOf("path not exist") != -1) {
      bool mostrarProf = debeMostrarProfesor(userKey);
      if (mostrarProf) {
        mostrarOLED("Por favor, acercarte", "a un profesor");
      } else {
        mostrarOLED("Sin reservas", nombre);
      }

      registrarAcceso(userKey, false, "", "Sin reservas");
      Serial.println(F("ℹ️ Usuario sin reservas activas (nodo /reservas/userKey no existe)."));
      return;
    }

    // CASO 2: Error real de red / Firebase al leer reservas.
    mostrarOLED("Error reservas", "Intente otra vez");
    Serial.print(F("❌ Error al leer /reservas: ")); Serial.println(reason);
    registrarErrorSistema("firebase_get_reservas", reason);
    return;
  }

  // JSON de reservas obtenido correctamente.
  // Se asume que dentro de /reservas/userKey hay 1 actividad (o se toma la primera).
  FirebaseJson reservasJson = fbdo.to<FirebaseJson>();
  size_t total = reservasJson.iteratorBegin();
  String actividadID = "";
  int tipoIter;
  String key, value;
  if (total > 0) {
    reservasJson.iteratorGet(0, tipoIter, key, value);
    actividadID = key;   // la clave es el ID de la actividad
  }
  reservasJson.iteratorEnd();

  if (actividadID == "") {
    bool mostrarProf = debeMostrarProfesor(userKey);
    if (mostrarProf) {
      mostrarOLED("Por favor, acercarte", "a un profesor");
    } else {
      mostrarOLED("Reserva no", "encontrada");
    }
    registrarAcceso(userKey, false, "", "No tiene clase activa");
    Serial.println(F("⚠️ /reservas/userKey existe, pero no se encontró actividad válida."));
    return;
  }

  ESP.wdtFeed(); yield();

  // === PASO 4: Obtener datos de la actividad en /actividades/actividadID ===
  String actPath = "/actividades/" + actividadID;
  if (!Firebase.RTDB.getJSON(&fbdo, actPath.c_str())) {
    String reason = fbdo.errorReason();
    mostrarOLED("Error actividad");
    Serial.print(F("❌ Error al obtener datos de la actividad: ")); Serial.println(reason);
    registrarErrorSistema("firebase_get_actividad", reason);
    return;
  }

  FirebaseJson actJson = fbdo.to<FirebaseJson>();
  FirebaseJsonData tipoData, horaInicioData, horaFinData;
  actJson.get(tipoData,       "tipo");
  actJson.get(horaInicioData, "horaInicio");
  actJson.get(horaFinData,    "horaFin");

  String tipoActividad = tipoData.stringValue;      // "indoor" / "outdoor"
  String horaInicio    = horaInicioData.stringValue;
  String horaFin       = horaFinData.stringValue;

  // === NORMALIZACIÓN y TOPE DE VECES ===
  tipoActividad.toLowerCase();
  if (tipoActividad == "indoor") {
    accesoActual.vecesPermitidas = 1;   // solo un ingreso posible
  } else if (tipoActividad == "outdoor") {
    accesoActual.vecesPermitidas = 2;   // dos ingresos (inicio y fin)
  } else {
    accesoActual.vecesPermitidas = 1;   // valor por defecto de seguridad
  }
  accesoActual.tipo = tipoActividad;

  ESP.wdtFeed(); yield();

  // === PASO 5: Leer contador de intentos en /reservas/userKey/actividadID/intentos ===
  String contadorPath = "/reservas/" + userKey + "/" + actividadID + "/intentos";
  int intentos = 0;
  if (Firebase.RTDB.getInt(&fbdo, contadorPath.c_str())) {
    intentos = fbdo.intData();
  } else {
    Serial.print(F("ℹ️ No se pudo leer intentos, se asume 0. Motivo: "));
    Serial.println(fbdo.errorReason());
  }

  Serial.print(F("Intentos previos: ")); Serial.println(intentos);
  Serial.print(F("Tope permitido: "));   Serial.println(accesoActual.vecesPermitidas);

  // === CORTE TEMPRANO POR LÍMITE DE INGRESOS ===
  if (intentos >= accesoActual.vecesPermitidas) {
    bool mostrarProf = debeMostrarProfesor(userKey);
    if (mostrarProf) {
      mostrarOLED("Por favor, acercarte", "a un profesor");
    } else {
      mostrarOLED("Max. ingresos", nombre);
    }
    registrarAcceso(userKey, false, tipoActividad, "Límite alcanzado");
    Serial.println(F("⛔ Límite de ingresos alcanzado (corte temprano)."));
    return;
  }

  // === PASO 6: Verificar si está dentro de la ventana horaria permitida ===
  bool permitido = verificarVentanaHorario(horaInicio, horaFin, tipoActividad, intentos);

  // === PASO 7: Resultado final: permitido / denegado ===
  if (permitido) {
    // Ingreso aceptado
    mostrarOLED("Bienvenido", nombre);
    registrarAcceso(userKey, true, tipoActividad, "Ingreso permitido");
    resetProfesorCounter();  // resetea contador de fallos

    // Se incrementa el número de intentos (ingresos realizados en esa reserva).
    if (Firebase.RTDB.setInt(&fbdo, contadorPath.c_str(), intentos + 1)) {
      String ultPath = "/reservas/" + userKey + "/" + actividadID + "/ultimoIngreso";
      Firebase.RTDB.setInt(&fbdo, ultPath.c_str(), (int)timeClient.getEpochTime());
    } else {
      String reason = fbdo.errorReason();
      Serial.print(F("❌ Error al actualizar intentos: ")); Serial.println(reason);
      registrarErrorSistema("firebase_set_intentos", reason);
    }
  } else {
    // Ingreso rechazado por estar fuera de la ventana de horario.
    bool mostrarProf = debeMostrarProfesor(userKey);
    if (mostrarProf) {
      mostrarOLED("Por favor, acercarte", "a un profesor");
    } else {
      mostrarOLED("Fuera de horario", nombre);
    }
    registrarAcceso(userKey, false, tipoActividad, "Fuera de ventana");
  }

  ESP.wdtFeed(); yield();
}

// ===== ESPERA RETIRO TARJETA =====
// Espera a que el usuario retire su tarjeta del lector, o hasta que pase el timeout.
// Esto ayuda a que no se lea inmediatamente de nuevo la misma tarjeta.
bool esperarRetiroTarjeta(uint32_t timeoutMs = 3000) {
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (!mfrc522.PICC_IsNewCardPresent() && !mfrc522.PICC_ReadCardSerial()) return true;
    delay(50); yield();
  }
  return false;
}

// ===== SETUP =====
// Se ejecuta una sola vez al encender o resetear la placa.
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n\n=== INICIANDO SISTEMA ==="));
  resetReasonGlobal = ESP.getResetReason();
  Serial.print(F("Motivo de reset: ")); Serial.println(resetReasonGlobal);
  ESP.wdtEnable(8000);   // habilita el watchdog con timeout de ~8 s

  // Inicialización de buses y dispositivos.
  SPI.begin();
  mfrc522.PCD_Init();    // inicializa lector RFID

  // Inicializa la pantalla OLED en la dirección I2C 0x3C.
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("Error OLED"));
    // Si falla la pantalla, se queda en bucle infinito para no seguir "ciego".
    for (;;) { ESP.wdtFeed(); delay(1000); }
  }

  mostrarOLED("Iniciando sistema...", "", 1500);
  conectarWiFi();
  conectarFirebase();
  timeClient.begin();         // inicia cliente NTP
  timeClient.update();        // primera sincronización de hora
  ultimoUpdateNTP = millis(); // para controlar próximos updates

  // Si el reset no fue "Power on" (ej: WDT reset, brownout, etc.),
  // se registra como error de sistema (para diagnóstico).
  if (resetReasonGlobal != "" && resetReasonGlobal != "Power on" && Firebase.ready() && WiFi.status() == WL_CONNECTED) {
    registrarErrorSistema("reset", resetReasonGlobal);
  }

  mostrarOLED("Sistema listo", "Acercar credencial", 3000);
  Serial.println(F("✅ Sistema listo"));
}

// ===== LOOP =====
// Bucle principal. Se ejecuta repetidamente mientras la placa esté encendida.
void loop() {
  ESP.wdtFeed(); yield();

  // Actualizar NTP solo cada cierto tiempo, y solo si hay WiFi.
  if (WiFi.status() == WL_CONNECTED && (millis() - ultimoUpdateNTP > INTERVALO_NTP_MS)) {
    timeClient.update();
    ultimoUpdateNTP = millis();
  }

  // Si el tiempo de mostrar el mensaje temporal expiró, se vuelve al mensaje base.
  if (mostrandoMensajeTemporal && millis() - tiempoUltimoMensaje > duracionMensaje) {
    mostrarOLED("Acercar credencial", "");
    mostrandoMensajeTemporal = false;
  }

  // Si no hay nueva tarjeta presente, no hacemos nada más en este ciclo.
  if (!mfrc522.PICC_IsNewCardPresent()) { delay(60); return; }
  if (!mfrc522.PICC_ReadCardSerial())   { delay(40); return; }

  // Se obtiene el UID de la tarjeta.
  uid = getUID();
  if (uid.length() < 4) {
    Serial.println(F("⚠️ UID inválido/espurio, se descarta."));
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    delay(80);
    return;
  }

  // Antirrebote: si es la misma tarjeta que se leyó hace muy poco, se ignora.
  if (uid == lastUID && (millis() - lastRead) < DEBOUNCE_MS) {
    Serial.println(F("⏳ Debounce: misma tarjeta detectada muy rápido, ignorada."));
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    delay(120);
    return;
  }

  // Ejecuta toda la lógica de verificación de acceso.
  verificarAcceso(uid);

  // Termina sesión con la tarjeta actual.
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  // Actualiza datos para el antirrebote.
  lastUID  = uid;
  lastRead = millis();

  // Espera a que el usuario retire la tarjeta antes de permitir una nueva lectura.
  if (!esperarRetiroTarjeta(3000)) {
    Serial.println(F("⌛ Timeout esperando retiro de tarjeta."));
  }
  delay(120);
}
