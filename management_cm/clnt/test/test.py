#!/usr/bin/env python2
"""
test script for mcs packer suit
"""
import os
from clnt import process_scheme

def main():
	"""main function"""
	print(os.getcwd())
	mypacker = process_scheme.Packer("management_cm/test.json")
	msg = {u'origin': u'b4301340-4ac1-11e7-8e58-305a3a540737',\
	u'success': True, u'enableCache': False, u'payload':\
	  {u'eventName': u'volumeID_test@volumeRemovedEvent',\
	   u'payload': {u'health': u'healthy', u'volumeID':\
	   u'test', u'uuid': u'6f268b30-4ac1-11e7-8048-fdc188e0701c'}}, u'opcode': 27,\
	   u'messageID': u'c624dda6-4ac1-11e7-8e58-305a3a540737',\
	   u'registrant': {u'type': u'CLIENT', u'id': u'nvme52.excelero.com'},\
	    u'requestMD5': u'135585e322ae12cf876c291b985a8053'}
	mypacker.pack("volume_deletion_message", True, msg["payload"])


main()
