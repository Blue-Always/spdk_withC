	SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
	include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

	APP = spdk_ram_bdev_bench
	C_SRCS = spdk_ram_bdev_bench.c

	# Add this line to your Makefile
	COMMON_CFLAGS += -I$(SPDK_ROOT_DIR)/dpdk/build/include


	SPDK_LIB_LIST = event event_bdev bdev bdev_malloc env_dpdk log

	include $(SPDK_ROOT_DIR)/mk/spdk.app.mk

