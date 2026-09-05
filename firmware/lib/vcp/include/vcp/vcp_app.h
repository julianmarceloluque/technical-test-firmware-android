/*
 * ============================================================================
 *  vcp_app.h - Capa de aplicacion: decide QUE responder a cada trama (A.4)
 * ============================================================================
 *
 *  DONDE ENCAJA
 *  ------------
 *      UART ISR
 *         | byte + marca de tiempo
 *         v
 *      vcp_rx.c        (¿esto es una trama? ¿esta sana?)
 *         | evento VCP_EV_FRAME
 *         v
 *      vcp_app.c       <-- ESTE MODULO
 *         |   ¿es para mi?  ¿que comando es?  ¿que corresponde contestar?
 *         |   ¿llego a tiempo?
 *         v
 *      vcp_build() -> vcp_uart_write()
 *
 *      y en paralelo, desde el lazo principal:
 *      vcp_app_poll() -> vcp_session_poll() -> pago_estado()
 *
 *  LAS DOS REGLAS QUE ESTE MODULO HACE CUMPLIR
 *  -------------------------------------------
 *
 *  E1.11 - "el controlador nunca transmite por iniciativa propia"
 *      La UNICA funcion de todo el proyecto que llama a vcp_uart_write() es
 *      vcp_app_on_frame(), y solo se la llama desde el evento VCP_EV_FRAME.
 *      No hay ninguna otra ruta hacia la linea. No hay temporizadores que
 *      transmitan, no hay "avisar cuando el pago resuelva".
 *
 *  E1.14 - "ninguna respuesta puede emitirse fuera de plazo ni corresponder a
 *           una trama anterior"
 *      La respuesta se ARMA a partir de la trama que se acaba de recibir, en
 *      la misma llamada, sobre una variable local. No existe un buffer de
 *      "respuesta pendiente" que alguien pueda transmitir mas tarde: si el
 *      plazo vencio, la respuesta simplemente no se manda y se descarta.
 *      Estructuralmente es imposible contestar una trama vieja.
 * ============================================================================
 */

#ifndef VCP_APP_H
#define VCP_APP_H

#include <stdint.h>
#include <stdbool.h>

#include "vcp/vcp_cfg.h"
#include "vcp/vcp_frame.h"
#include "vcp/vcp_rx.h"
#include "vcp/vcp_session.h"

/* ---------------------------------------------------------------------------
 * Que hizo la aplicacion con una trama
 * ------------------------------------------------------------------------- */
typedef enum {
    VCP_DEC_RESPONDER = 0,     /* se transmitio una respuesta                */
    VCP_DEC_NO_ES_MIA,         /* otra direccion: procesada, sin transmitir  */
    VCP_DEC_BROADCAST,         /* broadcast: procesada, sin transmitir       */
    VCP_DEC_CMD_NO_ADMITIDO,   /* comando desconocido o de sentido inverso   */
    VCP_DEC_LEN_INESPERADO,    /* el comando existe pero el LEN no cierra    */
    VCP_DEC_FUERA_DE_PLAZO     /* correspondia responder pero vencieron 5 ms */
} vcp_decision_tipo_t;

/*
 * Descripcion de la decision. La devuelve vcp_app_on_frame() para que el
 * banco de pruebas y los tests puedan ver QUE se decidio sin tener que
 * espiar la UART. En el firmware real se puede ignorar (pasar NULL).
 */
typedef struct {
    vcp_decision_tipo_t tipo;

    /* La respuesta que correspondia (valida incluso si no se transmitio,
     * para poder mostrar "esto habria contestado si hubiera llegado a tiempo"). */
    bool     hay_respuesta;
    uint8_t  resp_cmd;
    uint8_t  resp_len;
    uint8_t  resp_payload[VCP_MAX_PAYLOAD];

    /* La trama ya serializada, tal cual iria (o fue) a la linea. */
    uint8_t  tx[VCP_TRAMA_MAX_BYTES];
    uint8_t  tx_len;
    bool     transmitida;

    uint32_t retardo_ms;       /* now - t_etx: cuanto tardamos en decidir     */
    const char *nota;          /* texto corto para el log; nunca NULL         */
} vcp_decision_t;

/* ---------------------------------------------------------------------------
 * Contexto de la aplicacion
 * ------------------------------------------------------------------------- */
typedef struct {
    vcp_session_t venta;

    uint8_t  addr_propia;        /* normalmente VCP_ADDR_SELF                 */
    int16_t  temperatura_dc;     /* decimas de grado; en el equipo real, sensor*/

    /*
     * Si es true se aplica el corte del Anexo A.2: una trama detectada mas de
     * 5 ms despues de su ETX NO se contesta. Es lo correcto y es el valor por
     * defecto. El banco de pruebas permite apagarlo (--sin-plazo) solo para
     * mostrar la diferencia entre "que habria contestado" y "que contesto".
     */
    bool     plazo_estricto;

    /* --- contadores --- */
    uint32_t tx_emitidas;
    uint32_t tx_omitidas_plazo;
    uint32_t rx_propias;
    uint32_t rx_broadcast;
    uint32_t rx_ajenas;
    uint32_t rx_cmd_no_admitido;
    uint32_t rx_len_inesperado;
    uint32_t log_events;         /* LOG_EVENT recibidos                       */
    uint32_t log_bytes;          /* bytes de log acumulados                   */
} vcp_app_t;

/* ---------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

void vcp_app_init(vcp_app_t *app);

/*
 * Procesa una trama VALIDA (ya verificada por vcp_rx) y, si corresponde,
 * transmite la respuesta.
 *
 *   now_ms : ahora. Se compara contra f->t_etx_ms para el plazo de 5 ms.
 *   dec    : de salida, puede ser NULL.
 *
 * No bloquea. No hay ni una espera entre la entrada y vcp_uart_write().
 */
void vcp_app_on_frame(vcp_app_t *app, const vcp_frame_t *f, uint32_t now_ms,
                      vcp_decision_t *dec);

/*
 * A llamar en cada vuelta del lazo principal: hace avanzar el cobro y los
 * plazos. NO transmite (requisito E1.11).
 */
void vcp_app_poll(vcp_app_t *app, uint32_t now_ms);

/* Nombre legible de la decision, para logs. */
const char *vcp_dec_nombre(vcp_decision_tipo_t t);

#endif /* VCP_APP_H */
