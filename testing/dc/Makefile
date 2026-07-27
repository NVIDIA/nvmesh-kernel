obj-m += dcs.o
dcs-y += u.o
dcs-y += u_sock.o
dcs-y += s_sock.o
# dcs-y += u_dc.o
dcs-y += s_dc.o
dcs-y += s.o

obj-m += dcc.o
dcc-y += u.o
dcc-y += u_sock.o
dcc-y += c_sock.o
# dcc-y += u_dc.o
dcc-y += c_dc.o
dcc-y += c.o

ccflags-y += -I../../common -I../../common_public
ccflags-y += -DCONFIG_NVMEIB_DEBUG -DCONFIG_NVMEIB_FINE

# TCP testing
ifeq ($(shell grep iov_iter_kvec $(KSRC1)/include/linux/uio.h 2> /dev/null),)
        ccflags-y += -DKS_IOV_ITER_KVEC=0
    ifeq ($(shell grep "struct iov_iter" $(KSRC1)/include/linux/socket.h 2> /dev/null),)
        ccflags-y += -DKS_SOCKET_IOV_ITER=0
    else
        ccflags-y += -DKS_SOCKET_IOV_ITER=1
    endif
else
    ccflags-y += -DKS_IOV_ITER_KVEC=1
endif
ifeq ($(shell grep "define timer_setup" $(KSRC1)/include/linux/timer.h 2> /dev/null),)
    ccflags-y += -DKS_TIMER_SETUP=0
else
    ccflags-y += -DKS_TIMER_SETUP=1
endif


