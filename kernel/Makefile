# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_OKRAPM) += okrapm.o

okrapm-y := \
	src/main.o \
	src/handler.o \
	src/utils.o

# Include header path
ccflags-y += -I$(src)/include

