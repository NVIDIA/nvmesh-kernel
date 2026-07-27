#!/usr/bin/env python3

import sys
import json

def print_pretty(obj, indent=0):
    prefix = '\t' * indent

    if isinstance(obj, dict):
        if all(not isinstance(v, (dict, list)) for v in obj.values()):
            items = [f"{k}: {v}" for k, v in obj.items()]
            print(f"{prefix}{', '.join(items)}")
        else:
            for k, v in obj.items():
                if isinstance(v, (dict, list)):
                    print(f"{prefix}{k}:")
                    print_pretty(v, indent + 1)
                else:
                    print(f"{prefix}{k}: {v}")

    elif isinstance(obj, list):
        for item in obj:
            print_pretty(item, indent)

    else:
        print(f"{prefix}{obj}")

if __name__ == "__main__":
    try:
        data = json.load(sys.stdin)
        print_pretty(data)
    except json.JSONDecodeError as e:
        print(f"Invalid JSON: {e}", file=sys.stderr)
        sys.exit(1)
