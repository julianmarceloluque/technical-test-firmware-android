/*
 * ============================================================================
 *  port_pago_pc.c - Simulador del medio de pago para el banco de PC
 * ============================================================================
 *
 *  Reproduce el comportamiento que describe el Anexo A.5:
 *
 *    - pago_iniciar() no bloquea: arranca el cobro y vuelve.
 *    - pago_estado()  no bloquea: dice como va.
 *    - el resultado tarda entre 50 y 900 ms.
 *
 *  Ademas permite guionar el resultado, que es lo que hace posible testear los
 *  caminos de rechazo y de cobro colgado sin esperar a que pasen de verdad.
 *
 *  Por que el simulador necesita pago_sim_tick(): el firmware no le pasa la
 *  hora a pago_estado() (esa es la firma provista y no se toca), asi que la
 *  hora se la inyecta el banco de pruebas desde afuera. En el equipo real el
 *  lector tiene su propio reloj.
 * ============================================================================
 */

#include <string.h>

#include "port_pc.h"

typedef struct {
    bool          activo;        /* hay un cobro en vuelo                     */
    uint32_t      t_inicio;      /* cuando arranco                            */
    uint32_t      demora;        /* cuanto tarda en resolver                  */
    pago_estado_t resultado;     /* como va a resolver                        */
    uint16_t      txid;          /* id que devolvera si autoriza              */
    uint32_t      ahora;         /* hora inyectada por pago_sim_tick()        */
    uint16_t      proximo_txid;  /* generador de txid automatico              */
    uint32_t      n_iniciados;   /* cuantos cobros se pidieron                */

    /* Guion del PROXIMO cobro. */
    pago_estado_t g_resultado;
    uint32_t      g_demora;
    uint16_t      g_txid;
} pago_sim_t;

static pago_sim_t s;

void pago_sim_init(uint32_t now_ms)
{
    memset(&s, 0, sizeof(s));
    s.ahora        = now_ms;
    s.proximo_txid = 0x2A01u;      /* arranca en un valor reconocible         */

    /* Guion por defecto: autoriza a los 250 ms (dentro de los 50..900 ms del
     * enunciado) con txid automatico. */
    s.g_resultado = PAGO_AUTORIZADO;
    s.g_demora    = 250u;
    s.g_txid      = 0u;
}

void pago_sim_tick(uint32_t now_ms)
{
    s.ahora = now_ms;
}

void pago_sim_programar(pago_estado_t resultado, uint32_t demora_ms, uint16_t txid)
{
    s.g_resultado = resultado;
    s.g_demora    = demora_ms;
    s.g_txid      = txid;
}

uint32_t pago_sim_n_iniciados(void)
{
    return s.n_iniciados;
}

/* ---------------------------------------------------------------------------
 * Interfaz que usa el firmware
 * ------------------------------------------------------------------------- */
void pago_iniciar(uint16_t centavos)
{
    (void)centavos;   /* el simulador no cobra de verdad */

    s.activo      = true;
    s.t_inicio    = s.ahora;
    s.demora      = s.g_demora;
    s.resultado   = s.g_resultado;
    s.txid        = (s.g_txid != 0u) ? s.g_txid : s.proximo_txid++;
    s.n_iniciados++;
}

pago_estado_t pago_estado(uint16_t *txid)
{
    if (!s.activo) {
        /* Nadie pidio un cobro: no hay nada que informar. Devolver EN_CURSO
         * seria mentir; en un lector real esto seria un error de uso. Se
         * devuelve EN_CURSO igual porque es el valor neutro de la enum y la
         * maquina de estados solo llama a esto estando en COBRANDO. */
        return PAGO_EN_CURSO;
    }

    if (s.demora == PAGO_SIM_NUNCA) {
        return PAGO_EN_CURSO;      /* escenario del ejercicio E2.8 */
    }

    if ((uint32_t)(s.ahora - s.t_inicio) < s.demora) {
        return PAGO_EN_CURSO;
    }

    if (s.resultado == PAGO_AUTORIZADO) {
        if (txid != NULL) {
            *txid = s.txid;
        }
        return PAGO_AUTORIZADO;
    }

    return PAGO_RECHAZADO;
}
