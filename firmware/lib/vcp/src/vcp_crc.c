/*
 * ============================================================================
 *  vcp_crc.c - Implementacion del CRC-8 (poly 0x07, init 0x00)
 * ============================================================================
 *
 *  COMO FUNCIONA, PASO A PASO
 *  --------------------------
 *  Un CRC es una division polinomica en modulo 2 (o sea: sin acarreos, donde
 *  "restar" es XOR). En la practica el algoritmo bit a bit es:
 *
 *      1. crc = crc XOR byte_nuevo
 *      2. repetir 8 veces (una por bit del byte):
 *           si el bit mas alto (0x80) de crc esta en 1:
 *               crc = (crc << 1) XOR 0x07
 *           si no:
 *               crc = (crc << 1)
 *
 *  El "si el bit alto esta en 1, restale el polinomio" es exactamente lo que
 *  hace una division a mano: si el divisor entra, se resta.
 *
 *  POR QUE BIT A BIT Y NO CON TABLA
 *  --------------------------------
 *  La version con tabla (256 bytes de lookup) es ~8 veces mas rapida pero gasta
 *  256 bytes de flash. Aca la trama mas larga son 68 bytes de datos => 68*8 =
 *  544 iteraciones de tres instrucciones. En un Cortex-M0 a 48 MHz eso son
 *  unos 30-40 us, y ademas NO estan en el camino critico: el CRC se acumula
 *  byte a byte a medida que llegan (uno por milisegundo a 9600 baudios).
 *  Cuando llega el ETX solo queda comparar dos bytes.
 *
 *  Conclusion: la tabla no compra nada aca y cuesta flash. Si algun dia el
 *  protocolo subiera de velocidad, se cambia solo este archivo.
 * ============================================================================
 */

#include "vcp/vcp_crc.h"

#define VCP_CRC_POLY  0x07u
#define VCP_CRC_INIT  0x00u

uint8_t vcp_crc8_byte(uint8_t crc, uint8_t byte)
{
    uint8_t i;

    crc = (uint8_t)(crc ^ byte);

    for (i = 0u; i < 8u; i++) {
        if ((crc & 0x80u) != 0u) {
            /* El bit alto esta en 1: desplazo y "resto" el polinomio. */
            crc = (uint8_t)((uint8_t)(crc << 1) ^ VCP_CRC_POLY);
        } else {
            /* El bit alto esta en 0: solo desplazo. */
            crc = (uint8_t)(crc << 1);
        }
    }

    return crc;
}

uint8_t vcp_crc8(const uint8_t *datos, size_t len)
{
    uint8_t crc = VCP_CRC_INIT;
    size_t  i;

    /* Nota defensiva: si len es 0 no tocamos el puntero, asi que datos==NULL
     * es valido en ese caso. Si len>0 y datos==NULL el llamador tiene un bug;
     * lo cortamos aca en vez de dereferenciar NULL. */
    if (datos == NULL) {
        return crc;
    }

    for (i = 0u; i < len; i++) {
        crc = vcp_crc8_byte(crc, datos[i]);
    }

    return crc;
}
