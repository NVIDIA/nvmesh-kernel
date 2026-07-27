from client import process_scheme
import json

class response:
	result = 55
	IDs = [7, 6, 5, 4, 3,2,1, 0]

class block_header:
	state=1
	id = 44

class question:
	hdr = block_header()
 	resp = response()
	gid = 'fe80000000000000e41d2d0300b40351'
	disk = 'CVMD5165005D400AGN.1'

with open ("scheme.json") as scheme_fd:
	packer = process_scheme.Packer(scheme_fd)

payload = "yaron kahanovitch"
d = question()
#pack the message, msg is ready to be sent to the other side
msg = packer.pack("question", d, 99, payload)

#say we got a msg, we need first to unpack the header
header_size = packer.header_size

with open ("/proc/nvmeibc/mcs", "wb") as mail_fd0: 
	mail_fd0.write(msg)

mail_fd = open ("/proc/nvmeibc/mcs", "rb")
print "message sent"

msg = mail_fd.read(header_size)

(msg_len, opcode, timestamp, version) = packer.unpack_header(msg)
print "msg_len={0}, opcode={1},timestamp={2} ver={3}".format(msg_len, opcode, timestamp, version)
msg = mail_fd.read(msg_len)


#now we have the data needed and we can unpack the message
result = packer.unpack_msg(opcode, msg)
print "results: ", result

