# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_OPTEE) += optee.o
optee-objs += core.o
optee-objs += call_queue.o
optee-objs += call.o
optee-objs += notif.o
optee-objs += rpc.o
optee-objs += supp.o
optee-objs += shm_pool.o
optee-objs += device.o

# for tracing framework to find optee_trace.h
CFLAGS_call.o := -I$(src)
