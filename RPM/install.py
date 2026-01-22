#!/usr/bin/python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import argparse
import sys

from functools import cmp_to_key
from subprocess import call
from os import walk, path

parser = argparse.ArgumentParser(description='nvmesh-client/target upgrade tool.')
parser.add_argument('-f', '--run-from-version', help='Run upgrade scripts from this version', required=True)
parser.add_argument('-t', '--run-to-version', help='Run upgrade scripts to this version', required=True)
parser.add_argument('-s', '--scripts-directory', help='Directory contains all the upgrade scripts', required=True)
parser.add_argument('-p', '--package', help='The package to upgrade', required=True, choices=['NVMesh-target', 'NVMesh-client'])
parser.add_argument('-m', '--run-method', help='Whether to run the scripts including or not including the specified run-from-version', required=True, choices=['inclusive','exclusive'])
args = parser.parse_args()

upgradeVersionFilePath = '/var/opt/nvmesh/%s_upgrade_version' % args.package.split('-')[1]
isInclusive = True if args.run_method == 'inclusive' else False


def getVRPartsObj(versionRelease):
	parts = versionRelease.split('-')
	res = {
		'version': parts[0],
		'release': parts[1]
	}

	return res


def compareVersionRelease(vr1, vr2):
	vr1PartsObj = getVRPartsObj(vr1)
	vr2PartsObj = getVRPartsObj(vr2)

	versionCompareResult = compareV(vr1PartsObj['version'], vr2PartsObj['version'])

	if versionCompareResult == 0:
		return compareV(vr1PartsObj['release'], vr2PartsObj['release'])
	else:
		return versionCompareResult


def compareV(v1, v2):
	# easy comparison to see if versions are identical
	if v1 == v2:
		return 0

	v1Idx = 0
	v2Idx = 0

	# loop through each version segment of str1 and str2 and compare them
	while v1Idx < len(v1) or v2Idx < len(v2):
		while v1Idx < len(v1) and not v1[v1Idx].isalnum() and v1[v1Idx] != '~' and v1[v1Idx] != '^':
			v1Idx += 1
		while v2Idx < len(v2) and not v2[v2Idx].isalnum() and v2[v2Idx] != '~' and v2[v2Idx] != '^':
			v2Idx += 1


		# handle the tilde separator, it sorts before everything else
		if (v1Idx < len(v1) and v1[v1Idx] == '~') or (v2Idx < len(v2) and v2[v2Idx] == '~'):
			if v1[v1Idx] != '~':
				return 1
			if v2[v2Idx] != '~':
				return -1
			v1Idx += 1
			v2Idx += 1
			continue

		# Handle caret separator. Concept is the same as tilde,
		# except that if one of the strings ends (base version),
		# the other is considered as higher version.
		if (v1Idx < len(v1) and v1[v1Idx] == '^') or (v2Idx < len(v2) and v2[v2Idx] == '^'):
			if v1Idx == len(v1):
				return -1
			if v2Idx == len(v2):
				return 1
			if v1[v1Idx] != '^':
				return 1
			if v2[v2Idx] != '^':
				return -1
			v1Idx += 1
			v2Idx += 1
			continue

		# If we ran to the end of either, we are finished with the loop
		if v1Idx == len(v1) or v2Idx == len(v2):
			break

		str1Idx = v1Idx
		str2Idx = v2Idx

		# grab first completely alpha or completely numeric segment
		# leave v1Idx and v2Idx pointing to the start of the alpha or numeric
		# segment and walk str1Idx and str2Idx to end of segment
		if v1[str1Idx].isdigit():
			while str1Idx < len(v1) and v1[str1Idx].isdigit():
				str1Idx += 1
			while str2Idx < len(v2) and v2[str2Idx].isdigit():
				str2Idx += 1
			isNum = True
		else:
			while str1Idx < len(v1) and v1[str1Idx].isalpha():
				str1Idx += 1
			while str2Idx < len(v2) and v2[str2Idx].isalpha():
				str2Idx += 1
			isNum = False

		# this cannot happen, as we previously tested to make sure that
		# the first string has a non-null segment
		if v1Idx == str1Idx:
			return -1	# arbitrary

		# take care of the case where the two version segments are
		# different types: one numeric, the other alpha (i.e. empty)
		# numeric segments are always newer than alpha segments
		# XXX See patch #60884 (and details) from bugzilla #50977.
		if v2Idx == str2Idx:
			return 1 if isNum else -1

		v1SegStr = v1[ v1Idx : str1Idx ]
		v2SegStr = v2[ v2Idx : str2Idx ]

		if isNum:
			v1SegStr = int(v1SegStr)
			v2SegStr = int(v2SegStr)

		# will return which one is greater - even if the two
		# segments are alpha or if they are numeric.  don't return
		# if they are equal because there might be more segments to
		# compare
		if v1SegStr != v2SegStr:
			return -1 if v1SegStr < v2SegStr else 1

		v1Idx = str1Idx
		v2Idx = str2Idx


	# this catches the case where all numeric and alpha segments have
	# compared identically but the segment separating characters were
	# different
	if v1Idx == len(v1) and v2Idx == len(v2):
		return 0

	# whichever version still has characters left over wins */
	if v1Idx == len(v1):
		return -1
	else:
		return 1


def isInRange(vr, minVR, maxVR, isInclusive=False):
	if isInclusive and vr == minVR:
		return True

	return compareVersionRelease(vr, minVR) == 1 and compareVersionRelease(vr, maxVR) <= 0


if not args.scripts_directory or not args.run_from_version or not args.run_to_version:
	sys.exit("Missing arguments: --scripts-directory, --run-from-version and --run-to-version")

upgradeScripts = []

for (dirpath, dirnames, filenames) in walk(args.scripts_directory):
	upgradeScripts.extend(filenames)
	break

scriptsToRun = [ script for script in upgradeScripts if isInRange(script, args.run_from_version, args.run_to_version, isInclusive) ]
scriptsToRun.sort(key=cmp_to_key(compareVersionRelease))

print("Going to run the following %s upgrade scripts: %s" % (args.package, scriptsToRun))

for script in scriptsToRun:
	try:
		retval = call(path.join(args.scripts_directory, script), shell=True)

		if retval:
			raise Exception
		else:
			# write the succeeded upgrade script version to the upgradeVersionFile
			f = open(upgradeVersionFilePath, 'w')
			f.write(script)
			f.close()
	except Exception as e:
		msg = "Something bad happened while trying to run %s upgrade script: %s" % (args.package, script)
		sys.exit(msg)
