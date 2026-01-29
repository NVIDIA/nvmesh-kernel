#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import sys, os, time, atexit, fcntl, signal
from signal import SIGTERM

class Daemon:
	def __init__(self, pidfile, lockfile, stdin="/dev/null", notifyMainProcessOnStartup=False):
		self.stdin = stdin
		self.pidfile = pidfile
		self.lockfile = lockfile
		self.mainProcessPID = os.getpid()
		self.notifyMainProcessOnStartup = notifyMainProcessOnStartup
		self.masterPID = None
		signal.signal(signal.SIGUSR2, self.handleFinishSignal)

	def handleFinishSignal(self, signum, frame):
		print('Got SIGUSR2 PID {} signum {}'.format(os.getpid(), signum))
		sys.exit(0)

	def signalMainProcessOnStartup(self):
		print('I\'m about to exit after fork #1')
		os.kill(self.mainProcessPID, signal.SIGUSR2)

	def daemonize(self):
		"""
		do the UNIX double-fork magic, see Stevens' "Advanced
		Programming in the UNIX Environment" for details (ISBN 0201563177)
		http://www.erlenstar.demon.co.uk/unix/faq_2.html#SEC16
		"""
		try:
			pid = os.fork()
			if pid > 0:
				# wait for grandchild to finish startup before exit the main process if needed
				while True:
					time.sleep(5)
				sys.exit(0)
		except OSError as e:
			sys.stderr.write("fork #1 failed: %d (%s)\n" % (e.errno, e.strerror))
			sys.exit(1)

		# decouple from parent environment
		os.chdir("/")
		os.setsid()
		os.umask(0)

		# using pipe to verify the second process exists before creating the third process
		read_fd, write_fd = os.pipe()
		# do second fork
		try:
			pid = os.fork()
			if pid > 0:
				# exit from second parent
				print('I\'m about to exit after 2nd fork {}'.format(os.getpid()))
				# close the read pipe, so that only the third process is the reader.
				os.close(read_fd)
				sys.exit(0)
		except OSError as e:
			sys.stderr.write("fork #2 failed: %d (%s)\n" % (e.errno, e.strerror))
			sys.exit(1)

		print('I\'m the grandchild, PID: {}'.format(os.getpid()))

		# close the write pipe so that the third process is the only writer.
		# this will make sure we then get an EOF when the only writer exits.
		os.close(write_fd)

		# now, we're waiting on the read pipe for an EOF.
		os.read(read_fd, 1) == ""
		os.close(read_fd)
		# at this point, the second process is dead.

		# redirect standard file descriptors
		sys.stdout.flush()
		sys.stderr.flush()
		try:
			with open(self.stdin, 'r') as si:
				os.dup2(si.fileno(), sys.stdin.fileno())
		except Exception:
			pass

		# write pidfile
		atexit.register(self.delpid)
		pid = str(os.getpid())
		try:
			with open(self.pidfile, 'w+') as pf:
				pf.write("%s\n" % pid)
		except Exception:
			pass

		os.chmod(self.pidfile, 0o664)

		# in case the calling entity did not implement the notify main process to exit option
		if not self.notifyMainProcessOnStartup:
			self.signalMainProcessOnStartup()

		return 0

	def delpid(self):
		os.remove(self.pidfile)

	def check_pid(self, pid):
		""" Check For the existence of a unix pid. """
		try:
			os.kill(pid, 0)
		except OSError:
			return False
		else:
			return True

	def try_lock(self):
		try:
			fcntl.lockf(open(self.lockfile, 'w'), fcntl.LOCK_EX | fcntl.LOCK_NB)
			os.chmod(self.lockfile, 0o664)
		except IOError as e:
			# another instance is running
			print('Another instance is running, you can only run one instance at a time.' \
				  ' The lockfile is {}. I\'m PID: {}. EX: {}'.format(self.lockfile, os.getpid(), e))
			sys.exit(1)

	def get_pid(self):
		try:
			with open(self.pidfile, 'r') as pf:
				pid = int(pf.read().strip())
		except Exception:
			pid = None

		return pid

	def start(self):
		"""
		Start the daemon
		"""
		# Check for a pidfile to see if the daemon already runs
		pid = self.get_pid()

		if pid:
			if self.check_pid(pid):
				message = "pidfile %s already exist. Daemon already running?\n"
				sys.stderr.write(message % self.pidfile)
				sys.exit(1)
			else:
				self.delpid()

		# Start the daemon
		pid = self.daemonize()
		if pid == 0:
			self.try_lock()
			self.run()
		return pid

	def stop(self):
		"""
		Stop the daemon
		"""
		# Get the pid from the pidfile
		pid = self.get_pid()

		if not pid:
			message = "pidfile %s does not exist. Daemon not running?\n"
			sys.stderr.write(message % self.pidfile)
			return # not an error in a restart

		# Try killing the daemon process
		try:
			while 1:
				os.kill(pid, SIGTERM)
				time.sleep(0.1)
		except OSError as err:
			err = str(err)
			if err.find("No such process") > 0:
				if os.path.exists(self.pidfile):
					os.remove(self.pidfile)
			else:
				print(str(err))
				sys.exit(1)

	def restart(self):
		"""
		Restart the daemon
		"""
		self.stop()
		return self.start()

	def run(self):
		"""
		You should override this method when you subclass Daemon. It will be called after the process has been
		daemonized by start() or restart().
		"""
