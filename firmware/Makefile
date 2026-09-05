# =============================================================================
#  Makefile - Firmware VCP-1 (banco de pruebas para PC)
# =============================================================================
#
#  Uso:
#     make            compila los tres binarios en build/
#     make test       compila y corre las pruebas propias (E1.8)
#     make run        corre el banco con el vector del Anexo B (E1.7)
#     make salidas    regenera los archivos de salida/ que van en la entrega
#     make clean      borra build/
#
#  En Windows sin make, usar build.ps1 (PowerShell) o build.bat (cmd).
# =============================================================================

CC      ?= gcc
CSTD    ?= -std=c11

# Avisos exigentes a proposito. En firmware un warning ignorado es un bug
# esperando su turno: -Wconversion agarra las truncadas implicitas entre
# tipos de distinto tamano, que es la fuente clasica de errores con uint8_t.
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wcast-qual \
           -Wstrict-prototypes -Wmissing-prototypes
OPT     ?= -O2
INC     := -Ilib/vcp/include -Iport/pc -Itests
CFLAGS  := $(CSTD) $(WARN) $(OPT) $(INC)

BUILD   := build
ANEXOS  := ../Anexos
SALIDA  := ../salida

# La biblioteca de protocolo: no depende del sistema operativo ni del entorno.
LIB_SRC   := $(wildcard lib/vcp/src/*.c)
# El port: implementa el "entorno provisto" para PC.
PORT_SRC  := $(wildcard port/pc/*.c)
# Las pruebas.
TEST_SRC  := $(wildcard tests/*.c)

.PHONY: all test run salidas clean

all: $(BUILD)/vcp1 $(BUILD)/demo_venta $(BUILD)/tests

$(BUILD):
	@mkdir -p $(BUILD)

# ---- E1.7: banco de pruebas del stream -------------------------------------
$(BUILD)/vcp1: $(LIB_SRC) $(PORT_SRC) app/main_vcp1.c | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

# ---- E1.12: demostracion de la sesion de venta -----------------------------
$(BUILD)/demo_venta: $(LIB_SRC) $(PORT_SRC) app/main_demo_venta.c | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

# ---- E1.8: pruebas propias -------------------------------------------------
$(BUILD)/tests: $(LIB_SRC) $(PORT_SRC) $(TEST_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

test: $(BUILD)/tests
	@./$(BUILD)/tests

run: $(BUILD)/vcp1
	@./$(BUILD)/vcp1 $(ANEXOS)/stream_vcp1.txt

# Regenera los archivos de salida/ que forman parte de la entrega.
salidas: all
	@mkdir -p $(SALIDA)
	./$(BUILD)/vcp1 $(ANEXOS)/stream_vcp1.txt              > $(SALIDA)/01_anexoB_reproceso.txt
	./$(BUILD)/vcp1 --resync descarte $(ANEXOS)/stream_vcp1.txt > $(SALIDA)/02_anexoB_descarte.txt
	./$(BUILD)/vcp1 --sin-plazo $(ANEXOS)/stream_vcp1.txt  > $(SALIDA)/03_anexoB_sin_plazo.txt
	./$(BUILD)/demo_venta                                  > $(SALIDA)/04_demo_venta.txt
	./$(BUILD)/tests                                       > $(SALIDA)/05_tests.txt
	@echo "salidas regeneradas en $(SALIDA)/"

clean:
	rm -rf $(BUILD)
