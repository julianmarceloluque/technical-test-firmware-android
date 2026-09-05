/*
 * ============================================================================
 *  vcp_frame.c - Construccion de tramas y utilidades (ver vcp_frame.h)
 * ============================================================================
 */

#include <string.h>

#include "vcp/vcp_frame.h"
#include "vcp/vcp_crc.h"

/* ---------------------------------------------------------------------------
 * vcp_build - arma una trama completa lista para mandar por la UART
 * ------------------------------------------------------------------------- */
int vcp_build(uint8_t addr, uint8_t cmd, const uint8_t *payload, uint8_t len,
              uint8_t *out, size_t out_size)
{
    size_t  necesarios;
    uint8_t crc;

    /* ---- 1) Validaciones de entrada ------------------------------------
     * Se validan ANTES de escribir un solo byte. Si algo esta mal, 'out'
     * queda intacto: la funcion es "todo o nada". Eso evita dejar medias
     * tramas en un buffer que despues alguien transmite por error.
     */
    if (out == NULL) {
        return VCP_BUILD_ERR_NULL;
    }
    if (len > VCP_MAX_PAYLOAD) {
        return VCP_BUILD_ERR_LEN;
    }
    if ((len > 0u) && (payload == NULL)) {
        return VCP_BUILD_ERR_NULL;
    }

    /* STX + ADDR + LEN + CMD + payload + CRC + ETX */
    necesarios = 6u + (size_t)len;
    if (out_size < necesarios) {
        return VCP_BUILD_ERR_ESPACIO;
    }

    /* ---- 2) Cabecera ---------------------------------------------------
     * ATENCION al orden: ADDR, LEN, CMD.  (Anexo A.1)
     */
    out[0] = VCP_STX;
    out[1] = addr;
    out[2] = len;
    out[3] = cmd;

    /* ---- 3) Payload ----------------------------------------------------
     * Se copia tal cual: sin escapado, sin byte stuffing. Si el payload
     * contiene 0x02 o 0x03 va igual; el receptor se guia por LEN.
     */
    if (len > 0u) {
        memcpy(&out[4], payload, (size_t)len);
    }

    /* ---- 4) CRC --------------------------------------------------------
     * SOBRE ADDR + LEN + CMD + PAYLOAD  =  out[1] .. out[3+len]
     * O sea: arranca en out+1 (salteando el STX) y cubre 3 + len bytes.
     *
     * Esta es la linea que hay que mirar dos veces (requisito E1.9).
     * Si por error se pusiera vcp_crc8(out, 4 + len) el CRC incluiria el STX
     * y ninguna trama nuestra seria aceptada por la expendedora.
     */
    crc = vcp_crc8(&out[1], VCP_CRC_CABECERA_BYTES + (size_t)len);

    out[4u + len] = crc;
    out[5u + len] = VCP_ETX;

    return (int)necesarios;
}

/* ---------------------------------------------------------------------------
 * Enteros de 16 bits big-endian
 * ------------------------------------------------------------------------- */
void vcp_put_u16be(uint8_t *dst, uint16_t v)
{
    /* Primero el byte alto. Ejemplo: 0x03E8 -> dst[0]=0x03, dst[1]=0xE8 */
    dst[0] = (uint8_t)((v >> 8) & 0xFFu);
    dst[1] = (uint8_t)(v & 0xFFu);
}

uint16_t vcp_get_u16be(const uint8_t *src)
{
    /* El cast a uint16_t despues del corrimiento evita la promocion a int con
     * signo, que en micros de 16 bits puede dar sorpresas. */
    return (uint16_t)(((uint16_t)src[0] << 8) | (uint16_t)src[1]);
}

/* ---------------------------------------------------------------------------
 * Nombres legibles (solo para logs; no afectan el protocolo)
 * ------------------------------------------------------------------------- */
const char *vcp_cmd_nombre(uint8_t cmd)
{
    switch (cmd) {
    case VCP_CMD_ACK:          return "ACK";
    case VCP_CMD_STATUS_REQ:   return "STATUS_REQ";
    case VCP_CMD_STATUS_RESP:  return "STATUS_RESP";
    case VCP_CMD_VEND_REQUEST: return "VEND_REQUEST";
    case VCP_CMD_VEND_APPROVE: return "VEND_APPROVE";
    case VCP_CMD_VEND_DENY:    return "VEND_DENY";
    case VCP_CMD_VEND_PENDING: return "VEND_PENDING";
    case VCP_CMD_VEND_SUCCESS: return "VEND_SUCCESS";
    case VCP_CMD_VEND_FAILURE: return "VEND_FAILURE";
    case VCP_CMD_LOG_EVENT:    return "LOG_EVENT";
    default:                   return "DESCONOCIDO";
    }
}

const char *vcp_deny_nombre(uint8_t motivo)
{
    switch (motivo) {
    case VCP_DENY_PRECIO:    return "precio fuera de rango";
    case VCP_DENY_SELECCION: return "seleccion invalida";
    case VCP_DENY_PAGO:      return "pago rechazado";
    case VCP_DENY_EN_CURSO:  return "sesion en curso";
    case VCP_DENY_INTERNO:   return "error interno";
    default:                 return "motivo desconocido";
    }
}

/* ---------------------------------------------------------------------------
 * Direccion de los comandos
 * ------------------------------------------------------------------------- */
bool vcp_cmd_es_del_master(uint8_t cmd)
{
    switch (cmd) {
    /* Estos son los unicos que la expendedora nos puede mandar (Anexo A.3). */
    case VCP_CMD_STATUS_REQ:
    case VCP_CMD_VEND_REQUEST:
    case VCP_CMD_VEND_SUCCESS:
    case VCP_CMD_VEND_FAILURE:
    case VCP_CMD_LOG_EVENT:
        return true;

    /* ACK / STATUS_RESP / VEND_APPROVE / VEND_DENY / VEND_PENDING son
     * NUESTRAS respuestas. Si llegan por la linea, no son ordenes. */
    default:
        return false;
    }
}

bool vcp_len_esperada_ok(uint8_t cmd, uint8_t len)
{
    switch (cmd) {
    case VCP_CMD_STATUS_REQ:   return (len == 0u);
    case VCP_CMD_VEND_REQUEST: return (len == 3u);   /* sel(1) + precio(2)   */
    case VCP_CMD_VEND_SUCCESS: return (len == 2u);   /* txid(2)             */
    case VCP_CMD_VEND_FAILURE: return (len == 3u);   /* txid(2) + causa(1)  */
    case VCP_CMD_LOG_EVENT:    return (len <= VCP_MAX_PAYLOAD);  /* 0..64    */
    default:                   return false;
    }
}
