obj-m := tcp_s.o tcp_c.o

tcp_s-y := tcp_srv.o
tcp_c-y := tcp_clnt.o

#ccflags-y := -I$(PWD)

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
