obj-m += rdma_cm.o iw_cm.o

CFLAGS_cma_trace.o +=           -I$(src)
rdma_cm-y :=                    cma.o cma_trace.o

rdma_cm-$(CONFIG_INFINIBAND_ADDR_TRANS_CONFIGFS) += cma_configfs.o

iw_cm-y :=			iwcm.o iwpm_util.o iwpm_msg.o

