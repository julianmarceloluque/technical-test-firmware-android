/*
 * ============================================================================
 *  test_main.c - Runner de las pruebas y utilidades del mini framework
 * ============================================================================
 */

#include <stdio.h>

#include "test_util.h"

unsigned g_test_ok   = 0u;
unsigned g_test_fail = 0u;

static const char *s_grupo = "";

void test_grupo(const char *nombre)
{
    s_grupo = nombre;
    printf("\n--- %s ---\n", nombre);
}

void test_check(int cond, const char *desc, const char *archivo, int linea)
{
    if (cond) {
        g_test_ok++;
        printf("  ok    %s\n", desc);
    } else {
        g_test_fail++;
        printf("  FALLA %s\n        (%s:%d, grupo %s)\n",
               desc, archivo, linea, s_grupo);
    }
}

void test_check_u32(uint32_t obtenido, uint32_t esperado, const char *desc,
                    const char *archivo, int linea)
{
    if (obtenido == esperado) {
        g_test_ok++;
        printf("  ok    %s\n", desc);
    } else {
        g_test_fail++;
        printf("  FALLA %s\n        obtenido=%lu (0x%lX) esperado=%lu (0x%lX)\n"
               "        (%s:%d, grupo %s)\n",
               desc,
               (unsigned long)obtenido, (unsigned long)obtenido,
               (unsigned long)esperado, (unsigned long)esperado,
               archivo, linea, s_grupo);
    }
}

void test_check_mem(const void *obtenido, const void *esperado, size_t n,
                    const char *desc, const char *archivo, int linea)
{
    if (memcmp(obtenido, esperado, n) == 0) {
        g_test_ok++;
        printf("  ok    %s\n", desc);
    } else {
        const uint8_t *a = (const uint8_t *)obtenido;
        const uint8_t *b = (const uint8_t *)esperado;
        size_t i;
        g_test_fail++;
        printf("  FALLA %s\n        obtenido:", desc);
        for (i = 0u; i < n; i++) { printf(" %02X", (unsigned)a[i]); }
        printf("\n        esperado:");
        for (i = 0u; i < n; i++) { printf(" %02X", (unsigned)b[i]); }
        printf("\n        (%s:%d, grupo %s)\n", archivo, linea, s_grupo);
    }
}

int main(void)
{
    printf("============================================================"
           "================\n");
    printf(" VCP-1 - Pruebas propias (requisito E1.8)\n");
    printf("============================================================"
           "================\n");

    tests_crc();
    tests_frame();
    tests_rx();
    tests_app();

    printf("\n============================================================"
           "================\n");
    printf(" RESULTADO: %u verificaciones ok, %u fallas\n", g_test_ok, g_test_fail);
    printf("============================================================"
           "================\n");

    /* Codigo de salida distinto de cero si algo fallo: asi el script de build
     * o un CI se entera sin tener que leer la salida. */
    return (g_test_fail == 0u) ? 0 : 1;
}
