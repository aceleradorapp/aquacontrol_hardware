#include <Arduino.h>

// Teste simples: só para confirmar que a placa, a porta COM e o upload estão
// funcionando antes de começar a escrever o firmware de verdade.
void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
    digitalWrite(LED_BUILTIN, HIGH);
    delay(500);
    digitalWrite(LED_BUILTIN, LOW);
    delay(500);
}
