<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
-->

# `the-calculator.py` — LBA translation helper

Interactive Python shell for translating addresses across **volume → chunk → RAID → segment → disk** for NVMesh block volumes loaded from **`/proc`** or from **UM trace JSONL** exports.

**Script:** [`the-calculator.py`](the-calculator.py)

## Dependencies

- **Python 3**
- **IPython** (`pip install ipython`) — required because `main()` calls `IPython.embed()`.

## Loading topology

| Source | Flags | Parsed files |
|--------|--------|----------------|
| Proc volume dirs | `--procdir DIR [DIR ...]` | Each dir: `status`, `status.json` |
| Recursive proc scan | `--procdir ROOT -r` | Subdirectories under `ROOT` |
| UM volume dump | `--umvolume PATH` | JSONL (`VOLUME_PTR`, `CHUNK_PTR`, `PRAID_PTR`, `SEGMENT_PTR`) |

After load, the script prints the usage example and starts **IPython**.

## Volume keys: `str` vs `bytes`

Proc and UM loaders register volumes under **`bytes`** names (e.g. `b'vol-3wm'`). Use:

```python
VLBA(b'vol-3wm', lba)
# or
VLBA(volume(b'vol-3wm'), lba)
```

Using a bare bytes name without resolving through `volume()` was a bug (fixed): `VLBA` now accepts **`bytes`** and looks up the `Volume` object like **`str`**.

## Address chain

| Type | Role |
|------|------|
| **VLBA** | Volume linear LBA |
| **CLBA** | Offset within chunk |
| **RLBA** | Offset within RAID set |
| **SLBA** | Offset on one segment (one mirror leg) |
| **DLBA** | LBA on physical disk |

Mirror helpers: **`mirror_slbas`**, **`mirror_dlbas`**, **`lock_owner`** (from segment `lmap` `O<n>`), **`blockset`** / **`blockset_start`**, **`role`**.

Use **`pretty_print_volumes()`** after load to dump chunks, raids, and segments.

## CLI

```bash
python3 tools/the-calculator.py --procdir /proc/nvmeibc/volumes/<volume-id>
python3 tools/the-calculator.py --procdir /proc/nvmeibc/volumes -r
python3 tools/the-calculator.py --umvolume ./volume_conf.jsonl
```

---

## Example: **2-way mirror** volume `vol-2wm`

### `pretty_print_volumes()` (abbrev.)

```text
b'vol-2wm'
  chunk(0) [0,976383) size=32 width=1
    raid (0) 1+1 lid=0x10 ver=257
       seg (0) disk S795NG0WB00689.1     [  2877184,  3853567) uuid=e0d2ce51 lm=O0,C1
       seg (1) disk S3HCNX0K500634.1     [  1509632,  2486015) uuid=e0d31c70 lm=O1,C0
```

---

## Example: **3-way mirror** volume `vol-3wm`

### `pretty_print_volumes()` (abbrev.)

```text
b'vol-3wm'
  chunk(0) [0,976383) size=32 width=1
    raid (0) 1+2 lid=0x250 ver=272
       seg (0) disk S795NG0WB00689.1     [  2877184,  3853567) uuid=260f3991 lm=O0,C2,C1
       seg (1) disk S3HCNX0K500634.1     [  1509632,  2486015) uuid=260f87b0 lm=O1,C0,C2
       seg (2) disk 20C0A042TVSE.1       [  1509632,  2486015) uuid=260faec0 lm=O2,C1,C0
  chunk(1) [976384,1464575) size=32 width=1
    raid (0) 1+2 lid=0x40 ver=257
       seg (0) disk S795NG0WB00689.1     [  4829952,  5318143) uuid=d0335d50 lm=O0,C2,C1
       seg (1) disk S3HCNX0K600300.1     [  1509632,  1997823) uuid=d033d280 lm=O1,C0,C2
       seg (2) disk S3HCNX0K500634.1     [  2486016,  2974207) uuid=d033f990 lm=O2,C1,C0
```

Three segments per RAID (`1+2` = one data slice + two mirror replicas), `stripe_width=1`.

### VLBA `126` — captured IPython output

```python
lba = VLBA(b'vol-3wm', 126)
print(lba)
print(lba.chunk, lba.clba, lba.rlba)
print('seg:', lba.seg, 'role:', lba.role, 'blockset:', lba.blockset)
for sl in lba.mirror_slbas:
    print(sl, '->', sl.dlba)
print('lock_owner:', lba.lock_owner)
```

```text
VLBA(volume(b'vol-3wm'), 126)
chunk(b'vol-3wm', 0) CLBA(chunk(b'vol-3wm', 0), 126) RLBA(raid(b'vol-3wm', 0, 0), 126)
seg: seg(b'vol-3wm', 0, 0, 1) role: 0 blockset: 3
SLBA(seg(b'vol-3wm', 0, 0, 0), 126) -> DLBA("S795NG0WB00689.1", 2877310)
SLBA(seg(b'vol-3wm', 0, 0, 1), 126) -> DLBA("S3HCNX0K500634.1", 1509758)
SLBA(seg(b'vol-3wm', 0, 0, 2), 126) -> DLBA("20C0A042TVSE.1", 1509758)
lock_owner: seg(b'vol-3wm', 0, 0, 1)
```

**Reading this:**

- **VLBA 126** falls in **chunk 0** → **CLBA 126**, **RLBA 126** on **raid (0,0)**.
- **Primary segment** for this RLBA is **seg 1** (`S3HCNX0K500634.1`); **`role: 0`** means this segment is the slice “owner” role for this stripe (see `SLBA.role` in the script).
- **`blockset: 3`** — blockset index derived from RLBA and slice size (32 blocks per slice × 32 slices per blockset logic in `RLBA.blockset`).
- **`mirror_slbas`** lists all **three** mirror legs at the same segment-LBA **126**; **DLBA** = each segment’s `dlba_start + 126` (e.g. `2877184 + 126 = 2877310` on `S795NG0WB00689.1`).
- **`lock_owner`** is **seg 1**, consistent with that segment’s **`lm=O1,...`** (owner index **1** for lock placement on this topology).

---

## Related

- Mirror concepts (locks, blocksets, reads/writes): [`clnt/block/documentation/mirror.md`](../clnt/block/documentation/mirror.md)
