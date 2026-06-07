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

        .nvmeib_pet_messages : {
               . = ALIGN(8);
                __start_nvmeib_pet_messages = .;
                KEEP(*(.nvmeib_pet_messages))
                __stop_nvmeib_pet_messages = .;
        }

        .nvmeibc_wq_metrics : {
               . = ALIGN(8);
                __start_nvmeibc_wq_metrics = .;
                KEEP(*(.nvmeibc_wq_metrics))
                __stop_nvmeibc_wq_metrics = .;
        }
}
