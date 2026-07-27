#!/usr/bin/env python3
# PYTHON_ARGCOMPLETE_OK
import argparse
import json
import subprocess
import os
import sys
from collections import OrderedDict


def exec_cmd(cmd):
    if isinstance(cmd, str):
        cmd = ["bash", "-c", cmd]
    assert isinstance(cmd, (list, tuple))
    prc = subprocess.run(cmd, stderr=subprocess.PIPE, stdout=subprocess.PIPE)
    return prc.returncode, prc.stdout, prc.stderr


def argcomplete_if_possible(parser):
    """Try initialize argcomplete if available"""
    from importlib import import_module

    try:
        argcomplete = import_module("argcomplete")
    except:
        pass  # Too bad, but we can live with that
    else:
        argcomplete.autocomplete(parser)

    return parser


def create_argparser():
    """Prepare argparser"""
    parser = argparse.ArgumentParser()

    parser.add_argument(
        "project_root",
        action="store",
        nargs="?",
        type=str,
        help="NVMesh project root",
        default=os.path.abspath(
            os.path.join(os.path.dirname(os.path.realpath(__file__)), "..")
        ),
    )

    return argcomplete_if_possible(parser)


def is_token_used(project_root: str, token: str) -> bool:
	rv, _, _ = exec_cmd(f"cd '{project_root}' ; git grep '{token}[^A-Z]' ':!**/dictionary.json'")
	if rv == 0:
		return True
	elif rv == 1:
		return False
	assert False, f"Unexpect rv by git grep: {rv}"


def main():
	args = create_argparser().parse_args()
	setattr(
		args,
		"dictionary_file",
		os.path.join(os.path.join(args.project_root, "tools"), "dictionary.json"),
	)

	with open(args.dictionary_file, "r") as f:
		d = json.JSONDecoder(object_pairs_hook=OrderedDict).decode(f.read())

	total = len(d)
	unused_tokens = list()
	#print(args);
	if not os.path.isdir(f"{args.project_root}/.git"):
		print("Run this util from git repository (development laptop), not on nvmesh machine!")
		print(f"Cant find dir: {args.project_root}/.git")
		return -1;
	print(f"Analyzing dictionary at {args.dictionary_file}, {total}[entires]. It may take some time...")

	for i, tk in enumerate(d):
		#print(f"{i}||{tk}||");
		sys.stdout.write(f"\rUnused tokens: {len(unused_tokens):4}, Done: {i*100 // total:4}%")
		if not is_token_used(args.project_root, tk):
			unused_tokens.append(tk)
	print("Saving")
	d_ = {k: v for k, v in d.items() if not k in unused_tokens}
	with open(args.dictionary_file, "w") as f:
		json.dump(d_, f, indent=4)
	print("Stay safe dont use drugs")
	return 0


if __name__ == "__main__":
    sys.exit(main())
