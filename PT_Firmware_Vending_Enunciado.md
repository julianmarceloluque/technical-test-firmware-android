# Prueba técnica — Desarrollador Firmware + Android
**Control Global · Unidad Vending Control**

Candidato: Julián Marcelo Luque
Fecha de envío: \_\_/\_\_/2026 · Fecha límite de entrega: \_\_/\_\_/2026 (3 días corridos)

---

## 1. Qué esperamos de esta prueba

Esta instancia no busca medir cuánto sabés de máquinas expendedoras ni de nuestros protocolos: no los conocés todavía y no es un requisito. Lo que queremos ver es **cómo resolvés**: cómo leés una especificación, qué preguntas te hacés, cómo tratás los casos que la especificación no cubre y cómo justificás las decisiones que tomás.

En varios puntos del enunciado hay ambigüedades deliberadas. No están para que adivines la respuesta correcta: están para que tomes un criterio, lo dejes escrito y expliques por qué. **Una decisión discutible bien fundamentada vale más que una decisión "correcta" sin justificación.**

### Reglas

| ID | Regla |
|----|-------|
| R1 | Tiempo estimado de resolución: 6 horas y media. No dediques más de eso. Si algo te lleva más tiempo del previsto, entregalo incompleto y explicá hasta dónde llegaste y qué te faltó. |
| R2 | Plazo de entrega: 3 días corridos desde la recepción. Si necesitás más tiempo por tu carga laboral o por viajes, avisanos apenas lo veas y lo acordamos sin problema. Pedir una extensión no resta puntaje; entregar apurado sí se nota. |
| R3 | Podés usar cualquier material de consulta, incluidos asistentes de IA. Solo pedimos que lo declares en el README: qué usaste y para qué. No resta puntaje. |
| R4 | Vas a defender tu entrega en una instancia en vivo por videollamada. Todo lo que entregues tenés que poder explicarlo y modificarlo delante nuestro. |
| R5 | Si algo del enunciado es genuinamente ambiguo, no consultes: resolvé con el criterio que te parezca y dejalo documentado en la sección de supuestos. |
| R6 | Entrega: repositorio Git (público o privado con acceso) o archivo comprimido por correo. Estructura libre. |

### Cómo se evalúa

Con este peso relativo aproximado:

- Capacidad de análisis y decisión frente a casos no especificados: **alto**
- Robustez y calidad del código de firmware: **alto**
- Claridad al comunicar por escrito lo que hiciste y por qué: **medio-alto**
- Conocimiento previo del dominio (vending, Android, medios de pago): **no se evalúa**

---

## 2. Contexto

Control Global desarrolla el controlador embebido que se instala dentro de las máquinas expendedoras. Ese controlador se comunica por puerto serie con la máquina, gestiona el medio de pago, y reporta ventas y estado a un backend a través de un enlace celular que no siempre está disponible.

Los ejercicios que siguen son versiones simplificadas de problemas reales de esa unidad.

---

## Anexo A — Especificación del protocolo VCP-1

Protocolo serie orientado a bytes entre la máquina expendedora y el controlador.

**La expendedora es el master y el controlador es el slave.** El master gobierna la comunicación: interroga, y el slave responde. El controlador **nunca inicia una transmisión**; solo transmite como respuesta a una trama que acaba de recibir. Este es el rol que vas a implementar.

### A.1 Formato de trama

```
+-----+------+-----+-----+---------------+-----+-----+
| STX | ADDR | LEN | CMD |    PAYLOAD    | CRC | ETX |
+-----+------+-----+-----+---------------+-----+-----+
   1     1      1     1     0..64 bytes     1     1
```

| Campo | Valor / descripción |
|-------|---------------------|
| STX | `0x02` fijo. |
| ADDR | Dirección del dispositivo destinatario. La del controlador que estás escribiendo es `0x0A`. `0x00` es broadcast. |
| LEN | Cantidad de bytes de PAYLOAD. Rango válido: 0 a 64. |
| CMD | Código de comando (ver A.3). |
| PAYLOAD | LEN bytes. **Sin escapado ni byte stuffing**: cualquier byte, incluidos `0x02` y `0x03`, puede aparecer dentro del payload. |
| CRC | CRC-8, polinomio `0x07`, valor inicial `0x00`, sin reflexión ni XOR final. Se calcula sobre ADDR + LEN + CMD + PAYLOAD (no incluye STX, CRC ni ETX). |
| ETX | `0x03` fijo. |

### A.2 Condiciones de línea y tiempos

- La línea es half-duplex a 9600 8N1 y es ruidosa. Pueden aparecer bytes espurios entre tramas, y pueden perderse bytes dentro de una trama.
- **El bus es multipunto.** Sobre la misma línea cuelgan varios periféricos además del controlador: otros medios de pago, el validador de billetes, el monedero. Todos escuchan todo. **Solo debe responder aquel al que la trama está dirigida.** Si respondés a una trama que no es tuya, chocás con la respuesta del destinatario legítimo y corrompés las dos.
- Las tramas dirigidas a otra dirección hay que procesarlas igual, para no perder la sincronización, pero sin transmitir. Las de broadcast se procesan y **no** se responden.
- **Plazo de respuesta: 5 ms.** El controlador debe empezar a transmitir su respuesta dentro de los 5 ms posteriores al ETX de la trama recibida.
- **El plazo es duro: si no llegás, no contestes.** Una respuesta tardía se superpone con la trama siguiente del master o, peor, llega justo a tiempo para que el master la tome como respuesta al comando **nuevo**. Una respuesta vieja contestando un comando nuevo es peor que no contestar: no contestar solo cuesta un reintento.
- **Reintentos del master: cada 100 ms.** Si no recibe respuesta, repite la trama. **A la tercera sin respuesta declara al controlador fuera de servicio** y deja de operar con medios de pago hasta que vuelva a responder.
- **Timeout entre bytes: 5 ms.** Si dentro de una trama en curso pasan más de 5 ms sin recibir un byte, la trama se considera abortada. A 9600 baudios un byte tarda algo más de 1 ms, así que 5 ms son unos cinco tiempos de byte.
- El receptor no puede bloquear: los bytes llegan de a uno desde una interrupción de UART y el procesamiento corre en el lazo principal.
- La especificación **no define** qué hacer exactamente después de una trama inválida, ni varias de las situaciones que aparecen al responder. Esos criterios los tenés que definir vos.

### A.3 Comandos

| CMD | Nombre | Dirección | Payload |
|-----|--------|-----------|---------|
| `0x01` | ACK | Controlador → Máquina | vacío |
| `0x10` | STATUS_REQ | Máquina → Controlador | vacío. Es la interrogación periódica del master |
| `0x11` | STATUS_RESP | Controlador → Máquina | 4 bytes: estado (u8), reservado (u8), temperatura en décimas de °C (int16 big-endian) |
| `0x20` | VEND_REQUEST | Máquina → Controlador | 3 bytes: selección (u8), precio en centavos (u16 big-endian) |
| `0x21` | VEND_APPROVE | Controlador → Máquina | 2 bytes: id de transacción (u16 big-endian) |
| `0x22` | VEND_DENY | Controlador → Máquina | 1 byte: motivo (ver A.4) |
| `0x23` | VEND_PENDING | Controlador → Máquina | vacío. Significa: recibido, todavía no hay resultado |
| `0x30` | VEND_SUCCESS | Máquina → Controlador | 2 bytes: id de transacción (u16 big-endian) |
| `0x31` | VEND_FAILURE | Máquina → Controlador | 3 bytes: id de transacción (u16 big-endian), causa (u8) |
| `0x40` | LOG_EVENT | Máquina → Controlador | 0 a 64 bytes, contenido arbitrario |

### A.4 Qué responder a cada trama

| Trama recibida | Respuesta del controlador |
|---|---|
| `STATUS_REQ` | `STATUS_RESP`, **salvo que haya un resultado de venta pendiente de informar**, en cuyo caso se responde con `VEND_APPROVE` o `VEND_DENY` |
| `VEND_REQUEST` | `VEND_PENDING` si el pedido es válido y el cobro arranca; `VEND_DENY` si se rechaza de entrada |
| `VEND_SUCCESS` | `ACK` |
| `VEND_FAILURE` | `ACK` |
| `LOG_EVENT` | `ACK` |
| Trama dirigida a otra dirección | Nada. Se procesa para mantener la sincronía, no se transmite |
| Trama de broadcast (`0x00`) | Se procesa, no se responde |
| Trama inválida o comando no reconocido | Nada. El master reintentará |

Motivos de `VEND_DENY`:

| Código | Motivo |
|--------|--------|
| `0x01` | Precio fuera de rango |
| `0x02` | Selección inválida |
| `0x03` | Pago rechazado |
| `0x04` | Sesión en curso |
| `0x05` | Error interno |

Rangos válidos: selección de 1 a 80, precio de 50 a 20.000 centavos.

### A.5 Entorno provisto

Estas funciones te las damos declaradas en `vcp.h`. No las implementes, usalas:

- `vcp_uart_write()` escribe bytes en la línea. En el banco de pruebas para PC alcanza con que imprima la trama en hexadecimal, para poder verificar qué habrías transmitido.
- `pago_iniciar()` arranca un cobro por un importe. `pago_estado()` consulta cómo va: en curso, autorizado con su id de transacción, o rechazado. **El cobro tarda entre 50 y 900 ms en resolverse.** No existe una versión bloqueante de esta interfaz, y eso es deliberado.

Hacia el medio de pago el controlador es master; hacia la expendedora es slave. Los dos roles conviven en el mismo lazo.

---

## E1 — Receptor y respondedor VCP-1 (código, ~4 h)

Implementá en **C** el lado slave del protocolo VCP-1: recibir las tramas de la expendedora y responder según el Anexo A.4.

### Requisitos

| ID | Requisito |
|----|-----------|
| E1.1 | Procesamiento incremental byte a byte. La función de ingreso recibe un byte y retorna sin bloquear. |
| E1.2 | Sin memoria dinámica (`malloc`/`free`) y sin variables globales mutables fuera de la estructura de contexto. |
| E1.3 | Debe ser imposible desbordar el buffer, cualquiera sea la secuencia de bytes de entrada. |
| E1.4 | Ante una trama inválida, el receptor debe poder recuperarse y seguir aceptando tramas válidas posteriores. |
| E1.5 | Debe reportar los errores detectados de forma diferenciada (no alcanza con descartar en silencio). |
| E1.6 | El manejo del timeout entre bytes (A.2) tiene que estar contemplado en el diseño de la interfaz, aunque el vector de prueba de E1.7 no lo ejercite. |
| E1.7 | Un programa de prueba que corra en PC (Linux/Windows) e imprima el resultado de cada trama detectada y de cada error. El vector de entrada no debe estar embebido en el código: tiene que poder pasarse como argumento, archivo o entrada estándar, en el mismo formato hexadecimal del Anexo B. Con el vector del Anexo B tiene que funcionar sin modificaciones. |
| E1.8 | Pruebas propias que cubran los casos que consideres relevantes. Formato libre: no hace falta un framework. |
| E1.9 | Construcción de tramas para emisión, con el mismo CRC. Fijate bien en el Anexo A.1 antes de escribirla. |
| E1.10 | Respuesta a cada trama recibida según la tabla A.4, dentro del plazo de 5 ms. |
| E1.11 | El controlador **nunca transmite por iniciativa propia**. Toda emisión tiene que ser consecuencia de una trama recibida. |
| E1.12 | Manejo de la sesión de venta: el cobro tarda mucho más que el plazo de respuesta, así que el estado de la sesión tiene que sobrevivir entre interrogaciones y el resultado se informa cuando esté listo. |
| E1.13 | Filtrado por dirección: procesar todas las tramas, responder solo las propias. |
| E1.14 | Ninguna respuesta puede emitirse fuera de plazo ni corresponder a una trama anterior a la que se está contestando. |

Sobre el plazo de 5 ms: en un banco de pruebas para PC no se puede medir de verdad. Lo que vamos a mirar es que en el camino que va desde el ETX hasta la emisión de la respuesta no haya ninguna espera, y que el diseño haga imposible emitir una respuesta preparada para una trama anterior.

Al responder vas a encontrarte con varias situaciones que el Anexo A no resuelve. Elegí un criterio para cada una y anotalo; en el README queremos ver la lista de las que detectaste, no solo las que resolviste.

En `vcp.h` te dejamos una interfaz sugerida. Podés modificarla si te resulta mejor, siempre que expliques por qué.

### Anexo B — Vector de prueba

76 bytes. También te lo adjuntamos en `stream_vcp1.txt`.

```
AA 55 7F 02 0A 00 10 F7 03 02 0B 00 10 9C 03 02
0A 06 40 41 42 02 03 43 44 61 03 02 0A 03 20 05
03 E8 E3 03 02 0A 08 40 54 52 02 0A 02 21 00 2A
0F 03 02 0A 41 40 58 58 58 02 02 0A 02 30 00 2A
C6 03 02 00 02 40 42 43 15 03 00 FF
```

El vector contiene tramas válidas y tramas defectuosas, dirigidas al controlador y a otros dispositivos. No te decimos cuántas de cada una: parte del ejercicio es que lo determines vos. Para cada trama detectada, tu programa debería dejar claro si la habrías respondido y con qué.

Está capturado de una línea de laboratorio, así que puede contener tramas que en operación normal no deberían circular en ese sentido. Tratalas como corresponda.

### Entregable de E1

- Código fuente y forma de compilarlo (un `Makefile`, un script o una línea de comando en el README).
- Salida del programa de prueba, incluyendo las tramas que el controlador habría transmitido.
- En el README, máximo una carilla y media: qué decisiones de diseño tomaste, qué política de recuperación elegiste ante trama inválida **y por qué esa y no otra**, qué casos del vector la ejercitan, y la lista de situaciones no especificadas que encontraste al responder con el criterio que adoptaste en cada una.

---

## E2 — Consistencia ante corte de energía (escrito, ~1 h)

Este ejercicio continúa sobre lo que implementaste en E1. La máquina de estados de la sesión ya la escribiste en código, así que acá no hace falta volver a dibujarla: lo que sigue es qué pasa cuando esa sesión se interrumpe.

La secuencia completa es:

```
VEND_REQUEST → [cobro al medio de pago] → VEND_APPROVE → [entrega] → VEND_SUCCESS | VEND_FAILURE
```

La máquina puede quedarse sin energía en cualquier instante, incluido el instante posterior al cobro y anterior a la entrega. El controlador tiene una memoria no volátil de escritura limitada (del orden de 100.000 ciclos por sector) y un enlace al backend que puede estar caído durante horas.

**Consigna.** Máximo dos carillas, referidas a tu propia implementación.

| ID | Punto |
|----|-------|
| E2.2 | Qué se escribe en memoria no volátil, en qué momento exacto, y por qué en ese momento y no antes ni después. |
| E2.3 | Qué hace el controlador al arrancar si encuentra una sesión sin cerrar. Distinguí los casos según lo que alcanzó a registrarse. |
| E2.4 | Hay un caso en el que el sistema no puede saber si el producto se entregó. Identificalo y decidí qué hacer. Justificá a favor de quién resolvés la ambigüedad y qué consecuencia tiene esa decisión. |
| E2.5 | Cuántas escrituras a memoria no volátil implica tu diseño por venta, y qué vida útil da eso para una máquina de 200 ventas diarias. |
| E2.6 | Cómo se reporta todo esto al backend cuando el enlace estuvo caído durante el episodio. |
| E2.7 | Qué ocurre si el controlador pierde energía **durante** la escritura del estado de la sesión en memoria no volátil. Explicá cómo distinguirías al arrancar un registro completo y consistente de uno que quedó a medio escribir. |
| E2.8 | Hacia el medio de pago el controlador es master: es él quien pregunta. ¿Qué hacés si el cobro nunca resuelve y se queda en curso para siempre? Definí el plazo y qué le contestás mientras tanto a la expendedora. |
| E2.9 | Si por un timeout decidís reintentar un cobro, puede que el primero sí se haya procesado. ¿Cómo evitás cobrar dos veces? Si no se puede evitar del todo, decí qué queda expuesto. |

---

## E3 — Diagnóstico de un incidente en campo (escrito, ~1 h)

Situación real simplificada. Un cliente opera 60 máquinas con nuestro controlador. Desde hace cinco días reporta cobros sin entrega de producto.

Datos disponibles:

- Los reclamos representan aproximadamente el 2 % de las ventas con tarjeta de esos cinco días.
- Se concentran en 8 máquinas de las 60. Las 8 son del mismo modelo de expendedora, pero ese modelo también está presente en otras 14 máquinas que no presentan reclamos.
- La mayoría de los episodios ocurre entre las 12 y las 15 h.
- Hace seis días se desplegó una actualización de firmware a toda la flota.
- La telemetría de las 8 máquinas muestra reinicios del controlador que no aparecían antes.

Extracto de log de una de las máquinas afectadas:

```
13:42:07.114  VEND_REQ  sel=24 price=1800
13:42:07.240  PAY  auth_ok txid=8821
13:42:07.244  TX  VEND_APPROVE txid=8821
13:42:07.310  RX  ---- (sin respuesta)
13:42:09.310  TIMEOUT vend_confirm txid=8821
13:42:09.311  TELEM queue_depth=1180
13:42:09.480  BOOT  reset_cause=WDT
13:42:11.902  BOOT  ok fw=2.4.1
13:42:12.007  TELEM queue_depth=0
```

**Consigna.** Máximo dos carillas.

| ID | Punto |
|----|-------|
| E3.1 | Enumerá las hipótesis que considerás plausibles, ordenadas por la combinación de probabilidad y costo de descartarlas. Mínimo cuatro. |
| E3.2 | Para cada hipótesis: qué evidencia de los datos disponibles la sostiene, **qué evidencia la contradice**, y qué dato pedirías o qué medición harías para confirmarla o descartarla, indicando qué resultado esperarías en cada caso. |
| E3.3 | Hay al menos un dato en el log que es una pista fuerte. Señalá cuál y qué te sugiere. |
| E3.4 | Qué harías en las próximas 2 horas, antes de saber la causa raíz, para contener el problema. Distinguí contención de solución. |
| E3.5 | Qué instrumentarías en el firmware para que la próxima vez este problema se diagnostique en una hora y no en cinco días. |
| E3.6 | Tres renglones: qué le comunicarías al cliente hoy. |

---

## E4 — Plan de incorporación de Android (escrito, ~30 min)

Sabemos que no tenés experiencia en Android; lo declaraste en la entrevista y no es un problema para el arranque del puesto. **Este ejercicio no evalúa conocimiento de Android.** Evalúa cómo planificás incorporar una tecnología nueva.

Supuesto: en algún momento vas a tener que desarrollar una aplicación Android de uso interno para el técnico de campo, que se conecte al controlador por Bluetooth para leer estado, ver los últimos eventos y forzar una prueba de entrega.

Máximo una carilla:

| ID | Punto |
|----|-------|
| E4.1 | Cómo te organizarías para llegar a una primera versión funcional. Qué harías primero y qué dejarías para después. |
| E4.2 | Qué parte de tu experiencia previa considerás transferible y cuál no. |
| E4.3 | Los dos o tres riesgos que más te preocuparían del proyecto, y cómo los reducirías temprano. |
| E4.4 | Qué necesitarías de nosotros. |

---

## 3. Entrega y siguiente etapa

Enviá el material a gyaccuzzi@controlglobal.biz antes de la fecha límite, con el asunto **"Prueba técnica — Julián Luque"**.

Después de la entrega coordinamos una instancia técnica en vivo, remota por videollamada, de aproximadamente 80 minutos con el responsable de desarrollo. En esa instancia vamos a repasar tu entrega, te vamos a pedir que modifiques tu propio código en vivo frente a un requerimiento nuevo, y vas a tener espacio para las consultas técnicas que te surjan de la prueba. Es una instancia exclusivamente técnica: las condiciones de contratación y la coordinación del proceso las sigue viendo Recursos Humanos.

No hace falta que prepares nada adicional para esa instancia. Conectate desde la máquina donde tengas el entorno de desarrollo que usaste, porque vas a necesitar compartir pantalla y escribir código durante la reunión.
