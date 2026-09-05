/*
 * ============================================================================
 *  vcp_rx.c - Implementacion del receptor incremental VCP-1
 * ============================================================================
 *
 *  Leer primero vcp_rx.h: ahi esta el diagrama de la maquina de estados y la
 *  explicacion de la politica de re-sincronizacion.
 *
 *  ORGANIZACION DE ESTE ARCHIVO
 *  ----------------------------
 *    1. Estados internos y helpers chicos
 *    2. rx_paso()          -> procesa UN byte y dice si el candidato murio
 *    3. rx_resincronizar() -> arma la cola de reproceso cuando un candidato muere
 *    4. vcp_rx_byte()      -> el lazo que combina 2 y 3
 *    5. vcp_rx_tick()      -> vencimiento del timeout entre bytes
 * ============================================================================
 */

#include <string.h>

#include "vcp/vcp_rx.h"
#include "vcp/vcp_crc.h"

/* ===========================================================================
 * 1) Estados internos y helpers
 * ========================================================================= */

/*
 * Los estados son internos a este .c a proposito: nadie de afuera necesita
 * saber en cual estamos, y si manana se agrega uno no se rompe ningun header.
 */
typedef enum {
    RX_ESPERANDO_STX = 0,   /* tirando basura hasta que aparezca un 0x02      */
    RX_ADDR,                /* el proximo byte es ADDR                        */
    RX_LEN,                 /* el proximo byte es LEN                         */
    RX_CMD,                 /* el proximo byte es CMD                         */
    RX_PAYLOAD,             /* juntando los LEN bytes de payload              */
    RX_CRC,                 /* el proximo byte es el CRC                      */
    RX_ETX                  /* el proximo byte tiene que ser 0x03             */
} rx_estado_t;

/* Resultado de procesar un byte. */
typedef enum {
    PASO_SIGUE = 0,         /* byte consumido, el candidato sigue vivo        */
    PASO_MURIO              /* el candidato es invalido: hay que re-sincronizar */
} paso_res_t;

/*
 * Techo de iteraciones por byte de entrada. Es una RED DE SEGURIDAD, no parte
 * del algoritmo: el analisis dice que el peor caso son ~N^2/2 pasos con N=70
 * (unos 2450). Si alguna vez se superara, hay un bug: se corta, se cuenta en
 * err_interno y se sigue funcionando en vez de colgar el lazo principal.
 *
 * En firmware TODO lazo tiene que tener una cota demostrable. Si no la tiene,
 * tarde o temprano el watchdog la encuentra por vos (ver ejercicio E3).
 */
#define RX_MAX_PASOS_POR_BYTE  ((uint32_t)VCP_TRAMA_MAX_BYTES * VCP_TRAMA_MAX_BYTES)

/*
 * Calcula el delta de tiempo de un byte respecto de la base del candidato,
 * saturado a 255 ms.
 *
 * Por que uint8_t y no uint32_t: el timeout entre bytes (5 ms) acota cuanto
 * puede durar un candidato; 70 bytes * 5 ms = 350 ms es el maximo teorico y en
 * la practica son 73 ms (1.04 ms por byte a 9600). Guardar 70 deltas de 1 byte
 * cuesta 70 bytes de RAM en vez de 280.
 *
 * Y si alguna vez se pasara de 255: satura, o sea el byte queda marcado como
 * "muy viejo". Eso hace que la aplicacion NO conteste. Saturar hacia "viejo"
 * es el lado seguro del error: perder una respuesta cuesta un reintento del
 * master; contestar tarde puede hacer que el master tome nuestra respuesta
 * vieja como respuesta al comando nuevo (Anexo A.2).
 */
static uint8_t dt_sat(uint32_t t_abs, uint32_t base)
{
    /* Resta en aritmetica sin signo: se comporta bien cuando el contador de
     * milisegundos da la vuelta a los 49.7 dias. */
    uint32_t d = t_abs - base;
    return (d > 255u) ? 255u : (uint8_t)d;
}

/* Deja el candidato en cero y vuelve a buscar STX. NO toca la cola de
 * reproceso ni los contadores. */
static void rx_reset_candidato(vcp_rx_t *rx)
{
    rx->estado         = (uint8_t)RX_ESPERANDO_STX;
    rx->n              = 0u;
    rx->payload_leidos = 0u;
    rx->crc_calc       = 0u;
    rx->crc_rx         = 0u;
    rx->addr           = 0u;
    rx->len            = 0u;
    rx->cmd            = 0u;
}

/* Emite un evento al callback (si hay callback). */
static void emitir(vcp_rx_cb_t cb, void *usuario, vcp_event_t ev,
                   const vcp_frame_t *trama, uint8_t byte,
                   uint32_t off_ini, uint32_t off_fin,
                   uint32_t t_ms, uint32_t now_ms,
                   bool reproceso)
{
    vcp_rx_evento_t e;

    if (cb == NULL) {
        return;
    }

    e.ev        = ev;
    e.trama     = trama;
    e.byte      = byte;
    e.off_ini   = off_ini;
    e.off_fin   = off_fin;
    e.t_ms      = t_ms;
    e.now_ms    = now_ms;
    e.reproceso = reproceso;

    cb(usuario, &e);
}

/*
 * Guarda un byte en el buffer del candidato.
 *
 * Devuelve false si no entra. Esto NO deberia pasar nunca porque LEN se valida
 * (<= 64) antes de entrar al estado PAYLOAD, y 4 + 64 + 2 = 70 = el tamano del
 * buffer. Pero el requisito E1.3 dice "debe ser IMPOSIBLE desbordar, cualquiera
 * sea la secuencia de bytes de entrada", y la unica forma de que sea imposible
 * de verdad es que el chequeo este en el codigo y no solo en el razonamiento.
 * Defensa en profundidad: si algun dia alguien toca la maquina de estados y se
 * equivoca, el buffer no se pisa; se cuenta un error interno y se sigue.
 */
static bool crudo_push(vcp_rx_t *rx, uint8_t b, uint32_t t_abs)
{
    if (rx->n >= (uint8_t)VCP_TRAMA_MAX_BYTES) {
        rx->st.err_interno++;
        return false;
    }

    rx->crudo[rx->n] = b;

    if (rx->n == 0u) {
        rx->t_stx        = t_abs;   /* el STX fija la base de tiempos */
        rx->dt_crudo[0]  = 0u;
    } else {
        rx->dt_crudo[rx->n] = dt_sat(t_abs, rx->t_stx);
    }

    rx->n++;
    return true;
}

/* ===========================================================================
 * 2) rx_paso: procesa UN byte
 * ========================================================================= */
static paso_res_t rx_paso(vcp_rx_t *rx, uint8_t b, uint32_t t_abs, uint32_t now_ms,
                          uint32_t off, bool reproceso,
                          vcp_rx_cb_t cb, void *usuario)
{
    vcp_frame_t f;

    switch ((rx_estado_t)rx->estado) {

    /* ------------------------------------------------------------------ */
    case RX_ESPERANDO_STX:
        if (b != VCP_STX) {
            /* Ruido entre tramas (Anexo A.2) o resto de un candidato muerto. */
            rx->st.bytes_fuera_de_trama++;
            emitir(cb, usuario, VCP_EV_BYTE_FUERA_TRAMA, NULL, b,
                   off, off, t_abs, now_ms, reproceso);
            return PASO_SIGUE;
        }
        /* Arranca un candidato. Ojo: "candidato", no "trama". Este 0x02 puede
         * ser perfectamente un byte de payload de otra trama o puro ruido. */
        rx->off_stx  = off;
        rx->crc_calc = 0u;
        rx->n        = 0u;
        (void)crudo_push(rx, b, t_abs);     /* n pasa de 0 a 1, siempre entra */
        rx->t_ultimo_byte = t_abs;
        rx->estado = (uint8_t)RX_ADDR;
        return PASO_SIGUE;

    /* ------------------------------------------------------------------ */
    case RX_ADDR:
        if (!crudo_push(rx, b, t_abs)) { return PASO_MURIO; }
        rx->addr     = b;
        rx->crc_calc = vcp_crc8_byte(rx->crc_calc, b);  /* el CRC empieza aca */
        rx->t_ultimo_byte = t_abs;
        rx->estado   = (uint8_t)RX_LEN;
        return PASO_SIGUE;

    /* ------------------------------------------------------------------ */
    case RX_LEN:
        if (!crudo_push(rx, b, t_abs)) { return PASO_MURIO; }
        rx->t_ultimo_byte = t_abs;

        /*
         * VALIDACION CRITICA. Es la unica cosa que impide que un LEN corrupto
         * nos haga reservar mas payload del que entra en el buffer.
         * El rango valido es 0..64 (Anexo A.1). 65 o mas: el candidato muere
         * YA, sin esperar CRC ni ETX, porque ni siquiera tiene sentido seguir
         * juntando bytes.
         *
         * En el vector del Anexo B esto se ejercita con la trama que empieza
         * en el offset 50 y trae LEN = 0x41 = 65.
         */
        if (b > (uint8_t)VCP_MAX_PAYLOAD) {
            rx->st.err_len++;
            emitir(cb, usuario, VCP_EV_ERR_LEN, NULL, b,
                   rx->off_stx, off, t_abs, now_ms, reproceso);
            return PASO_MURIO;
        }

        rx->len      = b;
        rx->crc_calc = vcp_crc8_byte(rx->crc_calc, b);
        rx->estado   = (uint8_t)RX_CMD;
        return PASO_SIGUE;

    /* ------------------------------------------------------------------ */
    case RX_CMD:
        if (!crudo_push(rx, b, t_abs)) { return PASO_MURIO; }
        rx->cmd            = b;
        rx->crc_calc       = vcp_crc8_byte(rx->crc_calc, b);
        rx->payload_leidos = 0u;
        rx->t_ultimo_byte  = t_abs;
        /* Si LEN es 0 no hay payload: se salta directo al CRC. */
        rx->estado = (rx->len > 0u) ? (uint8_t)RX_PAYLOAD : (uint8_t)RX_CRC;
        return PASO_SIGUE;

    /* ------------------------------------------------------------------ */
    case RX_PAYLOAD:
        if (!crudo_push(rx, b, t_abs)) { return PASO_MURIO; }
        rx->crc_calc      = vcp_crc8_byte(rx->crc_calc, b);
        rx->t_ultimo_byte = t_abs;
        rx->payload_leidos++;

        /*
         * ACA ESTA LA CLAVE DEL PROTOCOLO SIN ESCAPADO:
         * el byte se guarda como payload SIN mirar su valor. Si vale 0x02 o
         * 0x03 no pasa nada especial. Lo unico que decide donde termina el
         * payload es el contador contra LEN.
         *
         * Un parser que aca hiciera "if (b == VCP_STX) reiniciar" se romperia
         * con la trama del offset 15 del vector, cuyo payload es
         * 41 42 02 03 43 44 (o sea: "AB", STX, ETX, "CD").
         */
        if (rx->payload_leidos >= rx->len) {
            rx->estado = (uint8_t)RX_CRC;
        }
        return PASO_SIGUE;

    /* ------------------------------------------------------------------ */
    case RX_CRC:
        if (!crudo_push(rx, b, t_abs)) { return PASO_MURIO; }
        /* Se guarda pero NO se compara todavia: primero queremos ver el ETX,
         * porque sin ETX ni siquiera sabemos si este byte era el CRC. */
        rx->crc_rx        = b;
        rx->t_ultimo_byte = t_abs;
        rx->estado        = (uint8_t)RX_ETX;
        return PASO_SIGUE;

    /* ------------------------------------------------------------------ */
    case RX_ETX:
        if (!crudo_push(rx, b, t_abs)) { return PASO_MURIO; }
        rx->t_ultimo_byte = t_abs;

        /*
         * ORDEN DE LAS VALIDACIONES: primero ETX, despues CRC.
         *
         * Por que en ese orden: el ETX es un chequeo ESTRUCTURAL (¿la trama
         * termina donde dijimos que terminaba?) y el CRC es un chequeo de
         * CONTENIDO. Si el ETX no esta, los limites de la trama estan mal, con
         * lo cual el CRC se calculo sobre el conjunto equivocado de bytes y su
         * resultado no significa nada. Reportar "CRC malo" en ese caso seria
         * enganoso para quien despues mira el log: lo que fallo es el encuadre.
         */
        if (b != VCP_ETX) {
            rx->st.err_framing++;
            emitir(cb, usuario, VCP_EV_ERR_FRAMING, NULL, b,
                   rx->off_stx, off, t_abs, now_ms, reproceso);
            return PASO_MURIO;
        }

        if (rx->crc_rx != rx->crc_calc) {
            rx->st.err_crc++;
            emitir(cb, usuario, VCP_EV_ERR_CRC, NULL, b,
                   rx->off_stx, off, t_abs, now_ms, reproceso);
            return PASO_MURIO;
        }

        /* ---- Trama valida ---- */
        f.addr = rx->addr;
        f.cmd  = rx->cmd;
        f.len  = rx->len;
        if (rx->len > 0u) {
            /* El payload arranca en crudo[4]: STX, ADDR, LEN, CMD, payload... */
            memcpy(f.payload, &rx->crudo[4], (size_t)rx->len);
        }
        /* El resto del payload se deja en cero para que el struct sea
         * determinista (util cuando se comparan tramas en los tests). */
        if (rx->len < (uint8_t)VCP_MAX_PAYLOAD) {
            memset(&f.payload[rx->len], 0, (size_t)(VCP_MAX_PAYLOAD - rx->len));
        }

        /*
         * t_etx_ms = el instante REAL en que llego este byte.
         *
         * Para una trama normal es "ahora". Para una trama recuperada
         * reprocesando bytes viejos es un instante PASADO, y por eso guardamos
         * los deltas: sin esto una trama rescatada 7 ms tarde pareceria recien
         * llegada y la contestariamos fuera de plazo (justo lo que E1.14
         * prohibe).
         */
        f.t_etx_ms = t_abs;

        rx->st.tramas_ok++;
        if (reproceso) {
            rx->st.tramas_recuperadas++;
        }

        emitir(cb, usuario, VCP_EV_FRAME, &f, 0u,
               rx->off_stx, off, t_abs, now_ms, reproceso);

        /*
         * Exito: el candidato era una trama de verdad, asi que sus bytes estan
         * consumidos legitimamente. NO se re-sincroniza; se sigue de largo con
         * el byte siguiente.
         */
        rx_reset_candidato(rx);
        return PASO_SIGUE;

    /* ------------------------------------------------------------------ */
    default:
        /* Estado imposible. Si llegamos aca hay corrupcion de memoria o un
         * bug: se reinicia el receptor en vez de seguir con estado invalido. */
        rx->st.err_interno++;
        rx_reset_candidato(rx);
        return PASO_SIGUE;
    }
}

/* ===========================================================================
 * 3) rx_resincronizar: que hacemos cuando un candidato muere
 * ========================================================================= */
/*
 * ESTA FUNCION ES LA POLITICA DE RECUPERACION. Es la decision de diseno que el
 * enunciado pide justificar (E1.4 y el entregable de E1).
 *
 * Situacion: veniamos armando un candidato que arrancaba con un 0x02 en el
 * offset off_stx, y fallo (LEN, CRC o framing). La pregunta es: ¿desde donde
 * seguimos buscando?
 *
 *   VCP_RESYNC_DESCARTE : tiramos los bytes del candidato y seguimos con los
 *       bytes NUEVOS. Barato, pero si aquel 0x02 no era un STX de verdad, los
 *       bytes que le siguen (que SI podian contener una trama buena) se
 *       pierden junto con el.
 *
 *   VCP_RESYNC_REPROCESO : re-inyectamos crudo[1..n-1] en la maquina de
 *       estados. Es decir: "asumo que ese 0x02 era basura y vuelvo a mirar
 *       todo lo demas". No se pierde ni un byte de informacion.
 *
 * Que va primero en la cola: los bytes del candidato muerto (que son los MAS
 * VIEJOS) y despues lo que hubiera quedado sin consumir de una re-sincronizacion
 * anterior. El orden del stream se preserva exactamente.
 *
 * COTA DE MEMORIA: los bytes que vuelven a la cola son siempre un subconjunto
 * de la ventana [off_stx+1 .. ultimo byte recibido], y esa ventana nunca supera
 * VCP_TRAMA_MAX_BYTES-1 bytes, porque un candidato no puede tener mas de
 * VCP_TRAMA_MAX_BYTES bytes. Por eso rep[] tiene el mismo tamano que crudo[].
 */
static void rx_resincronizar(vcp_rx_t *rx)
{
    uint8_t  nb[VCP_TRAMA_MAX_BYTES];   /* bytes nuevos de la cola  */
    uint8_t  nd[VCP_TRAMA_MAX_BYTES];   /* sus deltas de tiempo     */
    uint32_t base_abs;
    uint8_t  k = 0u;
    uint8_t  i;

    if (rx->politica == VCP_RESYNC_DESCARTE) {
        /* Politica simple: a la basura todo, incluida la cola pendiente. */
        rx->rep_n = 0u;
        rx->rep_i = 0u;
        rx_reset_candidato(rx);
        return;
    }

    /* --- Politica de reproceso --- */

    if (rx->n < 2u) {
        /* No hay nada que reprocesar (no deberia pasar: los fallos ocurren
         * con n >= 3). Se limpia y se sigue. */
        rx->rep_n = 0u;
        rx->rep_i = 0u;
        rx_reset_candidato(rx);
        return;
    }

    /* El primer byte de la nueva cola es crudo[1]: fija la base de tiempos. */
    base_abs = rx->t_stx + (uint32_t)rx->dt_crudo[1];

    /* (a) los bytes del candidato muerto, salteando su falso STX */
    for (i = 1u; (i < rx->n) && (k < (uint8_t)VCP_TRAMA_MAX_BYTES); i++) {
        nb[k] = rx->crudo[i];
        nd[k] = dt_sat(rx->t_stx + (uint32_t)rx->dt_crudo[i], base_abs);
        k++;
    }

    /* (b) lo que hubiera quedado sin consumir de una cola anterior */
    for (i = rx->rep_i; (i < rx->rep_n) && (k < (uint8_t)VCP_TRAMA_MAX_BYTES); i++) {
        nb[k] = rx->rep[i];
        nd[k] = dt_sat(rx->rep_t0 + (uint32_t)rx->rep_dt[i], base_abs);
        k++;
    }

    /* Se copia recien ahora: nb/nd son temporales para no pisar rep[] mientras
     * todavia lo estamos leyendo (aliasing). */
    memcpy(rx->rep,    nb, (size_t)k);
    memcpy(rx->rep_dt, nd, (size_t)k);
    rx->rep_n    = k;
    rx->rep_i    = 0u;
    rx->rep_t0   = base_abs;
    rx->rep_off0 = rx->off_stx + 1u;   /* el offset del primer byte re-inyectado */

    rx_reset_candidato(rx);
}

/* ===========================================================================
 * 4) vcp_rx_byte: el lazo principal del receptor
 * ========================================================================= */
void vcp_rx_byte(vcp_rx_t *rx, uint8_t byte, uint32_t now_ms,
                 vcp_rx_cb_t cb, void *usuario)
{
    bool     entrada_pendiente = true;   /* el byte nuevo todavia no se uso */
    uint32_t pasos = 0u;

    if (rx == NULL) {
        return;
    }

    rx->st.bytes_recibidos++;

    /*
     * El lazo procesa bytes de dos fuentes, siempre en este orden:
     *   1) la cola de reproceso (bytes viejos que hay que volver a mirar)
     *   2) el byte nuevo que acaba de entregar la UART
     *
     * Al salir del lazo la cola queda siempre vacia: cada llamada a esta
     * funcion deja el receptor en un estado consistente, sin trabajo diferido.
     * Eso es lo que garantiza que una trama detectada se reporte AL INSTANTE,
     * que es lo que necesita el plazo de 5 ms (E1.10).
     */
    for (;;) {
        uint8_t    b;
        uint32_t   off;
        uint32_t   t_abs;
        bool       reproceso;
        paso_res_t r;

        if (rx->rep_i < rx->rep_n) {
            /* --- fuente 1: cola de reproceso --- */
            b         = rx->rep[rx->rep_i];
            off       = rx->rep_off0 + (uint32_t)rx->rep_i;
            t_abs     = rx->rep_t0   + (uint32_t)rx->rep_dt[rx->rep_i];
            reproceso = true;
            rx->rep_i++;
            rx->st.bytes_reprocesados++;
        } else if (entrada_pendiente) {
            /* --- fuente 2: el byte nuevo --- */
            b         = byte;
            off       = rx->off_stream;
            t_abs     = now_ms;
            reproceso = false;
            rx->off_stream++;
            entrada_pendiente = false;
            /* la cola quedo consumida; se resetea para que no crezca */
            rx->rep_n = 0u;
            rx->rep_i = 0u;
        } else {
            break;   /* no queda nada por procesar */
        }

        r = rx_paso(rx, b, t_abs, now_ms, off, reproceso, cb, usuario);

        if (r == PASO_MURIO) {
            rx_resincronizar(rx);
        }

        /* Red de seguridad (ver comentario de RX_MAX_PASOS_POR_BYTE). */
        pasos++;
        if (pasos > RX_MAX_PASOS_POR_BYTE) {
            rx->st.err_interno++;
            rx->rep_n = 0u;
            rx->rep_i = 0u;
            rx_reset_candidato(rx);
            break;
        }
    }
}

/* ===========================================================================
 * 5) vcp_rx_tick: vencimiento del timeout entre bytes (Anexo A.2)
 * ========================================================================= */
void vcp_rx_tick(vcp_rx_t *rx, uint32_t now_ms, vcp_rx_cb_t cb, void *usuario)
{
    if (rx == NULL) {
        return;
    }

    /* Sin trama a medio armar no hay nada que pueda vencer. */
    if ((rx_estado_t)rx->estado == RX_ESPERANDO_STX) {
        return;
    }

    /*
     * Resta en aritmetica sin signo: si el contador de milisegundos da la
     * vuelta (uint32 -> cada 49.7 dias), (now - antes) sigue dando el intervalo
     * correcto. Escribirlo como (now > antes + 5) SI se rompe en la vuelta.
     * Es el bug clasico de timers en firmware.
     */
    if ((uint32_t)(now_ms - rx->t_ultimo_byte) <= (uint32_t)VCP_TIMEOUT_ENTRE_BYTES_MS) {
        return;
    }

    rx->st.err_timeout++;
    emitir(cb, usuario, VCP_EV_ERR_TIMEOUT, NULL, 0u,
           rx->off_stx, rx->off_stx + (uint32_t)rx->n - 1u,
           rx->t_ultimo_byte, now_ms, false);

    /*
     * POLITICA ANTE TIMEOUT: descartar SIEMPRE, aun cuando la politica general
     * sea de reproceso.
     *
     * Razonamiento: un timeout significa que la linea se callo a mitad de una
     * trama. Cualquier trama que pudiera rescatarse de esos bytes tendria su
     * ETX como minimo 5 ms en el pasado (justamente por eso vencio el timeout),
     * o sea que ya estaria fuera del plazo de respuesta y no la podriamos
     * contestar igual. Reprocesar costaria trabajo para producir un resultado
     * que de todos modos no se puede usar.
     *
     * A diferencia del caso "candidato invalido", donde el reproceso SI puede
     * rescatar una trama a tiempo (o al menos recuperar el encuadre para las
     * tramas siguientes), aca no hay nada que ganar.
     */
    rx->rep_n = 0u;
    rx->rep_i = 0u;
    rx_reset_candidato(rx);
}

/* ===========================================================================
 * API restante
 * ========================================================================= */
void vcp_rx_init(vcp_rx_t *rx)
{
    if (rx == NULL) {
        return;
    }

    /* memset completo: arranca todo en cero, incluidos los contadores. */
    memset(rx, 0, sizeof(*rx));
    rx->estado   = (uint8_t)RX_ESPERANDO_STX;
    rx->politica = VCP_RESYNC_REPROCESO;
}

void vcp_rx_set_resync(vcp_rx_t *rx, vcp_resync_t politica)
{
    if (rx != NULL) {
        rx->politica = politica;
    }
}

const vcp_rx_stats_t *vcp_rx_stats(const vcp_rx_t *rx)
{
    return &rx->st;
}

bool vcp_rx_en_trama(const vcp_rx_t *rx)
{
    return (rx != NULL) && ((rx_estado_t)rx->estado != RX_ESPERANDO_STX);
}

const char *vcp_ev_nombre(vcp_event_t ev)
{
    switch (ev) {
    case VCP_EV_NONE:             return "NONE";
    case VCP_EV_FRAME:            return "TRAMA";
    case VCP_EV_ERR_CRC:          return "ERR_CRC";
    case VCP_EV_ERR_LEN:          return "ERR_LEN";
    case VCP_EV_ERR_FRAMING:      return "ERR_FRAMING";
    case VCP_EV_ERR_TIMEOUT:      return "ERR_TIMEOUT";
    case VCP_EV_BYTE_FUERA_TRAMA: return "BYTE_FUERA_TRAMA";
    default:                      return "?";
    }
}
