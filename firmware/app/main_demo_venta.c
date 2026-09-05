/*
 * ============================================================================
 *  main_demo_venta.c - Demostracion de la SESION DE VENTA (requisito E1.12)
 * ============================================================================
 *
 *  POR QUE EXISTE ESTE PROGRAMA
 *  ----------------------------
 *  El vector del Anexo B no contiene ni un solo VEND_REQUEST valido: el unico
 *  que trae (offset 27) viene con el CRC roto a proposito. O sea que con el
 *  vector solo NO se puede mostrar el camino mas interesante del ejercicio:
 *  que pasa cuando llega un pedido de venta, el cobro tarda 250 ms y el plazo
 *  de respuesta es de 5 ms.
 *
 *  Este programa simula la EXPENDEDORA (el master): arma tramas de verdad con
 *  vcp_build(), se las entrega byte a byte al receptor con un reloj simulado a
 *  9600 baudios, y muestra el dialogo completo.
 *
 *  Es el mismo stack que corre en el equipo real. No hay atajos: las tramas de
 *  la maquina pasan por la maquina de estados de recepcion, por el CRC y por
 *  el filtro de direccion antes de llegar a la logica de venta.
 *
 *  ESCENARIOS
 *  ----------
 *    1  venta feliz: pedido -> PENDING -> (cobro) -> APPROVE -> SUCCESS
 *    2  pago rechazado por el medio de pago
 *    3  seleccion y precio fuera de rango
 *    4  VEND_REQUEST retransmitido (idempotencia)
 *    5  segundo pedido distinto con sesion abierta
 *    6  el cobro nunca resuelve (ejercicio E2.8)
 *    7  el master nunca confirma la entrega (ejercicio E2.4)
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vcp/vcp.h"
#include "port_pc.h"

/* A 9600 8N1 cada byte son 10 bits: 10/9600 s = 1.0417 ms. */
#define MS_POR_BYTE  1.0417

/* ---------------------------------------------------------------------------
 * La expendedora simulada
 * ------------------------------------------------------------------------- */
typedef struct {
    vcp_rx_t  rx;
    vcp_app_t app;
    double    reloj;          /* ms con decimales, para que se vea el detalle */
} maquina_t;

static uint32_t ahora(const maquina_t *m)
{
    return (uint32_t)m->reloj;
}

static void hex(const uint8_t *d, size_t n)
{
    size_t i;
    for (i = 0u; i < n; i++) {
        printf("%02X%s", (unsigned)d[i], (i + 1u < n) ? " " : "");
    }
}

/* ---------------------------------------------------------------------------
 * Callback: aca se ve la respuesta del controlador
 * ------------------------------------------------------------------------- */
static void on_evento(void *usuario, const vcp_rx_evento_t *ev)
{
    maquina_t     *m = (maquina_t *)usuario;
    vcp_decision_t dec;

    if (ev->ev != VCP_EV_FRAME) {
        if (ev->ev != VCP_EV_BYTE_FUERA_TRAMA) {
            printf("  t=%7.2f ms  RX ERROR    %s\n",
                   m->reloj, vcp_ev_nombre(ev->ev));
        }
        return;
    }

    vcp_app_on_frame(&m->app, ev->trama, ev->now_ms, &dec);

    if (dec.tipo == VCP_DEC_RESPONDER) {
        printf("  t=%7.2f ms  CTRL -> MAQ  %-13s ", m->reloj,
               vcp_cmd_nombre(dec.resp_cmd));
        hex(dec.tx, dec.tx_len);

        switch (dec.resp_cmd) {
        case VCP_CMD_STATUS_RESP:
            printf("   estado=%s temp=%.1fC",
                   (dec.resp_payload[0] == VCP_ESTADO_LISTO) ? "LISTO" : "OCUPADO",
                   (double)(int16_t)vcp_get_u16be(&dec.resp_payload[2]) / 10.0);
            break;
        case VCP_CMD_VEND_APPROVE:
            printf("   txid=%u", (unsigned)vcp_get_u16be(dec.resp_payload));
            break;
        case VCP_CMD_VEND_DENY:
            printf("   motivo=0x%02X (%s)",
                   (unsigned)dec.resp_payload[0],
                   vcp_deny_nombre(dec.resp_payload[0]));
            break;
        default:
            break;
        }
        printf("\n");
    } else {
        printf("  t=%7.2f ms  CTRL         (no responde: %s)\n",
               m->reloj, vcp_dec_nombre(dec.tipo));
    }
}

/* ---------------------------------------------------------------------------
 * Motor del simulador
 * ------------------------------------------------------------------------- */

/* Hace correr el reloj 'ms' milisegundos, girando el lazo principal como lo
 * haria el firmware real (tick del receptor + poll de la aplicacion). */
static void avanzar(maquina_t *m, double ms)
{
    double fin = m->reloj + ms;
    while (m->reloj < fin) {
        m->reloj += 0.5;                   /* paso del lazo: 0.5 ms */
        pago_sim_tick(ahora(m));
        vcp_rx_tick(&m->rx, ahora(m), on_evento, m);
        vcp_app_poll(&m->app, ahora(m));
    }
}

/* La expendedora manda una trama al controlador, byte a byte y en tiempo. */
static void maq_envia(maquina_t *m, uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    uint8_t trama[VCP_TRAMA_MAX_BYTES];
    int     n;
    int     i;

    n = vcp_build((uint8_t)VCP_ADDR_SELF, cmd, payload, len, trama, sizeof(trama));
    if (n < 0) {
        printf("  !! error armando la trama del master (%d)\n", n);
        return;
    }

    printf("  t=%7.2f ms  MAQ -> CTRL  %-13s ", m->reloj, vcp_cmd_nombre(cmd));
    hex(trama, (size_t)n);
    printf("\n");

    for (i = 0; i < n; i++) {
        m->reloj += MS_POR_BYTE;
        pago_sim_tick(ahora(m));
        vcp_rx_byte(&m->rx, trama[i], ahora(m), on_evento, m);
        /* El tick y el poll van en cada vuelta del lazo, igual que en el
         * firmware real: la venta avanza mientras entran los bytes. */
        vcp_rx_tick(&m->rx, ahora(m), on_evento, m);
        vcp_app_poll(&m->app, ahora(m));
    }
}

static void status_req(maquina_t *m)
{
    maq_envia(m, (uint8_t)VCP_CMD_STATUS_REQ, NULL, 0u);
}

static void vend_request(maquina_t *m, uint8_t sel, uint16_t precio)
{
    uint8_t p[3];
    p[0] = sel;
    vcp_put_u16be(&p[1], precio);
    maq_envia(m, (uint8_t)VCP_CMD_VEND_REQUEST, p, 3u);
}

static void vend_success(maquina_t *m, uint16_t txid)
{
    uint8_t p[2];
    vcp_put_u16be(p, txid);
    maq_envia(m, (uint8_t)VCP_CMD_VEND_SUCCESS, p, 2u);
}

static void vend_failure(maquina_t *m, uint16_t txid, uint8_t causa)
{
    uint8_t p[3];
    vcp_put_u16be(p, txid);
    p[2] = causa;
    maq_envia(m, (uint8_t)VCP_CMD_VEND_FAILURE, p, 3u);
}

static void reset(maquina_t *m)
{
    memset(m, 0, sizeof(*m));
    vcp_rx_init(&m->rx);
    vcp_app_init(&m->app);
    pago_sim_init(0u);
    puart_set_eco(false);
    puart_reset();
    m->reloj = 0.0;
}

static void titulo(const char *n, const char *desc)
{
    printf("\n============================================================"
           "================\n");
    printf(" %s\n", n);
    printf("------------------------------------------------------------"
           "----------------\n");
    printf(" %s\n\n", desc);
}

static void cierre(const maquina_t *m)
{
    printf("\n  [estado final de la sesion: %s | cobros iniciados: %u]\n",
           vcp_session_estado_nombre(m->app.venta.estado),
           (unsigned)pago_sim_n_iniciados());
}

/* ===========================================================================
 * Escenarios
 * ========================================================================= */

/* --- 1 ------------------------------------------------------------------ */
static void esc_venta_ok(maquina_t *m)
{
    titulo("ESCENARIO 1 - Venta completa",
           "El caso feliz. Fijate en el punto clave: el cobro tarda 250 ms,\n"
           " muchisimo mas que los 5 ms de plazo, asi que el controlador\n"
           " contesta VEND_PENDING y el resultado viaja recien en la respuesta\n"
           " a un STATUS_REQ POSTERIOR. Eso es el requisito E1.12.");

    reset(m);
    pago_sim_programar(PAGO_AUTORIZADO, 250u, 0x2210u);

    status_req(m);            avanzar(m, 90.0);
    vend_request(m, 24u, 1800u);

    /* El master interroga cada 100 ms (Anexo A.2). */
    avanzar(m, 90.0);  status_req(m);   /* el cobro sigue en curso */
    avanzar(m, 90.0);  status_req(m);   /* sigue en curso          */
    avanzar(m, 90.0);  status_req(m);   /* ya resolvio -> APPROVE  */
    avanzar(m, 90.0);
    vend_success(m, 0x2210u);
    avanzar(m, 90.0);  status_req(m);   /* vuelve a LISTO          */
    cierre(m);
}

/* --- 2 ------------------------------------------------------------------ */
static void esc_pago_rechazado(maquina_t *m)
{
    titulo("ESCENARIO 2 - El medio de pago rechaza",
           "Mismo flujo, pero el cobro sale rechazado. El resultado pendiente\n"
           " se informa como VEND_DENY motivo 0x03 y la sesion se cierra sola:\n"
           " el DENY se manda UNA vez (no se latchea como el APPROVE) porque no\n"
           " se movio plata.");

    reset(m);
    pago_sim_programar(PAGO_RECHAZADO, 150u, 0u);

    vend_request(m, 12u, 900u);
    avanzar(m, 90.0);  status_req(m);   /* todavia en curso */
    avanzar(m, 90.0);  status_req(m);   /* DENY 0x03        */
    avanzar(m, 90.0);  status_req(m);   /* ya no repite: STATUS_RESP LISTO */
    cierre(m);
}

/* --- 3 ------------------------------------------------------------------ */
static void esc_rangos(maquina_t *m)
{
    titulo("ESCENARIO 3 - Validacion de rangos (Anexo A.4)",
           "Seleccion valida 1..80, precio valido 50..20000 centavos.\n"
           " Se rechazan SIN iniciar cobro: el contador de cobros iniciados\n"
           " tiene que quedar en 0.");

    reset(m);

    printf("  -- seleccion 0 (fuera de 1..80)\n");
    vend_request(m, 0u, 1000u);       avanzar(m, 20.0);
    printf("  -- seleccion 81 (fuera de 1..80)\n");
    vend_request(m, 81u, 1000u);      avanzar(m, 20.0);
    printf("  -- precio 10 (menor a 50)\n");
    vend_request(m, 5u, 10u);         avanzar(m, 20.0);
    printf("  -- precio 20001 (mayor a 20000)\n");
    vend_request(m, 5u, 20001u);      avanzar(m, 20.0);
    printf("  -- seleccion 0 Y precio 10: se informa la seleccion (decision NE-04)\n");
    vend_request(m, 0u, 10u);         avanzar(m, 20.0);
    cierre(m);
}

/* --- 4 ------------------------------------------------------------------ */
static void esc_retransmision(maquina_t *m)
{
    titulo("ESCENARIO 4 - VEND_REQUEST retransmitido (idempotencia)",
           "El master reintenta cada 100 ms si no recibe respuesta. Si tratamos\n"
           " el reintento como pedido nuevo contestariamos DENY 0x04 y\n"
           " abortariamos una venta cuyo cobro YA arranco.\n"
           " Criterio (NE-02): mismo sel + mismo precio con sesion cobrando =\n"
           " retransmision -> se repite VEND_PENDING y NO se inicia otro cobro.");

    reset(m);
    pago_sim_programar(PAGO_AUTORIZADO, 400u, 0x3001u);

    vend_request(m, 7u, 1500u);       avanzar(m, 100.0);
    printf("  -- el master no recibio el PENDING y reintenta:\n");
    vend_request(m, 7u, 1500u);       avanzar(m, 100.0);
    printf("  -- y otra vez:\n");
    vend_request(m, 7u, 1500u);       avanzar(m, 250.0);
    status_req(m);
    cierre(m);
    printf("  (si 'cobros iniciados' fuera > 1, estariamos cobrando dos veces)\n");
}

/* --- 5 ------------------------------------------------------------------ */
static void esc_sesion_en_curso(maquina_t *m)
{
    titulo("ESCENARIO 5 - Pedido distinto con sesion abierta",
           "Ahora si es un pedido NUEVO (otra seleccion) mientras hay una venta\n"
           " en curso: VEND_DENY motivo 0x04 'sesion en curso'.");

    reset(m);
    pago_sim_programar(PAGO_AUTORIZADO, 400u, 0x4001u);

    vend_request(m, 7u, 1500u);       avanzar(m, 100.0);
    printf("  -- llega un pedido de OTRO producto:\n");
    vend_request(m, 9u, 800u);        avanzar(m, 400.0);
    status_req(m);
    cierre(m);
}

/* --- 6 ------------------------------------------------------------------ */
static void esc_pago_colgado(maquina_t *m)
{
    titulo("ESCENARIO 6 - El cobro nunca resuelve (ejercicio E2.8)",
           "Hacia el medio de pago somos MASTER: si el esclavo no contesta\n"
           " nunca, el plazo lo tenemos que poner nosotros. Sin ese plazo la\n"
           " sesion queda abierta para siempre y la maquina deja de vender.\n"
           " Plazo elegido: 3000 ms (3x el peor caso normal de 900 ms).\n"
           " Mientras tanto se contesta STATUS_RESP con estado OCUPADO.");

    reset(m);
    pago_sim_programar(PAGO_AUTORIZADO, PAGO_SIM_NUNCA, 0u);

    vend_request(m, 33u, 2500u);
    avanzar(m, 900.0);   status_req(m);
    avanzar(m, 1000.0);  status_req(m);
    avanzar(m, 1200.0);  status_req(m);   /* aca ya vencieron los 3000 ms */
    avanzar(m, 100.0);   status_req(m);
    cierre(m);
}

/* --- 7 ------------------------------------------------------------------ */
static void esc_sin_confirmar(maquina_t *m)
{
    titulo("ESCENARIO 7 - El master no confirma la entrega (ejercicio E2.4)",
           "Informamos VEND_APPROVE y no llega ni VEND_SUCCESS ni VEND_FAILURE.\n"
           " El APPROVE queda LATCHEADO: cada STATUS_REQ vuelve a recibir el\n"
           " mismo txid, porque si nuestra respuesta se perdio el master nunca\n"
           " se entero de que hay una venta aprobada... y la plata ya se cobro.\n"
           " A los 2000 ms se cierra la sesion y queda anotada para el backend:\n"
           " es el unico caso en que NO se puede saber si se entrego.");

    reset(m);
    pago_sim_programar(PAGO_AUTORIZADO, 100u, 0x8821u);

    vend_request(m, 24u, 1800u);
    avanzar(m, 150.0);   status_req(m);   /* APPROVE                     */
    avanzar(m, 100.0);   status_req(m);   /* APPROVE otra vez (latcheado)*/
    avanzar(m, 100.0);   status_req(m);   /* y otra                      */
    avanzar(m, 1900.0);  status_req(m);   /* vencio: vuelve a LISTO      */
    printf("\n  -- y ahora llega, tardisimo, un VEND_FAILURE con ese txid:\n");
    vend_failure(m, 0x8821u, 0x01u);
    cierre(m);
    printf("  (txid desconocido: %u -> se ACKea igual, decision NE-06)\n",
           (unsigned)m->app.venta.n_txid_desconocido);
}

/* ===========================================================================
 * main
 * ========================================================================= */
int main(int argc, char **argv)
{
    static maquina_t m;
    int quiere_todos = 1;
    int solo = 0;

    if (argc > 1) {
        solo = atoi(argv[1]);
        if ((solo >= 1) && (solo <= 7)) {
            quiere_todos = 0;
        } else {
            printf("uso: %s [1..7]   (sin argumento corre todos los escenarios)\n",
                   argv[0]);
            printf("  1 venta completa        5 pedido distinto con sesion abierta\n");
            printf("  2 pago rechazado        6 el cobro nunca resuelve\n");
            printf("  3 rangos invalidos      7 el master no confirma\n");
            printf("  4 retransmision\n");
            return 2;
        }
    }

    printf("============================================================"
           "================\n");
    printf(" VCP-1 - Demostracion de la sesion de venta (requisito E1.12)\n");
    printf(" La expendedora esta simulada: arma tramas reales y las entrega\n");
    printf(" byte a byte a 9600 8N1 (%.4f ms por byte).\n", MS_POR_BYTE);
    printf("============================================================"
           "================\n");

    if (quiere_todos || (solo == 1)) { esc_venta_ok(&m); }
    if (quiere_todos || (solo == 2)) { esc_pago_rechazado(&m); }
    if (quiere_todos || (solo == 3)) { esc_rangos(&m); }
    if (quiere_todos || (solo == 4)) { esc_retransmision(&m); }
    if (quiere_todos || (solo == 5)) { esc_sesion_en_curso(&m); }
    if (quiere_todos || (solo == 6)) { esc_pago_colgado(&m); }
    if (quiere_todos || (solo == 7)) { esc_sin_confirmar(&m); }

    printf("\n");
    return 0;
}
