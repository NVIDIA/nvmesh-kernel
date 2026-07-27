# NVMesh by Excelero

## Description
NVMesh by Excelero provides remote shared storage facilities with in-server flash performance characteristics while using commodity off-the-shelf components. It leverages Excelero’s patent pending remote direct disk access (RDDA) functionality when working with NVMe devices and RDMA capable NICs.

As NVMesh is a software only solution, it has the flexibility to provide storage in a hyper-converged architecture or as a top-of-rack flash appliance or as part of a dedicated storage rack.

NVMesh comprises three software elements:
* management server.
* storage client
* storage target (server)

### Management Server
The management server is used for providing storage definitions and monitoring the health and performance of the system.

### Storage Client
The storage client software implements block device functionality for storage consumers.

### Storage Target
The storage target software identifies storage hardware and sets up RDDA and non-RDDA pathways to the storage elements on behalf of the storage clients.
TOMA is part of the storage target

### RDMA Transport
The storage client and the storage target require one or more RDMA NICs installed on the machine which hosts them. Infiniband or ROCE supported. Mellanox OFED 3.x is used, downloadable from [Mellanox OFED download page](http://www.mellanox.com/page/products_dyn?product_family=26 "Mellanox
OpenFabrics Enterprise Distribution for Linux (MLNX_OFED)").

## Building
Use `build.sh` to build NVMesh. See `build.sh --help` output for details.

