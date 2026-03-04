#include <WiFi.h>
#include <DHT.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include "time.h" 



// ===========================================
//         CONFIGURAÇÕES GERAIS
// ===========================================
#define nomeWifi "wifi2"            //Informar nome da rede wifi que o ESP32 irá conectar
#define senhaWifi "123456789"       //Informar senha da rede wifi que o ESP32 irá conectar

const char* userEmail = "geovanna@gmail.com";   //Informar e-mail cadastrado no app
const char* userPassword = "123456";            //informar senha cadastrada no app

#define pinoSensor 4
#define tipoDoSensor DHT22
#define PINO_RELE 5 
DHT dht(pinoSensor, tipoDoSensor);
const char* api_key = "AIzaSyANQk8SJxGGcSh8MXqABR6Q8IGVBKjNeAA";
const char* database_url = "https://drybox-8ebef-default-rtdb.firebaseio.com";

// NTP (Relógio)
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = -10800; 
const int   daylightOffset_sec = 0; 

// Objetos do Firebase
FirebaseData meusDados;
FirebaseData streamDados;
FirebaseAuth autenticacao;
FirebaseConfig configuracao;

// Variáveis Globais
String idSessao = ""; 
String uidUsuario = "";
bool aquecedorHabilitado = false;
float tempConfigurada = 50.0;
const float histerese = 2.0;
unsigned long tempoDeParadaCalculado = 0; 
time_t fimProgramado = 0;             
unsigned long ultimoEnvio = 0;     
const long intervaloEnvio = 5000;   

// ===========================================
//  FORMATAR DATA
// ===========================================
String obterDataHoraFormatada() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    return "Erro Hora";
  }
  char timeStringBuff[50];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%d/%m/%Y - %H:%M:%S", &timeinfo);
  return String(timeStringBuff);
}


void streamCallback(FirebaseStream data) {
  Serial.printf("STREAM: %s | Tipo: %s\n", data.streamPath().c_str(), data.dataType().c_str());

  if (data.dataType() == "json") {
    FirebaseJson json;
    json.setJsonData(data.stringData()); 
    FirebaseJsonData result; 

    // Lê ID e Temperatura
    if (json.get(result, "sessao_id")) idSessao = result.to<String>(); 
    else if (json.get(result, "sessao")) idSessao = result.to<String>();
    
    if (json.get(result, "temp_configurada")) tempConfigurada = result.to<float>(); 
    
    // Lê Status
    if (json.get(result, "status_rele")) {
      bool novoStatus = result.to<bool>();
      
      
      if (!novoStatus) {
         aquecedorHabilitado = false;
         tempoDeParadaCalculado = 0;
         fimProgramado = 0;
         Serial.println("DESLIGADO MANUALMENTE.");
      } else {
         aquecedorHabilitado = true;
      }
    }
    
    // LÓGICA DE INÍCIO (Duração > 0)
    if (json.get(result, "duracao_desejada_minutos") && aquecedorHabilitado) {
      float duracaoMinutos = result.to<float>();
      
      if (duracaoMinutos > 0) {
        // 1. Calcula Timer Local (Millis)
        unsigned long duracaoMS = duracaoMinutos * 60 * 1000; 
        tempoDeParadaCalculado = millis() + duracaoMS; 
        
        // 2. Calcula Timer Absoluto para persistência
        time_t agora;
        time(&agora);
        fimProgramado = agora + (duracaoMinutos * 60);

        Serial.printf(">>> INICIANDO: %.0f min.\n", duracaoMinutos);
        Serial.printf(">>> Vai parar no: %ld\n", fimProgramado);

        // 3. Salva o tempo no Banco 
        String pathControle = "/usuarios/" + uidUsuario + "/dispositivos/controle";
        Firebase.RTDB.setInt(&meusDados, pathControle + "/fim_programado", (int)fimProgramado);
        
        // 4. Zera a duração para não rearmar errado
        Firebase.RTDB.setFloat(&meusDados, pathControle + "/duracao_desejada_minutos", 0); 
      }
    }
  } 
  else if (data.dataType() == "boolean" && data.streamPath() == "/status_rele") {
      bool status = data.to<bool>();
      if (!status) { 
        aquecedorHabilitado = false; 
        tempoDeParadaCalculado = 0; 
      } else {
        aquecedorHabilitado = true;
      }
  }
}

// ===========================================
//  FINALIZAR SECAGEM
// ===========================================
void finalizarSecagem() {
    Serial.println(">>> FINALIZANDO CICLO...");
    
    if (idSessao.length() > 2) {
       String basePath = "/historico_sessoes/" + uidUsuario + "/" + idSessao;
       Firebase.RTDB.setTimestamp(&meusDados, basePath + "/data_fim");
       Firebase.RTDB.setString(&meusDados, basePath + "/data_fim_formatada", obterDataHoraFormatada());
       Serial.println(">>> Histórico Atualizado.");
       idSessao = ""; 
    }

    aquecedorHabilitado = false; 
    tempoDeParadaCalculado = 0; 
    fimProgramado = 0;
    
    // Zera no banco
    String pathControle = "/usuarios/" + uidUsuario + "/dispositivos/controle";
    Firebase.RTDB.setBool(&meusDados, pathControle + "/status_rele", false); 
    Firebase.RTDB.setInt(&meusDados, pathControle + "/fim_programado", 0);
}

// ===========================================
//   SETUP
// ===========================================
void setup() {
  Serial.begin(115200);
  pinMode(PINO_RELE, OUTPUT);
  digitalWrite(PINO_RELE, HIGH); 

  Serial.print("Conectando WiFi");
  WiFi.begin(nomeWifi, senhaWifi);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi OK!");


  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){ Serial.println("Erro NTP"); }

  dht.begin();

  configuracao.api_key = api_key;
  configuracao.database_url = database_url;
  configuracao.token_status_callback = tokenStatusCallback;
  autenticacao.user.email = userEmail;
  autenticacao.user.password = userPassword;
  Firebase.begin(&configuracao, &autenticacao);
  Firebase.reconnectWiFi(true);
  
  meusDados.setBSSLBufferSize(4096, 1024);

  if (autenticacao.token.uid.length() > 0) {
    uidUsuario = String(autenticacao.token.uid.c_str()); 
    Serial.println("Logado: " + uidUsuario);

    // RECUPERAÇÃO DE ESTADO DO RELE
    Serial.println(">>> Verificando estado anterior...");
    String pathControle = "/usuarios/" + uidUsuario + "/dispositivos/controle";
    
    if (Firebase.RTDB.getJSON(&meusDados, pathControle)) {
       FirebaseJson &json = meusDados.jsonObject();
       FirebaseJsonData res;
       
       bool statusBanco = false;
       if (json.get(res, "status_rele")) statusBanco = res.to<bool>();

       // Se estava ligado, vamos ver se o tempo já acabou
       if (statusBanco) {
          int fimBanco = 0;
          if (json.get(res, "fim_programado")) fimBanco = res.to<int>();
          
          if (json.get(res, "sessao_id")) idSessao = res.to<String>();
          if (json.get(res, "temp_configurada")) tempConfigurada = res.to<float>();

          time_t agora;
          time(&agora);

          if (fimBanco > 0) {
             if (agora >= fimBanco) {
                Serial.println(">>> A LUZ VOLTOU, MAS O TEMPO JÁ ACABOU! FINALIZANDO.");
                idSessao = res.to<String>(); // Garante ID para fechar
                finalizarSecagem();
             } else {
                Serial.println(">>> A LUZ VOLTOU E AINDA TEM TEMPO! RETOMANDO.");
                aquecedorHabilitado = true;
                fimProgramado = fimBanco;
                // Recalcula millis para o loop
                unsigned long segundosRestantes = fimBanco - agora;
                tempoDeParadaCalculado = millis() + (segundosRestantes * 1000);
             }
          } else {
             // Se tá ligado mas não tem data fim (erro), desliga por segurança
             Serial.println(">>> Estado inconsistente. Desligando por segurança.");
             finalizarSecagem();
          }
       } else {
          Serial.println(">>> Sistema estava desligado.");
       }
    }

    // Inicia Stream
    Firebase.RTDB.beginStream(&streamDados, pathControle);
    Firebase.RTDB.setStreamCallback(&streamDados, streamCallback, nullptr);
  }
}

// ===========================================
//         LOOP CHECAGEM 
// ===========================================
void loop() {
  if (Firebase.ready() && autenticacao.token.uid.length() > 0) {
    Firebase.RTDB.readStream(&streamDados);
  }

  float umidade = dht.readHumidity();
  float temperatura = dht.readTemperature();
  bool leituraValida = !isnan(umidade) && !isnan(temperatura);


  if (aquecedorHabilitado) {
    
    bool tempoEsgotado = false;

    // Checagem 1
    if (tempoDeParadaCalculado > 0 && millis() > tempoDeParadaCalculado) {
       tempoEsgotado = true;
    }

    // Checagem 2: NTP
    if (fimProgramado > 0) {
       time_t agora;
       time(&agora);
       if (agora > fimProgramado) {
          tempoEsgotado = true;
       }
    }

    if (tempoEsgotado) {
      finalizarSecagem();
    } else {
      // TERMOSTATO
      if (leituraValida) {
        if (temperatura > tempConfigurada) digitalWrite(PINO_RELE, HIGH); 
        else if (temperatura < (tempConfigurada - histerese)) digitalWrite(PINO_RELE, LOW); 
      }
    }
  } else {
    digitalWrite(PINO_RELE, HIGH); 
  }

  // ENVIO SENSORES (5s) 
  unsigned long agora = millis();
  if (agora - ultimoEnvio > intervaloEnvio) {
    ultimoEnvio = agora;

    time_t instante_atual;
    time(&instante_atual);

    FirebaseJson json;

    json.set("ultimo_envio", (int)instante_atual);

    if (leituraValida && uidUsuario.length() > 0) {
        String pathSensores = "/usuarios/" + uidUsuario + "/dispositivos/sensores";

        json.set("temperatura", temperatura);
        json.set("umidade", umidade);
        
        Firebase.RTDB.setJSON(&meusDados, pathSensores, &json);
    }
  }
}