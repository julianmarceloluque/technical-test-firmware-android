/*
 * ============================================================================
 *  vcp_session.h - Maquina de estados de la SESION DE VENTA
 * ============================================================================
 *
 *  EL PROBLEMA (requisito E1.12)
 *  -----------------------------
 *  Hay dos relojes que no se llevan bien:
 *
 *    - Hacia la expendedora somos SLAVE y tenemos 5 ms para contestar.
 *    - Hacia el medio de pago somos MASTER y el cobro tarda 50 a 900 ms.
 *
 *  O sea: cuando llega un VEND_REQUEST NO podemos saber todavia si el cobro va
 *  a salir. Contestar "esperame" no es opcional, es la unica opcion fisica.
 *  Por eso el protocolo tiene VEND_PENDING.
 *
 *  La consecuencia de diseno es que la venta NO se resuelve dentro de la
 *  respuesta a una trama: se resuelve a lo largo de VARIAS interrogaciones del
 *  master. El estado tiene que sobrevivir entre tramas. Eso es esta estructura.
 *
 *  EL CICLO COMPLETO
 *  -----------------
 *
 *    INACTIVA
 *       |  llega VEND_REQUEST valido -> pago_iniciar()
 *       |  respondemos VEND_PENDING
 *       v
 *    COBRANDO ------------------------------- (el lazo principal consulta
 *       |                                      pago_estado() cada vuelta)
 *       |  pago AUTORIZADO  -> guardamos txid
 *       |  pago RECHAZADO   -> guardamos motivo 0x03
 *       |  vence el timeout -> guardamos motivo 0x05
 *       v
 *    RESULTADO_PENDIENTE
 *       |  llega el proximo STATUS_REQ -> contestamos VEND_APPROVE o VEND_DENY
 *       |
 *       +-- si fue DENY -----------------------------------> INACTIVA
 *       |
 *       v  si fue APPROVE
 *    ESPERANDO_CONFIRMA
 *       |  llega VEND_SUCCESS o VEND_FAILURE con nuestro txid -> INACTIVA
 *       |  vence VCP_CONFIRM_TIMEOUT_MS                       -> INACTIVA
 *       v                                                        (marcada como
 *    INACTIVA                                                     no confirmada)
 *
 *  REGLA DE ORO: NADA DE ESTE MODULO TRANSMITE.
 *  ------------------------------------------
 *  El requisito E1.11 dice que el controlador nunca transmite por iniciativa
 *  propia. Este modulo puede enterarse a las 14:03:07.512 de que el pago salio
 *  bien, pero no manda nada: solo deja el resultado "listo para informar". Lo
 *  informa vcp_app.c la proxima vez que el master pregunte. Si este modulo
 *  tuviera acceso a la UART, tarde o temprano alguien agregaria un "y avisale",
 *  y ahi se rompe la regla del bus multipunto.
 * ============================================================================
 */

#ifndef VCP_SESSION_H
#define VCP_SESSION_H

#include <stdint.h>
#include <stdbool.h>

#include "vcp/vcp_cfg.h"
#include "vcp/vcp_frame.h"

/* ---------------------------------------------------------------------------
 * Estados de la sesion
 * ------------------------------------------------------------------------- */
typedef enum {
    VENTA_INACTIVA = 0,          /* no hay venta en curso                    */
    VENTA_COBRANDO,              /* pago_iniciar() hecho, esperando resultado*/
    VENTA_RESULTADO_PENDIENTE,   /* hay resultado y todavia no lo informamos */
    VENTA_ESPERANDO_CONFIRMA     /* informamos APPROVE, falta VEND_SUCCESS/FAILURE */
} venta_estado_t;

/* Que hay que contestarle a un VEND_REQUEST. */
typedef enum {
    VENTA_RESP_PENDING = 0,      /* aceptado, el cobro arranco -> VEND_PENDING */
    VENTA_RESP_DENY              /* rechazado de entrada       -> VEND_DENY    */
} venta_resp_t;

/* ---------------------------------------------------------------------------
 * Contexto de la sesion
 * ------------------------------------------------------------------------- */
typedef struct {
    venta_estado_t estado;

    /* --- datos del pedido en curso --- */
    uint8_t  seleccion;
    uint16_t precio;

    /* --- resultado del cobro --- */
    bool     aprobado;        /* true = VEND_APPROVE, false = VEND_DENY       */
    uint16_t txid;            /* id de transaccion que dio el medio de pago   */
    uint8_t  motivo_deny;     /* vcp_deny_t, solo si aprobado == false        */

    /* --- tiempos --- */
    uint32_t t_inicio_cobro;  /* cuando llamamos a pago_iniciar()             */
    uint32_t t_informado;     /* cuando mandamos el primer VEND_APPROVE       */

    /* --- contadores (telemetria / defensa en la entrevista) --- */
    uint32_t n_pedidos;
    uint32_t n_aprobadas;
    uint32_t n_denegadas;
    uint32_t n_confirmadas_ok;      /* llego VEND_SUCCESS                     */
    uint32_t n_confirmadas_fail;    /* llego VEND_FAILURE                     */
    uint32_t n_sin_confirmar;       /* vencio el plazo de confirmacion (E2.4) */
    uint32_t n_txid_desconocido;    /* confirmacion que no matchea la sesion  */
    uint32_t n_retransmisiones;     /* VEND_REQUEST repetido del master       */
    uint32_t n_timeout_pago;        /* el cobro nunca resolvio (E2.8)         */
} vcp_session_t;

/* ---------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

void vcp_session_init(vcp_session_t *s);

/*
 * Llega un VEND_REQUEST dirigido a nosotros.
 *
 *   sel, precio  : ya desarmados del payload por vcp_app.c
 *   now_ms       : ahora
 *   motivo_deny  : de salida; solo valido si el retorno es VENTA_RESP_DENY
 *
 * Si acepta el pedido, ADENTRO llama a pago_iniciar(). Esa llamada no bloquea.
 */
venta_resp_t vcp_session_vend_request(vcp_session_t *s,
                                      uint8_t sel, uint16_t precio,
                                      uint32_t now_ms, uint8_t *motivo_deny);

/*
 * A llamar en cada vuelta del lazo principal.
 *
 * Es el lado MASTER del controlador: consulta pago_estado() y hace vencer los
 * plazos. Nunca transmite nada por la UART de la expendedora.
 */
void vcp_session_poll(vcp_session_t *s, uint32_t now_ms);

/* true si hay un resultado de venta esperando ser informado. Es la condicion
 * del Anexo A.4: "STATUS_RESP, salvo que haya un resultado de venta pendiente
 * de informar". */
bool vcp_session_hay_resultado(const vcp_session_t *s);

/*
 * Toma el resultado pendiente para meterlo en la respuesta a un STATUS_REQ.
 *
 * Efecto de lado deliberado (ver decision NE-03 en docs/01_E1_decisiones.md):
 *   - si es DENY    -> la sesion se cierra: el DENY se informa UNA sola vez.
 *   - si es APPROVE -> la sesion pasa a ESPERANDO_CONFIRMA y el resultado
 *                      QUEDA LATCHEADO: si el master vuelve a preguntar sin
 *                      haber confirmado, se le repite el mismo VEND_APPROVE
 *                      con el mismo txid.
 *
 * La asimetria no es un descuido: en el DENY no se movio plata, asi que perder
 * el mensaje solo cuesta que el master reintente la venta. En el APPROVE SI se
 * movio plata, asi que perder el mensaje significa cobrar sin entregar.
 */
void vcp_session_tomar_resultado(vcp_session_t *s, uint32_t now_ms,
                                 bool *aprobado, uint16_t *txid, uint8_t *motivo);

/* Llegaron las confirmaciones del master. */
void vcp_session_on_vend_success(vcp_session_t *s, uint16_t txid, uint32_t now_ms);
void vcp_session_on_vend_failure(vcp_session_t *s, uint16_t txid, uint8_t causa,
                                 uint32_t now_ms);

/* Byte de "estado" para el payload de STATUS_RESP (VCP_ESTADO_*). */
uint8_t vcp_session_estado_byte(const vcp_session_t *s);

/* Nombre legible del estado, para logs. */
const char *vcp_session_estado_nombre(venta_estado_t e);

#endif /* VCP_SESSION_H */
