/*
 * ============================================================================
 *  vcp.h - Header "paraguas" de la biblioteca VCP-1
 * ============================================================================
 *
 *  Incluir SOLO este header alcanza para usar toda la biblioteca:
 *
 *      #include "vcp/vcp.h"
 *
 *  QUE CAMBIO RESPECTO DEL vcp.h SUGERIDO EN Anexos/
 *  -------------------------------------------------
 *  El enunciado permite modificar la interfaz sugerida siempre que se
 *  justifique. El original quedo intacto en Anexos/vcp.h; estos son los
 *  cambios, con el motivo en una linea (el detalle esta en
 *  docs/01_E1_decisiones.md):
 *
 *   1. Un header por responsabilidad en vez de uno solo.
 *      Motivo: el original mezclaba protocolo, receptor, emision y entorno.
 *      Separarlos permite compilar la logica de protocolo sin arrastrar el
 *      entorno, y testear cada pieza sola.
 *
 *   2. vcp_rx_byte() y vcp_rx_tick() reportan por CALLBACK en vez de retornar
 *      un vcp_event_t.
 *      Motivo: con re-sincronizacion por reproceso, un solo byte de entrada
 *      puede generar mas de un evento (un error y, ademas, una trama valida
 *      escondida adentro del candidato fallido). Con la firma original habria
 *      que descartar uno de los dos.
 *
 *   3. vcp_frame_t suma el campo t_etx_ms.
 *      Motivo: el plazo de 5 ms del Anexo A.2 se cuenta desde el ETX. Sin la
 *      marca de tiempo de la trama, el requisito E1.14 no se puede hacer
 *      cumplir desde el codigo, solo prometer en el README.
 *
 *   4. vcp_rx_t tiene estado real en vez del placeholder.
 *      Motivo: obvio, era un placeholder.
 *
 *   5. Se agrega VCP_EV_BYTE_FUERA_TRAMA al enum de eventos.
 *      Motivo: requisito E1.5. Distinguir "linea ruidosa" de "tramas rotas"
 *      es la diferencia entre culpar al cableado o al firmware.
 *
 *   6. Se agrega vcp_rx_set_resync() para elegir politica de recuperacion.
 *      Motivo: permitir que el banco de pruebas corra el mismo vector con las
 *      dos politicas y muestre por que elegimos una.
 *
 *  Lo que NO cambio: el formato de trama, el CRC, los codigos de comando y de
 *  motivo, y las firmas del entorno provisto (vcp_uart_write, pago_iniciar,
 *  pago_estado). Eso es contrato con la otra punta y no se toca.
 * ============================================================================
 */

#ifndef VCP_H
#define VCP_H

#include "vcp/vcp_cfg.h"       /* constantes del protocolo                   */
#include "vcp/vcp_crc.h"       /* CRC-8                                      */
#include "vcp/vcp_frame.h"     /* tipos de trama + construccion para emision */
#include "vcp/vcp_rx.h"        /* receptor incremental                       */
#include "vcp/vcp_session.h"   /* maquina de estados de la venta             */
#include "vcp/vcp_app.h"       /* tabla de respuestas A.4                    */
#include "vcp/vcp_port.h"      /* entorno provisto (UART + medio de pago)    */

#endif /* VCP_H */
