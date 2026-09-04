/*
 * vcp.h — Receptor de tramas VCP-1
 * Control Global · Unidad Vending Control
 *
 * Interfaz SUGERIDA para el ejercicio E1. Podés modificarla, ampliarla o
 * reemplazarla si tu diseño lo requiere; solo justificá el cambio en el README.
 *
 * El controlador es SLAVE frente a la expendedora: nunca inicia una
 * transmisión, solo responde. Frente al medio de pago es master.
 */

#ifndef VCP_H
#define VCP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define VCP_STX             0x02u
#define VCP_ETX             0x03u
#define VCP_MAX_PAYLOAD     64u
#define VCP_ADDR_SELF       0x0Au   /* dirección de este controlador */
#define VCP_ADDR_BROADCAST  0x00u

typedef enum {
    VCP_EV_NONE = 0,        /* byte consumido, nada que reportar        */
    VCP_EV_FRAME,           /* trama completa y validada                */
    VCP_EV_ERR_CRC,         /* CRC no coincide                          */
    VCP_EV_ERR_LEN,         /* LEN fuera de rango                       */
    VCP_EV_ERR_FRAMING,     /* no apareció ETX donde correspondía       */
    VCP_EV_ERR_TIMEOUT      /* timeout entre bytes con trama en curso   */
} vcp_event_t;

typedef struct {
    uint8_t addr;
    uint8_t cmd;
    uint8_t len;
    uint8_t payload[VCP_MAX_PAYLOAD];
} vcp_frame_t;

typedef struct {
    /* Estado interno del receptor. Definilo vos. */
    int placeholder;
} vcp_rx_t;

/* Inicializa el contexto. Debe poder llamarse en cualquier momento. */
void vcp_rx_init(vcp_rx_t *rx);

/*
 * Procesa un byte recibido.
 *   now_ms: marca de tiempo monotónica en milisegundos del momento de recepción.
 *   out:    se completa únicamente cuando el retorno es VCP_EV_FRAME.
 * No debe bloquear ni reservar memoria.
 */
vcp_event_t vcp_rx_byte(vcp_rx_t *rx, uint8_t byte, uint32_t now_ms, vcp_frame_t *out);

/*
 * A invocar periódicamente desde el lazo principal para que el timeout entre
 * bytes pueda vencer aun cuando dejen de llegar bytes.
 */
vcp_event_t vcp_rx_tick(vcp_rx_t *rx, uint32_t now_ms);

/* CRC-8, poly 0x07, init 0x00, sin reflexión, sin XOR final. */
uint8_t vcp_crc8(const uint8_t *data, size_t len);

/* ------------------------------------------------------------------ */
/* Emisión                                                             */
/* ------------------------------------------------------------------ */

/* Comandos (Anexo A.3). */
typedef enum {
    VCP_CMD_ACK          = 0x01,
    VCP_CMD_STATUS_REQ   = 0x10,
    VCP_CMD_STATUS_RESP  = 0x11,
    VCP_CMD_VEND_REQUEST = 0x20,
    VCP_CMD_VEND_APPROVE = 0x21,
    VCP_CMD_VEND_DENY    = 0x22,
    VCP_CMD_VEND_PENDING = 0x23,
    VCP_CMD_VEND_SUCCESS = 0x30,
    VCP_CMD_VEND_FAILURE = 0x31,
    VCP_CMD_LOG_EVENT    = 0x40
} vcp_cmd_t;

/* Códigos de motivo de VEND_DENY (Anexo A.4). */
typedef enum {
    VCP_DENY_PRECIO     = 0x01,
    VCP_DENY_SELECCION  = 0x02,
    VCP_DENY_PAGO       = 0x03,
    VCP_DENY_EN_CURSO   = 0x04,
    VCP_DENY_INTERNO    = 0x05
} vcp_deny_t;

/*
 * Arma una trama completa en out. Retorna la cantidad de bytes escritos,
 * o un valor negativo si no se pudo. Implementala vos.
 */
int vcp_build(uint8_t addr, uint8_t cmd, const uint8_t *payload, uint8_t len,
              uint8_t *out, size_t out_size);

/* ------------------------------------------------------------------ */
/* Entorno provisto — no lo implementes, usalo                         */
/* ------------------------------------------------------------------ */

/*
 * Escribe bytes en la línea serie. En el banco de pruebas para PC alcanza
 * con que imprima la trama en hexadecimal.
 */
void vcp_uart_write(const uint8_t *data, size_t len);

/*
 * Medio de pago. El controlador es master de este lado: inicia el cobro y
 * después consulta. El cobro tarda entre 50 y 900 ms en resolverse, muy por
 * encima del plazo de respuesta de 5 ms hacia la expendedora.
 * No hay versión bloqueante, y es a propósito.
 */
typedef enum {
    PAGO_EN_CURSO = 0,
    PAGO_AUTORIZADO,
    PAGO_RECHAZADO
} pago_estado_t;

void          pago_iniciar(uint16_t centavos);
pago_estado_t pago_estado(uint16_t *txid);

#endif /* VCP_H */
