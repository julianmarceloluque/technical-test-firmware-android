/*
 * ============================================================================
 *  test_crc.c - Pruebas del CRC-8
 * ============================================================================
 *
 *  Los valores esperados NO estan inventados: salen de las tramas validas del
 *  vector del Anexo B, que es el unico oraculo confiable que tenemos. Si el
 *  CRC estuviera mal implementado (por ejemplo incluyendo el STX), estas
 *  pruebas fallarian y el resto del sistema tambien.
 * ============================================================================
 */

#include "test_util.h"
#include "vcp/vcp_crc.h"

void tests_crc(void)
{
    test_grupo("CRC-8 (poly 0x07, init 0x00)");

    /* --- Casos borde --- */
    {
        VERIFICAR_EQ(vcp_crc8(NULL, 0u), 0x00u,
                     "bloque vacio da 0x00 (valor inicial)");
    }
    {
        const uint8_t cero = 0x00u;
        VERIFICAR_EQ(vcp_crc8(&cero, 1u), 0x00u,
                     "un solo 0x00 da 0x00");
    }

    /* --- Del vector del Anexo B, trama del offset 3 ---
     * 02 0A 00 10 F7 03  ->  CRC sobre ADDR+LEN+CMD = 0A 00 10 */
    {
        const uint8_t d[] = { 0x0Au, 0x00u, 0x10u };
        VERIFICAR_EQ(vcp_crc8(d, sizeof(d)), 0xF7u,
                     "STATUS_REQ a 0x0A (Anexo B offset 3) -> 0xF7");
    }

    /* --- Trama del offset 9 (dirigida a 0x0B) --- */
    {
        const uint8_t d[] = { 0x0Bu, 0x00u, 0x10u };
        VERIFICAR_EQ(vcp_crc8(d, sizeof(d)), 0x9Cu,
                     "STATUS_REQ a 0x0B (Anexo B offset 9) -> 0x9C");
    }

    /* --- Trama del offset 15: LOG_EVENT con 0x02 y 0x03 en el payload ---
     * Es la prueba de que el CRC no le tiene miedo a los bytes de control. */
    {
        const uint8_t d[] = { 0x0Au, 0x06u, 0x40u,
                              0x41u, 0x42u, 0x02u, 0x03u, 0x43u, 0x44u };
        VERIFICAR_EQ(vcp_crc8(d, sizeof(d)), 0x61u,
                     "LOG_EVENT con STX/ETX en el payload -> 0x61");
    }

    /* --- Trama del offset 42: el VEND_APPROVE escondido --- */
    {
        const uint8_t d[] = { 0x0Au, 0x02u, 0x21u, 0x00u, 0x2Au };
        VERIFICAR_EQ(vcp_crc8(d, sizeof(d)), 0x0Fu,
                     "VEND_APPROVE txid=42 (Anexo B offset 42) -> 0x0F");
    }

    /* --- Trama del offset 58: VEND_SUCCESS --- */
    {
        const uint8_t d[] = { 0x0Au, 0x02u, 0x30u, 0x00u, 0x2Au };
        VERIFICAR_EQ(vcp_crc8(d, sizeof(d)), 0xC6u,
                     "VEND_SUCCESS txid=42 (Anexo B offset 58) -> 0xC6");
    }

    /* --- Trama del offset 66: broadcast --- */
    {
        const uint8_t d[] = { 0x00u, 0x02u, 0x40u, 0x42u, 0x43u };
        VERIFICAR_EQ(vcp_crc8(d, sizeof(d)), 0x15u,
                     "LOG_EVENT broadcast (Anexo B offset 66) -> 0x15");
    }

    /* --- Las dos tramas rotas del vector: el CRC NO tiene que coincidir --- */
    {
        const uint8_t d[] = { 0x0Au, 0x03u, 0x20u, 0x05u, 0x03u, 0xE8u };
        VERIFICAR(vcp_crc8(d, sizeof(d)) != 0xE3u,
                  "VEND_REQUEST del offset 27: el CRC del vector NO coincide");
        VERIFICAR_EQ(vcp_crc8(d, sizeof(d)), 0x1Cu,
                     "  (el correcto habria sido 0x1C, en el vector dice 0xE3)");
    }
    {
        const uint8_t d[] = { 0x0Au, 0x08u, 0x40u, 0x54u, 0x52u,
                              0x02u, 0x0Au, 0x02u, 0x21u, 0x00u, 0x2Au };
        VERIFICAR(vcp_crc8(d, sizeof(d)) != 0x0Fu,
                  "LOG_EVENT del offset 36: el CRC del vector NO coincide");
    }

    /* --- La version incremental tiene que dar lo mismo que la de bloque ---
     * Esto importa porque el receptor usa la incremental y el emisor la de
     * bloque: si difirieran, emitiriamos tramas que nosotros mismos
     * rechazariamos. */
    {
        const uint8_t d[] = { 0x0Au, 0x06u, 0x40u,
                              0x41u, 0x42u, 0x02u, 0x03u, 0x43u, 0x44u };
        uint8_t crc = 0x00u;
        size_t  i;
        for (i = 0u; i < sizeof(d); i++) {
            crc = vcp_crc8_byte(crc, d[i]);
        }
        VERIFICAR_EQ(crc, vcp_crc8(d, sizeof(d)),
                     "incremental byte a byte == calculo de bloque");
    }

    /* --- Un bit cambiado tiene que cambiar el CRC ---
     * Un CRC que no detecta un bit dado vuelto no sirve para nada. */
    {
        uint8_t a[] = { 0x0Au, 0x02u, 0x30u, 0x00u, 0x2Au };
        uint8_t b[] = { 0x0Au, 0x02u, 0x30u, 0x00u, 0x2Bu };  /* 1 bit */
        VERIFICAR(vcp_crc8(a, sizeof(a)) != vcp_crc8(b, sizeof(b)),
                  "un bit cambiado en el payload cambia el CRC");
    }
}
