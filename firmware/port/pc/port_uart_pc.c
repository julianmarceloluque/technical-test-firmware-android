/*
 * ============================================================================
 *  port_uart_pc.c - Implementacion de vcp_uart_write() para el banco de PC
 * ============================================================================
 *
 *  El Anexo A.5 dice: "En el banco de pruebas para PC alcanza con que imprima
 *  la trama en hexadecimal, para poder verificar que habrias transmitido".
 *  Eso es exactamente lo que hace.
 *
 *  Ademas guarda la ultima trama y lleva contadores, para que los tests puedan
 *  verificar la emision sin tener que parsear la salida de texto.
 *
 *  Nota sobre el requisito E1.2 ("sin variables globales mutables"): ese
 *  requisito aplica a la LOGICA DE PROTOCOLO, cuyo estado vive todo en
 *  vcp_rx_t / vcp_app_t. Este archivo es el SIMULADOR DEL ENTORNO, el
 *  equivalente al periferico de hardware; ahi el estado global es lo natural
 *  (un micro tiene un solo UART) y ademas es lo que hace que la firma provista
 *  vcp_uart_write(data, len), que no recibe contexto, sea implementable.
 * ============================================================================
 */

#include <stdio.h>
#include <string.h>

#include "port_pc.h"
#include "vcp/vcp_cfg.h"

static bool        s_eco      = true;
static const char *s_prefijo  = "[UART TX]";
static uint32_t    s_n_escrit = 0u;
static uint32_t    s_n_bytes  = 0u;
static uint8_t     s_ultima[VCP_TRAMA_MAX_BYTES];
static size_t      s_ultima_n = 0u;

void vcp_uart_write(const uint8_t *data, size_t len)
{
    size_t i;

    if ((data == NULL) || (len == 0u)) {
        return;
    }

    s_n_escrit++;
    s_n_bytes += (uint32_t)len;

    /* Guardamos copia de la ultima trama (recortada al maximo posible). */
    s_ultima_n = (len > sizeof(s_ultima)) ? sizeof(s_ultima) : len;
    memcpy(s_ultima, data, s_ultima_n);

    if (s_eco) {
        printf("%s", s_prefijo);
        for (i = 0u; i < len; i++) {
            printf(" %02X", (unsigned)data[i]);
        }
        printf("\n");
    }
}

void puart_set_eco(bool eco)
{
    s_eco = eco;
}

void puart_set_prefijo(const char *p)
{
    s_prefijo = (p != NULL) ? p : "";
}

uint32_t puart_n_escrituras(void)
{
    return s_n_escrit;
}

uint32_t puart_n_bytes(void)
{
    return s_n_bytes;
}

size_t puart_ultima(uint8_t *dst, size_t max)
{
    size_t n = (s_ultima_n > max) ? max : s_ultima_n;
    if ((dst != NULL) && (n > 0u)) {
        memcpy(dst, s_ultima, n);
    }
    return n;
}

void puart_reset(void)
{
    s_n_escrit = 0u;
    s_n_bytes  = 0u;
    s_ultima_n = 0u;
    memset(s_ultima, 0, sizeof(s_ultima));
}
