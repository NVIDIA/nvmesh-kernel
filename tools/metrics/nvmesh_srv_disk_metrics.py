#!/usr/bin/env python3
import sys, json, time

"""
Usage:
./pager.py -t 'now-1m' -l nvmeibs_trace_metrics -f 'func=disk_periodic_timer_work_func' | python3 ./nvmesh_srv_disk_metrics.py
Output format:
DATE         TIME            DISK                 DEV   VERB            BIN            IOPS       MBps    LAT(us)
-----------------------------------------------------------------------------------------------------------------
2026-02-19   11:21:02.772150 S3HCNX0K800441.1     0     READ            4K        113724.74     444.24      86.30
2026-02-19   11:21:02.772150 S3HCNX0K800122.1     2     GEN_RX          256K           0.00       0.00       0.00
2026-02-19   11:21:02.772150 S3HCNX0K701241.1     1     GEN_TX          256K           0.00       0.00       0.00   
2026-02-19   11:21:02.772150 S3HCNX0K800441.1     0     GEN_TX          256K           0.00       0.00       0.00
2026-02-19   11:21:02.772150 S3HCNX0K800122.1     2     GEN_TX          256K           0.00       0.00       0.00
2026-02-19   11:21:02.772150 S3HCNX0K701241.1     1     GEN_TX          256K           0.00       0.00       0.00
2026-02-19   11:21:02.772150 S3HCNX0K800441.1     0     GEN_TX          256K           0.00       0.00       0.00
2026-02-19   11:21:02.772150 S3HCNX0K800122.1     2     GEN_TX          256K           0.00       0.00       0.00
2026-02-19   11:21:02.772150 S3HCNX0K701241.1     1     GEN_TX          256K           0.00       0.00       0.00
2026-02-19   11:21:02.772150 S3HCNX0K800441.1     0     GEN_TX          256K           0.00       0.00       0.00
2026-02-19   11:21:02.772150 S3HCNX0K800122.1     2     GEN_TX          256K           0.00       0.00       0.00
"""

def trace_json():
    # State: key -> (last_ns, last_ops, last_size, last_lat)
    state = {}
    
    # u64 wraparound constant: 2^64
    U64_ROLLOVER = 1 << 64
    
    # Format: Date(12), Time(15), Disk(20), Dev(5), Verb(15), Bin(8), Metrics(10 each)
    fmt = "{:<12} {:<15} {:<20} {:<5} {:<15} {:<8} {:>10} {:>10} {:>10}"
    header = fmt.format('DATE', 'TIME', 'DISK', 'DEV', 'VERB', 'BIN', 'IOPS', 'MBps', 'LAT(us)')
    
    print(header)
    print("-" * len(header))

    for line in sys.stdin:
        try:
            js = json.loads(line)
            
            # Extract ID fields
            disk = js["DISK_ID_STR"]
            dev  = js["SEQ"]
            verb = js["IO_STAT_VERB"]
            bn   = js["STR"]
            ns   = js["nanoseconds"]
            
            # Extract Metrics (as u64 counters)
            c_ops  = int(js["IO_STAT_COUNTER_OPS"])
            c_size = int(js["IO_STAT_COUNTER_SIZE"])
            c_lat  = int(js["IO_STAT_COUNTER_LAT_100NS"])

            key = (disk, dev, bn, verb)
            curr_ts = ns / 1e9

            if key in state:
                prev_ts, p_ops, p_size, p_lat = state[key]
                dt = curr_ts - prev_ts
                
                if dt > 0:
                    # Logic: Calculate deltas and handle u64 wraparound
                    d_ops = c_ops - p_ops
                    if d_ops < 0: d_ops += U64_ROLLOVER
                    
                    d_sz = c_size - p_size
                    if d_sz < 0: d_sz += U64_ROLLOVER
                    
                    d_lat = c_lat - p_lat
                    if d_lat < 0: d_lat += U64_ROLLOVER

                    # Performance Math
                    iops = d_ops / dt
                    mbps = (d_sz / 1048576) / dt
                    # (Delta 100ns units / Delta Ops) / 10 = microseconds
                    avg_lat = (d_lat / d_ops / 10) if d_ops > 0 else 0.0

                    # Timestamp formatting from nanoseconds
                    tm_struct = time.localtime(curr_ts)
                    ts_str = time.strftime('%H:%M:%S', tm_struct) + f".{str(ns)[-9:-3]}"
                    dt_str = time.strftime('%Y-%m-%d', tm_struct)

                    print(fmt.format(
                        dt_str, ts_str, disk, dev, verb, bn,
                        f"{iops:.2f}", f"{mbps:.2f}", f"{avg_lat:.2f}"
                    ))

            # Update state for next interval
            state[key] = (curr_ts, c_ops, c_size, c_lat)
            sys.stdout.flush()

        except (json.JSONDecodeError, KeyError, ValueError):
            continue
        except KeyboardInterrupt:
            break

if __name__ == "__main__":
    trace_json()