#pragma once

#include "driver/uart.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*cdc_uart_rx_callback_t)(int uart, void *usr_data, uint8_t *data, size_t size);

typedef struct
{
    cdc_uart_rx_callback_t func;
    void *usr_data;
} cdc_uart_cb_t;

bool cdc_uart_init(uart_port_t uart, gpio_num_t tx_pin, gpio_num_t rx_pin, int buadrate);
bool cdc_uart_set_baudrate(uint32_t baudrate);
bool cdc_uart_get_baudrate(uint32_t *baudrate);
bool cdc_uart_write(const void *src, size_t size);
void cdc_uart_register_rx_callback(const cdc_uart_cb_t *cb);

#ifdef __cplusplus
}
#endif