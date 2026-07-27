PWD := $(shell pwd)
KDIR ?= ~/development/kernels/linux

default:
	${MAKE} -C ${KDIR} M=${PWD}

clean:
	${MAKE} -C ${KDIR} M=${PWD} clean

