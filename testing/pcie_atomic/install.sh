#!/bin/sh
modprobe nvmeib_common_mlx4_public
modprobe nvmeib_common_mlx5_public
modprobe nvmeib_common_public
modprobe nvmeib_common
insmod pcie_atom_test.ko




