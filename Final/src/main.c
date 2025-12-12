#include "xparameters.h"
#include "xgpio.h"
#include "xil_printf.h"
#include "sleep.h"
#include "xstatus.h"
#include <string.h> // memset, strcmp

// =============================
// CONFIG GENERAL
// =============================

// Base address de los GPIO (según tu xparameters.h)
#define GPIO_COLUMNS_BASEADDR   XPAR_XGPIO_0_BASEADDR // columnas (3 bits, IN)
#define GPIO_ROWS_BASEADDR      XPAR_XGPIO_1_BASEADDR // filas (4 bits, OUT)
#define GPIO_SERVO_BASEADDR     XPAR_XGPIO_2_BASEADDR // servo (1 bit, OUT)
#define GPIO_BUZZER_BASEADDR    XPAR_XGPIO_3_BASEADDR // buzzer (1 bit, OUT)
#define GPIO_ULTRA_TRIG_BASEADDR XPAR_XGPIO_5_BASEADDR // TRIG HC-SR04 (OUT)
#define GPIO_ULTRA_ECHO_BASEADDR XPAR_XGPIO_4_BASEADDR // ECHO HC-SR04 (IN)

XGpio columns_gpio;
XGpio rows_gpio;
XGpio servo_gpio;
XGpio buzzer_gpio;
XGpio ultra_trig_gpio;
XGpio ultra_echo_gpio;

// Clave correcta
#define PASSWORD             "1234"
#define PASSWORD_LENGTH      4
#define PASSWORD_BUFFER_MAX  8 // por si marcas más de la cuenta

// ======================
// Teclado 4x3
// ======================
const char keypad_map[4][3] = {
    {'1','2','3'},
    {'4','5','6'},
    {'7','8','9'},
    {'*','0','#'}
};

// Escanea el teclado: devuelve la tecla o 0 si no hay ninguna
char scan_keypad(void) {
    int row, col;

    for (row = 0; row < 4; row++) {
        // Activar solo la fila actual (bit en 0, los demás en 1)
        // Filas activas en bajo, columnas con pull‑up
        u32 row_value = ~(1u << row) & 0x0F;
        XGpio_DiscreteWrite(&rows_gpio, 1, row_value);

        usleep(1000); // pequeño delay

        // Leer columnas (3 bits)
        u32 col_read = XGpio_DiscreteRead(&columns_gpio, 1);

        for (col = 0; col < 3; col++) {
            // Si la columna está en 0, hay tecla presionada
            if (!(col_read & (1u << col))) {
                return keypad_map[row][col];
            }
        }
    }

    return 0; // No hay tecla
}

// ======================
// Servo SG90
// ======================

// Un período de servo: 20 ms, con 'high_time_us' en alto
void servo_pulse_us(u32 high_time_us) {
    // señal alta
    XGpio_DiscreteWrite(&servo_gpio, 1, 1);
    usleep(high_time_us);

    // señal baja
    XGpio_DiscreteWrite(&servo_gpio, 1, 0);
    usleep(20000 - high_time_us); // resto del período (20 ms)
}

// Mover a cierto ángulo (0–180)
void servo_goto_angle(int angle) {
    if (angle < 0)  angle = 0;
    if (angle > 180) angle = 180;

    // SG90 típico: ~1000 µs = 0°, ~2000 µs = 180°
    u32 high_us = 1000 + (angle * 1000) / 180;

    // Mandar varios pulsos para que el servo llegue a la posición
    for (int i = 0; i < 50; i++) { // 50 pulsos ≈ 1 segundo
        servo_pulse_us(high_us);
    }
}

// Función comodín: abrir 5 s y cerrar
void abrir_cerradura_5s(void) {
    xil_printf("Abriendo cerradura...\r\n");
    servo_goto_angle(0);  // ABIERTO

    xil_printf("Cerradura abierta 5 segundos...\r\n");
    sleep(5);               // 5 segundos

    xil_printf("Cerrando cerradura...\r\n");
    servo_goto_angle(150);    // CERRADO
}
/*
void abrir_cerradura_5s(void) {
    xil_printf("Abriendo cerradura...\r\n");
    servo_goto_angle(30);  // ABIERTO (invertido)

    xil_printf("Cerradura abierta 5 segundos...\r\n");
    sleep(5);

    xil_printf("Cerrando cerradura...\r\n");
    servo_goto_angle(180); // CERRADO (invertido)
}*/

// ======================
// Buzzer de error
// ======================

void buzzer_on(void) {
    XGpio_DiscreteWrite(&buzzer_gpio, 1, 0);
}

void buzzer_off(void) {
    XGpio_DiscreteWrite(&buzzer_gpio, 1, 1);
}

// Beep de error: 3 pitidos cortos
void buzzer_error_beep(void) {
    for (int i = 0; i < 3; i++) {
        buzzer_on();
        usleep(200000); // 200 ms encendido
        buzzer_off();
        usleep(200000); // 200 ms apagado
    }
}

// ======================
// HC-SR04 - sensor ultrasónico
// ======================
//
// Distancia en cm a partir del ancho del pulso ECHO.
// Devuelve XST_SUCCESS si la medida fue válida y pone en *dist_cm el valor.
//

int ultrasonic_measure_cm(int *dist_cm) {
    u32 timeout;
    u32 count;
    u32 echo;

    // 1) Generar pulso de disparo en TRIG (10 us)
    XGpio_DiscreteWrite(&ultra_trig_gpio, 1, 0);
    usleep(2);
    XGpio_DiscreteWrite(&ultra_trig_gpio, 1, 1);
    usleep(10);
    XGpio_DiscreteWrite(&ultra_trig_gpio, 1, 0);

    // 2) Esperar flanco de subida en ECHO (comienza el pulso)
    timeout = 30000; // 30 ms de timeout en la espera del inicio del pulso
    count   = 0;
    do {
        echo = XGpio_DiscreteRead(&ultra_echo_gpio, 1) & 0x01;
        if (echo)
            break;
        usleep(1); // 1 us
        count++;
    } while (count < timeout);

    if (!echo) {
        // No vimos el inicio del pulso
        return XST_FAILURE;
    }

    // 3) Medir duración del pulso alto en ECHO
    count = 0;
    while ((XGpio_DiscreteRead(&ultra_echo_gpio, 1) & 0x01) && (count < 30000)) {
        usleep(1); // 1 us
        count++;
    }

    if (count >= 30000) {
        // Pulso demasiado largo o error
        return XST_FAILURE;
    }

    // count ~ tiempo en microsegundos
    // Distancia (cm) ≈ (tiempo_us * 0.0343) / 2
    float distance_f = (count * 0.0343f) / 2.0f;
    int distance_int = (int)(distance_f + 0.5f); // redondeo al entero más cercano

    *dist_cm = distance_int;
    return XST_SUCCESS;
}


// ======================
// MAIN
// ======================
int main(void)
{
    int status;

    // Buffer de clave
    char password_buffer[PASSWORD_BUFFER_MAX + 1];
    int  password_index = 0;

    xil_printf("Inicializando teclado + servo + buzzer + ultrasonico...\r\n");

    // ============================
    // CONFIG GPIO COLUMNAS (IN)
    // ============================
    XGpio_Config columns_cfg;
    memset(&columns_cfg, 0, sizeof(XGpio_Config));

    columns_cfg.BaseAddress = GPIO_COLUMNS_BASEADDR;
    columns_cfg.IsDual      = 0;

    status = XGpio_CfgInitialize(&columns_gpio, &columns_cfg, columns_cfg.BaseAddress);
    if (status != XST_SUCCESS) {
        xil_printf("Error inicializando GPIO columnas, status=%d\r\n", status);
        return XST_FAILURE;
    }

    // Canal 1 como entrada: 3 bits (0b111 = 0x7)
    XGpio_SetDataDirection(&columns_gpio, 1, 0x07);

    // ============================
    // CONFIG GPIO FILAS (OUT)
    // ============================
    XGpio_Config rows_cfg;
    memset(&rows_cfg, 0, sizeof(XGpio_Config));

    rows_cfg.BaseAddress = GPIO_ROWS_BASEADDR;
    rows_cfg.IsDual      = 0;

    status = XGpio_CfgInitialize(&rows_gpio, &rows_cfg, rows_cfg.BaseAddress);
    if (status != XST_SUCCESS) {
        xil_printf("Error inicializando GPIO filas, status=%d\r\n", status);
        return XST_FAILURE;
    }

    // Canal 1 como salida: 4 bits (0b0000 = todas salidas)
    XGpio_SetDataDirection(&rows_gpio, 1, 0x00);

    // ============================
    // CONFIG GPIO SERVO (OUT)
    // ============================
    XGpio_Config servo_cfg;
    memset(&servo_cfg, 0, sizeof(XGpio_Config));

    servo_cfg.BaseAddress = GPIO_SERVO_BASEADDR;
    servo_cfg.IsDual      = 0;

    status = XGpio_CfgInitialize(&servo_gpio, &servo_cfg, servo_cfg.BaseAddress);
    if (status != XST_SUCCESS) {
        xil_printf("Error inicializando GPIO servo, status=%d\r\n", status);
        return XST_FAILURE;
    }

    XGpio_SetDataDirection(&servo_gpio, 1, 0x00); // 1 bit salida

    // ============================
    // CONFIG GPIO BUZZER (OUT)
    // ============================
    XGpio_Config buzzer_cfg;
    memset(&buzzer_cfg, 0, sizeof(XGpio_Config));

    buzzer_cfg.BaseAddress = GPIO_BUZZER_BASEADDR;
    buzzer_cfg.IsDual      = 0;

    status = XGpio_CfgInitialize(&buzzer_gpio, &buzzer_cfg, buzzer_cfg.BaseAddress);
    if (status != XST_SUCCESS) {
        xil_printf("Error inicializando GPIO buzzer, status=%d\r\n", status);
        return XST_FAILURE;
    }

    XGpio_SetDataDirection(&buzzer_gpio, 1, 0x00); // 1 bit salida
    buzzer_off();

    // ============================
    // CONFIG GPIO ULTRASONICO: TRIG (OUT)
    // ============================
    XGpio_Config ultra_trig_cfg;
    memset(&ultra_trig_cfg, 0, sizeof(XGpio_Config));

    ultra_trig_cfg.BaseAddress = GPIO_ULTRA_TRIG_BASEADDR;
    ultra_trig_cfg.IsDual      = 0;

    status = XGpio_CfgInitialize(&ultra_trig_gpio, &ultra_trig_cfg, ultra_trig_cfg.BaseAddress);
    if (status != XST_SUCCESS) {
        xil_printf("Error inicializando GPIO TRIG ultrasonico, status=%d\r\n", status);
        return XST_FAILURE;
    }

    XGpio_SetDataDirection(&ultra_trig_gpio, 1, 0x00); // salida
    XGpio_DiscreteWrite(&ultra_trig_gpio, 1, 0);       // TRIG en bajo

    // ============================
    // CONFIG GPIO ULTRASONICO: ECHO (IN)
    // ============================
    XGpio_Config ultra_echo_cfg;
    memset(&ultra_echo_cfg, 0, sizeof(XGpio_Config));

    ultra_echo_cfg.BaseAddress = GPIO_ULTRA_ECHO_BASEADDR;
    ultra_echo_cfg.IsDual      = 0;

    status = XGpio_CfgInitialize(&ultra_echo_gpio, &ultra_echo_cfg, ultra_echo_cfg.BaseAddress);
    if (status != XST_SUCCESS) {
        xil_printf("Error inicializando GPIO ECHO ultrasonico, status=%d\r\n", status);
        return XST_FAILURE;
    }

    XGpio_SetDataDirection(&ultra_echo_gpio, 1, 0x01); // bit 0 como entrada

    // ============================
    // ESTADO INICIAL
    // ============================
    xil_printf("Posicion inicial: CERRADO (0 grados)\r\n");
    servo_goto_angle(0);

    xil_printf("Clave configurada: %s\r\n", PASSWORD);
    xil_printf("Usa '*' para borrar, '#' para confirmar.\r\n");
    xil_printf("Teclado listo, ingresa la clave y presiona '#'.\r\n");

    memset(password_buffer, 0, sizeof(password_buffer));
    password_index = 0;

    while (1) {
        // ============================
        // 1) Lógica del TECLADO
        // ============================
        char key = scan_keypad();

        if (key != 0) {
            xil_printf("Tecla presionada: %c\r\n", key);

            if (key == '*') {
                // Borrar la clave ingresada
                memset(password_buffer, 0, sizeof(password_buffer));
                password_index = 0;
                xil_printf("Clave borrada.\r\n");
            }
            else if (key == '#') {
                // Confirmar clave
                password_buffer[password_index] = '\0'; // terminamos el string

                xil_printf("Clave ingresada: %s\r\n", password_buffer);

                if ((password_index == PASSWORD_LENGTH) &&
                    (strcmp(password_buffer, PASSWORD) == 0)) {

                    xil_printf("CLAVE CORRECTA.\r\n");
                    abrir_cerradura_5s();

                } else {
                    xil_printf("CLAVE INCORRECTA.\r\n");
                    buzzer_error_beep(); // 🔔 suena el buzzer
                }

                // Después de confirmar, vaciar buffer para siguiente intento
                memset(password_buffer, 0, sizeof(password_buffer));
                password_index = 0;
            }
            else {
                // Es un dígito '0'..'9' o símbolo
                if (password_index < PASSWORD_BUFFER_MAX) {
                    password_buffer[password_index++] = key;
                    xil_printf("Digitando clave... (%d)\r\n", password_index);
                } else {
                    xil_printf("Buffer de clave lleno, presiona '*' o '#'.\r\n");
                }
            }

            usleep(300000); // debounce ~300 ms
        }

        // ============================
        // 2) Lógica del ULTRASONICO
        // ============================
        int dist_cm;
        status = ultrasonic_measure_cm(&dist_cm);
        if (status == XST_SUCCESS) {
            xil_printf("Distancia medida: %d cm\r\n", dist_cm);
            if (dist_cm > 0 && dist_cm <= 5) {
                xil_printf("Objeto a menos de 5 cm, abriendo cerradura...\r\n");
                abrir_cerradura_5s();
            }
        }

        // pequeño delay para no saturar
        usleep(20000); // 20 ms
    }
}
