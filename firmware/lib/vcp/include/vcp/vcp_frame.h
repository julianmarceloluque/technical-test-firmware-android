/*
 * ============================================================================
 *  vcp_frame.h - Estructura de trama, construccion (emision) y utilidades
 * ============================================================================
 *
 *  FORMATO DE TRAMA (Anexo A.1)
 *  ----------------------------
 *    +-----+------+-----+-----+---------------+-----+-----+
 *    | STX | ADDR | LEN | CMD |    PAYLOAD    | CRC | ETX |
 *    +-----+------+-----+-----+---------------+-----+-----+
 *       1     1      1     1     0..64 bytes     1     1
 *
 *  Dos cosas para tener SIEMPRE presentes:
 *
 *  1) El orden es ADDR, LEN, CMD. No CMD, LEN. (Se lee de arriba a abajo en la
 *     tabla del anexo y es facil invertirlo de memoria.)
 *
 *  2) NO hay byte stuffing / escapado. Dentro del PAYLOAD pueden aparecer
 *     0x02 (STX) y 0x03 (ETX) como bytes de datos normales. El unico que sabe
 *     donde termina el payload es el campo LEN. Un parser que "resincronice
 *     cada vez que ve un 0x02" se rompe con el vector del Anexo B (la trama 3
 *     del vector lleva 02 03 adentro del payload a proposito).
 * ============================================================================
 */

#ifndef VCP_FRAME_H
#define VCP_FRAME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "vcp/vcp_cfg.h"

/* ---------------------------------------------------------------------------
 * Comandos (Anexo A.3)
 * ------------------------------------------------------------------------- */
typedef enum {
    VCP_CMD_ACK          = 0x01,  /* Controlador -> Maquina : vacio           */
    VCP_CMD_STATUS_REQ   = 0x10,  /* Maquina -> Controlador : vacio           */
    VCP_CMD_STATUS_RESP  = 0x11,  /* Controlador -> Maquina : 4 bytes         */
    VCP_CMD_VEND_REQUEST = 0x20,  /* Maquina -> Controlador : 3 bytes         */
    VCP_CMD_VEND_APPROVE = 0x21,  /* Controlador -> Maquina : 2 bytes (txid)  */
    VCP_CMD_VEND_DENY    = 0x22,  /* Controlador -> Maquina : 1 byte (motivo) */
    VCP_CMD_VEND_PENDING = 0x23,  /* Controlador -> Maquina : vacio           */
    VCP_CMD_VEND_SUCCESS = 0x30,  /* Maquina -> Controlador : 2 bytes (txid)  */
    VCP_CMD_VEND_FAILURE = 0x31,  /* Maquina -> Controlador : 3 bytes         */
    VCP_CMD_LOG_EVENT    = 0x40   /* Maquina -> Controlador : 0..64 bytes     */
} vcp_cmd_t;

/* ---------------------------------------------------------------------------
 * Motivos de VEND_DENY (Anexo A.4)
 * ------------------------------------------------------------------------- */
typedef enum {
    VCP_DENY_PRECIO     = 0x01,   /* precio fuera de rango (50..20000)        */
    VCP_DENY_SELECCION  = 0x02,   /* seleccion invalida (1..80)               */
    VCP_DENY_PAGO       = 0x03,   /* el medio de pago rechazo el cobro        */
    VCP_DENY_EN_CURSO   = 0x04,   /* ya hay una sesion de venta abierta       */
    VCP_DENY_INTERNO    = 0x05    /* error interno del controlador            */
} vcp_deny_t;

/* ---------------------------------------------------------------------------
 * Trama ya validada que el receptor entrega a la capa de aplicacion
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t  addr;                        /* ADDR de la trama recibida        */
    uint8_t  cmd;                         /* CMD                              */
    uint8_t  len;                         /* cantidad de bytes utiles de payload */
    uint8_t  payload[VCP_MAX_PAYLOAD];    /* copia del payload                */

    /*
     * AGREGADO respecto del vcp.h sugerido -------------------------------
     * Instante (ms monotonicos) en que se recibio el ETX de esta trama.
     *
     * Para que sirve: el plazo de respuesta de 5 ms del Anexo A.2 se cuenta
     * DESDE EL ETX. Si la trama viene con su propia marca de tiempo, la capa
     * de aplicacion puede decidir con un solo resta si todavia esta a tiempo
     * de contestar, o si le corresponde callarse (requisito E1.14).
     *
     * Sin este campo el plazo seria un comentario en el README en vez de una
     * regla que el codigo pueda hacer cumplir.
     */
    uint32_t t_etx_ms;
} vcp_frame_t;

/* ---------------------------------------------------------------------------
 * Construccion de tramas para EMISION (requisito E1.9)
 * ------------------------------------------------------------------------- */

/* Codigos de error de vcp_build(). Son negativos para poder distinguirlos del
 * "cantidad de bytes escritos", que siempre es >= 6. */
#define VCP_BUILD_ERR_NULL    (-1)   /* puntero nulo donde no correspondia    */
#define VCP_BUILD_ERR_LEN     (-2)   /* len > VCP_MAX_PAYLOAD                 */
#define VCP_BUILD_ERR_ESPACIO (-3)   /* el buffer de salida es muy chico      */

/*
 * Arma una trama completa (STX..ETX) en 'out'.
 *
 *   addr     : direccion destino. Como somos slave, siempre respondemos con
 *              NUESTRA propia direccion (el master sabe a quien interrogo).
 *   cmd      : comando (VCP_CMD_*).
 *   payload  : bytes de payload; puede ser NULL si len == 0.
 *   len      : 0..64.
 *   out      : buffer destino.
 *   out_size : tamano de 'out'. La funcion NUNCA escribe mas alla.
 *
 *   ->  cantidad de bytes escritos (6 + len), o un valor negativo VCP_BUILD_ERR_*.
 *
 * No usa memoria dinamica ni estado global: el llamador provee el buffer.
 * Eso la hace reentrante y testeable (requisito E1.2).
 */
int vcp_build(uint8_t addr, uint8_t cmd, const uint8_t *payload, uint8_t len,
              uint8_t *out, size_t out_size);

/* ---------------------------------------------------------------------------
 * Utilidades de payload
 * ------------------------------------------------------------------------- */

/*
 * El protocolo usa enteros de 16 bits en BIG-ENDIAN (el byte mas significativo
 * primero). Ejemplo: 1000 centavos = 0x03E8 viaja como  03 E8.
 *
 * Estas dos funciones evitan escribir el desarme a mano en cada uso (que es
 * donde se cuelan los bugs de endianness) y hacen el codigo independiente del
 * endianness del micro donde compile.
 */
void     vcp_put_u16be(uint8_t *dst, uint16_t v);
uint16_t vcp_get_u16be(const uint8_t *src);

/* Nombre legible del comando, para los logs. Nunca devuelve NULL. */
const char *vcp_cmd_nombre(uint8_t cmd);

/* Nombre legible del motivo de un VEND_DENY. Nunca devuelve NULL. */
const char *vcp_deny_nombre(uint8_t motivo);

/*
 * true si 'cmd' es un comando que la EXPENDEDORA (el master) nos puede enviar.
 *
 * Por que existe: el Anexo A.3 tiene una columna "Direccion". ACK, STATUS_RESP,
 * VEND_APPROVE, VEND_DENY y VEND_PENDING son cosas que emitimos NOSOTROS. Si
 * una de esas llega por la linea dirigida a nuestra direccion, o alguien esta
 * inyectando trafico, o es un artefacto de una captura de laboratorio (el
 * enunciado avisa: "puede contener tramas que en operacion normal no deberian
 * circular en ese sentido"). En cualquiera de los dos casos NO es una orden
 * para nosotros y no se responde. Ver caso NE-05 en docs/01_E1_decisiones.md.
 */
bool vcp_cmd_es_del_master(uint8_t cmd);

/*
 * Longitud de payload que el Anexo A.3 exige para cada comando del master.
 *   -> true  si 'len' es la esperada para 'cmd'
 *   -> false si no (trama estructuralmente valida pero semanticamente rota)
 *
 * LOG_EVENT acepta cualquier longitud 0..64, por eso no alcanza con una tabla
 * de igualdad y hay una funcion.
 */
bool vcp_len_esperada_ok(uint8_t cmd, uint8_t len);

#endif /* VCP_FRAME_H */
