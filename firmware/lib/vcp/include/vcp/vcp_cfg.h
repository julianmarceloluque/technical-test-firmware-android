/*
 * ============================================================================
 *  vcp_cfg.h - Parametros de configuracion del protocolo VCP-1
 * ============================================================================
 *
 *  QUE ES ESTE ARCHIVO
 *  -------------------
 *  Todos los "numeros magicos" del protocolo viven aca y en ningun otro lado.
 *  Si manana la direccion del controlador cambia, o el plazo de respuesta pasa
 *  de 5 a 3 ms, se toca UNA linea de este archivo y no hay que buscar el valor
 *  desperdigado por diez .c distintos.
 *
 *  Es una practica estandar en firmware: separar POLITICA (los valores) de
 *  MECANISMO (el codigo que los usa).
 *
 *  Referencias al enunciado: Anexo A.1 (formato), A.2 (tiempos), A.4 (rangos).
 * ============================================================================
 */

#ifndef VCP_CFG_H
#define VCP_CFG_H

/* ---------------------------------------------------------------------------
 * 1) Bytes fijos de la trama (Anexo A.1)
 * ------------------------------------------------------------------------- */
#define VCP_STX                 0x02u   /* byte de inicio de trama            */
#define VCP_ETX                 0x03u   /* byte de fin de trama               */

/* ---------------------------------------------------------------------------
 * 2) Direcciones (Anexo A.1)
 * ------------------------------------------------------------------------- */
#define VCP_ADDR_SELF           0x0Au   /* la direccion de ESTE controlador   */
#define VCP_ADDR_BROADCAST      0x00u   /* trama para todos; no se responde   */

/* ---------------------------------------------------------------------------
 * 3) Tamanos
 * ------------------------------------------------------------------------- */

/* Maximo de bytes de PAYLOAD que admite el protocolo (Anexo A.1: 0..64). */
#define VCP_MAX_PAYLOAD         64u

/*
 * Tamano maximo de una trama COMPLETA, en bytes:
 *
 *   STX(1) + ADDR(1) + LEN(1) + CMD(1) + PAYLOAD(64) + CRC(1) + ETX(1) = 70
 *
 * Este es el tamano del buffer del receptor. Que este calculado a partir de
 * VCP_MAX_PAYLOAD y no escrito "70" a mano evita que se desincronicen si
 * manana el payload maximo cambia.
 */
#define VCP_TRAMA_MAX_BYTES     (4u + VCP_MAX_PAYLOAD + 2u)

/* Cantidad de bytes de la trama que entran en el calculo del CRC cuando el
 * payload esta vacio: ADDR + LEN + CMD = 3. (El CRC NO incluye STX ni ETX.) */
#define VCP_CRC_CABECERA_BYTES  3u

/* ---------------------------------------------------------------------------
 * 4) Tiempos (Anexo A.2)
 * ------------------------------------------------------------------------- */

/*
 * Timeout entre bytes: si estando a mitad de una trama pasan MAS de 5 ms sin
 * recibir el byte siguiente, la trama se considera abortada.
 * A 9600 8N1 un byte tarda 10 bits / 9600 bps = 1.0417 ms, asi que 5 ms son
 * casi cinco tiempos de byte: suficiente holgura para no cortar por jitter.
 */
#define VCP_TIMEOUT_ENTRE_BYTES_MS   5u

/*
 * Plazo de respuesta: el controlador tiene 5 ms desde el ETX de la trama
 * recibida para EMPEZAR a transmitir. Si no llega, NO contesta (el enunciado
 * es explicito: una respuesta tardia es peor que ninguna respuesta, porque el
 * master puede tomarla como respuesta al comando SIGUIENTE).
 */
#define VCP_PLAZO_RESPUESTA_MS       5u

/*
 * Cuanto esperamos la confirmacion (VEND_SUCCESS / VEND_FAILURE) despues de
 * haber informado un VEND_APPROVE. Pasado ese plazo damos la venta por no
 * confirmada, cerramos la sesion y la dejamos anotada para el backend.
 *
 * El valor sale del log del ejercicio E3, donde se ve exactamente esa espera:
 *     13:42:07.244  TX VEND_APPROVE txid=8821
 *     13:42:09.310  TIMEOUT vend_confirm txid=8821     -> ~2.07 s
 */
#define VCP_CONFIRM_TIMEOUT_MS       2000u

/*
 * Cuanto toleramos que el medio de pago se quede "EN CURSO" antes de darlo por
 * colgado. El enunciado dice que el cobro tarda entre 50 y 900 ms; 3000 ms es
 * mas de 3 veces el peor caso normal, asi que un vencimiento significa
 * "algo se rompio", no "tardo un poco mas". Ver E2.8.
 */
#define VCP_PAGO_TIMEOUT_MS          3000u

/* ---------------------------------------------------------------------------
 * 5) Rangos de validacion de VEND_REQUEST (Anexo A.4)
 * ------------------------------------------------------------------------- */
#define VCP_SEL_MIN             1u        /* seleccion valida: 1..80          */
#define VCP_SEL_MAX             80u
#define VCP_PRECIO_MIN          50u       /* centavos: 50..20000              */
#define VCP_PRECIO_MAX          20000u

/* ---------------------------------------------------------------------------
 * 6) Codigos de "estado" del byte 0 de STATUS_RESP
 * ------------------------------------------------------------------------- */
/*
 * OJO: el Anexo A.3 dice que STATUS_RESP lleva "estado (u8)" pero NO define
 * que valores puede tomar ese byte. Es una de las ambiguedades deliberadas del
 * enunciado. Los inventamos nosotros y lo documentamos (ver docs/01_E1_decisiones.md,
 * caso NE-08). Criterio: 0x00 = todo bien, valores crecientes = peor.
 */
#define VCP_ESTADO_LISTO            0x00u /* operativo, sin sesion en curso   */
#define VCP_ESTADO_OCUPADO          0x01u /* hay una sesion de venta abierta  */
#define VCP_ESTADO_FUERA_SERVICIO   0x02u /* medio de pago no disponible      */

/* Temperatura por defecto que reporta el banco de pruebas, en DECIMAS de
 * grado Celsius (235 = 23.5 C). En un equipo real esto lo da un sensor. */
#define VCP_TEMP_DEFECTO_DECIMAS    235

#endif /* VCP_CFG_H */
