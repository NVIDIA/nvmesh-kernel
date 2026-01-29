SECTIONS {
        .nvmeib_memmgr_metrics : {
                . = ALIGN(8);
                __start_nvmeib_memmgr_metrics = .;
                KEEP(*(.nvmeib_memmgr_metrics))
                __stop_nvmeib_memmgr_metrics = .;
        }
}
