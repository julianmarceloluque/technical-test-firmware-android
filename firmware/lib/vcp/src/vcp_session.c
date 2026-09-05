/*
 * ============================================================================
 *  vcp_session.c - Maquina de estados de la sesion de venta
 * ============================================================================
 *
 *  Leer primero vcp_session.h: ahi esta el diagrama del ciclo completo.
 *
 *  Los comentarios marcados con [E2:...] senalan los puntos exactos donde el
 *  ejercicio E2 (consistencia ante corte de energia) propone escribir en la
 *  memoria no volatil. No estan implementados porque E2 es escrito, pero
 *  quedan anclados al codigo real para que el documento y el programa hablen
 *  de lo mismo.
 * ============================================================================
 */

#include <string.h>

#include "vcp/vcp_session.h"
#include "vcp/vcp_port.h"

void vcp_session_init(vcp_session_t *s)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->estado = VENTA_INACTIVA;
}

/* ---------------------------------------------------------------------------
 * Llega un VEND_REQUEST
 * ------------------------------------------------------------------------- */
venta_resp_t vcp_session_vend_request(vcp_session_t *s,
                                      uint8_t sel, uint16_t precio,
                                      uint32_t now_ms, uint8_t *motivo_deny)
{
    s->n_pedidos++;

    /* ---- (1) ¿Es una RETRANSMISION del mismo pedido? -------------------
     *
     * Situacion real: mandamos VEND_PENDING, el ruido se lo come, y el master
     * (que reintenta cada 100 ms, Anexo A.2) vuelve a mandar el MISMO
     * VEND_REQUEST.
     *
     * Si tratamos ese reintento como "pedido nuevo", caeriamos en el caso (2)
     * y contestariamos VEND_DENY 0x04 "sesion en curso"... abortando una venta
     * cuyo cobro YA ARRANCO. El cliente se quedaria sin producto y con el
     * cobro potencialmente hecho. Es el peor resultado posible.
     *
     * Criterio adoptado: si el pedido tiene la misma seleccion y el mismo
     * precio y la sesion esta cobrando, es un reintento -> se repite la misma
     * respuesta (VEND_PENDING). La operacion se vuelve IDEMPOTENTE, que es lo
     * que uno quiere de cualquier protocolo con reintentos.
     *
     * Riesgo asumido: si la expendedora pidiera legitimamente dos veces el
     * mismo producto al mismo precio mientras el primer cobro sigue abierto,
     * la trataria como una sola. Es aceptable: una expendedora no vende dos
     * unidades en paralelo, tiene un solo mecanismo de entrega.
     * (Decision NE-02 en docs/01_E1_decisiones.md.)
     */
    if ((s->estado == VENTA_COBRANDO) &&
        (sel == s->seleccion) && (precio == s->precio)) {
        s->n_retransmisiones++;
        return VENTA_RESP_PENDING;
    }

    /* ---- (2) ¿Hay una sesion abierta? ----------------------------------
     *
     * Se chequea ANTES que los rangos a proposito. Si estamos ocupados, el
     * motivo relevante para la expendedora es "estoy ocupado" (0x04), no
     * "ademas tu seleccion es invalida". El estado del sistema pesa mas que la
     * validacion del contenido: es lo que le dice al master que espere en vez
     * de que corrija el pedido. (Decision NE-01.)
     */
    if (s->estado != VENTA_INACTIVA) {
        *motivo_deny = (uint8_t)VCP_DENY_EN_CURSO;
        s->n_denegadas++;
        return VENTA_RESP_DENY;
    }

    /* ---- (3) Validacion de rangos (Anexo A.4) --------------------------
     *
     * Primero la seleccion y despues el precio. Si los dos estan mal se
     * informa "seleccion invalida", porque la seleccion identifica QUE se
     * quiere vender: sin un producto valido, discutir el precio no tiene
     * sentido. (Decision NE-04.)
     */
    if ((sel < (uint8_t)VCP_SEL_MIN) || (sel > (uint8_t)VCP_SEL_MAX)) {
        *motivo_deny = (uint8_t)VCP_DENY_SELECCION;
        s->n_denegadas++;
        return VENTA_RESP_DENY;
    }
    if ((precio < (uint16_t)VCP_PRECIO_MIN) || (precio > (uint16_t)VCP_PRECIO_MAX)) {
        *motivo_deny = (uint8_t)VCP_DENY_PRECIO;
        s->n_denegadas++;
        return VENTA_RESP_DENY;
    }

    /* ---- (4) Arranca el cobro ------------------------------------------
     *
     * [E2: PUNTO DE ESCRITURA NVM #1]
     * Aca, ANTES de llamar a pago_iniciar(), es donde el ejercicio E2 escribe
     * el registro de intencion de cobro. La razon es simple: si escribimos
     * DESPUES y se corta la luz en el medio, al arrancar no sabriamos que
     * hubo un cobro en vuelo. Ver docs/03_E2_energia.md, punto E2.2.
     */
    s->seleccion      = sel;
    s->precio         = precio;
    s->txid           = 0u;
    s->aprobado       = false;
    s->motivo_deny    = 0u;
    s->t_inicio_cobro = now_ms;
    s->estado         = VENTA_COBRANDO;

    pago_iniciar(precio);

    return VENTA_RESP_PENDING;
}

/* ---------------------------------------------------------------------------
 * Lazo principal: el lado MASTER hacia el medio de pago
 * ------------------------------------------------------------------------- */
void vcp_session_poll(vcp_session_t *s, uint32_t now_ms)
{
    if (s == NULL) {
        return;
    }

    switch (s->estado) {

    case VENTA_COBRANDO: {
        uint16_t      txid = 0u;
        pago_estado_t e    = pago_estado(&txid);

        if (e == PAGO_AUTORIZADO) {
            /* [E2: PUNTO DE ESCRITURA NVM #2] - cobro confirmado con su txid.
             * Este es el momento en que el dinero ya se movio: si el registro
             * no queda en NVM ahora, un corte deja plata cobrada sin rastro. */
            s->txid     = txid;
            s->aprobado = true;
            s->estado   = VENTA_RESULTADO_PENDIENTE;
            s->n_aprobadas++;

        } else if (e == PAGO_RECHAZADO) {
            s->aprobado    = false;
            s->motivo_deny = (uint8_t)VCP_DENY_PAGO;
            s->estado      = VENTA_RESULTADO_PENDIENTE;
            s->n_denegadas++;

        } else if ((uint32_t)(now_ms - s->t_inicio_cobro) > (uint32_t)VCP_PAGO_TIMEOUT_MS) {
            /*
             * EL COBRO NUNCA RESUELVE (ejercicio E2.8).
             *
             * Somos master de este lado: si el esclavo no contesta nunca,
             * el que tiene que poner un plazo somos nosotros. Sin este corte,
             * la sesion queda COBRANDO para siempre y el controlador queda
             * inutil: todo VEND_REQUEST posterior recibe DENY 0x04 y la
             * maquina deja de vender aunque el resto funcione.
             *
             * Que le contestamos mientras tanto: VEND_PENDING a los reintentos
             * y STATUS_RESP (estado OCUPADO) a las interrogaciones. Recien al
             * vencer el plazo damos un resultado, y es DENY 0x05 "error
             * interno", no 0x03 "pago rechazado": no sabemos si se cobro, y
             * mentirle al master diciendo que el medio de pago rechazo seria
             * cerrar la ambiguedad del lado equivocado.
             */
            s->aprobado    = false;
            s->motivo_deny = (uint8_t)VCP_DENY_INTERNO;
            s->estado      = VENTA_RESULTADO_PENDIENTE;
            s->n_timeout_pago++;
            s->n_denegadas++;
        }
        break;
    }

    case VENTA_ESPERANDO_CONFIRMA:
        /*
         * Ya informamos VEND_APPROVE y el master no confirmo ni con
         * VEND_SUCCESS ni con VEND_FAILURE.
         *
         * [E2: PUNTO DE ESCRITURA NVM #3] - venta cerrada sin confirmacion:
         * es el caso E2.4, el unico en el que el sistema NO PUEDE SABER si el
         * producto se entrego. Se cierra la sesion (si no, el controlador
         * queda trabado) y se deja anotado para el backend.
         */
        if ((uint32_t)(now_ms - s->t_informado) > (uint32_t)VCP_CONFIRM_TIMEOUT_MS) {
            s->n_sin_confirmar++;
            s->estado = VENTA_INACTIVA;
        }
        break;

    case VENTA_INACTIVA:
    case VENTA_RESULTADO_PENDIENTE:
    default:
        /* Nada que hacer: estos estados solo avanzan por tramas recibidas. */
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Informe del resultado
 * ------------------------------------------------------------------------- */
bool vcp_session_hay_resultado(const vcp_session_t *s)
{
    /*
     * Ojo con ESPERANDO_CONFIRMA: tambien cuenta como "resultado pendiente".
     * Mientras el master no confirme, cada STATUS_REQ vuelve a recibir el
     * mismo VEND_APPROVE. Ver el comentario de vcp_session_tomar_resultado().
     */
    return (s->estado == VENTA_RESULTADO_PENDIENTE) ||
           (s->estado == VENTA_ESPERANDO_CONFIRMA);
}

void vcp_session_tomar_resultado(vcp_session_t *s, uint32_t now_ms,
                                 bool *aprobado, uint16_t *txid, uint8_t *motivo)
{
    *aprobado = s->aprobado;
    *txid     = s->txid;
    *motivo   = s->motivo_deny;

    if (s->estado == VENTA_RESULTADO_PENDIENTE) {
        if (s->aprobado) {
            /*
             * APPROVE: se LATCHEA. Pasamos a esperar confirmacion y arrancamos
             * el reloj del plazo. Si el master vuelve a preguntar sin haber
             * confirmado, hay_resultado() sigue dando true y le repetimos
             * exactamente el mismo VEND_APPROVE con el mismo txid.
             *
             * Por que repetir y no mandar STATUS_RESP: si nuestra respuesta se
             * perdio en el ruido, el master no sabe que hay una venta aprobada
             * y nunca va a entregar el producto, pero la plata ya se cobro.
             * Repetir el mismo txid es seguro porque el master puede detectar
             * el duplicado (mismo id de transaccion) y entregar una sola vez.
             */
            s->t_informado = now_ms;
            s->estado      = VENTA_ESPERANDO_CONFIRMA;
        } else {
            /*
             * DENY: se informa UNA sola vez y la sesion se cierra.
             *
             * Si el mensaje se pierde, el master reintenta el STATUS_REQ, le
             * contestamos STATUS_RESP normal, y como no hay venta abierta va a
             * reintentar el VEND_REQUEST. Como en el DENY no se movio plata,
             * el peor caso es un reintento. Latchear un DENY, en cambio,
             * bloquearia el proximo pedido legitimo.
             */
            s->estado = VENTA_INACTIVA;
        }
    }
    /* Si ya estabamos en ESPERANDO_CONFIRMA no se toca nada: el plazo de
     * confirmacion se cuenta desde el PRIMER informe, no desde el ultimo. */
}

/* ---------------------------------------------------------------------------
 * Confirmaciones del master
 * ------------------------------------------------------------------------- */
void vcp_session_on_vend_success(vcp_session_t *s, uint16_t txid, uint32_t now_ms)
{
    (void)now_ms;

    /*
     * ¿Y si el txid no coincide con nuestra sesion, o no hay sesion?
     *
     * Pasa de verdad: es lo que ocurre en el vector del Anexo B, donde llega un
     * VEND_SUCCESS txid=0x002A sin que hayamos aprobado nada (la captura es de
     * laboratorio). Tambien puede pasar en campo si el controlador se reinicio
     * despues de aprobar (ejercicio E3).
     *
     * Criterio: se cuenta, se registra, y AUN ASI se responde ACK. El ACK es
     * un acuse de RECEPCION a nivel protocolo, no una aprobacion semantica.
     * No responder haria que el master reintente 3 veces y despues nos declare
     * fuera de servicio (Anexo A.2), lo cual seria un problema mucho mayor que
     * un txid huerfano. (Decision NE-06.)
     */
    if ((s->estado != VENTA_ESPERANDO_CONFIRMA) || (txid != s->txid)) {
        s->n_txid_desconocido++;
        return;
    }

    /* [E2: PUNTO DE ESCRITURA NVM #4] - venta cerrada y confirmada. */
    s->n_confirmadas_ok++;
    s->estado = VENTA_INACTIVA;
}

void vcp_session_on_vend_failure(vcp_session_t *s, uint16_t txid, uint8_t causa,
                                 uint32_t now_ms)
{
    (void)causa;    /* la causa se registra para el backend; no cambia el flujo */
    (void)now_ms;

    if ((s->estado != VENTA_ESPERANDO_CONFIRMA) || (txid != s->txid)) {
        s->n_txid_desconocido++;
        return;
    }

    /*
     * [E2: PUNTO DE ESCRITURA NVM #5] - venta cobrada y NO entregada.
     * Este registro es el que dispara la devolucion. Es el mas importante de
     * los cinco: si se pierde, el cliente pago y no reclama nadie.
     */
    s->n_confirmadas_fail++;
    s->estado = VENTA_INACTIVA;
}

/* ---------------------------------------------------------------------------
 * Estado para STATUS_RESP
 * ------------------------------------------------------------------------- */
uint8_t vcp_session_estado_byte(const vcp_session_t *s)
{
    return (s->estado == VENTA_INACTIVA) ? (uint8_t)VCP_ESTADO_LISTO
                                         : (uint8_t)VCP_ESTADO_OCUPADO;
}

const char *vcp_session_estado_nombre(venta_estado_t e)
{
    switch (e) {
    case VENTA_INACTIVA:            return "INACTIVA";
    case VENTA_COBRANDO:            return "COBRANDO";
    case VENTA_RESULTADO_PENDIENTE: return "RESULTADO_PENDIENTE";
    case VENTA_ESPERANDO_CONFIRMA:  return "ESPERANDO_CONFIRMA";
    default:                        return "?";
    }
}
