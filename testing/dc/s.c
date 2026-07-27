#include "u.h"
#include "s_dc.h"

MODULE_AUTHOR("Excelero");
MODULE_DESCRIPTION("NVMesh DC test driver - server");
MODULE_LICENSE("Dual BSD/GPL");

static int __init _in_(void) /* Constructor */
{
	int rv = 0;

	FIN;
	trace("hello from the server\n");
	rv = s_dc_in_();
	FOUT;
	return rv;
}

void __exit _out_(void) /* Destructor */
{
	FIN;
	trace("bye from the server\n");
	s_dc_out_();
	FOUT;
}

module_init(_in_);
module_exit(_out_);

