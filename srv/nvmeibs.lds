SECTIONS {
        .nvmeibs_memmgr_metrics : {
               . = ALIGN(8);
                __start_nvmeibs_memmgr_metrics = .;
                KEEP(*(.nvmeibs_memmgr_metrics))
                __stop_nvmeibs_memmgr_metrics = .;
        }
}