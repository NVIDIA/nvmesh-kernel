from client import process_scheme
import json

class block_header:
	id = 5
	state = 3
	def __init__(self):
		pass

class response:
	result = 101
	IDs = [99, 88 ,77, 66, 55, 44, 33, 22]

class question:
	hdr = block_header()
	resp = response()
	gid = 'fe80000000000000e41d2d0300'
	disk = 'CVMD5165005D400AGN.1'

with open ("scheme.json") as scheme_fd:
	packer = process_scheme.Packer(scheme_fd)

payload = "yaron kahanovitch"
d = question()

msg = packer.pack("question", d, 55, payload);
print "message packed"

(msg_len, opcode, timestamp, sw_ver) = packer.unpack_header(msg[:packer.header_size])
print "msg_len={0}, opcode={1},timestamp={2}".format(msg_len, opcode, timestamp)
result = packer.unpack_msg(opcode, msg[packer.header_size:])
print result

