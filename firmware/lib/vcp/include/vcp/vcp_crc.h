/*
 * ============================================================================
 *  vcp_crc.h - CRC-8 del protocolo VCP-1
 * ============================================================================
 *
 *  QUE ES UN CRC
 *  -------------
 *  Un CRC (Cyclic Redundancy Check) es un "resumen" de unos pocos bits que
 *  viaja junto a los datos. El emisor lo calcula sobre los datos y lo manda;
 *  el receptor lo vuelve a calcular sobre lo que recibio y compara. Si no
 *  coinciden, algun byte se corrompio en el camino.
 *
 *  No es criptografia: no protege contra un atacante, protege contra RUIDO.
 *  Y el enunciado dice que la linea es ruidosa (Anexo A.2).
 *
 *  PARAMETROS DE ESTE CRC (Anexo A.1)
 *  ----------------------------------
 *      polinomio  : 0x07
 *      valor ini  : 0x00
 *      reflexion  : no (ni de entrada ni de salida)
 *      XOR final  : no
 *      cobertura  : ADDR + LEN + CMD + PAYLOAD   <-- NO incluye STX ni ETX
 *
 *  Esa ultima linea es la trampa del ejercicio E1.9 ("fijate bien en el
 *  Anexo A.1 antes de escribirla"): es facil calcular el CRC sobre la trama
 *  entera y que todo "parezca" andar hasta que se prueba contra el vector real.
 *
 *  Este es el CRC-8 clasico, tambien conocido como CRC-8/SMBUS.
 * ============================================================================
 */

#ifndef VCP_CRC_H
#define VCP_CRC_H

#include <stdint.h>
#include <stddef.h>

/*
 * Calcula el CRC-8 de un bloque de 'len' bytes.
 *
 *   datos : puntero al primer byte. Si len == 0 puede ser NULL.
 *   len   : cantidad de bytes.
 *   ->      el CRC resultante.
 *
 * Se usa en la EMISION, donde ya tenemos la trama entera armada en memoria.
 */
uint8_t vcp_crc8(const uint8_t *datos, size_t len);

/*
 * Version INCREMENTAL: mete un solo byte en un CRC que se viene acumulando.
 *
 *   crc  : valor acumulado hasta ahora (arrancar en 0x00).
 *   byte : el byte nuevo.
 *   ->     el CRC actualizado.
 *
 * Se usa en la RECEPCION: como los bytes llegan de a uno y el receptor no
 * puede guardar y recorrer la trama al final "por segunda vez" sin costo,
 * vamos acumulando el CRC a medida que entra cada byte. Cuando llega el byte
 * de CRC de la trama, ya tenemos el nuestro listo para comparar: cero trabajo
 * extra en el camino critico ETX -> respuesta (requisito E1.10 / E1.14).
 *
 * Identidad util para los tests:
 *     vcp_crc8(d, n)  ==  aplicar vcp_crc8_byte() sobre d[0..n-1] desde 0x00
 */
uint8_t vcp_crc8_byte(uint8_t crc, uint8_t byte);

#endif /* VCP_CRC_H */
