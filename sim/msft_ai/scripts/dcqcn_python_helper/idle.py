import os, csv
from collections import defaultdict
from .constants import DCQCN_BASE_KNOBS, DCQCN_DEFAULTS

def knob_value(name, trial_knobs):
    if name in trial_knobs:
        try: return float(trial_knobs[name])
        except: pass
    if name in os.environ:
        try: return float(os.environ[name])
        except: pass
    if name in DCQCN_BASE_KNOBS:
        try: return float(DCQCN_BASE_KNOBS[name])
        except: pass
    return float(DCQCN_DEFAULTS.get(name, 0.0))

def compute_idle_fraction(cnp_csv, sim_dur_us, trial_knobs):
    epoch_us = knob_value("DCQCN_EPOCH_US", trial_knobs)
    cooldown_epochs = int(round(knob_value("DCQCN_HI_COOLDOWN", trial_knobs)))
    cooldown_us = max(0.0, epoch_us * cooldown_epochs)
    if sim_dur_us <= 0 or not os.path.exists(cnp_csv) or cooldown_us <= 0:
        return {}
    intervals = defaultdict(list)
    try:
        with open(cnp_csv, newline="") as f:
            rdr = csv.DictReader(f)
            if not rdr.fieldnames or 'flow' not in rdr.fieldnames or 'ts_us' not in rdr.fieldnames or 'event' not in rdr.fieldnames:
                return {}
            for row in rdr:
                try:
                    if row['event'].strip().upper() not in ("RCVD","RECV","RECEIVED"):
                        continue
                    fid = int(row['flow']); t  = float(row['ts_us'])
                except:
                    continue
                s = t; e = min(sim_dur_us, t + cooldown_us)
                if s < e: intervals[fid].append((s, e))
    except Exception:
        return {}
    idle_frac = {}
    for fid, ivals in intervals.items():
        if not ivals: continue
        ivals.sort()
        merged = []
        cs, ce = ivals[0]
        for s, e in ivals[1:]:
            if s <= ce: ce = max(ce, e)
            else: merged.append((cs, ce)); cs, ce = s, e
        merged.append((cs, ce))
        total_idle = sum(e - s for s, e in merged)
        idle_frac[fid] = max(0.0, min(1.0, total_idle / sim_dur_us))
    return idle_frac

def compute_idle_windows(cnp_csv, sim_dur_us, trial_knobs):
    epoch_us = knob_value("DCQCN_EPOCH_US", trial_knobs)
    cooldown_epochs = int(round(knob_value("DCQCN_HI_COOLDOWN", trial_knobs)))
    cooldown_us = max(0.0, epoch_us * cooldown_epochs)
    out = defaultdict(list)
    if sim_dur_us <= 0 or not os.path.exists(cnp_csv) or cooldown_us <= 0:
        return out
    try:
        with open(cnp_csv, newline="") as f:
            rdr = csv.DictReader(f)
            if not rdr.fieldnames or 'flow' not in rdr.fieldnames or 'ts_us' not in rdr.fieldnames or 'event' not in rdr.fieldnames:
                return out
            tmp = defaultdict(list)
            for row in rdr:
                try:
                    if row['event'].strip().upper() not in ("RCVD","RECV","RECEIVED"): continue
                    fid = int(row['flow']); t = float(row['ts_us'])
                except:
                    continue
                s = t; e = min(sim_dur_us, t + cooldown_us)
                if s < e: tmp[fid].append((s, e))
            for fid, ivals in tmp.items():
                if not ivals: continue
                ivals.sort()
                merged = []
                cs, ce = ivals[0]
                for s, e in ivals[1:]:
                    if s <= ce: ce = max(ce, e)
                    else: merged.append((cs, ce)); cs, ce = s, e
                merged.append((cs, ce))
                out[fid] = merged
    except Exception:
        return out
    return out

def build_effective_sender_rate_from_idle(target_samples, idle_windows_by_flow):
    eff = {}
    for fid, samples in target_samples.items():
        try: fid_int = int(fid)
        except Exception: fid_int = fid
        windows = sorted(idle_windows_by_flow.get(fid_int, []))
        if not windows:
            eff[fid] = samples; continue
        out = []
        wi = 0
        for (t, r) in samples:
            while wi < len(windows) and t > windows[wi][1]:
                wi += 1
            if wi < len(windows) and windows[wi][0] <= t <= windows[wi][1]:
                out.append((t, 0.0))
            else:
                out.append((t, r))
        eff[fid] = out
    return eff
