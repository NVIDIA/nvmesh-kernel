#!/bin/bash
default_core_file='core'
read -p "core file name? (Default ${default_core_file})? " core_file
if [ "_${core_file}" == "_" ]; then
	core_file=${default_core_file};
fi;
sudo gdb ./blk_unitest ${core_file} << END


set \$i=0
while \$i<=nvmeibc_trace_long->active_buf
eval "dump binary memory nvmeibc_trace_long%d.0 nvmeibc_trace_long->bufs[%d] nvmeibc_trace_long->bufs[%d]+4096", \$i, \$i, \$i
set \$i=\$i+1
end
END

./pager --clnt > log_from_sim_core_dump.txt

