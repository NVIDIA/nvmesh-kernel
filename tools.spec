# -*- mode: python -*-

import os
import sys
import json
import yaml

default_analysis_args = {
    'noarchive': False,
}

# TODO: Take list of files
confpath = os.getenv('TOOLS_CONF', './tools.yaml')
try:
    with open(confpath, 'rb') as conffile:
        toolconf = yaml.safe_load(conffile.read())
except Exception as e:
    print(f'Failed to load {confpath}. {repr(e)}')
    exit(1)

analyses = {}
for tool, tconf in toolconf['tools'].items():
    a_kwargs = {}
    a_kwargs.update(default_analysis_args)
    a_kwargs.update(toolconf.get('analysis', {}))
    a_kwargs.update(tconf.get('analysis', {}))
    print(f'source: {tconf["source"]}, kwargs: {a_kwargs}')
    analyses[tool] = Analysis([tconf['source']], **a_kwargs)

pyzs = {tool: PYZ(a.pure, a.zipped_data) for tool, a in analyses.items()}
exes = {tool: EXE(pyzs[tool],
        analyses[tool].scripts,
        [],
        exclude_binaries=True,
        name=os.path.join('dist', tool),
        debug=False,
        strip=False,
        upx=True,
        runtime_tmpdir=None,
        console=True ) for tool in analyses}

coll = COLLECT(
        *list(exes.values()),
        *[a.binaries for a in analyses.values()],
        *[a.datas for a in analyses.values()],
        upx=True,
        upx_exclude=[],
        name='pytools',
)
