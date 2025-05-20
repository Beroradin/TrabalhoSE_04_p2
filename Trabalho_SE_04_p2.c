#include <stdio.h>               
#include <string.h>              
#include <stdlib.h>              
#include "pico/stdlib.h"         
#include "hardware/adc.h"        
#include "pico/cyw43_arch.h"     // Suporte ao chip Wi-Fi CYW43 da Pico W
#include "lwip/pbuf.h"           // Estruturas de buffer de rede (Lightweight IP)
#include "lwip/tcp.h"            // Implementação do protocolo TCP (Lightweight IP)
#include "lwip/netif.h"          // Interfaces de rede para LWIP
#include <dht.h>                 // Biblioteca para sensores DHT utilizando PIO
#include "hardware/i2c.h"        
#include "hardware/pwm.h"        
#include "hardware/timer.h"      
#include "hardware/clocks.h"     
#include "hardware/gpio.h"       
#include "lib/ssd/ssd1306.h"     // Biblioteca do Display
#include "lib/ssd/font.h"       

// Credenciais WIFI
#define WIFI_SSID "SEU SSID"
#define WIFI_PASSWORD "SUA SENHA"

// Constantes e definições
#define LED_PIN CYW43_WL_GPIO_LED_PIN   // GPIO do CI CYW43
#define I2C_PORT i2c1
#define I2C_SDA 14
#define I2C_SCL 15
#define endereco 0x3C
#define WIDTH 128    
#define HEIGHT 64    
#define BUZZER 21
#define BUTTONA_PIN 5  
#define BUTTONB_PIN 6 
#define ADC_PIN 28 // GPIO para o LDR
#define RED_PIN  13
#define TEMP_MAX 31.0f // Temperatura máxima em Celsius
#define LUM_MAX 3500.0f // Valor máximo do LDR (ajustar conforme necessário)

// Estrutura do display SSD1306
ssd1306_t ssd;   

// Variáveis globais para armazenar valores dos sensores
float g_temperature_c = 0.0f;
float g_humidity = 0.0f;
float g_adc_value = 0.0f;

// Modelo do sensor DHT e pino de dados - mudar caso seja necessário
static const dht_model_t DHT_MODEL = DHT11;
static const uint DATA_PIN = 16;

// Variáveis globais de uso geral
volatile bool stateBuzzer = true; // Estado do buzzer
volatile bool stateLED = true; // Estado do LED
absolute_time_t last_interrupt_time = 0;

// Definindo a frequência desejada
#define PWM_FREQ_BUZZER 1000  // 1 kHz
#define PWM_WRAP   255   // 8 bits de wrap (256 valores)

// Protipótipos de funções
static float celsius_to_fahrenheit(float temperature);
void init_i2c();
void init_pwm();
void configurarBuzzer(uint32_t volume);
void gpio_callback(uint gpio, uint32_t events);
void configurarLED(bool enableLED);
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);

// Função principal
int main()
{
    // Inicialização de alguns fatores do sistema
    sleep_ms(500);
    stdio_init_all();
    init_i2c(); 
    init_pwm(); 
    bool cor = true;
    bool enable = false;
    uint32_t volume = 0;
    adc_init();
    adc_gpio_init(ADC_PIN); // GPIO 28 como entrada analógica
    adc_select_input(2);

    // Configuração de botões, IRQ e LED
    gpio_init(BUTTONA_PIN);
    gpio_set_dir(BUTTONA_PIN, GPIO_IN);
    gpio_pull_up(BUTTONA_PIN);
    gpio_init(BUTTONB_PIN);
    gpio_set_dir(BUTTONB_PIN, GPIO_IN);
    gpio_pull_up(BUTTONB_PIN);
    gpio_set_irq_enabled(BUTTONA_PIN, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(BUTTONB_PIN, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_callback(gpio_callback);
    irq_set_enabled(IO_IRQ_BANK0, true);
    gpio_init(RED_PIN);
    gpio_set_dir(RED_PIN, GPIO_OUT);
    gpio_put(RED_PIN, 0);

    // Inicialização do DHT
    dht_t dht;
    dht_init(&dht, DHT_MODEL, pio0, DATA_PIN, true /* pull_up */);

    char temperatura_C[16], temperatura_F[16], umidade[16]; // Mostrar os valores no display OLED

    //Inicializa a arquitetura do cyw43
    while (cyw43_arch_init())
    {
        printf("Falha ao inicializar Wi-Fi\n");
        sleep_ms(100);
        return -1;
    }

    // GPIO do CI CYW43 em nível baixo
    cyw43_arch_gpio_put(LED_PIN, 0);
    cyw43_arch_enable_sta_mode();

    // Conectar à rede WiFi - no meu caso que é WPA/WPA2, utilizei MIXED_PSK, mudar conforme necessidade
    printf("Conectando ao Wi-Fi...\n");
    while (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_MIXED_PSK, 20000))
    {
        printf("Falha ao conectar ao Wi-Fi\n");
        sleep_ms(100);
        return -1;
    }
    printf("Conectado ao Wi-Fi\n");

    // Caso seja a interface de rede padrão - imprimir o IP do dispositivo.
    if (netif_default)
    {
        printf("IP do dispositivo: %s\n", ipaddr_ntoa(&netif_default->ip_addr));
    }

    // Configura o servidor TCP - cria novos PCBs TCP. É o primeiro passo para estabelecer uma conexão TCP.
    struct tcp_pcb *server = tcp_new();
    if (!server)
    {
        printf("Falha ao criar servidor TCP\n");
        return -1;
    }

    //vincula um PCB (Protocol Control Block) TCP a um endereço IP e porta específicos.
    if (tcp_bind(server, IP_ADDR_ANY, 80) != ERR_OK)
    {
        printf("Falha ao associar servidor TCP à porta 80\n");
        return -1;
    }

    // Coloca um PCB (Protocol Control Block) TCP em modo de escuta, permitindo que ele aceite conexões de entrada.
    server = tcp_listen(server);

    // Define uma função de callback para aceitar conexões TCP de entrada. É um passo importante na configuração de servidores TCP.
    tcp_accept(server, tcp_server_accept);
    printf("Servidor ouvindo na porta 80\n");


    while (true) {

        // Limpa o buffer do display
        ssd1306_fill(&ssd, false);
        ssd1306_send_data(&ssd); // Envia os dados para o display
        dht_start_measurement(&dht);
        
        // Obtém os dados do sensor DHT11
        dht_result_t result = dht_finish_measurement_blocking(&dht, &g_humidity, &g_temperature_c);
        if (result == DHT_RESULT_OK) {
            printf("%.1f C (%.1f F), %.1f%% Umidade\n", g_temperature_c, celsius_to_fahrenheit(g_temperature_c), g_humidity);
            
            // Preparando as strings para exibição
            strcpy(temperatura_C, "Temp C: ");
            sprintf(temperatura_C + strlen(temperatura_C), "%.1f", g_temperature_c);
            
            strcpy(temperatura_F, "Temp F: ");
            sprintf(temperatura_F + strlen(temperatura_F), "%.1f", celsius_to_fahrenheit(g_temperature_c));
            
            strcpy(umidade, "Umid: ");
            sprintf(umidade + strlen(umidade), "%.1f%%", g_humidity);
        } else if (result == DHT_RESULT_TIMEOUT) {
            printf("Sensor DHT não respondeu, veja as conexoes");
        } else {
            assert(result == DHT_RESULT_BAD_CHECKSUM);
            printf("Erro de checksum, sensor DHT com defeito");
        }

        // Leitura do valor ADC
        adc_select_input(2); // Seleciona o canal ADC para o pino 28
        g_adc_value = adc_read(); // Lê o valor ADC
        printf("Valor ADC: %.1f\n", g_adc_value);

        // Desenha os elementos no display
        ssd1306_draw_string(&ssd, "EMBARCATECH", 8, 6);        // Desenha uma string
        ssd1306_draw_string(&ssd, "Sensores", 20, 16);         // Desenha uma string
        ssd1306_line(&ssd, 3, 25, 123, 25, cor);               // Desenha uma linha
        ssd1306_draw_string(&ssd, temperatura_C, 6, 30);       // Desenha uma string
        ssd1306_draw_string(&ssd, temperatura_F, 6, 40);       // Desenha uma string
        ssd1306_draw_string(&ssd, umidade, 6, 50);             // Desenha uma string

        // Envia os dados para o display
        ssd1306_send_data(&ssd);

        if (stateBuzzer && g_temperature_c > TEMP_MAX) {
            volume = 5;  // Liga o buzzer bem baixinho
        }
        else {
            volume = 0; // Desliga o buzzer
        }
        configurarBuzzer(volume);

        sleep_ms(500);

        if (stateLED && g_adc_value > LUM_MAX) { // LDR pode ser ajustado conforme iluminação ambiente
            enable = true; // Liga o LED
        }
        else {
            enable = false; // Desliga o LED
        }
        configurarLED(enable);

        cyw43_arch_poll(); // Necessário para manter o Wi-Fi ativo
        sleep_ms(100); 
    }
    //Desligar a arquitetura CYW43.
    cyw43_arch_deinit();
    return 0;
}

// Função de callback ao aceitar conexões TCP
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    tcp_recv(newpcb, tcp_server_recv);
    return ERR_OK;
}

// Função de callback para processar requisições HTTP
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    if (!p)
    {
        // Fecha a conexão do cliente
        tcp_close(tpcb);
        return ERR_OK;
    }

    // Alocação do request na memória dinâmica
    char *request = (char *)malloc(p->len + 1);
    memcpy(request, p->payload, p->len);
    request[p->len] = '\0';

    printf("Request: %s\n", request);

    // Indica se precisamos redirecioonar 
    bool redirect = false;

    // Verificar se há comandos na URL - Estava dificultando a mudança via hardware
    if (strstr(request, "GET /buzzer_off") != NULL) {
        stateBuzzer = false;
        printf("Buzzer desativado via web\n");
        redirect = true;
    } else if (strstr(request, "GET /buzzer_on") != NULL) {
        stateBuzzer = true;
        printf("Buzzer ativado via web\n");
        redirect = true;
    } else if (strstr(request, "GET /led_off") != NULL) {
        stateLED = false;
        printf("LED desativado via web\n");
        redirect = true;
    } else if (strstr(request, "GET /led_on") != NULL) {
        stateLED = true;
        printf("LED ativado via web\n");
        redirect = true;
    }

    // Se a solicitação foi para um comando, enviar redirecionamento para a página principal
    if (redirect) {
        char redirect_response[512];
        int len = snprintf(redirect_response, sizeof(redirect_response),
                 "HTTP/1.1 303 See Other\r\n"
                 "Location: /\r\n"
                 "Content-Type: text/html; charset=UTF-8\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "<html><body><h1>Redirecionando...</h1></body></html>");

        // Escreve a resposta de redirecionamento
        err_t write_err = tcp_write(tpcb, redirect_response, strlen(redirect_response), 0);
        if (write_err != ERR_OK) {
            printf("Erro ao escrever dados de redirecionamento: %d\n", write_err);
        }
    } else {
        // Cria a resposta HTML
        char html[4096];
        int len = snprintf(html, sizeof(html),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/html; charset=UTF-8\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "<!DOCTYPE html>\n"
                 "<html>\n"
                 "<head>\n"
                 "<meta charset=\"UTF-8\">\n"
                 "<title>Embarcatech - Monitoramento</title>\n"
                 "<style>\n"
                 "body { background-color:rgb(119, 136, 233); font-family: Arial, sans-serif; text-align: center; margin-top: 50px; }\n"
                 "h1 { font-size: 60px; margin-bottom: 30px; }\n"
                 ".sensor-box { background-color: #f0f0f0; border-radius: 15px; padding: 20px; margin: 20px auto; width: 80%%; max-width: 600px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); }\n"
                 ".sensor-value { font-size: 48px; margin: 10px 0; color: #333; }\n"
                 ".sensor-label { font-size: 24px; color: #666; }\n"
                 ".control-panel { background-color: #e0e0e0; border-radius: 15px; padding: 20px; margin: 20px auto; width: 80%%; max-width: 600px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); }\n"
                 ".btn { display: inline-block; background-color: #4CAF50; border: none; color: white; padding: 15px 32px; text-align: center; text-decoration: none; font-size: 16px; margin: 10px 5px; cursor: pointer; border-radius: 8px; }\n"
                 ".btn-danger { background-color: #f44336; }\n"
                 ".btn-warning { background-color: #ff9800; }\n"
                 ".btn-primary { background-color: #2196F3; }\n"
                 ".status-indicator { display: inline-block; width: 20px; height: 20px; border-radius: 50%%; margin-right: 10px; vertical-align: middle; }\n"
                 ".status-on { background-color: #4CAF50; }\n"
                 ".status-off { background-color: #f44336; }\n"
                 "</style>\n"
                 "</head>\n"
                 "<body>\n"
                 "<h1>Embarcatech: Monitoramento de Ambiente</h1>\n"
                 "<div class=\"sensor-box\">\n"
                 "  <div class=\"sensor-label\">Temperatura DHT11</div>\n"
                 "  <div class=\"sensor-value\">%.1f &deg;C</div>\n"
                 "</div>\n"
                 "<div class=\"sensor-box\">\n"
                 "  <div class=\"sensor-label\">Umidade DHT11</div>\n"
                 "  <div class=\"sensor-value\">%.1f %%</div>\n"
                 "</div>\n"
                 "<div class=\"sensor-box\">\n"
                 "  <div class=\"sensor-label\">Valor ADC (Pino 28)</div>\n"
                 "  <div class=\"sensor-value\">%.1f</div>\n"
                 "</div>\n"
                 "<div class=\"control-panel\">\n"
                 "  <h2>Controle de Periféricos</h2>\n"
                 "  <div>\n"
                 "    <span class=\"status-indicator %s\"></span>\n"
                 "    <span>Status do Buzzer: %s</span>\n"
                 "  </div>\n"
                 "  <a href=\"/buzzer_on\" class=\"btn btn-primary\">Ativar Buzzer</a>\n"
                 "  <a href=\"/buzzer_off\" class=\"btn btn-danger\">Desativar Buzzer</a>\n"
                 "  <div style=\"margin-top: 20px;\">\n"
                 "    <span class=\"status-indicator %s\"></span>\n"
                 "    <span>Status do LED: %s</span>\n"
                 "  </div>\n"
                 "  <a href=\"/led_on\" class=\"btn btn-warning\">Ativar LED</a>\n"
                 "  <a href=\"/led_off\" class=\"btn btn-danger\">Desativar LED</a>\n"
                 "</div>\n"
                 "<script>\n"
                 "setTimeout(function() {\n"
                 "  window.location.href = '/';\n"
                 "}, 5000);\n"
                 "</script>\n"
                 "</body>\n"
                 "</html>\n",
                 g_temperature_c, g_humidity, g_adc_value,
                 stateBuzzer ? "status-on" : "status-off", stateBuzzer ? "Ativado" : "Desativado",
                 stateLED ? "status-on" : "status-off", stateLED ? "Ativado" : "Desativado");

        // Verifica se o buffer é grande o suficiente
        if (len >= sizeof(html)) {
            printf("Alerta: Resposta HTML truncada\n");
        }

        // Escreve a resposta completa de uma vez
        err_t write_err = tcp_write(tpcb, html, strlen(html), 0);
        if (write_err != ERR_OK) {
            printf("Erro ao escrever dados: %d\n", write_err);
        }
    }

    // Envia os dados
    err_t out_err = tcp_output(tpcb);
    if (out_err != ERR_OK) {
        printf("Erro ao enviar dados: %d\n", out_err);
    }

    // Libera recursos
    free(request);
    pbuf_free(p);

    // Em vez de fechar a conexão imediatamente, configuramos para fechar após o envio
    tpcb->flags |= TF_RXCLOSED;
    
    return ERR_OK;
}

// Converte Celsius para Fahrenheit
static float celsius_to_fahrenheit(float temperature) {
    return temperature * (9.0f / 5) + 32;
}

// Inicializa o barramento I2C
void init_i2c(){

    // Inicialização do I2C usando 400KHz.
   i2c_init(I2C_PORT, 400 * 2000);

   gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);                    // Set the GPIO pin function to I2C
   gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);                    // Set the GPIO pin function to I2C
   gpio_pull_up(I2C_SDA);                                        // Pull up the data line
   gpio_pull_up(I2C_SCL);                                        // Pull up the clock line                   
   ssd1306_init(&ssd, WIDTH, HEIGHT, false, endereco, I2C_PORT); // Inicializa o display
   ssd1306_config(&ssd);                                         // Configura o display
   ssd1306_send_data(&ssd);                                      // Envia os dados para o display

   // Limpa o display. O display inicia com todos os pixels apagados.
   ssd1306_fill(&ssd, false);
   ssd1306_send_data(&ssd);
}

// Configura o volume do buzzer
void configurarBuzzer(uint32_t volume) {
    pwm_set_gpio_level(BUZZER, volume);
}

// Configura o LED
void configurarLED(bool enableLED) {
    if (enableLED) {
        gpio_put(RED_PIN, 1); // Liga o LED
    } else {
        gpio_put(RED_PIN, 0); // Desliga o LED
    }
}

void init_pwm() {
    
    // Inicializa o PWM para o buzzer
    gpio_set_function(BUZZER, GPIO_FUNC_PWM);
    
    // Obtém os números dos canais PWM para os pinos
    uint slice_num_buzzer = pwm_gpio_to_slice_num(BUZZER);
    
    // Configuração da frequência PWM
    pwm_set_clkdiv(slice_num_buzzer, (float)clock_get_hz(clk_sys) / PWM_FREQ_BUZZER / (PWM_WRAP + 1));
    
    // Configura o wrap do contador PWM para 8 bits (256)
    pwm_set_wrap(slice_num_buzzer, PWM_WRAP);
    
    // Habilita o PWM
    pwm_set_enabled(slice_num_buzzer, true);
}

// Configuração do GPIO para interrupção
void gpio_callback(uint gpio, uint32_t events) {
    absolute_time_t now = get_absolute_time();
    int64_t diff = absolute_time_diff_us(last_interrupt_time, now);

    if (diff < 250000) return;
    last_interrupt_time = now;

    if (gpio == BUTTONA_PIN) {
        stateBuzzer = !stateBuzzer; // Alterna o estado do buzzer
    }
    else if (gpio == BUTTONB_PIN) {
        stateLED = !stateLED; // Alterna o estado do LED
    }   
}