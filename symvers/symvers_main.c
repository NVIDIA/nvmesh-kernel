#include "symvers_config.h"
#include <linux/module.h>
#include <linux/version.h>

MODULE_LICENSE("Dual BSD/GPL");

struct symver {
    const char *sym;
    CRCTYPE ver;
};

#define SYMVER(_sym, _ver) { _sym, _ver }

struct symver symbols[] = {
#include "symbols.c"
};

static int check_symver(struct symver *sv)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,4,0)
    return 0;
#else
	struct module *owner = NULL;
	CRCTYPE *crc = NULL;

	preempt_disable();
	find_symbol(sv->sym, &owner, &crc, true, false);
	preempt_enable();

        if (!crc) {
            pr_warn("Symbol '%s' not found\n", sv->sym);
            return -EINVAL;
        }

        if (*crc != sv->ver) {
            pr_warn("Bad symbol '%s' version %#x != %#x\n",
                    sv->sym, (u32)sv->ver, (u32)*crc);
            return -EINVAL;
        }

        pr_debug("%s: CRC: %#x owner: %s\n",
		 sv->sym, (u32)*crc, owner ? owner->name : "NULL");

        return 0;
#endif
}

static int __init symvers_init(void)
{
    int rc;
    int i;

    for (i = 0; i < ARRAY_SIZE(symbols); i++) {
        rc = check_symver(&symbols[i]);
        if (rc)
            return rc;
    }

    return 0;
}


static void __exit symvers_exit(void)
{
}

module_init(symvers_init);
module_exit(symvers_exit);
