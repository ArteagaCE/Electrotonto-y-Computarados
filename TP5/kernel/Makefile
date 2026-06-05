# Makefile para cross-compilación del módulo cdd_sensor
#
# Uso:
#   make              — compila el módulo (requiere KERNEL_DIR)
#   make clean        — limpia artefactos
#
# Variables configurables (pasar por CLI o exportar):
#
#   KERNEL_DIR   — ruta al árbol de fuentes del kernel de la RPi
#                  Ejemplo: $(HOME)/rpi-kernel/linux
#
#   ARCH         — arquitectura objetivo
#                  RPi Zero/1/2/3 (32-bit OS) : arm
#                  RPi 3/4/5      (64-bit OS) : arm64
#
#   CROSS_COMPILE — prefijo del toolchain
#                  32-bit : arm-linux-gnueabihf-
#                  64-bit : aarch64-linux-gnu-
#
# ──────────────────────────────────────────────────────────
# Pasos previos (una sola vez en la PC de desarrollo):
#
#   1. Clonar el kernel de la RPi:
#      git clone --depth=1 -b rpi-6.6.y \
#          https://github.com/raspberrypi/linux $(HOME)/rpi-kernel/linux
#
#   2. Preparar los headers (para 32-bit RPi Zero / Pi 2 / Pi 3):
#      cd $(HOME)/rpi-kernel/linux
#      ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- make bcm2835_defconfig
#      ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- make modules_prepare
#
#   3. Verificar la versión del kernel en la RPi:
#      uname -r   (en la RPi)
#   Si no coincide exactamente con el branch clonado, el módulo no cargará.
# ──────────────────────────────────────────────────────────

ARCH         ?= arm
CROSS_COMPILE ?= arm-linux-gnueabihf-
KERNEL_DIR   ?= $(HOME)/rpi-kernel/linux

MODULE_NAME  := cdd_sensor
obj-m        := $(MODULE_NAME).o

.PHONY: all clean

all:
	$(MAKE) -C $(KERNEL_DIR) \
		M=$(CURDIR) \
		ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) \
		modules

clean:
	$(MAKE) -C $(KERNEL_DIR) \
		M=$(CURDIR) \
		ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) \
		clean
