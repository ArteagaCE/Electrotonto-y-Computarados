# ──────────────────────────────────────────────────────────
# TP5 — Makefile principal
# Targets:
#   make module    → cross-compila el .ko
#   make deploy    → envía .ko + userapp a la RPi por SCP
#   make load      → inserta el módulo en la RPi (via SSH)
#   make unload    → quita el módulo de la RPi (via SSH)
#   make run       → lanza la aplicación de usuario en la RPi
#   make clean     → limpia artefactos
#   make all       → module + deploy + load + run
# ──────────────────────────────────────────────────────────

# ── Configuración de la RPi (ajustar antes de usar) ──────
RPI_HOST    ?= pi@raspberrypi.local
RPI_DESTDIR ?= /home/pi/tp5

# ── Cross-compilación (ver kernel/Makefile para detalles) ─
ARCH          ?= arm
CROSS_COMPILE ?= arm-linux-gnueabihf-
KERNEL_DIR    ?= $(HOME)/rpi-kernel/linux

KERNEL_SRC := kernel
MODULE_KO  := $(KERNEL_SRC)/cdd_sensor.ko
USERAPP    := userapp

.PHONY: all module deploy load unload run clean

all: module deploy load run

module:
	$(MAKE) -C $(KERNEL_SRC) \
		ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) \
		KERNEL_DIR=$(KERNEL_DIR)

deploy: module
	@echo ">>> Copiando binarios a $(RPI_HOST):$(RPI_DESTDIR)"
	ssh $(RPI_HOST) "mkdir -p $(RPI_DESTDIR)/userapp/templates"
	scp $(MODULE_KO) $(RPI_HOST):$(RPI_DESTDIR)/
	scp -r $(USERAPP)/sensor_server.py \
	       $(USERAPP)/requirements.txt \
	       $(RPI_HOST):$(RPI_DESTDIR)/userapp/
	scp $(USERAPP)/templates/index.html \
	       $(RPI_HOST):$(RPI_DESTDIR)/userapp/templates/
	@echo ">>> Instalando dependencias Python en la RPi"
	ssh $(RPI_HOST) "cd $(RPI_DESTDIR)/userapp && pip3 install -r requirements.txt --break-system-packages"

load:
	@echo ">>> Cargando módulo en la RPi"
	ssh $(RPI_HOST) "sudo insmod $(RPI_DESTDIR)/cdd_sensor.ko && dmesg | tail -5"

unload:
	@echo ">>> Descargando módulo de la RPi"
	ssh $(RPI_HOST) "sudo rmmod cdd_sensor && dmesg | tail -3"

run:
	@echo ">>> Iniciando servidor web en la RPi (Ctrl+C para detener)"
	ssh -t $(RPI_HOST) "sudo python3 $(RPI_DESTDIR)/userapp/sensor_server.py"

clean:
	$(MAKE) -C $(KERNEL_SRC) \
		ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) \
		KERNEL_DIR=$(KERNEL_DIR) \
		clean
