/*
 * ============================================================================
 *  vcp_app.c - Implementacion de la tabla de respuestas del Anexo A.4
 * ============================================================================
 *
 *  LA TABLA A.4, TAL CUAL:
 *
 *    Trama recibida            Respuesta
 *    ------------------------  --------------------------------------------
 *    STATUS_REQ                STATUS_RESP, salvo que haya un resultado de
 *                              venta pendiente -> VEND_APPROVE / VEND_DENY
 *    VEND_REQUEST              VEND_PENDING si arranca el cobro; VEND_DENY si
 *                              se rechaza de entrada
 *    VEND_SUCCESS              ACK
 *    VEND_FAILURE              ACK
 *    LOG_EVENT                 ACK
 *    dirigida a otra direccion nada (se procesa para no perder sincronia)
 *    broadcast (0x00)          nada
 *    invalida / no reconocida  nada (el master reintenta)
 * ============================================================================
 */

#include <string.h>

#include "vcp/vcp_app.h"
#include "vcp/vcp_port.h"

void vcp_app_init(vcp_app_t *app)
{
    if (app == NULL) {
        return;
    }
    memset(app, 0, sizeof(*app));
    app->addr_propia    = (uint8_t)VCP_ADDR_SELF;
    app->temperatura_dc = (int16_t)VCP_TEMP_DEFECTO_DECIMAS;
    app->plazo_estricto = true;
    vcp_session_init(&app->venta);
}

/* Deja la estructura de decision en un estado conocido. */
static void dec_reset(vcp_decision_t *dec, uint32_t retardo_ms)
{
    memset(dec, 0, sizeof(*dec));
    dec->tipo       = VCP_DEC_CMD_NO_ADMITIDO;
    dec->retardo_ms = retardo_ms;
    dec->nota       = "";
}

/* ---------------------------------------------------------------------------
 * vcp_app_on_frame
 * ------------------------------------------------------------------------- */
void vcp_app_on_frame(vcp_app_t *app, const vcp_frame_t *f, uint32_t now_ms,
                      vcp_decision_t *dec)
{
    vcp_decision_t local;      /* usado si el llamador pasa NULL */
    int            n;
    uint32_t       retardo;

    if ((app == NULL) || (f == NULL)) {
        return;
    }
    if (dec == NULL) {
        dec = &local;
    }

    /* Cuanto paso desde el ETX de esta trama hasta este instante.
     * Resta sin signo: aguanta la vuelta del contador de milisegundos. */
    retardo = (uint32_t)(now_ms - f->t_etx_ms);
    dec_reset(dec, retardo);

    /* ====================================================================
     * PASO 1 - FILTRO DE DIRECCION (requisito E1.13)
     * ====================================================================
     *
     * El bus es multipunto: sobre la misma linea cuelgan el validador de
     * billetes, el monedero y otros medios de pago. Todos escuchan todo.
     * Si respondemos una trama que no es nuestra, chocamos con la respuesta
     * del destinatario legitimo y se corrompen las dos (Anexo A.2).
     *
     * "Procesar" una trama ajena significa: contarla, mantener la sincronia
     * del receptor, y NADA MAS. En particular NO toca la sesion de venta:
     * un VEND_REQUEST dirigido a 0x0B es una venta de OTRO dispositivo; si
     * arrancaramos un cobro por ella estariamos cobrando por una operacion
     * que no nos corresponde. (Decision NE-07.)
     */
    if (f->addr == (uint8_t)VCP_ADDR_BROADCAST) {
        app->rx_broadcast++;
        dec->tipo = VCP_DEC_BROADCAST;
        dec->nota = "broadcast: se procesa, no se responde";
        if (f->cmd == (uint8_t)VCP_CMD_LOG_EVENT) {
            app->log_events++;
            app->log_bytes += f->len;
        }
        return;
    }

    if (f->addr != app->addr_propia) {
        app->rx_ajenas++;
        dec->tipo = VCP_DEC_NO_ES_MIA;
        dec->nota = "otra direccion: se procesa, no se responde";
        return;
    }

    app->rx_propias++;

    /* ====================================================================
     * PASO 2 - ¿ES UN COMANDO QUE EL MASTER NOS PUEDE MANDAR?
     * ====================================================================
     *
     * El Anexo A.3 tiene una columna "Direccion" y hay que respetarla. Si por
     * la linea llega un VEND_APPROVE dirigido a nuestra direccion, eso no es
     * una orden: VEND_APPROVE es algo que emitimos NOSOTROS.
     *
     * Esto no es teorico: en el vector del Anexo B hay exactamente una de esas
     * (offset 42), escondida adentro del payload de un LOG_EVENT con CRC roto.
     * El propio enunciado avisa: "puede contener tramas que en operacion normal
     * no deberian circular en ese sentido. Tratalas como corresponda".
     *
     * Criterio: se registra y NO se responde. Es el mismo tratamiento que un
     * comando desconocido, y la tabla A.4 dice "nada" para ese caso.
     * (Decision NE-05.)
     */
    if (!vcp_cmd_es_del_master(f->cmd)) {
        app->rx_cmd_no_admitido++;
        dec->tipo = VCP_DEC_CMD_NO_ADMITIDO;
        dec->nota = "comando no admitido en este sentido: no se responde";
        return;
    }

    /* ====================================================================
     * PASO 3 - ¿EL LEN COINCIDE CON LO QUE EL COMANDO EXIGE?
     * ====================================================================
     *
     * Una trama puede tener CRC perfecto y aun asi ser incoherente: por
     * ejemplo un VEND_REQUEST con LEN=2 cuando el Anexo A.3 pide 3 bytes.
     * El CRC solo dice "los bytes llegaron como salieron", no "los bytes
     * tienen sentido".
     *
     * Si no validaramos esto, leeriamos payload[1] y payload[2] de una trama
     * que solo tiene 2 bytes: leeriamos basura del buffer. El chequeo es lo
     * que permite que el resto del codigo acceda al payload sin miedo.
     *
     * Criterio: no se responde (tabla A.4, "comando no reconocido"). El master
     * reintenta. (Decision NE-09.)
     */
    if (!vcp_len_esperada_ok(f->cmd, f->len)) {
        app->rx_len_inesperado++;
        dec->tipo = VCP_DEC_LEN_INESPERADO;
        dec->nota = "LEN no coincide con el comando: no se responde";
        return;
    }

    /* ====================================================================
     * PASO 4 - DECIDIR LA RESPUESTA (tabla A.4)
     * ==================================================================== */
    switch (f->cmd) {

    /* ---------------------------------------------------------------- */
    case VCP_CMD_STATUS_REQ: {
        if (vcp_session_hay_resultado(&app->venta)) {
            /* Hay un resultado de venta esperando: tiene PRIORIDAD sobre el
             * estado. Es la excepcion que marca la tabla A.4. */
            bool     aprobado = false;
            uint16_t txid     = 0u;
            uint8_t  motivo   = 0u;

            vcp_session_tomar_resultado(&app->venta, now_ms,
                                        &aprobado, &txid, &motivo);

            if (aprobado) {
                dec->resp_cmd = (uint8_t)VCP_CMD_VEND_APPROVE;
                dec->resp_len = 2u;
                vcp_put_u16be(dec->resp_payload, txid);
                dec->nota = "resultado de venta pendiente: se informa APPROVE";
            } else {
                dec->resp_cmd        = (uint8_t)VCP_CMD_VEND_DENY;
                dec->resp_len        = 1u;
                dec->resp_payload[0] = motivo;
                dec->nota = "resultado de venta pendiente: se informa DENY";
            }
        } else {
            /* Caso normal: la interrogacion periodica del master.
             *
             * Payload de STATUS_RESP (Anexo A.3), 4 bytes:
             *   [0] estado (u8)
             *   [1] reservado (u8)
             *   [2..3] temperatura en decimas de grado, int16 BIG-ENDIAN
             *
             * El campo "reservado" va en 0x00. Es lo unico razonable: mandar
             * basura ahi ataria a un futuro uso del campo. (Decision NE-08.)
             */
            dec->resp_cmd        = (uint8_t)VCP_CMD_STATUS_RESP;
            dec->resp_len        = 4u;
            dec->resp_payload[0] = vcp_session_estado_byte(&app->venta);
            dec->resp_payload[1] = 0x00u;
            vcp_put_u16be(&dec->resp_payload[2], (uint16_t)app->temperatura_dc);
            dec->nota = "interrogacion periodica";
        }
        break;
    }

    /* ---------------------------------------------------------------- */
    case VCP_CMD_VEND_REQUEST: {
        uint8_t      sel    = f->payload[0];
        uint16_t     precio = vcp_get_u16be(&f->payload[1]);
        uint8_t      motivo = 0u;
        venta_resp_t r;

        r = vcp_session_vend_request(&app->venta, sel, precio, now_ms, &motivo);

        if (r == VENTA_RESP_PENDING) {
            dec->resp_cmd = (uint8_t)VCP_CMD_VEND_PENDING;
            dec->resp_len = 0u;
            dec->nota     = "pedido aceptado, cobro en curso";
        } else {
            dec->resp_cmd        = (uint8_t)VCP_CMD_VEND_DENY;
            dec->resp_len        = 1u;
            dec->resp_payload[0] = motivo;
            dec->nota            = vcp_deny_nombre(motivo);
        }
        break;
    }

    /* ---------------------------------------------------------------- */
    case VCP_CMD_VEND_SUCCESS: {
        uint16_t txid = vcp_get_u16be(&f->payload[0]);
        vcp_session_on_vend_success(&app->venta, txid, now_ms);
        dec->resp_cmd = (uint8_t)VCP_CMD_ACK;
        dec->resp_len = 0u;
        dec->nota     = "confirmacion de entrega";
        break;
    }

    /* ---------------------------------------------------------------- */
    case VCP_CMD_VEND_FAILURE: {
        uint16_t txid  = vcp_get_u16be(&f->payload[0]);
        uint8_t  causa = f->payload[2];
        vcp_session_on_vend_failure(&app->venta, txid, causa, now_ms);
        dec->resp_cmd = (uint8_t)VCP_CMD_ACK;
        dec->resp_len = 0u;
        dec->nota     = "fallo de entrega: hay que devolver";
        break;
    }

    /* ---------------------------------------------------------------- */
    case VCP_CMD_LOG_EVENT:
        /* Contenido arbitrario; no lo interpretamos, lo registramos. */
        app->log_events++;
        app->log_bytes += f->len;
        dec->resp_cmd = (uint8_t)VCP_CMD_ACK;
        dec->resp_len = 0u;
        dec->nota     = "evento de log registrado";
        break;

    /* ---------------------------------------------------------------- */
    default:
        /* No deberia llegar: el paso 2 ya filtro los comandos ajenos. */
        app->rx_cmd_no_admitido++;
        dec->tipo = VCP_DEC_CMD_NO_ADMITIDO;
        dec->nota = "comando no reconocido";
        return;
    }

    dec->hay_respuesta = true;

    /* ====================================================================
     * PASO 5 - SERIALIZAR
     * ====================================================================
     * Se responde SIEMPRE con nuestra propia direccion: el master sabe a quien
     * interrogo y espera la respuesta de esa direccion.
     */
    n = vcp_build(app->addr_propia, dec->resp_cmd,
                  dec->resp_payload, dec->resp_len,
                  dec->tx, sizeof(dec->tx));

    if (n < 0) {
        /* Solo puede pasar por un bug propio (payload mal armado). Se cuenta
         * como comando no admitido y no se transmite: mejor callarse que
         * mandar una trama malformada al bus. */
        dec->tipo          = VCP_DEC_CMD_NO_ADMITIDO;
        dec->hay_respuesta = false;
        dec->nota          = "error interno armando la respuesta";
        return;
    }
    dec->tx_len = (uint8_t)n;

    /* ====================================================================
     * PASO 6 - GUARDIA DE PLAZO (Anexo A.2 / requisito E1.14)
     * ====================================================================
     *
     * "El plazo es duro: si no llegas, no contestes." El enunciado explica por
     * que: una respuesta tardia se superpone con la trama siguiente del master
     * o, peor, llega justo a tiempo para que el master la tome como respuesta
     * al comando NUEVO. No contestar solo cuesta un reintento.
     *
     * ¿Cuando puede pasar esto en este diseno? En un solo caso: cuando la
     * trama fue RECUPERADA reprocesando bytes viejos (ver vcp_rx.h). Ahi el
     * ETX ocurrio en el pasado y el retardo es real, no un artefacto.
     *
     * Fijate donde esta este chequeo: DESPUES de decidir y armar la respuesta.
     * Es a proposito. El estado interno (la sesion de venta) SI se actualiza
     * -la trama llego y es valida, ignorarla nos dejaria desincronizados con
     * la expendedora-; lo unico que se suprime es la EMISION.
     */
    if (app->plazo_estricto && (retardo > (uint32_t)VCP_PLAZO_RESPUESTA_MS)) {
        app->tx_omitidas_plazo++;
        dec->tipo        = VCP_DEC_FUERA_DE_PLAZO;
        dec->transmitida = false;
        return;
    }

    /* ====================================================================
     * PASO 7 - TRANSMITIR
     * ====================================================================
     * Este es el UNICO llamado a vcp_uart_write() de todo el firmware, y esta
     * dentro del manejo de una trama recibida. De ahi sale el cumplimiento del
     * requisito E1.11 por construccion, no por disciplina.
     */
    vcp_uart_write(dec->tx, (size_t)dec->tx_len);
    app->tx_emitidas++;
    dec->tipo        = VCP_DEC_RESPONDER;
    dec->transmitida = true;
}

/* ---------------------------------------------------------------------------
 * vcp_app_poll
 * ------------------------------------------------------------------------- */
void vcp_app_poll(vcp_app_t *app, uint32_t now_ms)
{
    if (app == NULL) {
        return;
    }

    /*
     * Lo unico que pasa aca es que el cobro avanza y los plazos vencen.
     * Ni una linea transmite. Si el pago se autoriza en este instante, el
     * resultado queda guardado y espera a que el master pregunte.
     */
    vcp_session_poll(&app->venta, now_ms);
}

const char *vcp_dec_nombre(vcp_decision_tipo_t t)
{
    switch (t) {
    case VCP_DEC_RESPONDER:       return "RESPONDER";
    case VCP_DEC_NO_ES_MIA:       return "NO_ES_MIA";
    case VCP_DEC_BROADCAST:       return "BROADCAST";
    case VCP_DEC_CMD_NO_ADMITIDO: return "CMD_NO_ADMITIDO";
    case VCP_DEC_LEN_INESPERADO:  return "LEN_INESPERADO";
    case VCP_DEC_FUERA_DE_PLAZO:  return "FUERA_DE_PLAZO";
    default:                      return "?";
    }
}
