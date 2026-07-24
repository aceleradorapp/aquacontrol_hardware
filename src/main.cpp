#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <esp_system.h> // esp_reset_reason() — motivo do ultimo boot (12-espc)

// Bibliotecas nativas para controle dos registradores de hardware do ESP32
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// Versao do firmware (12-espc, Diagnostico Completo) — sem CI/build number automatico
// neste projeto, entao isso e incrementado manualmente a cada mudanca relevante. A data/
// hora de compilacao (__DATE__/__TIME__) e automatica e sempre confiavel, essa nao precisa
// de manutencao manual.
#define FIRMWARE_VERSAO "1.1.0"

// Arquivo de credenciais (Certifique-se de que ele existe na pasta include)
#include "Segredos.h"

// --- Rede: reaproveitado do firmware antigo (07-espc) — o resto foi todo zerado, era só
// simulação de bancada. IP fixo pra o Brain sempre achar este módulo no mesmo endereço.
IPAddress ipEstatico(192, 168, 98, 223);
IPAddress ipGateway(192, 168, 98, 1);
IPAddress ipSubmascara(255, 255, 255, 0);
IPAddress ipDns(192, 168, 98, 1);

WebServer server(80);
Preferences preferencias;

// LED azul onboard (Pino 2 na maioria das placas ESP32 clássicas). Aceso fixo = Wi-Fi
// conectado (dá pra confirmar a conexao só olhando a placa, sem cabo serial); apaga se a
// conexao cair e volta a acender quando reconectar (ver conectarWifi()/loop()). Também pisca
// brevemente a cada comando de relé recebido, como feedback extra de atividade — depois do
// pulso ele volta pro estado fixo aceso (não pro apagado), já que a conexao continua ativa.
const int pinoLed = 2;

// --- GPIOs diretos do ESP32 (11-espc) ---
// 2x módulos de relé de 8 canais = 16 saídas, cada uma num GPIO nativo do ESP32 — sem
// expansor I2C (o MCP23017 foi removido, ver 01-espc-geral/11_esquema_ligacao_reles.md).
// Lógica ACTIVE LOW: o relé liga quando o pino fica em LOW. Índice do array = posição no
// JSON de 16 posições que o Brain/Display já esperam (0-7 = módulo 1, 8-15 = módulo 2) —
// mesma numeração de sempre, só a forma de acionar o pino que mudou.
const uint8_t TOTAL_RELES = 16;
const uint8_t PINOS_RELES[TOTAL_RELES] = {
    13, 14, 27, 26, 25, 33, 32, 4,  // Módulo 1 (canais 1-8, índices 0-7)
    16, 17, 5,  18, 19, 21, 22, 23  // Módulo 2 (canais 9-16, índices 8-15)
};
// GPIO 02 e GPIO 15 ficam reservados (não usados aqui) para expansões futuras (LED de
// status, fita WS2812B etc.) — ver a especificação de fiação.

// IP do backend (AquaControl_Brain) recebido no handshake (POST /api/config) e persistido
// em NVS — sobrevive a reinicializações, não precisa handshake de novo todo boot.
String ipBackend = "";

// Hostname da rede (12-espc, Diagnostico Completo/Editar Controlador): persistido em NVS,
// editável via POST /api/config-dispositivo (dashboard). Precisa ser aplicado com
// WiFi.setHostname() ANTES de conectar (ver conectarWifi()) — mudar em tempo real não
// atualiza o nome já registrado no DHCP/mDNS do roteador, por isso o endpoint reinicia o
// módulo depois de salvar (mesmo padrão do Reset Wi-Fi do Display).
const char* const HOSTNAME_PADRAO = "aquacontrol-reles";
String hostnameDispositivo = HOSTNAME_PADRAO;

// Traduz o código de status do Wi-Fi para algo legível no serial — sem isso, um SSID
// errado e uma senha errada parecem exatamente iguais (só pontinhos para sempre).
const char* descreverStatusWifi(wl_status_t status) {
    switch (status) {
        case WL_NO_SSID_AVAIL:   return "rede nao encontrada (confira o nome exato do SSID, maiusculas/espacos)";
        case WL_CONNECT_FAILED:  return "falha na conexao (provavelmente senha errada)";
        case WL_CONNECTION_LOST: return "conexao perdida";
        case WL_DISCONNECTED:    return "desconectado";
        case WL_IDLE_STATUS:     return "ocioso, ainda tentando";
        default:                 return "status desconhecido";
    }
}

// Função responsável por gerenciar a conexão Wi-Fi (reaproveitada do firmware antigo,
// intocada — só essa parte sobreviveu à limpeza, ver 07-espc)
void conectarWifi() {
    Serial.println();
    Serial.printf("Conectando na rede \"%s\"...\n", WIFI_SSID);

    // Zera qualquer estado anterior do rádio antes de tentar — evita ficar preso num
    // modo misto (AP+STA) ou numa conexão fantasma de um teste anterior.
    WiFi.mode(WIFI_STA);
    // Precisa vir ANTES do begin() — é o que faz o roteador (DHCP) e qualquer mDNS
    // registrarem este hostname de verdade, não só um valor interno decorativo.
    WiFi.setHostname(hostnameDispositivo.c_str());
    WiFi.disconnect(true);
    delay(100);
    WiFi.config(ipEstatico, ipGateway, ipSubmascara, ipDns);
    WiFi.begin(WIFI_SSID, WIFI_SENHA);

    int tentativas = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        tentativas++;

        // A cada 5s, mostra o motivo real (não só pontinhos) para dar para diagnosticar
        if (tentativas % 10 == 0) {
            Serial.printf(" [status=%d: %s]\n", WiFi.status(), descreverStatusWifi(WiFi.status()));
        }

        // Se demorar muito para conectar (mais de 15 segundos), reinicia o modem
        if (tentativas > 30) {
            Serial.println("\nFalha prolongada. Reiniciando modem Wi-Fi...");
            WiFi.disconnect(true);
            delay(1000);
            WiFi.begin(WIFI_SSID, WIFI_SENHA);
            tentativas = 0;
        }
    }

    Serial.println();
    Serial.print("Wi-Fi conectado com sucesso! IP do controlador: ");
    Serial.println(WiFi.localIP());

    digitalWrite(pinoLed, HIGH); // LED azul fixo aceso = Wi-Fi conectado
}

// Configura os 16 GPIOs como OUTPUT e força todos pra HIGH (desligado, Active LOW) — estado
// "seguro" de boot, evita qualquer relé ligar sozinho ao energizar. Diferente da versão
// I2C anterior, não há "falha de inicialização" possível aqui: são pinos nativos do próprio
// chip, sempre disponíveis — por isso não existe mais um estado "indisponível"/retry
// periódico (11-espc): a resposta dos endpoints de relé é sempre imediata.
void inicializarReles() {
    for (uint8_t i = 0; i < TOTAL_RELES; i++) {
        pinMode(PINOS_RELES[i], OUTPUT);
        digitalWrite(PINOS_RELES[i], HIGH); // desligado (Active LOW)
    }
    Serial.println("[RELES] 16 GPIOs inicializados e desligados.");
}

// Lê o IP do backend E o hostname salvos na NVS (se já houve handshake/edição antes)
void carregarConfigSalva() {
    preferencias.begin("aqua_hw", true); // true = somente leitura
    ipBackend = preferencias.getString("server_ip", "");
    hostnameDispositivo = preferencias.getString("hostname", HOSTNAME_PADRAO);
    preferencias.end();

    if (ipBackend.length() > 0) {
        Serial.printf("[CONFIG] IP do backend recuperado da NVS: %s\n", ipBackend.c_str());
    }
    Serial.printf("[CONFIG] Hostname: %s\n", hostnameDispositivo.c_str());
}

// Grava o IP do backend na NVS — chamado pelo handshake (POST /api/config)
void salvarConfig(const String& ip) {
    ipBackend = ip;
    preferencias.begin("aqua_hw", false); // false = leitura/escrita
    preferencias.putString("server_ip", ip);
    preferencias.end();
}

// Grava o hostname na NVS — chamado por POST /api/config-dispositivo (12-espc, Editar
// Controlador no dashboard). Não tenta aplicar WiFi.setHostname() em tempo real: o valor só
// é lido de verdade no próximo conectarWifi(), então quem chama isso reinicia o módulo logo
// em seguida (ver handlePostConfigDispositivo).
void salvarHostname(const String& novoHostname) {
    hostnameDispositivo = novoHostname;
    preferencias.begin("aqua_hw", false);
    preferencias.putString("hostname", novoHostname);
    preferencias.end();
}

// POST /api/config — handshake de cadastro (espc/07): o AquaControl_Brain manda o próprio
// IP aqui assim que o usuário cadastra este módulo como "atuador" no dashboard.
void handlePostConfig() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"status\":\"erro: sem corpo de dados\"}");
        return;
    }

    JsonDocument doc;
    DeserializationError erro = deserializeJson(doc, server.arg("plain"));
    if (erro || doc["server_ip"].isNull()) {
        server.send(400, "application/json", "{\"status\":\"erro: json invalido ou faltando server_ip\"}");
        return;
    }

    salvarConfig(doc["server_ip"].as<String>());
    Serial.printf("[CONFIG] Handshake recebido. IP do backend salvo: %s\n", ipBackend.c_str());

    JsonDocument resposta;
    resposta["status"] = "ok";
    resposta["modulo"] = "RELES_GPIO_16CH";
    resposta["online"] = true;

    String saida;
    serializeJson(resposta, saida);
    server.send(200, "application/json", saida);
}

// Valida um hostname simples (letras/numeros/hifen, 1-32 chars) — nao pode ter espaco nem
// caractere especial (o DHCP/mDNS do roteador nao aceita isso de forma confiavel).
bool hostnameValido(const String& valor) {
    if (valor.length() == 0 || valor.length() > 32) return false;
    for (size_t i = 0; i < valor.length(); i++) {
        char c = valor.charAt(i);
        bool ok = isalnum(c) || c == '-';
        if (!ok) return false;
    }
    return true;
}

// POST /api/config-dispositivo — editavel pelo dashboard (12-espc, botao "Editar
// Controlador"). Por enquanto so o hostname (o "nome" amigavel do modulo vive so no banco
// do Brain, nao precisa de nada aqui). Reinicia o modulo depois de salvar — WiFi.setHostname
// so tem efeito de verdade no proximo WiFi.begin() (ver conectarWifi()), entao aplicar sem
// reiniciar deixaria o valor salvo divergente do que o roteador realmente enxerga ate o
// proximo boot de qualquer jeito; reiniciar na hora evita esse estado "pela metade".
void handlePostConfigDispositivo() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"status\":\"erro: sem corpo de dados\"}");
        return;
    }

    JsonDocument doc;
    DeserializationError erro = deserializeJson(doc, server.arg("plain"));
    if (erro) {
        server.send(400, "application/json", "{\"status\":\"erro: json invalido\"}");
        return;
    }

    bool precisaReiniciar = false;

    if (!doc["hostname"].isNull()) {
        String novoHostname = doc["hostname"].as<String>();
        if (!hostnameValido(novoHostname)) {
            server.send(400, "application/json", "{\"status\":\"erro: hostname invalido (use so letras, numeros e hifen, ate 32 caracteres)\"}");
            return;
        }
        salvarHostname(novoHostname);
        Serial.printf("[CONFIG] Hostname atualizado para: %s (reiniciando...)\n", novoHostname.c_str());
        precisaReiniciar = true;
    }

    JsonDocument resposta;
    resposta["status"] = "ok";
    resposta["reinicio_necessario"] = precisaReiniciar;
    String saida;
    serializeJson(resposta, saida);
    server.send(200, "application/json", saida);

    if (precisaReiniciar) {
        delay(300); // da tempo da resposta HTTP realmente sair antes do reset
        ESP.restart();
    }
}

// Traduz o motivo do ultimo boot (esp_reset_reason(), ESP-IDF) pra algo legivel no
// Diagnostico Completo — sem isso so da pra ver um numero de enum sem contexto nenhum.
const char* descreverMotivoReset(esp_reset_reason_t motivo) {
    switch (motivo) {
        case ESP_RST_POWERON:   return "Ligacao normal (power-on)";
        case ESP_RST_EXT:       return "Reset externo (botao/pino EN)";
        case ESP_RST_SW:        return "Reset via software (ESP.restart())";
        case ESP_RST_PANIC:     return "Pane de firmware (exception/panic)";
        case ESP_RST_INT_WDT:   return "Watchdog interno";
        case ESP_RST_TASK_WDT:  return "Watchdog de tarefa (task travada)";
        case ESP_RST_WDT:       return "Outro watchdog";
        case ESP_RST_DEEPSLEEP: return "Saiu de deep sleep";
        case ESP_RST_BROWNOUT:  return "Queda de tensao (brownout)";
        case ESP_RST_SDIO:      return "Reset via SDIO";
        default:                return "Desconhecido";
    }
}

// GET /api/reles — lê o estado atual das 16 saídas direto nos GPIOs, convertendo a lógica
// física Active LOW (LOW = ligado) para o array lógico 1=ligado/0=desligado que o resto do
// sistema (Brain, Display) entende. digitalRead() num pino já configurado OUTPUT no ESP32
// retorna o nível que ele mesmo está segurando — sem necessidade de guardar o estado à
// parte em RAM. Sempre 200: não há "hardware indisponível" possível aqui (11-espc).
void handleGetReles() {
    JsonDocument doc;
    JsonArray reles = doc["reles"].to<JsonArray>();

    for (uint8_t i = 0; i < TOTAL_RELES; i++) {
        bool ligado = (digitalRead(PINOS_RELES[i]) == LOW);
        reles.add(ligado ? 1 : 0);
    }

    String saida;
    serializeJson(doc, saida);
    server.send(200, "application/json", saida);
}

// POST /api/reles — recebe o array lógico (1=ligado/0=desligado) e converte de volta pra
// Active LOW (1 -> LOW, 0 -> HIGH) escrevendo direto em cada GPIO. Sempre 200 (11-espc).
void handlePostReles() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"status\":\"erro: sem corpo de dados\"}");
        return;
    }

    JsonDocument doc;
    DeserializationError erro = deserializeJson(doc, server.arg("plain"));
    if (erro || !doc["reles"].is<JsonArray>()) {
        server.send(400, "application/json", "{\"status\":\"erro: json invalido, esperado array \\\"reles\\\"\"}");
        return;
    }

    digitalWrite(pinoLed, LOW); // pisca (LED fica fixo aceso enquanto conectado — ver conectarWifi())

    uint8_t indice = 0;
    for (JsonVariant valor : doc["reles"].as<JsonArray>()) {
        if (indice >= TOTAL_RELES) break;
        bool ligar = valor.as<int>() == 1;
        digitalWrite(PINOS_RELES[indice], ligar ? LOW : HIGH);
        indice++;
    }

    delay(80); // pulso do LED visível a olho nu
    digitalWrite(pinoLed, HIGH); // volta pro estado fixo aceso (conectado), nao apaga

    Serial.printf("[RELES] %d saida(s) atualizada(s) via POST /api/reles.\n", indice);
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

// GET /api/status — checagem de "esta vivo e acessivel" (pensada pra ser aberta direto no
// navegador) + Diagnostico Completo (12-espc): tudo que o dashboard mostra no modal de
// diagnostico do modulo. Sempre 200, todos os campos calculados na hora (nenhum e cacheado).
void handleGetStatus() {
    JsonDocument doc;

    // --- Rede ---
    doc["status"] = "online";
    doc["ip"] = WiFi.localIP().toString();
    doc["hostname"] = WiFi.getHostname();
    doc["ssid"] = WiFi.SSID();
    doc["rssi_dbm"] = WiFi.RSSI();
    doc["gateway"] = WiFi.gatewayIP().toString();
    doc["dns"] = WiFi.dnsIP().toString();
    doc["mac_address"] = WiFi.macAddress();
    doc["uptime_s"] = millis() / 1000;
    doc["ip_backend_salvo"] = ipBackend;
    doc["controle_reles"] = "gpio_direto"; // 11-espc: sem MCP23017/I2C, GPIOs nativos do ESP32

    // --- Chip / CPU ---
    doc["chip_modelo"] = ESP.getChipModel();
    doc["chip_revisao"] = ESP.getChipRevision();
    doc["chip_cores"] = ESP.getChipCores();
    doc["cpu_freq_mhz"] = ESP.getCpuFreqMHz();

    // --- Memoria ---
    doc["heap_livre_bytes"] = ESP.getFreeHeap();
    doc["heap_total_bytes"] = ESP.getHeapSize();
    doc["psram_total_bytes"] = ESP.getPsramSize();   // 0 neste chip (ESP32 classico, sem PSRAM)
    doc["psram_livre_bytes"] = ESP.getPsramSize() > 0 ? ESP.getFreePsram() : 0;

    // --- Flash / armazenamento ---
    doc["flash_total_bytes"] = ESP.getFlashChipSize();
    doc["sketch_usado_bytes"] = ESP.getSketchSize();
    doc["sketch_livre_bytes"] = ESP.getFreeSketchSpace();

    // LittleFS pode nao ter partição de dados dependendo do particionamento — tenta montar
    // (formata sozinho na primeira vez se preciso) e reporta indisponivel em vez de travar
    // se não existir, em vez de assumir que sempre está disponível.
    bool littlefsOk = LittleFS.begin(true);
    doc["littlefs_disponivel"] = littlefsOk;
    doc["littlefs_total_bytes"] = littlefsOk ? LittleFS.totalBytes() : 0;
    doc["littlefs_usado_bytes"] = littlefsOk ? LittleFS.usedBytes() : 0;

    // --- Firmware / boot ---
    doc["firmware_versao"] = FIRMWARE_VERSAO;
    doc["firmware_compilado_em"] = __DATE__ " " __TIME__;
    doc["motivo_reset"] = descreverMotivoReset(esp_reset_reason());

    String saida;
    serializeJson(doc, saida);
    server.send(200, "application/json", saida);
}

void setup() {
    // 💡 DESATIVA O DETECTOR DE BROWNOUT (Evita resets por queda de tensão no pico do Wi-Fi)
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== AquaControl_Hardware - Modulo de Reles (16x GPIO direto) ===");

    pinMode(pinoLed, OUTPUT);
    digitalWrite(pinoLed, LOW);

    inicializarReles();

    carregarConfigSalva();
    conectarWifi();

    server.on("/api/config", HTTP_POST, handlePostConfig);
    server.on("/api/config-dispositivo", HTTP_POST, handlePostConfigDispositivo);
    server.on("/api/status", HTTP_GET, handleGetStatus);
    server.on("/api/reles", HTTP_GET, handleGetReles);
    server.on("/api/reles", HTTP_POST, handlePostReles);
    server.begin();
    Serial.println("WebServer do Modulo de Reles ativo na porta 80.");
}

void loop() {
    // Garante que o Wi-Fi continua conectado antes de tentar responder requisicoes
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Conexao Wi-Fi perdida! Reconectando...");
        digitalWrite(pinoLed, LOW); // apaga enquanto desconectado — volta a acender em conectarWifi()
        conectarWifi();
    }

    server.handleClient();
}
