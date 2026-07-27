#ifndef NVMEIBC_DEFS_H
#define NVMEIBC_DEFS_H

#define P2NV(ib_port) ib_port->nic_dev->dev
#define P2IB(ib_port) P2NV(ib_port)->ib_dev

#endif

