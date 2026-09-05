/*
 * ============================================================================
 *  test_app.c - Pruebas de la tabla A.4 y de la sesion de venta
 * ============================================================================
 *
 *  Cubre E1.10 (respuesta correcta a cada trama), E1.11 (nunca transmitir por
 *  iniciativa propia), E1.12 (la sesion sobrevive entre interrogaciones),
 *  E1.13 (filtrado por direccion) y E1.14 (nada fuera de plazo).
 * ============================================================================
 */

#include "test_util.h"
#include "vcp/vcp.h"
#include "port_pc.h"

/* Arma una trama "recibida" sin pasar por la UART: para probar la capa de
 * aplicacion sola. El receptor ya se prueba en test_rx.c. */
static vcp_frame_t hacer(uint8_t addr, uint8_t cmd,
                         const uint8_t *pay, uint8_t len, uint32_t t_etx)
{
    vcp_frame_t f;
    memset(&f, 0, sizeof(f));
    f.addr     = addr;
    f.cmd      = cmd;
    f.len      = len;
    f.t_etx_ms = t_etx;
    if ((len > 0u) && (pay != NULL)) {
        memcpy(f.payload, pay, len);
    }
    return f;
}

static void preparar(vcp_app_t *app)
{
    vcp_app_init(app);
    pago_sim_init(0u);
    puart_set_eco(false);
    puart_reset();
}

void tests_app(void)
{
    test_grupo("Aplicacion: filtrado por direccion (E1.13)");

    {
        vcp_app_t      app;
        vcp_decision_t d;
        vcp_frame_t    f;

        preparar(&app);

        f = hacer(0x0Au, (uint8_t)VCP_CMD_STATUS_REQ, NULL, 0u, 1000u);
        vcp_app_on_frame(&app, &f, 1000u, &d);
        VERIFICAR_EQ(d.tipo, VCP_DEC_RESPONDER, "trama propia: se responde");
        VERIFICAR_EQ(d.resp_cmd, VCP_CMD_STATUS_RESP, "  con STATUS_RESP");

        f = hacer(0x0Bu, (uint8_t)VCP_CMD_STATUS_REQ, NULL, 0u, 1000u);
        vcp_app_on_frame(&app, &f, 1000u, &d);
        VERIFICAR_EQ(d.tipo, VCP_DEC_NO_ES_MIA, "trama ajena: NO se responde");
        VERIFICAR(!d.transmitida, "  y no se toca la UART");

        f = hacer(0x00u, (uint8_t)VCP_CMD_STATUS_REQ, NULL, 0u, 1000u);
        vcp_app_on_frame(&app, &f, 1000u, &d);
        VERIFICAR_EQ(d.tipo, VCP_DEC_BROADCAST, "broadcast: NO se responde");

        VERIFICAR_EQ(puart_n_escrituras(), 1u,
                     "en total solo hubo UNA escritura a la UART");
    }

    /* --- Una VEND_REQUEST dirigida a otro NO puede iniciar un cobro --- */
    {
        vcp_app_t      app;
        vcp_decision_t d;
        uint8_t        pay[3];
        vcp_frame_t    f;

        preparar(&app);
        pay[0] = 24u;
        vcp_put_u16be(&pay[1], 1800u);

        f = hacer(0x0Bu, (uint8_t)VCP_CMD_VEND_REQUEST, pay, 3u, 100u);
        vcp_app_on_frame(&app, &f, 100u, &d);
        VERIFICAR_EQ(pago_sim_n_iniciados(), 0u,
                     "un VEND_REQUEST para otra direccion no arranca cobro");
        VERIFICAR_EQ(app.venta.estado, VENTA_INACTIVA,
                     "  ni toca la sesion (decision NE-07)");
    }

    test_grupo("Aplicacion: comandos de sentido inverso (NE-05)");

    {
        vcp_app_t      app;
        vcp_decision_t d;
        uint8_t        pay[2] = { 0x00u, 0x2Au };
        vcp_frame_t    f;

        preparar(&app);

        /* Es la trama que aparece escondida en el offset 42 del Anexo B. */
        f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_APPROVE, pay, 2u, 100u);
        vcp_app_on_frame(&app, &f, 100u, &d);
        VERIFICAR_EQ(d.tipo, VCP_DEC_CMD_NO_ADMITIDO,
                     "VEND_APPROVE recibido: no es una orden, no se responde");

        f = hacer(0x0Au, (uint8_t)VCP_CMD_ACK, NULL, 0u, 100u);
        vcp_app_on_frame(&app, &f, 100u, &d);
        VERIFICAR_EQ(d.tipo, VCP_DEC_CMD_NO_ADMITIDO, "ACK recibido: tampoco");

        f = hacer(0x0Au, 0x99u, NULL, 0u, 100u);
        vcp_app_on_frame(&app, &f, 100u, &d);
        VERIFICAR_EQ(d.tipo, VCP_DEC_CMD_NO_ADMITIDO,
                     "comando inventado: tampoco");

        VERIFICAR_EQ(puart_n_escrituras(), 0u, "ninguna respuesta transmitida");
    }

    test_grupo("Aplicacion: LEN incoherente con el comando (NE-09)");

    {
        vcp_app_t      app;
        vcp_decision_t d;
        uint8_t        pay[2] = { 24u, 0x07u };
        vcp_frame_t    f;

        preparar(&app);

        f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_REQUEST, pay, 2u, 100u);
        vcp_app_on_frame(&app, &f, 100u, &d);
        VERIFICAR_EQ(d.tipo, VCP_DEC_LEN_INESPERADO,
                     "VEND_REQUEST con LEN=2 (necesita 3): no se responde");
        VERIFICAR_EQ(pago_sim_n_iniciados(), 0u,
                     "  y sobre todo: no arranca ningun cobro");

        f = hacer(0x0Au, (uint8_t)VCP_CMD_STATUS_REQ, pay, 2u, 100u);
        vcp_app_on_frame(&app, &f, 100u, &d);
        VERIFICAR_EQ(d.tipo, VCP_DEC_LEN_INESPERADO,
                     "STATUS_REQ con payload: no se responde");
    }

    test_grupo("Aplicacion: STATUS_RESP");

    {
        vcp_app_t      app;
        vcp_decision_t d;
        vcp_frame_t    f;
        const uint8_t  esperado[] = { 0x02u, 0x0Au, 0x04u, 0x11u,
                                      0x00u, 0x00u, 0x00u, 0xEBu };

        preparar(&app);
        app.temperatura_dc = 235;   /* 23.5 C */

        f = hacer(0x0Au, (uint8_t)VCP_CMD_STATUS_REQ, NULL, 0u, 100u);
        vcp_app_on_frame(&app, &f, 100u, &d);

        VERIFICAR_EQ(d.resp_len, 4u, "STATUS_RESP lleva 4 bytes de payload");
        VERIFICAR_EQ(d.resp_payload[0], VCP_ESTADO_LISTO, "estado = LISTO");
        VERIFICAR_EQ(d.resp_payload[1], 0x00u, "reservado = 0x00");
        VERIFICAR_EQ(vcp_get_u16be(&d.resp_payload[2]), 235u,
                     "temperatura 23.5 C = 235 decimas, big-endian");
        VERIFICAR_MEM(d.tx, esperado, sizeof(esperado),
                      "la trama emitida empieza como corresponde");
        VERIFICAR_EQ(d.tx[d.tx_len - 1u], VCP_ETX, "y termina en ETX");
    }

    /* --- Temperatura negativa: el int16 con signo tiene que sobrevivir --- */
    {
        vcp_app_t      app;
        vcp_decision_t d;
        vcp_frame_t    f;

        preparar(&app);
        app.temperatura_dc = -55;   /* -5.5 C, una maquina refrigerada */

        f = hacer(0x0Au, (uint8_t)VCP_CMD_STATUS_REQ, NULL, 0u, 100u);
        vcp_app_on_frame(&app, &f, 100u, &d);
        VERIFICAR_EQ((int16_t)vcp_get_u16be(&d.resp_payload[2]), -55,
                     "temperatura negativa viaja bien (complemento a dos)");
    }

    test_grupo("Sesion de venta: caso completo (E1.12)");

    {
        vcp_app_t      app;
        vcp_decision_t d;
        uint8_t        pay[3];
        vcp_frame_t    f;
        uint16_t       txid;

        preparar(&app);
        pago_sim_programar(PAGO_AUTORIZADO, 250u, 0x1234u);

        /* 1) llega el pedido */
        pay[0] = 24u;
        vcp_put_u16be(&pay[1], 1800u);
        f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_REQUEST, pay, 3u, 0u);
        vcp_app_on_frame(&app, &f, 0u, &d);
        VERIFICAR_EQ(d.resp_cmd, VCP_CMD_VEND_PENDING,
                     "pedido valido -> VEND_PENDING");
        VERIFICAR_EQ(pago_sim_n_iniciados(), 1u, "  y arranca el cobro");
        VERIFICAR_EQ(app.venta.estado, VENTA_COBRANDO, "  estado COBRANDO");

        /* 2) el master pregunta mientras el cobro esta en curso */
        pago_sim_tick(100u);
        vcp_app_poll(&app, 100u);
        f = hacer(0x0Au, (uint8_t)VCP_CMD_STATUS_REQ, NULL, 0u, 100u);
        vcp_app_on_frame(&app, &f, 100u, &d);
        VERIFICAR_EQ(d.resp_cmd, VCP_CMD_STATUS_RESP,
                     "cobro en curso -> sigue contestando STATUS_RESP");
        VERIFICAR_EQ(d.resp_payload[0], VCP_ESTADO_OCUPADO, "  estado OCUPADO");

        /* 3) el cobro resuelve; OJO: el poll NO transmite (E1.11) */
        {
            uint32_t antes = puart_n_escrituras();
            pago_sim_tick(300u);
            vcp_app_poll(&app, 300u);
            VERIFICAR_EQ(puart_n_escrituras(), antes,
                         "E1.11: el cobro resolvio y NO se transmitio nada");
            VERIFICAR_EQ(app.venta.estado, VENTA_RESULTADO_PENDIENTE,
                         "  el resultado queda esperando a que pregunten");
        }

        /* 4) recien ahora, al preguntar, se informa */
        f = hacer(0x0Au, (uint8_t)VCP_CMD_STATUS_REQ, NULL, 0u, 300u);
        vcp_app_on_frame(&app, &f, 300u, &d);
        VERIFICAR_EQ(d.resp_cmd, VCP_CMD_VEND_APPROVE,
                     "hay resultado pendiente -> VEND_APPROVE (excepcion A.4)");
        txid = vcp_get_u16be(d.resp_payload);
        VERIFICAR_EQ(txid, 0x1234u, "  con el txid del medio de pago");
        VERIFICAR_EQ(app.venta.estado, VENTA_ESPERANDO_CONFIRMA,
                     "  y pasa a esperar la confirmacion");

        /* 5) el APPROVE queda latcheado si el master vuelve a preguntar */
        f = hacer(0x0Au, (uint8_t)VCP_CMD_STATUS_REQ, NULL, 0u, 400u);
        vcp_app_on_frame(&app, &f, 400u, &d);
        VERIFICAR_EQ(d.resp_cmd, VCP_CMD_VEND_APPROVE,
                     "sin confirmar: se repite el APPROVE (NE-03)");
        VERIFICAR_EQ(vcp_get_u16be(d.resp_payload), 0x1234u,
                     "  con el MISMO txid (para que el master lo deduplique)");

        /* 6) llega la confirmacion */
        {
            uint8_t p2[2];
            vcp_put_u16be(p2, 0x1234u);
            f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_SUCCESS, p2, 2u, 500u);
            vcp_app_on_frame(&app, &f, 500u, &d);
            VERIFICAR_EQ(d.resp_cmd, VCP_CMD_ACK, "VEND_SUCCESS -> ACK");
            VERIFICAR_EQ(app.venta.estado, VENTA_INACTIVA, "  sesion cerrada");
            VERIFICAR_EQ(app.venta.n_confirmadas_ok, 1u, "  venta confirmada");
        }

        /* 7) y vuelve a contestar el estado normal */
        f = hacer(0x0Au, (uint8_t)VCP_CMD_STATUS_REQ, NULL, 0u, 600u);
        vcp_app_on_frame(&app, &f, 600u, &d);
        VERIFICAR_EQ(d.resp_cmd, VCP_CMD_STATUS_RESP, "vuelve a STATUS_RESP");
        VERIFICAR_EQ(d.resp_payload[0], VCP_ESTADO_LISTO, "  estado LISTO");
    }

    test_grupo("Sesion de venta: rechazos");

    /* --- Pago rechazado --- */
    {
        vcp_app_t      app;
        vcp_decision_t d;
        uint8_t        pay[3];
        vcp_frame_t    f;

        preparar(&app);
        pago_sim_programar(PAGO_RECHAZADO, 100u, 0u);

        pay[0] = 5u;
        vcp_put_u16be(&pay[1], 900u);
        f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_REQUEST, pay, 3u, 0u);
        vcp_app_on_frame(&app, &f, 0u, &d);
        VERIFICAR_EQ(d.resp_cmd, VCP_CMD_VEND_PENDING, "primero VEND_PENDING");

        pago_sim_tick(200u);
        vcp_app_poll(&app, 200u);
        f = hacer(0x0Au, (uint8_t)VCP_CMD_STATUS_REQ, NULL, 0u, 200u);
        vcp_app_on_frame(&app, &f, 200u, &d);
        VERIFICAR_EQ(d.resp_cmd, VCP_CMD_VEND_DENY, "cobro rechazado -> DENY");
        VERIFICAR_EQ(d.resp_payload[0], VCP_DENY_PAGO, "  motivo 0x03");

        /* El DENY se informa UNA sola vez (NE-03). */
        f = hacer(0x0Au, (uint8_t)VCP_CMD_STATUS_REQ, NULL, 0u, 300u);
        vcp_app_on_frame(&app, &f, 300u, &d);
        VERIFICAR_EQ(d.resp_cmd, VCP_CMD_STATUS_RESP,
                     "el DENY no se repite: vuelve STATUS_RESP");
    }

    /* --- Rangos --- */
    {
        vcp_app_t      app;
        vcp_decision_t d;
        uint8_t        pay[3];
        vcp_frame_t    f;

        /* seleccion 0 */
        preparar(&app);
        pay[0] = 0u; vcp_put_u16be(&pay[1], 1000u);
        f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_REQUEST, pay, 3u, 0u);
        vcp_app_on_frame(&app, &f, 0u, &d);
        VERIFICAR_EQ(d.resp_payload[0], VCP_DENY_SELECCION, "seleccion 0 -> 0x02");

        /* seleccion 81 */
        preparar(&app);
        pay[0] = 81u; vcp_put_u16be(&pay[1], 1000u);
        f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_REQUEST, pay, 3u, 0u);
        vcp_app_on_frame(&app, &f, 0u, &d);
        VERIFICAR_EQ(d.resp_payload[0], VCP_DENY_SELECCION, "seleccion 81 -> 0x02");

        /* precio 49 */
        preparar(&app);
        pay[0] = 10u; vcp_put_u16be(&pay[1], 49u);
        f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_REQUEST, pay, 3u, 0u);
        vcp_app_on_frame(&app, &f, 0u, &d);
        VERIFICAR_EQ(d.resp_payload[0], VCP_DENY_PRECIO, "precio 49 -> 0x01");

        /* precio 20001 */
        preparar(&app);
        pay[0] = 10u; vcp_put_u16be(&pay[1], 20001u);
        f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_REQUEST, pay, 3u, 0u);
        vcp_app_on_frame(&app, &f, 0u, &d);
        VERIFICAR_EQ(d.resp_payload[0], VCP_DENY_PRECIO, "precio 20001 -> 0x01");

        /* los limites SI son validos */
        preparar(&app);
        pay[0] = 1u; vcp_put_u16be(&pay[1], 50u);
        f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_REQUEST, pay, 3u, 0u);
        vcp_app_on_frame(&app, &f, 0u, &d);
        VERIFICAR_EQ(d.resp_cmd, VCP_CMD_VEND_PENDING, "sel=1 precio=50 se acepta");

        preparar(&app);
        pay[0] = 80u; vcp_put_u16be(&pay[1], 20000u);
        f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_REQUEST, pay, 3u, 0u);
        vcp_app_on_frame(&app, &f, 0u, &d);
        VERIFICAR_EQ(d.resp_cmd, VCP_CMD_VEND_PENDING,
                     "sel=80 precio=20000 se acepta");

        /* los dos mal: se informa la seleccion (NE-04) */
        preparar(&app);
        pay[0] = 0u; vcp_put_u16be(&pay[1], 10u);
        f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_REQUEST, pay, 3u, 0u);
        vcp_app_on_frame(&app, &f, 0u, &d);
        VERIFICAR_EQ(d.resp_payload[0], VCP_DENY_SELECCION,
                     "sel y precio invalidos: se informa la seleccion (NE-04)");
    }

    test_grupo("Sesion de venta: reintentos del master (NE-01, NE-02)");

    /* --- Retransmision del mismo pedido: idempotente --- */
    {
        vcp_app_t      app;
        vcp_decision_t d;
        uint8_t        pay[3];
        vcp_frame_t    f;

        preparar(&app);
        pago_sim_programar(PAGO_AUTORIZADO, 500u, 0u);
        pay[0] = 7u; vcp_put_u16be(&pay[1], 1500u);

        f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_REQUEST, pay, 3u, 0u);
        vcp_app_on_frame(&app, &f, 0u, &d);
        VERIFICAR_EQ(d.resp_cmd, VCP_CMD_VEND_PENDING, "primer pedido: PENDING");

        f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_REQUEST, pay, 3u, 100u);
        vcp_app_on_frame(&app, &f, 100u, &d);
        VERIFICAR_EQ(d.resp_cmd, VCP_CMD_VEND_PENDING,
                     "el mismo pedido repetido: PENDING otra vez, no DENY");
        VERIFICAR_EQ(pago_sim_n_iniciados(), 1u,
                     "  y NO se inicia un segundo cobro (no se cobra dos veces)");
    }

    /* --- Pedido distinto con sesion abierta: DENY 0x04 --- */
    {
        vcp_app_t      app;
        vcp_decision_t d;
        uint8_t        pay[3];
        vcp_frame_t    f;

        preparar(&app);
        pago_sim_programar(PAGO_AUTORIZADO, 500u, 0u);

        pay[0] = 7u; vcp_put_u16be(&pay[1], 1500u);
        f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_REQUEST, pay, 3u, 0u);
        vcp_app_on_frame(&app, &f, 0u, &d);

        pay[0] = 9u; vcp_put_u16be(&pay[1], 800u);
        f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_REQUEST, pay, 3u, 100u);
        vcp_app_on_frame(&app, &f, 100u, &d);
        VERIFICAR_EQ(d.resp_payload[0], VCP_DENY_EN_CURSO,
                     "otro producto con sesion abierta -> DENY 0x04");
        VERIFICAR_EQ(pago_sim_n_iniciados(), 1u, "  sin iniciar otro cobro");

        /* Y el estado pesa mas que el rango (NE-01): pedido invalido con
         * sesion abierta tambien da 0x04. */
        pay[0] = 0u; vcp_put_u16be(&pay[1], 5u);
        f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_REQUEST, pay, 3u, 150u);
        vcp_app_on_frame(&app, &f, 150u, &d);
        VERIFICAR_EQ(d.resp_payload[0], VCP_DENY_EN_CURSO,
                     "con sesion abierta, 0x04 gana sobre el error de rango");
    }

    test_grupo("Sesion de venta: plazos propios (E2.4, E2.8)");

    /* --- El cobro nunca resuelve --- */
    {
        vcp_app_t      app;
        vcp_decision_t d;
        uint8_t        pay[3];
        vcp_frame_t    f;

        preparar(&app);
        pago_sim_programar(PAGO_AUTORIZADO, PAGO_SIM_NUNCA, 0u);

        pay[0] = 33u; vcp_put_u16be(&pay[1], 2500u);
        f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_REQUEST, pay, 3u, 0u);
        vcp_app_on_frame(&app, &f, 0u, &d);

        pago_sim_tick(2000u);
        vcp_app_poll(&app, 2000u);
        VERIFICAR_EQ(app.venta.estado, VENTA_COBRANDO,
                     "a los 2 s todavia se espera el cobro");

        pago_sim_tick(3500u);
        vcp_app_poll(&app, 3500u);
        VERIFICAR_EQ(app.venta.estado, VENTA_RESULTADO_PENDIENTE,
                     "a los 3.5 s vence el plazo del medio de pago");

        f = hacer(0x0Au, (uint8_t)VCP_CMD_STATUS_REQ, NULL, 0u, 3500u);
        vcp_app_on_frame(&app, &f, 3500u, &d);
        VERIFICAR_EQ(d.resp_payload[0], VCP_DENY_INTERNO,
                     "se informa DENY 0x05 error interno, no 0x03 pago rechazado");
        VERIFICAR_EQ(app.venta.n_timeout_pago, 1u, "queda contabilizado");
    }

    /* --- El master nunca confirma --- */
    {
        vcp_app_t      app;
        vcp_decision_t d;
        uint8_t        pay[3];
        vcp_frame_t    f;

        preparar(&app);
        pago_sim_programar(PAGO_AUTORIZADO, 50u, 0x8821u);

        pay[0] = 24u; vcp_put_u16be(&pay[1], 1800u);
        f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_REQUEST, pay, 3u, 0u);
        vcp_app_on_frame(&app, &f, 0u, &d);

        pago_sim_tick(100u);
        vcp_app_poll(&app, 100u);
        f = hacer(0x0Au, (uint8_t)VCP_CMD_STATUS_REQ, NULL, 0u, 100u);
        vcp_app_on_frame(&app, &f, 100u, &d);
        VERIFICAR_EQ(d.resp_cmd, VCP_CMD_VEND_APPROVE, "se informa el APPROVE");

        vcp_app_poll(&app, 1500u);
        VERIFICAR_EQ(app.venta.estado, VENTA_ESPERANDO_CONFIRMA,
                     "a 1.4 s del informe todavia se espera confirmacion");

        vcp_app_poll(&app, 2200u);
        VERIFICAR_EQ(app.venta.estado, VENTA_INACTIVA,
                     "a los 2 s se cierra la sesion sin confirmar");
        VERIFICAR_EQ(app.venta.n_sin_confirmar, 1u,
                     "queda anotada para el backend (caso E2.4)");
    }

    /* --- Confirmacion con txid desconocido --- */
    {
        vcp_app_t      app;
        vcp_decision_t d;
        uint8_t        pay[2];
        vcp_frame_t    f;

        preparar(&app);
        vcp_put_u16be(pay, 0x002Au);        /* como en el Anexo B */
        f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_SUCCESS, pay, 2u, 0u);
        vcp_app_on_frame(&app, &f, 0u, &d);
        VERIFICAR_EQ(d.resp_cmd, VCP_CMD_ACK,
                     "VEND_SUCCESS con txid desconocido: se ACKea igual (NE-06)");
        VERIFICAR_EQ(app.venta.n_txid_desconocido, 1u, "  pero queda registrado");
        VERIFICAR_EQ(app.venta.estado, VENTA_INACTIVA, "  y no toca la sesion");
    }

    test_grupo("Aplicacion: guardia de plazo (E1.14)");

    {
        vcp_app_t      app;
        vcp_decision_t d;
        vcp_frame_t    f;

        preparar(&app);

        /* ETX en t=1000, decidimos en t=1004: 4 ms, dentro del plazo. */
        f = hacer(0x0Au, (uint8_t)VCP_CMD_STATUS_REQ, NULL, 0u, 1000u);
        vcp_app_on_frame(&app, &f, 1004u, &d);
        VERIFICAR_EQ(d.tipo, VCP_DEC_RESPONDER, "retardo de 4 ms: se responde");

        /* Justo en el limite: 5 ms. */
        f = hacer(0x0Au, (uint8_t)VCP_CMD_STATUS_REQ, NULL, 0u, 2000u);
        vcp_app_on_frame(&app, &f, 2005u, &d);
        VERIFICAR_EQ(d.tipo, VCP_DEC_RESPONDER, "retardo de 5 ms: se responde");

        /* Un milisegundo tarde. */
        f = hacer(0x0Au, (uint8_t)VCP_CMD_STATUS_REQ, NULL, 0u, 3000u);
        vcp_app_on_frame(&app, &f, 3006u, &d);
        VERIFICAR_EQ(d.tipo, VCP_DEC_FUERA_DE_PLAZO, "retardo de 6 ms: NO se responde");
        VERIFICAR(!d.transmitida, "  la trama NO sale a la linea");
        VERIFICAR(d.tx_len > 0u,
                  "  pero se muestra que habria contestado (para el log)");
        VERIFICAR_EQ(app.tx_omitidas_plazo, 1u, "  y queda contabilizada");
    }

    /* --- Aun fuera de plazo, el estado interno se actualiza --- */
    {
        vcp_app_t      app;
        vcp_decision_t d;
        uint8_t        pay[3];
        vcp_frame_t    f;

        preparar(&app);
        pago_sim_programar(PAGO_AUTORIZADO, 100u, 0u);

        pay[0] = 24u; vcp_put_u16be(&pay[1], 1800u);
        f = hacer(0x0Au, (uint8_t)VCP_CMD_VEND_REQUEST, pay, 3u, 1000u);
        vcp_app_on_frame(&app, &f, 1050u, &d);   /* 50 ms tarde */

        VERIFICAR_EQ(d.tipo, VCP_DEC_FUERA_DE_PLAZO,
                     "VEND_REQUEST detectado tarde: no se contesta");
        VERIFICAR_EQ(app.venta.estado, VENTA_COBRANDO,
                     "  PERO el cobro SI arranca: la trama llego y es valida");
        VERIFICAR_EQ(pago_sim_n_iniciados(), 1u,
                     "  ignorarla nos dejaria desincronizados con la maquina");
    }

    test_grupo("E1.11: el controlador nunca transmite solo");

    {
        vcp_app_t app;
        int       i;
        uint32_t  t;

        preparar(&app);
        pago_sim_programar(PAGO_AUTORIZADO, 100u, 0u);

        /* Se gira el lazo principal 5 segundos sin recibir NI UNA trama. */
        for (i = 0; i < 5000; i++) {
            t = (uint32_t)i;
            pago_sim_tick(t);
            vcp_app_poll(&app, t);
        }
        VERIFICAR_EQ(puart_n_escrituras(), 0u,
                     "5 s de lazo principal sin tramas: cero transmisiones");
    }
}
