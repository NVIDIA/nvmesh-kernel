#include "u.h"
#include "c_dc.h"

MODULE_AUTHOR("NVIDIA CORPORATION");
MODULE_DESCRIPTION("NVMesh DC test driver - client");
MODULE_LICENSE("GPL and additional rights");

static int __init _in_(void) /* Constructor */
{
	int rv = 0;

	FIN;
	trace("hello from the client\n");
	rv = c_dc_in_();
	FOUT;
	return rv;
}

void __exit _out_(void) /* Destructor */
{
	FIN;
	trace("bye from the client\n");
	c_dc_out_();
	FOUT;
}

module_init(_in_);
module_exit(_out_);

