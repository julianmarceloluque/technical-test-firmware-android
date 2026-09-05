/*
 * ============================================================================
 *  vcp_rx.h - Receptor incremental de tramas VCP-1
 * ============================================================================
 *
 *  QUE PROBLEMA RESUELVE
 *  ---------------------
 *  Los bytes llegan de a uno desde la interrupcion de la UART. No existe
 *  "leer una trama"; existe "me llego un byte, veremos". Este modulo es la
 *  maquina de estados que va juntando esos bytes hasta que aparece una trama
 *  completa y validada, y que avisa cuando algo salio mal.
 *
 *  REGLAS QUE CUMPLE (requisitos E1.1 a E1.6)
 *  ------------------------------------------
 *   - E1.1  Entra UN byte y la funcion retorna. Nunca bloquea, nunca espera.
 *   - E1.2  Cero malloc/free. Cero variables globales mutables: TODO el estado
 *           vive en vcp_rx_t, que provee el llamador. Se pueden tener dos
 *           receptores en el mismo programa sin que se pisen (util en tests).
 *   - E1.3  Imposible desbordar: el buffer se dimensiona con VCP_TRAMA_MAX_BYTES
 *           y ademas cada escritura verifica el indice antes de escribir.
 *   - E1.4  Ante trama invalida se re-sincroniza y sigue aceptando tramas.
 *   - E1.5  Cada error se reporta con un evento propio (CRC / LEN / FRAMING /
 *           TIMEOUT / byte fuera de trama), no se descarta en silencio.
 *   - E1.6  El timeout entre bytes esta en la interfaz: vcp_rx_byte() recibe la
 *           marca de tiempo y vcp_rx_tick() permite que el plazo venza aunque
 *           dejen de llegar bytes.
 *
 *  LA MAQUINA DE ESTADOS
 *  ---------------------
 *
 *      ESPERANDO_STX --(byte==0x02)--> ADDR --> LEN --> CMD --+
 *            ^                                                |
 *            |                                       (len>0)  |  (len==0)
 *            |                                                v
 *            |                                            PAYLOAD
 *            |                                                |
 *            |                                                v
 *            +---(trama OK / error / timeout)------------    CRC --> ETX
 *
 *  En ESPERANDO_STX cualquier byte distinto de 0x02 se descarta como "byte
 *  espurio" (el Anexo A.2 avisa que la linea es ruidosa y que entre tramas
 *  pueden aparecer bytes sueltos).
 *
 *  LA POLITICA DE RE-SINCRONIZACION (la decision de diseno central)
 *  ----------------------------------------------------------------
 *  Cuando un candidato a trama falla hay que decidir DESDE DONDE seguir
 *  buscando. Hay dos opciones razonables y este modulo implementa las dos
 *  para poder compararlas en el banco de pruebas:
 *
 *   a) VCP_RESYNC_DESCARTE  - "tiro todo y busco el proximo 0x02 a partir del
 *      byte siguiente al error". Simple y barato.
 *
 *   b) VCP_RESYNC_REPROCESO - "el 0x02 con el que arranque puede no haber sido
 *      un STX de verdad, sino un byte de payload o de ruido. Vuelvo a mirar los
 *      bytes que ya consumi, empezando UNO DESPUES de ese 0x02, por si adentro
 *      habia una trama de verdad". Es la que usamos por defecto.
 *
 *  Con el vector del Anexo B la diferencia es concreta: la opcion (a) pierde
 *  DOS tramas validas que la opcion (b) encuentra.
 *  Ver docs/02_E1_analisis_vector.md.
 *
 *  Costo de (b): en el peor caso, reprocesar un candidato de N bytes cuesta
 *  O(N^2) pasos de maquina de estados, con N <= 70. Son unos 2400 pasos de
 *  tres instrucciones en el PEOR caso absoluto, y solo cuando hay corrupcion.
 *  El camino normal (trama sana) sigue siendo un paso por byte.
 *
 *  EFECTO SECUNDARIO IMPORTANTE (por eso existe vcp_frame_t.t_etx_ms):
 *  una trama recuperada por reproceso se descubre DESPUES de su propio ETX
 *  (hay que esperar a que el candidato falso muera). Si ese retardo supera los
 *  5 ms del Anexo A.2, la trama esta detectada pero YA NO SE PUEDE CONTESTAR.
 *  Quien decide eso es la capa de aplicacion (vcp_app.c), no el receptor: el
 *  receptor informa el hecho, la aplicacion aplica la politica.
 * ============================================================================
 */

#ifndef VCP_RX_H
#define VCP_RX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "vcp/vcp_cfg.h"
#include "vcp/vcp_frame.h"

/* ---------------------------------------------------------------------------
 * Eventos que el receptor le reporta a la capa de arriba
 * ------------------------------------------------------------------------- */
typedef enum {
    VCP_EV_NONE = 0,          /* nada que reportar (no se emite al callback) */
    VCP_EV_FRAME,             /* trama completa y validada                   */
    VCP_EV_ERR_CRC,           /* estructura ok, pero el CRC no coincide      */
    VCP_EV_ERR_LEN,           /* LEN > 64: imposible, se corta ahi mismo     */
    VCP_EV_ERR_FRAMING,       /* donde tenia que ir el ETX habia otra cosa   */
    VCP_EV_ERR_TIMEOUT,       /* >5 ms sin bytes con una trama a medio armar */

    /* AGREGADO respecto del vcp.h sugerido:
     * un byte descartado mientras buscabamos STX. Sirve al requisito E1.5
     * ("reportar los errores de forma diferenciada"): permite separar
     * "linea ruidosa" de "tramas rotas", que son dos fallas distintas con dos
     * causas distintas. El banco de pruebas los agrupa para no inundar. */
    VCP_EV_BYTE_FUERA_TRAMA
} vcp_event_t;

/* ---------------------------------------------------------------------------
 * Politica de re-sincronizacion (ver cabecera del archivo)
 * ------------------------------------------------------------------------- */
typedef enum {
    VCP_RESYNC_REPROCESO = 0, /* por defecto: re-examina desde STX+1           */
    VCP_RESYNC_DESCARTE       /* alternativa simple: descarta todo el candidato */
} vcp_resync_t;

/* ---------------------------------------------------------------------------
 * Contadores de diagnostico
 * ------------------------------------------------------------------------- */
/*
 * Por que contadores y no solo eventos: en campo nadie mira un log byte a byte.
 * Lo que sirve es "en las ultimas 24 h hubo 12.000 tramas y 4 errores de CRC".
 * Estos mismos contadores son los que en el ejercicio E3.5 propongo mandar por
 * telemetria para diagnosticar en una hora en vez de en cinco dias.
 */
typedef struct {
    uint32_t bytes_recibidos;       /* bytes entregados por la UART             */
    uint32_t bytes_reprocesados;    /* re-inyectados por re-sincronizacion      */
    uint32_t bytes_fuera_de_trama;  /* descartados buscando STX                 */

    uint32_t tramas_ok;             /* tramas validas (de cualquier direccion)  */
    uint32_t tramas_recuperadas;    /* de esas, cuantas aparecieron reprocesando*/

    uint32_t err_crc;
    uint32_t err_len;
    uint32_t err_framing;
    uint32_t err_timeout;
    uint32_t err_interno;           /* la red de seguridad; deberia quedar en 0 */
} vcp_rx_stats_t;

/* ---------------------------------------------------------------------------
 * Descripcion de un evento entregado al callback
 * ------------------------------------------------------------------------- */
typedef struct {
    vcp_event_t        ev;
    const vcp_frame_t *trama;     /* != NULL solo si ev == VCP_EV_FRAME       */
    uint8_t            byte;      /* solo si ev == VCP_EV_BYTE_FUERA_TRAMA    */
    uint32_t           off_ini;   /* offset del STX del candidato             */
    uint32_t           off_fin;   /* offset del byte donde se detecto         */

    /*
     * OJO CON ESTOS DOS TIEMPOS, que es donde se juega el requisito E1.14:
     *
     *   t_ms   : cuando LLEGO REALMENTE por la linea el byte que cierra el
     *            evento (el ETX, en el caso de una trama valida).
     *   now_ms : que hora es AHORA, o sea el instante en que el lazo principal
     *            se esta enterando del evento.
     *
     * Para una trama normal los dos valen lo mismo. Para una trama recuperada
     * re-sincronizando, now_ms > t_ms: la trama existe pero se descubrio
     * tarde. La diferencia entre ambos es exactamente lo que hay que comparar
     * contra el plazo de 5 ms del Anexo A.2.
     */
    uint32_t           t_ms;
    uint32_t           now_ms;

    bool               reproceso; /* surgio re-examinando bytes ya recibidos  */
} vcp_rx_evento_t;

/*
 * Callback de eventos.
 *
 * POR QUE UN CALLBACK Y NO "retornar el evento" COMO EN EL vcp.h SUGERIDO
 * ----------------------------------------------------------------------
 * La interfaz sugerida era:
 *     vcp_event_t vcp_rx_byte(rx, byte, now_ms, out);
 * es decir: un byte de entrada -> como maximo un evento de salida.
 *
 * Con la politica de reproceso eso deja de ser cierto: un solo byte puede
 * disparar un error Y ademas, al re-examinar el candidato fallido, descubrir
 * una trama valida adentro. Son DOS eventos para UN byte. Con la firma
 * original habria que elegir cual reportar y perder el otro.
 *
 * Alternativas que descarte:
 *   - Cola de eventos en el contexto: hay que dimensionarla para el peor caso
 *     (hasta 11 tramas minimas dentro de un candidato de 70 bytes) y agrega un
 *     "drenar la cola" que el llamador se puede olvidar de hacer.
 *   - Recursion: prohibida en firmware, la pila no es negociable.
 *
 * El callback no aloca, no crece, y se ejecuta en el mismo instante en que el
 * evento ocurre, que es justo lo que necesita el plazo de 5 ms.
 */
typedef void (*vcp_rx_cb_t)(void *usuario, const vcp_rx_evento_t *ev);

/* ---------------------------------------------------------------------------
 * Contexto del receptor
 * ------------------------------------------------------------------------- */
/*
 * Los campos estan visibles solo porque en C no hay otra forma de que el
 * llamador reserve la estructura sin malloc. Se los trata como PRIVADOS:
 * fuera de vcp_rx.c nadie los toca. Para leer contadores esta vcp_rx_stats().
 */
typedef struct {
    /* --- maquina de estados --- */
    uint8_t  estado;                          /* rx_estado_t, interno         */

    /* --- candidato en curso ---
     * 'crudo' guarda los bytes TAL CUAL llegaron, incluido el STX en crudo[0].
     * Se guardan crudos (y no solo el payload) porque la re-sincronizacion
     * necesita volver a pasarlos por la maquina de estados. */
    uint8_t  crudo[VCP_TRAMA_MAX_BYTES];
    uint8_t  n;                               /* bytes usados de crudo        */
    uint8_t  payload_leidos;                  /* bytes de payload ya juntados */

    /* --- marcas de tiempo de cada byte del candidato ---
     * t_stx     : instante absoluto (ms) en que llego crudo[0].
     * dt_crudo  : para cada byte, cuantos ms despues del STX llego, saturado
     *             a 255. Con 1 byte por posicion en vez de 4 se ahorra RAM y
     *             alcanza de sobra: un candidato dura como mucho 70*5 = 350 ms
     *             por el timeout entre bytes, y en la practica 73 ms.
     *
     * Para que hace falta esto: cuando una trama se recupera reprocesando
     * bytes viejos, su ETX ocurrio EN EL PASADO. Sin estas marcas, la trama
     * rescatada pareceria recien llegada y la contestariamos fuera de plazo,
     * que es exactamente lo que prohibe el requisito E1.14. */
    uint32_t t_stx;
    uint8_t  dt_crudo[VCP_TRAMA_MAX_BYTES];

    /* --- campos ya parseados del candidato --- */
    uint8_t  addr;
    uint8_t  len;
    uint8_t  cmd;
    uint8_t  crc_rx;                          /* el CRC que vino en la trama  */
    uint8_t  crc_calc;                        /* el que venimos acumulando    */

    /* --- tiempos --- */
    uint32_t t_ultimo_byte;                   /* base del timeout entre bytes */

    /* --- buffer de re-sincronizacion ---
     * Cuando un candidato falla, sus bytes (menos el primero) se copian aca
     * para volver a pasarlos por la maquina de estados. */
    uint8_t  rep[VCP_TRAMA_MAX_BYTES];
    uint8_t  rep_dt[VCP_TRAMA_MAX_BYTES];     /* mismas marcas, misma idea    */
    uint8_t  rep_n;                           /* cuantos hay                  */
    uint8_t  rep_i;                           /* cuantos ya se consumieron    */
    uint32_t rep_off0;                        /* offset del primero           */
    uint32_t rep_t0;                          /* instante absoluto de rep[0]  */

    /* --- offsets, solo diagnostico e informes --- */
    uint32_t off_stream;                      /* offset del proximo byte nuevo*/
    uint32_t off_stx;                         /* offset del STX del candidato */

    /* --- configuracion --- */
    vcp_resync_t politica;

    /* --- contadores --- */
    vcp_rx_stats_t st;
} vcp_rx_t;

/* ---------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/*
 * Deja el receptor listo para arrancar. Se puede llamar EN CUALQUIER MOMENTO
 * (por ejemplo si la capa de arriba decide que la linea se volvio loca y
 * quiere empezar de cero). Politica por defecto: VCP_RESYNC_REPROCESO.
 */
void vcp_rx_init(vcp_rx_t *rx);

/* Cambia la politica de re-sincronizacion. Pensada para el banco de pruebas,
 * que corre el mismo vector con las dos y muestra la diferencia. */
void vcp_rx_set_resync(vcp_rx_t *rx, vcp_resync_t politica);

/*
 * Procesa UN byte recibido.
 *
 *   byte    : el byte que entrego la UART.
 *   now_ms  : marca de tiempo monotonica, en ms, del INSTANTE DE RECEPCION.
 *             En un port real la toma la interrupcion de UART y la encola
 *             junto al byte; asi el plazo de 5 ms se mide desde que el byte
 *             entro de verdad, y no desde que el lazo principal lo miro.
 *   cb      : se invoca 0, 1 o varias veces con los eventos que este byte
 *             haya provocado. Puede ser NULL (el receptor sigue contando).
 *   usuario : puntero opaco que se le reenvia al callback.
 *
 * No bloquea, no reserva memoria, no recursiona.
 */
void vcp_rx_byte(vcp_rx_t *rx, uint8_t byte, uint32_t now_ms,
                 vcp_rx_cb_t cb, void *usuario);

/*
 * A llamar periodicamente desde el lazo principal.
 *
 * Sirve para UNA sola cosa: que el timeout entre bytes pueda vencer cuando
 * dejan de llegar bytes. Si la trama se corta a la mitad, sin este tick el
 * receptor se quedaria esperando el resto para siempre y la proxima trama
 * buena se pegaria con la basura anterior.
 */
void vcp_rx_tick(vcp_rx_t *rx, uint32_t now_ms, vcp_rx_cb_t cb, void *usuario);

/* Acceso de solo lectura a los contadores. */
const vcp_rx_stats_t *vcp_rx_stats(const vcp_rx_t *rx);

/* true si hay una trama a medio recibir (util para tests y telemetria). */
bool vcp_rx_en_trama(const vcp_rx_t *rx);

/* Nombre legible del evento, para logs. Nunca devuelve NULL. */
const char *vcp_ev_nombre(vcp_event_t ev);

#endif /* VCP_RX_H */
