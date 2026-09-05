/*
 * ============================================================================
 *  test_frame.c - Pruebas de vcp_build() y utilidades (requisito E1.9)
 * ============================================================================
 */

#include "test_util.h"
#include "vcp/vcp_frame.h"
#include "vcp/vcp_crc.h"

void tests_frame(void)
{
    test_grupo("Construccion de tramas (vcp_build)");

    /* --- La prueba mas importante: reproducir una trama del vector ---
     * Si el CRC incluyera el STX, o si el orden fuera CMD antes que LEN, esto
     * fallaria. Es el chequeo que pide E1.9 ("fijate bien en el Anexo A.1"). */
    {
        uint8_t out[VCP_TRAMA_MAX_BYTES];
        const uint8_t esperado[] = { 0x02u, 0x0Au, 0x00u, 0x10u, 0xF7u, 0x03u };
        int n = vcp_build(0x0Au, (uint8_t)VCP_CMD_STATUS_REQ, NULL, 0u,
                          out, sizeof(out));
        VERIFICAR_EQ(n, 6, "trama sin payload mide 6 bytes");
        VERIFICAR_MEM(out, esperado, sizeof(esperado),
                      "STATUS_REQ a 0x0A reproduce el offset 3 del Anexo B");
    }

    /* --- Con payload, incluyendo bytes de control adentro --- */
    {
        uint8_t out[VCP_TRAMA_MAX_BYTES];
        const uint8_t payload[] = { 0x41u, 0x42u, 0x02u, 0x03u, 0x43u, 0x44u };
        const uint8_t esperado[] = { 0x02u, 0x0Au, 0x06u, 0x40u,
                                     0x41u, 0x42u, 0x02u, 0x03u, 0x43u, 0x44u,
                                     0x61u, 0x03u };
        int n = vcp_build(0x0Au, (uint8_t)VCP_CMD_LOG_EVENT,
                          payload, (uint8_t)sizeof(payload), out, sizeof(out));
        VERIFICAR_EQ(n, 12, "trama con 6 bytes de payload mide 12");
        VERIFICAR_MEM(out, esperado, sizeof(esperado),
                      "el payload con 0x02/0x03 va SIN escapar (offset 15)");
    }

    /* --- Payload maximo (64 bytes) --- */
    {
        uint8_t out[VCP_TRAMA_MAX_BYTES];
        uint8_t payload[VCP_MAX_PAYLOAD];
        int i;
        for (i = 0; i < (int)VCP_MAX_PAYLOAD; i++) { payload[i] = (uint8_t)i; }
        VERIFICAR_EQ(vcp_build(0x0Au, 0x40u, payload, (uint8_t)VCP_MAX_PAYLOAD,
                               out, sizeof(out)),
                     (int)VCP_TRAMA_MAX_BYTES,
                     "payload de 64 bytes entra justo en el buffer (70 bytes)");
    }

    /* --- Rechazos --- */
    {
        uint8_t out[VCP_TRAMA_MAX_BYTES];
        uint8_t payload[VCP_MAX_PAYLOAD];
        VERIFICAR(vcp_build(0x0Au, 0x40u, payload, 65u, out, sizeof(out))
                      == VCP_BUILD_ERR_LEN,
                  "len 65 se rechaza (maximo 64)");
        VERIFICAR(vcp_build(0x0Au, 0x40u, payload, 10u, out, 8u)
                      == VCP_BUILD_ERR_ESPACIO,
                  "buffer de salida chico se rechaza, no se desborda");
        VERIFICAR(vcp_build(0x0Au, 0x40u, NULL, 3u, out, sizeof(out))
                      == VCP_BUILD_ERR_NULL,
                  "payload NULL con len>0 se rechaza");
        VERIFICAR(vcp_build(0x0Au, 0x40u, NULL, 0u, NULL, 10u)
                      == VCP_BUILD_ERR_NULL,
                  "buffer de salida NULL se rechaza");
    }

    /* --- El buffer no se toca si la validacion falla --- */
    {
        uint8_t out[VCP_TRAMA_MAX_BYTES];
        uint8_t payload[VCP_MAX_PAYLOAD];
        memset(out, 0xAAu, sizeof(out));
        (void)vcp_build(0x0Au, 0x40u, payload, 65u, out, sizeof(out));
        VERIFICAR_EQ(out[0], 0xAAu,
                     "ante error, vcp_build no escribe nada (todo o nada)");
    }

    /* --- El CRC NO incluye STX ni ETX --- */
    {
        uint8_t out[VCP_TRAMA_MAX_BYTES];
        int n = vcp_build(0x0Au, (uint8_t)VCP_CMD_VEND_APPROVE,
                          (const uint8_t[]){ 0x00u, 0x2Au }, 2u,
                          out, sizeof(out));
        uint8_t crc_correcto = vcp_crc8(&out[1], 3u + 2u);   /* ADDR..PAYLOAD */
        uint8_t crc_con_stx  = vcp_crc8(&out[0], 4u + 2u);   /* mal a proposito */
        VERIFICAR_EQ(out[n - 2], crc_correcto,
                     "el CRC emitido cubre ADDR+LEN+CMD+PAYLOAD");
        VERIFICAR(crc_correcto != crc_con_stx,
                  "  (incluir el STX daria otro valor: el error seria silencioso)");
        VERIFICAR_EQ(out[n - 1], VCP_ETX, "la trama termina en ETX");
        VERIFICAR_EQ(out[0], VCP_STX, "la trama empieza en STX");
    }

    test_grupo("Utilidades de payload");

    /* --- Enteros de 16 bits big-endian --- */
    {
        uint8_t b[2];
        vcp_put_u16be(b, 1000u);              /* 1000 = 0x03E8 */
        VERIFICAR_EQ(b[0], 0x03u, "u16be: primero el byte alto");
        VERIFICAR_EQ(b[1], 0xE8u, "u16be: despues el byte bajo");
        VERIFICAR_EQ(vcp_get_u16be(b), 1000u, "u16be: ida y vuelta");
    }
    {
        const uint8_t b[2] = { 0xFFu, 0xFFu };
        VERIFICAR_EQ(vcp_get_u16be(b), 65535u, "u16be: valor maximo");
    }

    /* --- Direccion de los comandos (columna "Direccion" del Anexo A.3) --- */
    {
        VERIFICAR(vcp_cmd_es_del_master(VCP_CMD_STATUS_REQ),
                  "STATUS_REQ lo manda el master");
        VERIFICAR(vcp_cmd_es_del_master(VCP_CMD_VEND_REQUEST),
                  "VEND_REQUEST lo manda el master");
        VERIFICAR(vcp_cmd_es_del_master(VCP_CMD_VEND_SUCCESS),
                  "VEND_SUCCESS lo manda el master");
        VERIFICAR(vcp_cmd_es_del_master(VCP_CMD_VEND_FAILURE),
                  "VEND_FAILURE lo manda el master");
        VERIFICAR(vcp_cmd_es_del_master(VCP_CMD_LOG_EVENT),
                  "LOG_EVENT lo manda el master");
        VERIFICAR(!vcp_cmd_es_del_master(VCP_CMD_ACK),
                  "ACK lo emitimos nosotros: no es una orden");
        VERIFICAR(!vcp_cmd_es_del_master(VCP_CMD_VEND_APPROVE),
                  "VEND_APPROVE lo emitimos nosotros: no es una orden");
        VERIFICAR(!vcp_cmd_es_del_master(VCP_CMD_STATUS_RESP),
                  "STATUS_RESP lo emitimos nosotros: no es una orden");
        VERIFICAR(!vcp_cmd_es_del_master(0x99u),
                  "un comando inventado no se acepta");
    }

    /* --- Longitudes esperadas --- */
    {
        VERIFICAR(vcp_len_esperada_ok(VCP_CMD_STATUS_REQ, 0u),
                  "STATUS_REQ con LEN=0 es correcto");
        VERIFICAR(!vcp_len_esperada_ok(VCP_CMD_STATUS_REQ, 1u),
                  "STATUS_REQ con LEN=1 no lo es");
        VERIFICAR(vcp_len_esperada_ok(VCP_CMD_VEND_REQUEST, 3u),
                  "VEND_REQUEST necesita 3 bytes");
        VERIFICAR(!vcp_len_esperada_ok(VCP_CMD_VEND_REQUEST, 2u),
                  "VEND_REQUEST con 2 bytes se rechaza (leeriamos basura)");
        VERIFICAR(vcp_len_esperada_ok(VCP_CMD_LOG_EVENT, 0u),
                  "LOG_EVENT acepta 0 bytes");
        VERIFICAR(vcp_len_esperada_ok(VCP_CMD_LOG_EVENT, 64u),
                  "LOG_EVENT acepta 64 bytes");
    }
}
