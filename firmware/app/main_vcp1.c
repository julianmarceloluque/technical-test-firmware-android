/*
 * ============================================================================
 *  main_vcp1.c - Banco de pruebas del receptor/respondedor (requisito E1.7)
 * ============================================================================
 *
 *  QUE HACE
 *  --------
 *  Lee un vector de bytes en hexadecimal (por archivo, por argumento o por la
 *  entrada estandar), se lo entrega byte a byte al receptor VCP-1 y muestra:
 *
 *    - cada trama detectada, con sus campos ya desarmados;
 *    - que decidio la capa de aplicacion y QUE TRAMA habria transmitido;
 *    - cada error, diferenciado por tipo, con el detalle de por que fallo;
 *    - un resumen final con todos los contadores.
 *
 *  El vector NO esta embebido en el codigo (lo pide E1.7): con el archivo
 *  Anexos/stream_vcp1.txt tiene que funcionar sin tocar nada.
 *
 *  EL RELOJ SIMULADO
 *  -----------------
 *  El plazo de 5 ms del Anexo A.2 no se puede medir de verdad en una PC, pero
 *  SI se puede simular: a 9600 8N1 cada byte tarda 10 bits / 9600 = 1.0417 ms.
 *  El banco le pasa al receptor esa marca de tiempo por cada byte, con lo cual
 *  el plazo se evalua igual que en el equipo real.
 *
 *  Esto hace visible algo que de otra forma quedaria escondido: la trama del
 *  offset 58 del Anexo B se RECUPERA re-sincronizando, pero se descubre unos
 *  8 ms despues de su propio ETX, o sea fuera de plazo. El programa muestra
 *  las dos cosas: que le habria contestado, y por que no lo hace.
 *  Con --sin-plazo se ve el otro escenario.
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "vcp/vcp.h"
#include "port_pc.h"

#define MAX_ENTRADA  8192u   /* mas que suficiente para cualquier captura */

/* ---------------------------------------------------------------------------
 * Contexto del banco de pruebas (todo el estado del programa, sin globales)
 * ------------------------------------------------------------------------- */
typedef struct {
    const uint8_t *ent;          /* el vector de entrada completo            */
    size_t         n_ent;

    vcp_app_t     *app;
    double         byte_ms;      /* ms por byte del reloj simulado           */
    int            silencioso;   /* -q: procesa igual, pero no imprime detalle */

    unsigned       n_trama;
    unsigned       n_error;

    /* Agrupacion de bytes fuera de trama, para no imprimir uno por linea. */
    int            ruido_activo;
    uint32_t       ruido_off0;
    uint32_t       ruido_n;
    double         ruido_t0;
    int            ruido_repro;
} banco_t;

/* ---------------------------------------------------------------------------
 * Utilidades de impresion
 * ------------------------------------------------------------------------- */
static void imprimir_hex(const uint8_t *d, size_t n)
{
    size_t i;
    for (i = 0u; i < n; i++) {
        printf("%02X%s", (unsigned)d[i], (i + 1u < n) ? " " : "");
    }
}

/* Imprime el tramo [ini..fin] del vector de entrada. */
static void imprimir_tramo(const banco_t *b, uint32_t ini, uint32_t fin)
{
    if ((size_t)fin >= b->n_ent) {
        fin = (uint32_t)(b->n_ent - 1u);
    }
    if (ini > fin) {
        printf("(vacio)");
        return;
    }
    imprimir_hex(&b->ent[ini], (size_t)(fin - ini + 1u));
}

static double t_de_offset(const banco_t *b, uint32_t off)
{
    return (double)off * b->byte_ms;
}

/* Si veniamos juntando bytes de ruido, los vuelca en una sola linea. */
static void flush_ruido(banco_t *b)
{
    if (!b->ruido_activo) {
        return;
    }
    if (b->silencioso) {
        b->ruido_activo = 0;
        return;
    }
    printf("  t=%8.2f ms  off %u..%u  %-10s %u byte(s) descartados: ",
           b->ruido_t0,
           (unsigned)b->ruido_off0,
           (unsigned)(b->ruido_off0 + b->ruido_n - 1u),
           b->ruido_repro ? "RE-EXAMEN" : "RUIDO",
           (unsigned)b->ruido_n);
    imprimir_tramo(b, b->ruido_off0, b->ruido_off0 + b->ruido_n - 1u);
    printf("\n");
    b->ruido_activo = 0;
}

static void linea(void)
{
    printf("  ----------------------------------------------------------------"
           "------------\n");
}

/* ---------------------------------------------------------------------------
 * Interpretacion legible del payload de las tramas que nos importan
 * ------------------------------------------------------------------------- */
static void describir_payload(const vcp_frame_t *f)
{
    switch (f->cmd) {
    case VCP_CMD_VEND_REQUEST:
        if (f->len == 3u) {
            printf("               seleccion=%u  precio=%u centavos\n",
                   (unsigned)f->payload[0],
                   (unsigned)vcp_get_u16be(&f->payload[1]));
        }
        break;
    case VCP_CMD_VEND_SUCCESS:
        if (f->len == 2u) {
            printf("               txid=%u (0x%04X)\n",
                   (unsigned)vcp_get_u16be(f->payload),
                   (unsigned)vcp_get_u16be(f->payload));
        }
        break;
    case VCP_CMD_VEND_FAILURE:
        if (f->len == 3u) {
            printf("               txid=%u  causa=0x%02X\n",
                   (unsigned)vcp_get_u16be(f->payload),
                   (unsigned)f->payload[2]);
        }
        break;
    case VCP_CMD_VEND_APPROVE:
        if (f->len == 2u) {
            printf("               txid=%u (0x%04X)\n",
                   (unsigned)vcp_get_u16be(f->payload),
                   (unsigned)vcp_get_u16be(f->payload));
        }
        break;
    case VCP_CMD_LOG_EVENT: {
        size_t i;
        printf("               texto=\"");
        for (i = 0u; i < f->len; i++) {
            int c = (int)f->payload[i];
            printf("%c", isprint(c) ? c : '.');
        }
        printf("\"\n");
        break;
    }
    default:
        break;
    }
}

/* Explica el contenido de la respuesta que armamos. */
static void describir_respuesta(const vcp_decision_t *d)
{
    switch (d->resp_cmd) {
    case VCP_CMD_STATUS_RESP: {
        int16_t t = (int16_t)vcp_get_u16be(&d->resp_payload[2]);
        printf("               estado=0x%02X %s  reservado=0x%02X  temp=%.1f C\n",
               (unsigned)d->resp_payload[0],
               (d->resp_payload[0] == VCP_ESTADO_LISTO) ? "LISTO" : "OCUPADO",
               (unsigned)d->resp_payload[1],
               (double)t / 10.0);
        break;
    }
    case VCP_CMD_VEND_APPROVE:
        printf("               txid=%u\n",
               (unsigned)vcp_get_u16be(d->resp_payload));
        break;
    case VCP_CMD_VEND_DENY:
        printf("               motivo=0x%02X (%s)\n",
               (unsigned)d->resp_payload[0],
               vcp_deny_nombre(d->resp_payload[0]));
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * El callback del receptor: aca se ve el flujo completo de una trama
 * ------------------------------------------------------------------------- */
static void on_evento(void *usuario, const vcp_rx_evento_t *ev)
{
    banco_t       *b = (banco_t *)usuario;
    vcp_decision_t dec;

    /* Los bytes de ruido se agrupan; cualquier otro evento primero los vuelca. */
    if (ev->ev == VCP_EV_BYTE_FUERA_TRAMA) {
        if (!b->ruido_activo ||
            (b->ruido_off0 + b->ruido_n != ev->off_ini) ||
            (b->ruido_repro != (int)ev->reproceso)) {
            flush_ruido(b);
            b->ruido_activo = 1;
            b->ruido_off0   = ev->off_ini;
            b->ruido_n      = 0u;
            b->ruido_t0     = t_de_offset(b, ev->off_ini);
            b->ruido_repro  = (int)ev->reproceso;
        }
        b->ruido_n++;
        return;
    }
    flush_ruido(b);

    /* --------------------------------------------------------------- */
    if (ev->ev == VCP_EV_FRAME) {
        const vcp_frame_t *f = ev->trama;

        b->n_trama++;

        /* En modo -q la trama se procesa igual (hay que responderla): lo
         * unico que se saltea es la impresion del detalle. */
        if (b->silencioso) {
            vcp_app_on_frame(b->app, f, ev->now_ms, &dec);
            return;
        }

        linea();
        printf("  TRAMA #%u   off %u..%u   t_etx=%.2f ms%s\n",
               b->n_trama,
               (unsigned)ev->off_ini, (unsigned)ev->off_fin,
               t_de_offset(b, ev->off_fin),
               ev->reproceso ? "   [RECUPERADA re-sincronizando]" : "");

        printf("    crudo    : ");
        imprimir_tramo(b, ev->off_ini, ev->off_fin);
        printf("\n");

        printf("    campos   : ADDR=0x%02X %-9s LEN=%-2u CMD=0x%02X %s\n",
               (unsigned)f->addr,
               (f->addr == VCP_ADDR_SELF)      ? "(propia)" :
               (f->addr == VCP_ADDR_BROADCAST) ? "(bcast)"  : "(ajena)",
               (unsigned)f->len,
               (unsigned)f->cmd, vcp_cmd_nombre(f->cmd));

        if (f->len > 0u) {
            printf("    payload  : ");
            imprimir_hex(f->payload, f->len);
            printf("\n");
            describir_payload(f);
        } else {
            printf("    payload  : (vacio)\n");
        }

        /* ---- AQUI SE DECIDE Y SE TRANSMITE ----
         * Fijate que entre la deteccion de la trama y esta llamada no hay
         * ninguna espera, ningun buffer intermedio y ninguna cola. Es lo que
         * pide el requisito E1.10/E1.14. */
        vcp_app_on_frame(b->app, f, ev->now_ms, &dec);

        printf("    decision : %s", vcp_dec_nombre(dec.tipo));
        if (dec.hay_respuesta) {
            printf(" -> %s", vcp_cmd_nombre(dec.resp_cmd));
        }
        printf("   (retardo desde ETX: %u ms)\n", (unsigned)dec.retardo_ms);
        printf("    nota     : %s\n", dec.nota);

        if (dec.tipo == VCP_DEC_RESPONDER) {
            printf("    TX       : ");
            imprimir_hex(dec.tx, dec.tx_len);
            printf("\n");
            describir_respuesta(&dec);
        } else if (dec.tipo == VCP_DEC_FUERA_DE_PLAZO) {
            printf("    TX       : ");
            imprimir_hex(dec.tx, dec.tx_len);
            printf("   <-- NO TRANSMITIDA\n");
            describir_respuesta(&dec);
            printf("    motivo   : el ETX fue hace %u ms y el plazo es %u ms.\n"
                   "               Contestar ahora se superpondria con la trama\n"
                   "               siguiente del master (Anexo A.2). El master\n"
                   "               reintenta a los 100 ms.\n",
                   (unsigned)dec.retardo_ms, (unsigned)VCP_PLAZO_RESPUESTA_MS);
        } else {
            printf("    TX       : (nada)\n");
        }
        return;
    }

    /* --------------------------------------------------------------- */
    /* A partir de aca, errores. */
    b->n_error++;
    if (b->silencioso) {
        return;
    }
    linea();
    printf("  ERROR #%u   %s   off %u..%u   t=%.2f ms%s\n",
           b->n_error, vcp_ev_nombre(ev->ev),
           (unsigned)ev->off_ini, (unsigned)ev->off_fin,
           t_de_offset(b, ev->off_fin),
           ev->reproceso ? "   [durante re-sincronizacion]" : "");

    printf("    crudo    : ");
    imprimir_tramo(b, ev->off_ini, ev->off_fin);
    printf("\n");

    switch (ev->ev) {
    case VCP_EV_ERR_LEN:
        printf("    detalle  : LEN=0x%02X (%u) y el maximo del protocolo es %u.\n",
               (unsigned)ev->byte, (unsigned)ev->byte, (unsigned)VCP_MAX_PAYLOAD);
        printf("               Se corta en el acto, sin esperar CRC ni ETX:\n"
               "               seguir juntando bytes con un LEN imposible es\n"
               "               justamente lo que desborda un buffer.\n");
        break;

    case VCP_EV_ERR_CRC: {
        /* Recalculamos con la misma funcion publica para mostrar el detalle. */
        uint32_t ini = ev->off_ini;
        uint8_t  len = b->ent[ini + 2u];
        uint8_t  esperado = vcp_crc8(&b->ent[ini + 1u], (size_t)(3u + len));
        printf("    detalle  : CRC recibido 0x%02X, calculado 0x%02X.\n",
               (unsigned)b->ent[ini + 4u + len], (unsigned)esperado);
        printf("               ADDR=0x%02X LEN=%u CMD=0x%02X (%s): la estructura\n"
               "               cierra pero el contenido no es el que salio del\n"
               "               emisor. Se descarta entera.\n",
               (unsigned)b->ent[ini + 1u], (unsigned)len,
               (unsigned)b->ent[ini + 3u], vcp_cmd_nombre(b->ent[ini + 3u]));
        break;
    }

    case VCP_EV_ERR_FRAMING:
        printf("    detalle  : donde tenia que ir el ETX (0x03) habia 0x%02X.\n",
               (unsigned)ev->byte);
        printf("               El encuadre esta mal: el 0x02 del offset %u no\n"
               "               era un STX de verdad.\n", (unsigned)ev->off_ini);
        break;

    case VCP_EV_ERR_TIMEOUT:
        printf("    detalle  : pasaron mas de %u ms sin bytes con una trama a\n"
               "               medio armar. Se aborta y se vuelve a buscar STX.\n",
               (unsigned)VCP_TIMEOUT_ENTRE_BYTES_MS);
        break;

    default:
        break;
    }

    printf("    accion   : no se transmite nada (tabla A.4). El master\n"
           "               reintenta a los 100 ms.\n");
    return;
}

/* ---------------------------------------------------------------------------
 * Lectura del vector en hexadecimal
 * ------------------------------------------------------------------------- */
/*
 * Acepta el formato del Anexo B y algunas variantes comunes:
 *   - bytes en hexadecimal separados por espacios, tabs o saltos de linea
 *   - prefijo 0x opcional
 *   - comas
 *   - lineas de comentario que empiezan con # o //
 */
static int parsear_hex(const char *txt, uint8_t *out, size_t max, size_t *n_out)
{
    size_t n = 0u;
    const char *p = txt;

    /*
     * Muchos editores de Windows guardan con BOM (los tres bytes EF BB BF
     * al principio del archivo). No es parte del contenido y hay que
     * saltearlo, o el primer "byte hexadecimal" seria basura.
     */
    if (((unsigned char)p[0] == 0xEFu) &&
        ((unsigned char)p[1] == 0xBBu) &&
        ((unsigned char)p[2] == 0xBFu)) {
        p += 3;
    }

    while (*p != '\0') {
        int hi, lo;

        /* comentarios */
        if ((*p == '#') || ((p[0] == '/') && (p[1] == '/'))) {
            while ((*p != '\0') && (*p != '\n')) { p++; }
            continue;
        }
        if (isspace((unsigned char)*p) || (*p == ',') || (*p == ';')) {
            p++;
            continue;
        }
        if ((p[0] == '0') && ((p[1] == 'x') || (p[1] == 'X'))) {
            p += 2;
            continue;
        }
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) {
            fprintf(stderr,
                    "error: caracter inesperado en la entrada, "
                    "posicion %u: 0x%02X\n",
                    (unsigned)(p - txt), (unsigned)(unsigned char)*p);
            fprintf(stderr,
                    "  se esperan pares de digitos hexadecimales separados "
                    "por espacios.\n"
                    "  en PowerShell conviene pasar el archivo como argumento: "
                    "su tuberia\n"
                    "  re-codifica el texto. Para stdin: "
                    "cmd /c \"type archivo.txt | vcp1.exe\"\n");
            return -1;
        }

        hi = isdigit((unsigned char)p[0]) ? (p[0] - '0')
                                          : (tolower((unsigned char)p[0]) - 'a' + 10);
        lo = isdigit((unsigned char)p[1]) ? (p[1] - '0')
                                          : (tolower((unsigned char)p[1]) - 'a' + 10);
        if (n >= max) {
            fprintf(stderr, "error: la entrada supera %u bytes\n", (unsigned)max);
            return -1;
        }
        out[n++] = (uint8_t)((hi << 4) | lo);
        p += 2;
    }

    *n_out = n;
    return 0;
}

static int leer_todo(FILE *fp, char *dst, size_t max)
{
    size_t n = fread(dst, 1u, max - 1u, fp);
    dst[n] = '\0';
    return (int)n;
}

/* ---------------------------------------------------------------------------
 * Ayuda
 * ------------------------------------------------------------------------- */
static void uso(const char *prog)
{
    printf(
"uso: %s [opciones] [archivo]\n"
"\n"
"  Sin archivo, lee el vector de la entrada estandar.\n"
"\n"
"  --hex \"AA 55 ..\"   vector pasado directo por argumento\n"
"  --byte-ms <n>      ms por byte del reloj simulado (default 1.042 = 9600 8N1;\n"
"                     0 = sin simulacion de tiempo, todo llega instantaneo)\n"
"  --resync <modo>    reproceso | descarte   (default: reproceso)\n"
"  --sin-plazo        no aplicar el corte de 5 ms del Anexo A.2\n"
"  --addr <hh>        direccion propia en hex (default 0A)\n"
"  -q                 solo el resumen final\n"
"  -h, --help         esta ayuda\n"
"\n"
"ejemplos:\n"
"  %s ../../Anexos/stream_vcp1.txt\n"
"  type ..\\..\\Anexos\\stream_vcp1.txt | %s\n"
"  %s --resync descarte ../../Anexos/stream_vcp1.txt\n",
    prog, prog, prog, prog);
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    static char    texto[MAX_ENTRADA * 4u];
    static uint8_t bytes[MAX_ENTRADA];
    size_t         n_bytes = 0u;

    vcp_rx_t      rx;
    vcp_app_t     app;
    banco_t       banco;
    vcp_resync_t  politica   = VCP_RESYNC_REPROCESO;
    double        byte_ms    = 10.0 / 9.6;   /* 1.0416... ms a 9600 8N1 */
    bool          plazo      = true;
    uint8_t       addr       = (uint8_t)VCP_ADDR_SELF;
    int           quiet      = 0;
    const char   *archivo    = NULL;
    const char   *hex_arg    = NULL;
    size_t        i;
    int           a;

    /* ---- 1) Argumentos ---- */
    for (a = 1; a < argc; a++) {
        if ((strcmp(argv[a], "-h") == 0) || (strcmp(argv[a], "--help") == 0)) {
            uso(argv[0]);
            return 0;
        } else if ((strcmp(argv[a], "--hex") == 0) && ((a + 1) < argc)) {
            hex_arg = argv[++a];
        } else if ((strcmp(argv[a], "--byte-ms") == 0) && ((a + 1) < argc)) {
            byte_ms = atof(argv[++a]);
        } else if ((strcmp(argv[a], "--resync") == 0) && ((a + 1) < argc)) {
            a++;
            if (strcmp(argv[a], "descarte") == 0) {
                politica = VCP_RESYNC_DESCARTE;
            } else if (strcmp(argv[a], "reproceso") == 0) {
                politica = VCP_RESYNC_REPROCESO;
            } else {
                fprintf(stderr, "error: --resync espera 'reproceso' o 'descarte'\n");
                return 2;
            }
        } else if (strcmp(argv[a], "--sin-plazo") == 0) {
            plazo = false;
        } else if ((strcmp(argv[a], "--addr") == 0) && ((a + 1) < argc)) {
            addr = (uint8_t)strtoul(argv[++a], NULL, 16);
        } else if (strcmp(argv[a], "-q") == 0) {
            quiet = 1;
        } else if (argv[a][0] == '-') {
            fprintf(stderr, "error: opcion desconocida '%s'\n", argv[a]);
            uso(argv[0]);
            return 2;
        } else {
            archivo = argv[a];
        }
    }

    /* ---- 2) Entrada ---- */
    if (hex_arg != NULL) {
        strncpy(texto, hex_arg, sizeof(texto) - 1u);
        texto[sizeof(texto) - 1u] = '\0';
    } else if (archivo != NULL) {
        FILE *fp = fopen(archivo, "rb");
        if (fp == NULL) {
            fprintf(stderr, "error: no puedo abrir '%s'\n", archivo);
            return 2;
        }
        (void)leer_todo(fp, texto, sizeof(texto));
        fclose(fp);
    } else {
        (void)leer_todo(stdin, texto, sizeof(texto));
    }

    if (parsear_hex(texto, bytes, sizeof(bytes), &n_bytes) != 0) {
        return 2;
    }
    if (n_bytes == 0u) {
        fprintf(stderr, "error: no hay bytes en la entrada\n");
        return 2;
    }

    /* ---- 3) Inicializacion ---- */
    vcp_rx_init(&rx);
    vcp_rx_set_resync(&rx, politica);

    vcp_app_init(&app);
    app.addr_propia    = addr;
    app.plazo_estricto = plazo;

    pago_sim_init(0u);
    puart_set_eco(false);   /* la traza la imprime el banco, mas legible */
    puart_reset();

    memset(&banco, 0, sizeof(banco));
    banco.ent     = bytes;
    banco.n_ent   = n_bytes;
    banco.app     = &app;
    banco.byte_ms    = byte_ms;
    banco.silencioso = quiet;

    /* ---- 4) Encabezado ---- */
    if (!quiet) {
        printf("============================================================"
               "================\n");
        printf(" VCP-1 - Banco de pruebas del receptor / respondedor  (ejercicio E1.7)\n");
        printf("============================================================"
               "================\n");
        printf(" Direccion propia   : 0x%02X\n", (unsigned)addr);
        printf(" Politica de resync : %s\n",
               (politica == VCP_RESYNC_REPROCESO)
                   ? "REPROCESO  (al fallar, re-examina desde STX+1)"
                   : "DESCARTE   (al fallar, tira el candidato entero)");
        printf(" Plazo de respuesta : %u ms  %s\n", (unsigned)VCP_PLAZO_RESPUESTA_MS,
               plazo ? "(estricto: una trama detectada tarde NO se contesta)"
                     : "(DESACTIVADO por --sin-plazo)");
        printf(" Reloj simulado     : %.3f ms por byte", byte_ms);
        if (byte_ms > 0.0) {
            printf("  (~%.0f baudios 8N1)\n", 10000.0 / byte_ms);
        } else {
            printf("  (sin simulacion de tiempo)\n");
        }
        printf(" Entrada            : %s  (%u bytes)\n",
               (hex_arg != NULL) ? "--hex" : (archivo != NULL) ? archivo : "stdin",
               (unsigned)n_bytes);
        printf("------------------------------------------------------------"
               "----------------\n\n");
    }   /* fin de if (!quiet) */

    /* ---- 5) EL LAZO PRINCIPAL ----
     *
     * Esto es una version en camara lenta del lazo del firmware real:
     *
     *     while (1) {
     *         now = reloj_ms();
     *         while (uart_fifo_pop(&b, &t_b))  vcp_rx_byte(&rx, b, t_b, cb, ctx);
     *         vcp_rx_tick(&rx, now, cb, ctx);
     *         vcp_app_poll(&app, now);
     *     }
     *
     * Aca los bytes vienen del vector en vez de la FIFO de la UART, pero el
     * orden de las llamadas es identico.
     */
    for (i = 0u; i < n_bytes; i++) {
        uint32_t now = (uint32_t)(t_de_offset(&banco, (uint32_t)i));

        pago_sim_tick(now);
        vcp_rx_byte(&rx, bytes[i], now, on_evento, &banco);
        vcp_rx_tick(&rx, now, on_evento, &banco);
        vcp_app_poll(&app, now);
    }

    /* Al terminar el vector avanzamos el reloj lo suficiente para que venza el
     * timeout entre bytes si quedo una trama a medio recibir. En el equipo real
     * esto pasa solo, porque el lazo sigue girando. */
    {
        uint32_t now = (uint32_t)(t_de_offset(&banco, (uint32_t)n_bytes) + 50.0);
        pago_sim_tick(now);
        vcp_rx_tick(&rx, now, on_evento, &banco);
        vcp_app_poll(&app, now);
    }
    flush_ruido(&banco);

    /* ---- 6) Resumen ---- */
    {
        const vcp_rx_stats_t *st = vcp_rx_stats(&rx);

        printf("\n");
        printf("============================================================"
               "================\n");
        printf(" RESUMEN\n");
        printf("============================================================"
               "================\n");
        printf(" RECEPCION\n");
        printf("   bytes recibidos          : %u\n", (unsigned)st->bytes_recibidos);
        printf("   bytes fuera de trama     : %u   (ruido + descartes de resync)\n",
               (unsigned)st->bytes_fuera_de_trama);
        printf("   bytes reprocesados       : %u\n", (unsigned)st->bytes_reprocesados);
        printf("   tramas validas           : %u\n", (unsigned)st->tramas_ok);
        printf("     de ellas, recuperadas  : %u   (aparecieron re-sincronizando)\n",
               (unsigned)st->tramas_recuperadas);
        printf(" ERRORES (diferenciados, requisito E1.5)\n");
        printf("   CRC no coincide          : %u\n", (unsigned)st->err_crc);
        printf("   LEN fuera de rango       : %u\n", (unsigned)st->err_len);
        printf("   framing (falta ETX)      : %u\n", (unsigned)st->err_framing);
        printf("   timeout entre bytes      : %u\n", (unsigned)st->err_timeout);
        printf("   internos (deberia ser 0) : %u\n", (unsigned)st->err_interno);
        printf(" APLICACION\n");
        printf("   tramas propias (0x%02X)    : %u\n",
               (unsigned)addr, (unsigned)app.rx_propias);
        printf("   tramas broadcast         : %u\n", (unsigned)app.rx_broadcast);
        printf("   tramas de otra direccion : %u\n", (unsigned)app.rx_ajenas);
        printf("   comandos no admitidos    : %u   (sentido inverso / desconocido)\n",
               (unsigned)app.rx_cmd_no_admitido);
        printf("   LEN inesperado           : %u\n", (unsigned)app.rx_len_inesperado);
        printf("   LOG_EVENT registrados    : %u  (%u bytes)\n",
               (unsigned)app.log_events, (unsigned)app.log_bytes);
        printf(" EMISION\n");
        printf("   respuestas transmitidas  : %u\n", (unsigned)app.tx_emitidas);
        printf("   omitidas por plazo       : %u\n", (unsigned)app.tx_omitidas_plazo);
        printf("   escrituras a la UART     : %u  (%u bytes)\n",
               (unsigned)puart_n_escrituras(), (unsigned)puart_n_bytes());
        printf(" SESION DE VENTA\n");
        printf("   estado final             : %s\n",
               vcp_session_estado_nombre(app.venta.estado));
        printf("   pedidos / aprobadas / denegadas : %u / %u / %u\n",
               (unsigned)app.venta.n_pedidos,
               (unsigned)app.venta.n_aprobadas,
               (unsigned)app.venta.n_denegadas);
        printf("   confirmaciones con txid desconocido : %u\n",
               (unsigned)app.venta.n_txid_desconocido);
        printf("============================================================"
               "================\n");
    }

    return 0;
}
