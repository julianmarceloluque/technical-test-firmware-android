/*
 * ============================================================================
 *  port_pc.h - Extras del port para PC (NO existen en el equipo real)
 * ============================================================================
 *
 *  vcp_port.h declara la interfaz que el firmware USA. Este header declara los
 *  controles del SIMULADOR: cosas que solo tienen sentido en el banco de
 *  pruebas, como "hace que el proximo cobro sea rechazado" o "deja de imprimir
 *  lo que se transmite".
 *
 *  La biblioteca vcp/ NO incluye este archivo. Si algun dia alguien lo incluye
 *  desde lib/vcp/, es un error de arquitectura: significa que la logica de
 *  protocolo se entero de que esta corriendo en un simulador.
 * ============================================================================
 */

#ifndef PORT_PC_H
#define PORT_PC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "vcp/vcp_port.h"

/* ---------------------------------------------------------------------------
 * UART simulada
 * ------------------------------------------------------------------------- */

/* Si esta en true (por defecto), vcp_uart_write() imprime la trama en
 * hexadecimal por stdout, que es lo que pide el Anexo A.5. El banco de pruebas
 * la apaga cuando quiere formatear la salida a su manera. */
void puart_set_eco(bool eco);

/* Prefijo que se imprime antes de los bytes. Por defecto "[UART TX]". */
void puart_set_prefijo(const char *p);

/* Cuantas veces se llamo a vcp_uart_write() y cuantos bytes salieron. */
uint32_t puart_n_escrituras(void);
uint32_t puart_n_bytes(void);

/* Copia la ultima trama transmitida. Devuelve cuantos bytes copio.
 * Se usa en los tests para verificar QUE se transmitio sin parsear stdout. */
size_t puart_ultima(uint8_t *dst, size_t max);

/* Vuelve todo a cero. */
void puart_reset(void);

/* ---------------------------------------------------------------------------
 * Medio de pago simulado
 * ------------------------------------------------------------------------- */

/*
 * El simulador necesita saber que hora es, porque el cobro "tarda". En el
 * equipo real el lector de tarjetas tiene su propio reloj; aca se lo damos
 * nosotros desde el lazo del banco de pruebas.
 *
 * Importante: esto NO es una concesion del firmware al simulador. El firmware
 * llama a pago_estado() sin pasarle la hora, igual que en el equipo real.
 */
void pago_sim_tick(uint32_t now_ms);

/* Deja el simulador en su estado inicial. */
void pago_sim_init(uint32_t now_ms);

/*
 * Programa como va a resolver el PROXIMO cobro que se inicie.
 *   resultado : PAGO_AUTORIZADO o PAGO_RECHAZADO
 *   demora_ms : cuanto tarda en resolverse (el enunciado dice 50..900 ms)
 *   txid      : id de transaccion a devolver si autoriza; 0 = automatico
 *
 * Si demora_ms == PAGO_SIM_NUNCA, el cobro se queda EN CURSO para siempre:
 * es el escenario del ejercicio E2.8.
 */
#define PAGO_SIM_NUNCA  0xFFFFFFFFu
void pago_sim_programar(pago_estado_t resultado, uint32_t demora_ms, uint16_t txid);

/* Cuantos cobros se iniciaron desde el ultimo init. Sirve para el test de
 * idempotencia: un VEND_REQUEST retransmitido no debe iniciar un segundo cobro. */
uint32_t pago_sim_n_iniciados(void);

#endif /* PORT_PC_H */
