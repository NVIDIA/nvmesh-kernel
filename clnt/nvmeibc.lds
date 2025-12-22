SECTIONS {
        .nvmeibc_memmgr_metrics : {
               . = ALIGN(8);
                __start_nvmeibc_memmgr_metrics = .;
                KEEP(*(.nvmeibc_memmgr_metrics))
                __stop_nvmeibc_memmgr_metrics = .;
        }

        .nvmeibc_error_tags : {
               . = ALIGN(8);
                __start_nvmeibc_error_tags = .;
                KEEP(*(.nvmeibc_error_tags))
                __stop_nvmeibc_error_tags = .;
        }

        .nvmeibc_io_pet_msgs : {
               . = ALIGN(8);
                __start_nvmeibc_io_pet_msgs = .;
                KEEP(*(.nvmeibc_io_pet_msgs))
                __stop_nvmeibc_io_pet_msgs = .;
        }
}
