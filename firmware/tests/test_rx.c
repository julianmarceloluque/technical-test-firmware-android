/*
 * ============================================================================
 *  test_rx.c - Pruebas del receptor incremental
 * ============================================================================
 *
 *  Cubre los requisitos E1.1, E1.3, E1.4, E1.5 y E1.6:
 *   - procesamiento byte a byte sin bloquear
 *   - imposibilidad de desbordar el buffer
 *   - recuperacion despues de una trama invalida
 *   - reporte diferenciado de errores
 *   - timeout entre bytes
 * ============================================================================
 */

#include "test_util.h"
#include "vcp/vcp_rx.h"
#include "vcp/vcp_frame.h"

/* ---------------------------------------------------------------------------
 * Recolector de eventos: guarda lo que el receptor va reportando para poder
 * revisarlo despues de alimentar el stream completo.
 * ------------------------------------------------------------------------- */
#define MAX_EV 64

typedef struct {
    struct {
        vcp_event_t ev;
        vcp_frame_t trama;
        uint32_t    off_ini;
        uint32_t    off_fin;
        uint32_t    t_ms;
        uint32_t    now_ms;
        int         reproceso;
    } e[MAX_EV];
    unsigned n;            /* eventos con detalle guardado (tope MAX_EV)   */
    unsigned n_ruido;      /* los bytes fuera de trama se cuentan aparte    */

    /*
     * Los contadores por tipo se llevan SIEMPRE, aunque el detalle ya no
     * entre en el arreglo. Sin esto, las pruebas de estres (miles de bytes
     * basura) llenarian el arreglo de errores y despues no verian la trama
     * valida del final: pareceria un bug del receptor cuando en realidad es
     * un limite del recolector de la prueba.
     */
    unsigned cuenta[8];
} recolector_t;

static void colector(void *usuario, const vcp_rx_evento_t *ev)
{
    recolector_t *r = (recolector_t *)usuario;

    if (ev->ev == VCP_EV_BYTE_FUERA_TRAMA) {
        r->n_ruido++;
        return;
    }
    if ((unsigned)ev->ev < 8u) {
        r->cuenta[(unsigned)ev->ev]++;
    }
    if (r->n >= MAX_EV) {
        return;   /* ya no guardamos el detalle, pero el contador sigue */
    }
    r->e[r->n].ev        = ev->ev;
    r->e[r->n].off_ini   = ev->off_ini;
    r->e[r->n].off_fin   = ev->off_fin;
    r->e[r->n].t_ms      = ev->t_ms;
    r->e[r->n].now_ms    = ev->now_ms;
    r->e[r->n].reproceso = (int)ev->reproceso;
    if (ev->trama != NULL) {
        r->e[r->n].trama = *ev->trama;
    }
    r->n++;
}

/* Alimenta un stream completo, un byte por milisegundo. */
static void alimentar(vcp_rx_t *rx, recolector_t *r,
                      const uint8_t *d, size_t n, uint32_t t0)
{
    size_t i;
    for (i = 0u; i < n; i++) {
        vcp_rx_byte(rx, d[i], t0 + (uint32_t)i, colector, r);
    }
}

/* Cuenta cuantos eventos de un tipo hubo (sin tope). */
static unsigned contar(const recolector_t *r, vcp_event_t ev)
{
    return ((unsigned)ev < 8u) ? r->cuenta[(unsigned)ev] : 0u;
}

/* ===========================================================================
 * Pruebas
 * ========================================================================= */
void tests_rx(void)
{
    test_grupo("Receptor: caso normal");

    /* --- Una trama sola, limpia --- */
    {
        vcp_rx_t     rx;
        recolector_t r;
        memset(&r, 0, sizeof(r));
        const uint8_t s[] = { 0x02u, 0x0Au, 0x00u, 0x10u, 0xF7u, 0x03u };

        vcp_rx_init(&rx);
        alimentar(&rx, &r, s, sizeof(s), 100u);

        VERIFICAR_EQ(r.n, 1u, "una trama limpia produce un solo evento");
        VERIFICAR_EQ(r.e[0].ev, VCP_EV_FRAME, "el evento es VCP_EV_FRAME");
        VERIFICAR_EQ(r.e[0].trama.addr, 0x0Au, "ADDR = 0x0A");
        VERIFICAR_EQ(r.e[0].trama.cmd, 0x10u, "CMD = STATUS_REQ");
        VERIFICAR_EQ(r.e[0].trama.len, 0u, "LEN = 0");
        VERIFICAR_EQ(r.e[0].trama.t_etx_ms, 105u,
                     "t_etx_ms es el instante del ETX (100 + 5 bytes)");
        VERIFICAR_EQ(r.e[0].now_ms, r.e[0].trama.t_etx_ms,
                     "trama normal: se detecta en el mismo instante del ETX");
        VERIFICAR(!vcp_rx_en_trama(&rx), "al terminar no queda trama a medias");
    }

    /* --- Ruido antes de la trama --- */
    {
        vcp_rx_t     rx;
        recolector_t r;
        memset(&r, 0, sizeof(r));
        const uint8_t s[] = { 0xAAu, 0x55u, 0x7Fu,
                              0x02u, 0x0Au, 0x00u, 0x10u, 0xF7u, 0x03u };
        vcp_rx_init(&rx);
        alimentar(&rx, &r, s, sizeof(s), 0u);
        VERIFICAR_EQ(contar(&r, VCP_EV_FRAME), 1u,
                     "el ruido previo no impide detectar la trama");
        VERIFICAR_EQ(r.n_ruido, 3u, "los 3 bytes espurios se reportan");
        VERIFICAR_EQ(r.e[0].off_ini, 3u, "el STX estaba en el offset 3");
    }

    /* --- Payload con STX y ETX adentro: LA prueba del protocolo sin escapado --- */
    {
        vcp_rx_t     rx;
        recolector_t r;
        memset(&r, 0, sizeof(r));
        const uint8_t s[] = { 0x02u, 0x0Au, 0x06u, 0x40u,
                              0x41u, 0x42u, 0x02u, 0x03u, 0x43u, 0x44u,
                              0x61u, 0x03u };
        const uint8_t pay[] = { 0x41u, 0x42u, 0x02u, 0x03u, 0x43u, 0x44u };
        vcp_rx_init(&rx);
        alimentar(&rx, &r, s, sizeof(s), 0u);
        VERIFICAR_EQ(contar(&r, VCP_EV_FRAME), 1u,
                     "un 0x02/0x03 en el payload NO rompe el parseo");
        VERIFICAR_EQ(r.e[0].trama.len, 6u, "LEN = 6");
        VERIFICAR_MEM(r.e[0].trama.payload, pay, sizeof(pay),
                      "el payload llega intacto");
    }

    /* --- Payload maximo (64 bytes) --- */
    {
        vcp_rx_t     rx;
        recolector_t r;
        memset(&r, 0, sizeof(r));
        uint8_t s[VCP_TRAMA_MAX_BYTES];
        int i;
        uint8_t pay[VCP_MAX_PAYLOAD];

        for (i = 0; i < (int)VCP_MAX_PAYLOAD; i++) { pay[i] = (uint8_t)(i * 3); }
        VERIFICAR_EQ(vcp_build(0x0Au, 0x40u, pay, (uint8_t)VCP_MAX_PAYLOAD,
                               s, sizeof(s)),
                     (int)VCP_TRAMA_MAX_BYTES, "se arma una trama de 70 bytes");
        vcp_rx_init(&rx);
        alimentar(&rx, &r, s, sizeof(s), 0u);
        VERIFICAR_EQ(contar(&r, VCP_EV_FRAME), 1u,
                     "el payload maximo (64) se recibe entero");
        VERIFICAR_MEM(r.e[0].trama.payload, pay, VCP_MAX_PAYLOAD,
                      "los 64 bytes llegan intactos");
    }

    /* --- Dos tramas pegadas, sin separacion --- */
    {
        vcp_rx_t     rx;
        recolector_t r;
        memset(&r, 0, sizeof(r));
        const uint8_t s[] = { 0x02u, 0x0Au, 0x00u, 0x10u, 0xF7u, 0x03u,
                              0x02u, 0x0Bu, 0x00u, 0x10u, 0x9Cu, 0x03u };
        vcp_rx_init(&rx);
        alimentar(&rx, &r, s, sizeof(s), 0u);
        VERIFICAR_EQ(contar(&r, VCP_EV_FRAME), 2u,
                     "dos tramas pegadas se detectan las dos");
        VERIFICAR_EQ(r.e[1].trama.addr, 0x0Bu, "la segunda es para 0x0B");
        VERIFICAR_EQ(r.n_ruido, 0u, "sin bytes descartados entre ellas");
    }

    test_grupo("Receptor: errores diferenciados (E1.5)");

    /* --- CRC malo --- */
    {
        vcp_rx_t     rx;
        recolector_t r;
        memset(&r, 0, sizeof(r));
        const uint8_t s[] = { 0x02u, 0x0Au, 0x00u, 0x10u, 0xF6u, 0x03u };
        vcp_rx_init(&rx);
        alimentar(&rx, &r, s, sizeof(s), 0u);
        VERIFICAR_EQ(contar(&r, VCP_EV_ERR_CRC), 1u, "CRC malo -> VCP_EV_ERR_CRC");
        VERIFICAR_EQ(contar(&r, VCP_EV_FRAME), 0u, "y NO se entrega la trama");
        VERIFICAR_EQ(vcp_rx_stats(&rx)->err_crc, 1u, "el contador de CRC sube");
    }

    /* --- LEN fuera de rango --- */
    {
        vcp_rx_t     rx;
        recolector_t r;
        memset(&r, 0, sizeof(r));
        const uint8_t s[] = { 0x02u, 0x0Au, 0x41u, 0x40u, 0x00u, 0x03u };
        vcp_rx_init(&rx);
        alimentar(&rx, &r, s, sizeof(s), 0u);
        VERIFICAR_EQ(contar(&r, VCP_EV_ERR_LEN), 1u, "LEN=65 -> VCP_EV_ERR_LEN");
        VERIFICAR_EQ(r.e[0].off_fin, 2u,
                     "se corta en el byte del LEN, sin esperar CRC ni ETX");
    }

    /* --- Falta el ETX --- */
    {
        vcp_rx_t     rx;
        recolector_t r;
        memset(&r, 0, sizeof(r));
        const uint8_t s[] = { 0x02u, 0x0Au, 0x00u, 0x10u, 0xF7u, 0x99u };
        vcp_rx_init(&rx);
        alimentar(&rx, &r, s, sizeof(s), 0u);
        VERIFICAR_EQ(contar(&r, VCP_EV_ERR_FRAMING), 1u,
                     "sin ETX -> VCP_EV_ERR_FRAMING");
    }

    /* --- Framing tiene prioridad sobre CRC ---
     * Trama con CRC malo Y sin ETX: se informa el problema estructural, que es
     * el que explica de verdad lo que paso. */
    {
        vcp_rx_t     rx;
        recolector_t r;
        memset(&r, 0, sizeof(r));
        const uint8_t s[] = { 0x02u, 0x0Au, 0x00u, 0x10u, 0x11u, 0x99u };
        vcp_rx_init(&rx);
        alimentar(&rx, &r, s, sizeof(s), 0u);
        VERIFICAR_EQ(contar(&r, VCP_EV_ERR_FRAMING), 1u,
                     "CRC malo + ETX ausente se informa como FRAMING");
        VERIFICAR_EQ(contar(&r, VCP_EV_ERR_CRC), 0u,
                     "  (y no como CRC: sin ETX el CRC no significa nada)");
    }

    test_grupo("Receptor: timeout entre bytes (E1.6)");

    /* --- La trama se corta a la mitad y la linea se calla --- */
    {
        vcp_rx_t     rx;
        recolector_t r;
        memset(&r, 0, sizeof(r));
        const uint8_t medio[] = { 0x02u, 0x0Au, 0x06u, 0x40u, 0x41u };
        const uint8_t buena[] = { 0x02u, 0x0Au, 0x00u, 0x10u, 0xF7u, 0x03u };

        vcp_rx_init(&rx);
        alimentar(&rx, &r, medio, sizeof(medio), 1000u);
        VERIFICAR(vcp_rx_en_trama(&rx), "quedo una trama a medio recibir");

        vcp_rx_tick(&rx, 1006u, colector, &r);   /* 1004 + 2 ms: todavia no */
        VERIFICAR_EQ(contar(&r, VCP_EV_ERR_TIMEOUT), 0u,
                     "a los 2 ms de silencio todavia no vence");

        vcp_rx_tick(&rx, 1010u, colector, &r);   /* 6 ms de silencio: vence */
        VERIFICAR_EQ(contar(&r, VCP_EV_ERR_TIMEOUT), 1u,
                     "a los 6 ms de silencio vence el timeout");
        VERIFICAR(!vcp_rx_en_trama(&rx), "y el candidato se abandona");

        /* Y lo importante: despues del timeout se siguen aceptando tramas. */
        alimentar(&rx, &r, buena, sizeof(buena), 1020u);
        VERIFICAR_EQ(contar(&r, VCP_EV_FRAME), 1u,
                     "despues del timeout se sigue recibiendo normal (E1.4)");
    }

    /* --- El timeout no se dispara si los bytes siguen llegando --- */
    {
        vcp_rx_t     rx;
        recolector_t r;
        memset(&r, 0, sizeof(r));
        const uint8_t s[] = { 0x02u, 0x0Au, 0x00u, 0x10u, 0xF7u, 0x03u };
        size_t i;
        vcp_rx_init(&rx);
        for (i = 0u; i < sizeof(s); i++) {
            uint32_t t = 500u + (uint32_t)i * 4u;   /* 4 ms entre bytes: ok */
            vcp_rx_byte(&rx, s[i], t, colector, &r);
            vcp_rx_tick(&rx, t, colector, &r);
        }
        VERIFICAR_EQ(contar(&r, VCP_EV_ERR_TIMEOUT), 0u,
                     "4 ms entre bytes esta dentro del limite de 5 ms");
        VERIFICAR_EQ(contar(&r, VCP_EV_FRAME), 1u, "y la trama se entrega");
    }

    /* --- El contador de ms da la vuelta (a los 49.7 dias) --- */
    {
        vcp_rx_t     rx;
        recolector_t r;
        memset(&r, 0, sizeof(r));
        const uint8_t medio[] = { 0x02u, 0x0Au, 0x06u, 0x40u };
        uint32_t t0 = 0xFFFFFFFEu;    /* a 2 ms de dar la vuelta */
        size_t i;

        vcp_rx_init(&rx);
        for (i = 0u; i < sizeof(medio); i++) {
            vcp_rx_byte(&rx, medio[i], t0 + (uint32_t)i, colector, &r);
        }
        /* t0+3 = 0x00000001 (ya dio la vuelta). Silencio de 2 ms: no vence. */
        vcp_rx_tick(&rx, 0x00000003u, colector, &r);
        VERIFICAR_EQ(contar(&r, VCP_EV_ERR_TIMEOUT), 0u,
                     "wraparound del contador: no vence antes de tiempo");
        /* 8 ms despues del ultimo byte: ahora si. */
        vcp_rx_tick(&rx, 0x00000009u, colector, &r);
        VERIFICAR_EQ(contar(&r, VCP_EV_ERR_TIMEOUT), 1u,
                     "wraparound del contador: vence cuando corresponde");
    }

    test_grupo("Receptor: imposible desbordar (E1.3)");

    /* --- Miles de STX seguidos --- */
    {
        vcp_rx_t     rx;
        recolector_t r;
        memset(&r, 0, sizeof(r));
        const uint8_t buena[] = { 0x02u, 0x0Au, 0x00u, 0x10u, 0xF7u, 0x03u };
        int i;

        vcp_rx_init(&rx);
        for (i = 0; i < 5000; i++) {
            vcp_rx_byte(&rx, 0x02u, (uint32_t)i, colector, &r);
        }
        VERIFICAR_EQ(vcp_rx_stats(&rx)->err_interno, 0u,
                     "5000 STX seguidos: cero errores internos (E1.3)");
        VERIFICAR_EQ(contar(&r, VCP_EV_FRAME), 0u,
                     "5000 STX seguidos: ninguna trama fantasma");

        /*
         * LIMITACION CONOCIDA DEL DISENO DE UN SOLO CANDIDATO
         * ---------------------------------------------------
         * Si la trama buena viene PEGADA a la avalancha, sin ni una pausa, el
         * receptor no la encuentra. Motivo: con la politica de reproceso el
         * inicio del candidato avanza una posicion por cada byte nuevo, o sea
         * al mismo ritmo que el stream, y nunca alcanza el STX verdadero.
         *
         * No es un desborde ni una corrupcion: es que la ventana queda corrida.
         * Para que ocurra hace falta un stream continuo, sin ningun hueco de
         * 5 ms, lo cual en un bus half-duplex donde el master espera respuesta
         * no pasa. Ver docs/01_E1_decisiones.md, seccion "Limitaciones
         * conocidas", donde esta la alternativa (candidatos en paralelo) y su
         * costo.
         *
         * Lo que SI tiene que pasar: con una pausa (que es lo que ocurre en la
         * linea real) el timeout limpia todo y se vuelve a recibir normal.
         */
        vcp_rx_tick(&rx, 5100u, colector, &r);   /* silencio -> vence timeout */
        VERIFICAR(!vcp_rx_en_trama(&rx),
                  "una pausa en la linea limpia el estado (timeout)");
        alimentar(&rx, &r, buena, sizeof(buena), 6000u);
        VERIFICAR_EQ(contar(&r, VCP_EV_FRAME), 1u,
                     "y despues de la pausa se acepta una trama valida (E1.4)");
    }

    /* --- LEN grande seguido de basura, muchas veces --- */
    {
        vcp_rx_t     rx;
        recolector_t r;
        memset(&r, 0, sizeof(r));
        const uint8_t buena[] = { 0x02u, 0x0Au, 0x00u, 0x10u, 0xF7u, 0x03u };
        int i;

        vcp_rx_init(&rx);
        for (i = 0; i < 3000; i++) {
            /* patron adversarial: STX, addr, LEN=64, y basura */
            uint8_t b = (uint8_t)((i % 5 == 0) ? 0x02u :
                                  (i % 5 == 2) ? 0x40u : (uint8_t)i);
            vcp_rx_byte(&rx, b, (uint32_t)i, colector, &r);
        }
        VERIFICAR_EQ(vcp_rx_stats(&rx)->err_interno, 0u,
                     "patron adversarial: cero errores internos");

        /* Igual que arriba: con una pausa se recupera. */
        vcp_rx_tick(&rx, 3100u, colector, &r);
        alimentar(&rx, &r, buena, sizeof(buena), 4000u);
        VERIFICAR(contar(&r, VCP_EV_FRAME) >= 1u,
                  "tras una pausa vuelve a detectar tramas validas");
    }

    /* --- Todos los valores de byte posibles como LEN --- */
    {
        int len;
        for (len = 0; len < 256; len++) {
            vcp_rx_t     rx;
            recolector_t r;
        memset(&r, 0, sizeof(r));
            uint8_t s[4];
            int i;
            vcp_rx_init(&rx);
            s[0] = 0x02u; s[1] = 0x0Au; s[2] = (uint8_t)len; s[3] = 0x40u;
            alimentar(&rx, &r, s, sizeof(s), 0u);
            /* despues, 300 bytes de relleno */
            for (i = 0; i < 300; i++) {
                vcp_rx_byte(&rx, (uint8_t)i, (uint32_t)(10 + i), colector, &r);
            }
            if (vcp_rx_stats(&rx)->err_interno != 0u) {
                break;
            }
        }
        VERIFICAR_EQ(len, 256, "ningun valor de LEN (0..255) produce error interno");
    }

    test_grupo("Receptor: politica de re-sincronizacion (E1.4)");

    /*
     * El caso del Anexo B, offsets 36..49: un LOG_EVENT con CRC roto que tiene
     * ADENTRO del payload una trama VEND_APPROVE perfectamente valida.
     *
     *   02 0A 08 40 54 52 | 02 0A 02 21 00 2A 0F 03 |
     *   ^ candidato falso   ^ trama de verdad
     */
    {
        const uint8_t s[] = { 0x02u, 0x0Au, 0x08u, 0x40u, 0x54u, 0x52u,
                              0x02u, 0x0Au, 0x02u, 0x21u, 0x00u, 0x2Au,
                              0x0Fu, 0x03u };
        /* con REPROCESO */
        {
            vcp_rx_t     rx;
            recolector_t r;
        memset(&r, 0, sizeof(r));
            vcp_rx_init(&rx);
            vcp_rx_set_resync(&rx, VCP_RESYNC_REPROCESO);
            alimentar(&rx, &r, s, sizeof(s), 0u);
            VERIFICAR_EQ(contar(&r, VCP_EV_ERR_CRC), 1u,
                         "REPROCESO: el candidato externo falla por CRC");
            VERIFICAR_EQ(contar(&r, VCP_EV_FRAME), 1u,
                         "REPROCESO: y encuentra la trama escondida");
            VERIFICAR_EQ(r.e[1].trama.cmd, VCP_CMD_VEND_APPROVE,
                         "REPROCESO: la trama escondida es VEND_APPROVE");
            VERIFICAR(r.e[1].reproceso,
                      "REPROCESO: viene marcada como recuperada");
            VERIFICAR_EQ(vcp_rx_stats(&rx)->tramas_recuperadas, 1u,
                         "REPROCESO: el contador de recuperadas sube");
        }
        /* con DESCARTE */
        {
            vcp_rx_t     rx;
            recolector_t r;
        memset(&r, 0, sizeof(r));
            vcp_rx_init(&rx);
            vcp_rx_set_resync(&rx, VCP_RESYNC_DESCARTE);
            alimentar(&rx, &r, s, sizeof(s), 0u);
            VERIFICAR_EQ(contar(&r, VCP_EV_FRAME), 0u,
                         "DESCARTE: la trama escondida se pierde");
        }
    }

    /*
     * El otro caso del Anexo B (offsets 57..73): el 0x02 duplicado. El primero
     * es basura y arranca un candidato que se traga la trama buena Y ademas la
     * siguiente. Con reproceso se recuperan las dos.
     */
    {
        const uint8_t s[] = { 0x02u,
                              0x02u, 0x0Au, 0x02u, 0x30u, 0x00u, 0x2Au, 0xC6u, 0x03u,
                              0x02u, 0x00u, 0x02u, 0x40u, 0x42u, 0x43u, 0x15u, 0x03u };
        {
            vcp_rx_t     rx;
            recolector_t r;
        memset(&r, 0, sizeof(r));
            vcp_rx_init(&rx);
            vcp_rx_set_resync(&rx, VCP_RESYNC_REPROCESO);
            alimentar(&rx, &r, s, sizeof(s), 0u);
            VERIFICAR_EQ(contar(&r, VCP_EV_FRAME), 2u,
                         "REPROCESO: recupera las DOS tramas tapadas");
        }
        {
            vcp_rx_t     rx;
            recolector_t r;
        memset(&r, 0, sizeof(r));
            vcp_rx_init(&rx);
            vcp_rx_set_resync(&rx, VCP_RESYNC_DESCARTE);
            alimentar(&rx, &r, s, sizeof(s), 0u);
            VERIFICAR_EQ(contar(&r, VCP_EV_FRAME), 0u,
                         "DESCARTE: pierde las dos");
        }
    }

    /* --- Una trama recuperada se detecta DESPUES de su ETX --- */
    {
        vcp_rx_t     rx;
        recolector_t r;
        memset(&r, 0, sizeof(r));
        const uint8_t s[] = { 0x02u,
                              0x02u, 0x0Au, 0x02u, 0x30u, 0x00u, 0x2Au, 0xC6u, 0x03u,
                              0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x99u };
        vcp_rx_init(&rx);
        alimentar(&rx, &r, s, sizeof(s), 0u);   /* 1 byte por ms */
        VERIFICAR_EQ(contar(&r, VCP_EV_FRAME), 1u, "se recupera la trama");
        {
            unsigned i;
            for (i = 0u; i < r.n; i++) {
                if (r.e[i].ev == VCP_EV_FRAME) {
                    VERIFICAR_EQ(r.e[i].t_ms, 8u,
                                 "su ETX habia llegado en t=8 ms");
                    VERIFICAR(r.e[i].now_ms > r.e[i].t_ms,
                              "pero se detecta MAS TARDE (por eso existe el plazo)");
                }
            }
        }
    }

    /* --- Recuperacion despues de una trama invalida (E1.4 explicito) --- */
    {
        vcp_rx_t     rx;
        recolector_t r;
        memset(&r, 0, sizeof(r));
        const uint8_t s[] = {
            0x02u, 0x0Au, 0x00u, 0x10u, 0xFFu, 0x03u,      /* CRC malo      */
            0x02u, 0x0Au, 0x41u,                            /* LEN malo      */
            0xAAu, 0xBBu,                                   /* ruido         */
            0x02u, 0x0Au, 0x00u, 0x10u, 0xF7u, 0x03u        /* trama buena   */
        };
        vcp_rx_init(&rx);
        alimentar(&rx, &r, s, sizeof(s), 0u);
        VERIFICAR(contar(&r, VCP_EV_FRAME) >= 1u,
                  "despues de dos tramas rotas se sigue aceptando una buena");
    }
}
