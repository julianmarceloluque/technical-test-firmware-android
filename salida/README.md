# Salidas de los programas

Generadas con `firmware\build.ps1 -Salidas` (o `make salidas`). Se incluyen en
la entrega para que se pueda ver el resultado sin compilar.

| Archivo | Comando | Qué muestra |
|---|---|---|
| `01_anexoB_reproceso.txt` | `vcp1 ..\Anexos\stream_vcp1.txt` | El vector del Anexo B con la política por defecto. **6 tramas**, 4 errores, 2 recuperadas, 2 respuestas emitidas, 1 omitida por plazo. |
| `02_anexoB_descarte.txt` | `vcp1 --resync descarte ...` | El **mismo** vector con la política simple: solo **3 tramas**. Es la justificación empírica de la decisión de diseño. |
| `03_anexoB_sin_plazo.txt` | `vcp1 --sin-plazo ...` | Igual que 01 pero sin la guardia de 5 ms: se ve qué se habría transmitido si el plazo no existiera (3 respuestas en vez de 2). |
| `04_demo_venta.txt` | `demo_venta` | Los 7 escenarios de la sesión de venta, con el diálogo completo y las marcas de tiempo. |
| `05_tests.txt` | `tests` | Las 179 verificaciones. |

## Los dos momentos que vale la pena mirar

**En `01`, la TRAMA #5** (offset 58): se recupera re-sincronizando, se muestra
el `ACK` que correspondía, y dice `NO TRANSMITIDA` porque su ETX fue 8 ms antes
y el plazo son 5 ms.

**En `04`, el escenario 1**: entre `VEND_REQUEST` (t≈96 ms) y `VEND_APPROVE`
(t≈394 ms) hay tres `STATUS_REQ` contestados con `STATUS_RESP OCUPADO`. Ahí se
ve por qué la venta no puede resolverse dentro de una respuesta.
