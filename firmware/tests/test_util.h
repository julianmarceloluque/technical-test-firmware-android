/*
 * ============================================================================
 *  test_util.h - Mini framework de pruebas (requisito E1.8)
 * ============================================================================
 *
 *  El enunciado dice "formato libre: no hace falta un framework". Este es el
 *  framework mas chico que sigue siendo util: un contador de exitos, uno de
 *  fallas, y una macro que imprime archivo y linea cuando algo no da.
 *
 *  Traer CUnit o Unity para esto seria agregar una dependencia que despues hay
 *  que instalar en la maquina de quien corrija.
 * ============================================================================
 */

#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern unsigned g_test_ok;
extern unsigned g_test_fail;

void test_grupo(const char *nombre);
void test_check(int cond, const char *desc, const char *archivo, int linea);
void test_check_u32(uint32_t obtenido, uint32_t esperado, const char *desc,
                    const char *archivo, int linea);
void test_check_mem(const void *obtenido, const void *esperado, size_t n,
                    const char *desc, const char *archivo, int linea);

/* Verifica una condicion booleana. */
#define VERIFICAR(cond, desc)  test_check((cond) ? 1 : 0, (desc), __FILE__, __LINE__)

/* Verifica igualdad numerica e imprime los dos valores si no coinciden. */
#define VERIFICAR_EQ(obt, esp, desc) \
    test_check_u32((uint32_t)(obt), (uint32_t)(esp), (desc), __FILE__, __LINE__)

/* Verifica igualdad de un bloque de bytes. */
#define VERIFICAR_MEM(obt, esp, n, desc) \
    test_check_mem((obt), (esp), (n), (desc), __FILE__, __LINE__)

/* Cada archivo de pruebas expone una funcion con esta forma. */
void tests_crc(void);
void tests_frame(void);
void tests_rx(void);
void tests_app(void);

#endif /* TEST_UTIL_H */
