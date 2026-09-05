/*
 * ============================================================================
 *  vcp_port.h - "Entorno provisto": lo que el firmware USA pero NO implementa
 * ============================================================================
 *
 *  El enunciado (Anexo A.5) dice: estas funciones te las damos declaradas, no
 *  las implementes, usalas.
 *
 *  En terminos de arquitectura esto es la CAPA DE PORT (o HAL). La biblioteca
 *  vcp/ depende de estas firmas y de nada mas del sistema. Eso permite:
 *
 *    - compilar el mismo codigo de protocolo para el micro real y para el PC;
 *    - escribir tests que reemplacen el medio de pago por un simulador sin
 *      tocar una linea de la logica de venta;
 *    - que el dia que cambie el lector de tarjetas, se reescriba un solo .c.
 *
 *  Las implementaciones para PC estan en firmware/port/pc/.
 * ============================================================================
 */

#ifndef VCP_PORT_H
#define VCP_PORT_H

#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * UART hacia la expendedora
 * ------------------------------------------------------------------------- */
/*
 * Escribe bytes en la linea serie.
 *
 * En el banco de pruebas para PC alcanza con que imprima la trama en
 * hexadecimal (asi lo pide el Anexo A.5), que es exactamente lo que hace
 * port/pc/port_uart_pc.c.
 *
 * En el equipo real esta funcion arranca el DMA / carga el primer byte en el
 * registro de transmision y vuelve enseguida: NO espera a que la trama termine
 * de salir. El plazo del Anexo A.2 es "empezar a transmitir dentro de 5 ms",
 * no "terminar".
 */
void vcp_uart_write(const uint8_t *data, size_t len);

/* ---------------------------------------------------------------------------
 * Medio de pago (aca el controlador es MASTER)
 * ------------------------------------------------------------------------- */
typedef enum {
    PAGO_EN_CURSO = 0,   /* todavia no se sabe                              */
    PAGO_AUTORIZADO,     /* cobro autorizado; txid trae el id de transaccion */
    PAGO_RECHAZADO       /* el medio de pago dijo que no                    */
} pago_estado_t;

/*
 * Arranca un cobro por 'centavos'. No bloquea.
 * El resultado tarda entre 50 y 900 ms en aparecer (Anexo A.5).
 */
void pago_iniciar(uint16_t centavos);

/*
 * Consulta como viene el cobro. No bloquea.
 *   txid : de salida. Solo tiene sentido cuando devuelve PAGO_AUTORIZADO.
 *
 * Que NO exista una version bloqueante es deliberado y es el corazon del
 * ejercicio: obliga a que la venta sea una maquina de estados y no una funcion
 * que "espera el cobro y contesta".
 */
pago_estado_t pago_estado(uint16_t *txid);

#endif /* VCP_PORT_H */
