import argparse
import json
import math
import multiprocessing
import multiprocessing.pool
import os
import time
from dataclasses import dataclass
from enum import Enum
from typing import List, Optional, Tuple
from random import seed, shuffle, sample
from pathlib import Path

import numpy as np

from common import (BYTES_TO_BITS, HTSIM_DATACENTER_ROOT, MS_TO_S, NS_TO_PS, NS_TO_US, PROJECT_ROOT,
                    S_TO_US, SCRIPTS_PATH, TRAFFIC_GEN_CDF_FILES_ROOT,
                    TRAFFIC_GEN_ROOT, US_TO_NS, Gb_TO_b, Gb_TO_Mb,
                    PartialParams, try_except_wrapper)

CM_DIR = os.path.join(SCRIPTS_PATH, "connection_matrices")
TOPO_DIR = os.path.join(SCRIPTS_PATH, "topologies")
DEFAULT_EXP_ROOT = os.path.join(PROJECT_ROOT, "experiments")

DEFAULT_MSS_BYTES = 4096
DEFAULT_HEADER_BYTES = 64
DEFAULT_ACK_WIRE_BYTES = 64
DEFAULT_MTU_BYTES = DEFAULT_MSS_BYTES + DEFAULT_HEADER_BYTES
DEFAULT_ONE_WAY_HOPS = 6
DEFAULT_LINK_DELAY_US = 1

DEFAULT_SMARTT_FASTI_SCALING_FACTOR: float = 1    # 1 MTU per ACK
DEFAULT_SMARTT_PI_SCALING_FACTOR: float = 2       # 1/cwnd MTU per ACK
DEFAULT_SMARTT_FI_SCALING_FACTOR: float = 0.02    # 1/cwnd MTU per ACK
DEFAULT_SMARTT_FD_SCALING_FACTOR: float = 0.5     # 0.5 MTU per ACK
DEFAULT_SMARTT_MD_SCALING_FACTOR: float = 1       # 0.5 MTU per ACK

SIGCOMM24_SMARTT_FASTI_SCALING_FACTOR: float = 2  # 2 MTU per ACK
SIGCOMM24_SMARTT_PI_SCALING_FACTOR: float = 2     # 1/cwnd MTU per ACK
SIGCOMM24_SMARTT_FI_SCALING_FACTOR: float = 0.006 # 0.25/cwnd MTU per ACK
SIGCOMM24_SMARTT_FD_SCALING_FACTOR: float = 0.8   # 0.8 MTU per ACK
SIGCOMM24_SMARTT_MD_SCALING_FACTOR: float = 2     # 1 MTU per ACK

SIM_SEED = 42
TRAFFIC_GEN_SEED = 42
ALL_SCHEMES = [
    PartialParams(cc_algo="smartt", smartt_wtd=True),
    PartialParams(cc_algo="smartt"),
    PartialParams(cc_algo="smartt_ecn_aimd"),
    PartialParams(cc_algo="smartt_ecn_aifd"),
    PartialParams(cc_algo="smartt_ecn_fimd"),
    PartialParams(cc_algo="smartt_ecn_fifd"),
    PartialParams(cc_algo="smartt_rtt"),
    PartialParams(cc_algo="nscc"),
    PartialParams(cc_algo="uec_mprdma"),
    PartialParams(cc_algo="rccc"),
    PartialParams(cc_algo="eqds"),
]

class OneWayExperimentMode(Enum):
    pod = "oneWayPod"
    tor = "oneWayTor"

    def __str__(self):
        return self.value

class dataCollectionConfig(Enum):
    incast = "incast"
    all = "all"
    permutation = "permutation"
    oneWay = "oneWay"
    rss_params = "rss_params"
    frozen_threshold = "frozen_threshold"
    default = "default"

    @property
    def corresponding_file(self):
        if self.value == "incast":
            return "collect_default.json"
        if self.value == "all":
            return "collect_all_metrics.json"
        if self.value == "permutation":
            return "collect_default.json"
        if self.value == "oneWay":
            return "collect_permutation.json"
        if self.value == "rss_params":
            return "collect_rss.json"
        if self.value == "frozen_threshold":
            return "collect_rss.json"
        if self.value == "default":
            return "collect_default.json"


def get_end_time_us_helper(msg_size_bytes: int, link_speed_gbps: int, effective_oversub: int):
    MIN_SIM_US = 10000
    SIM_HEADROOM = 100
    msg_size_bits = msg_size_bytes * 8
    tot_msg_bits = msg_size_bits * effective_oversub
    link_speed_bps = link_speed_gbps * Gb_TO_b
    end_time_us = (tot_msg_bits / link_speed_bps) * SIM_HEADROOM * S_TO_US
    end_time_us = max(MIN_SIM_US, end_time_us)
    return int(end_time_us)


def get_tx_delay_ns(wire_bytes: int, effective_link_speed_gbps: float):
    return wire_bytes * BYTES_TO_BITS / effective_link_speed_gbps


def get_base_rtt_ns(
    link_speed_gbps: float,
    one_way_hops: int = DEFAULT_ONE_WAY_HOPS,
    link_delay_ns: int = int(DEFAULT_LINK_DELAY_US * US_TO_NS),
    mtu_bytes: int = DEFAULT_MTU_BYTES,
    ack_wire_bytes: int = DEFAULT_ACK_WIRE_BYTES,
):
    data_tx_delay_ns = one_way_hops * get_tx_delay_ns(mtu_bytes, link_speed_gbps)
    ack_tx_delay_ns = one_way_hops * get_tx_delay_ns(ack_wire_bytes, link_speed_gbps)
    rtprop_ns = 2 * one_way_hops * link_delay_ns
    return rtprop_ns + data_tx_delay_ns + ack_tx_delay_ns


def worker(cmd: str):
    start = time.time()
    print(cmd)
    print("")
    os.system(cmd)
    end = time.time()
    return cmd, end-start


@dataclass
class Params:
    # Frequently changed
    link_speed_gbps: int
    nodes: int
    over_sub: int
    msg_size_bytes: int
    matrix: str
    cc_algo: str  # scheme param
    load_balancing_algo: str = "reps"  # scheme param
    incast_degree: Optional[int] = None
    load: Optional[float] = None
    workl_duration_ms: Optional[float] = None
    _cm_file: Optional[str] = None
    _end_time_us: Optional[int] = None
    collect_all_metrics: bool = False
    data_collection_config: dataCollectionConfig = dataCollectionConfig.default
    disable_trim: bool = False
    low_priority_trim: bool = False
    no_droping_low_header: bool = False
    switch_random_drop_prob: float = 0.

    smartt_fasti_scaling_factor: float = DEFAULT_SMARTT_FASTI_SCALING_FACTOR
    smartt_pi_scaling_factor: float = DEFAULT_SMARTT_PI_SCALING_FACTOR
    smartt_fi_scaling_factor: float = DEFAULT_SMARTT_FI_SCALING_FACTOR
    smartt_fd_scaling_factor: float = DEFAULT_SMARTT_FD_SCALING_FACTOR
    smartt_md_scaling_factor: float = DEFAULT_SMARTT_MD_SCALING_FACTOR
    smartt_wtd: bool = False  # By default, wait to decrease is disabled

    rss_number_of_subflows: int = 16
    rss_update_interval: int = 0.1 # in microseconds
    rss_worse_entropy_metric: str = "mean_rtt"
    rss_max_number_of_skipped_rounds: int = 0
    rss_frozen_threshold: float = 0 # fraction of the best path metric under which we don't reroute
    rss_period_jitter: int = 25 # As a percentage of rss_update_interval 
    uss_number_of_subflows: int = 16
    enable_sleek: bool = False
    enable_precise_fast_loss_recovery: int = -1 # = pflr scheme id if >= 0
    pflr_proactive_probe: int = -1
    pflr_proactive_rtx_probe: bool = False
    pflr4_pkt_per_slot: int = 16
    pflr4_use_ev_recovery: bool = False

    # Rarely changed
    queue_size_bdp: float = 1
    ecn_kmin_bdp: float = 1/5
    ecn_kmax_bdp: float = 4/5
    init_cwnd_bdp: float = 1.25
    mss_bytes: int = DEFAULT_MSS_BYTES
    sim_seed: int = SIM_SEED
    traffic_gen_seed: int = TRAFFIC_GEN_SEED
    topology_tiers: int = 3
    asymmetry: str = None
    ecmp_nodes: int = 0

    # Never changed
    topo: str = "fat_tree"
    link_delay_ns: int = int(DEFAULT_LINK_DELAY_US * US_TO_NS)
    header_bytes: int = DEFAULT_HEADER_BYTES
    ack_wire_bytes: int = DEFAULT_ACK_WIRE_BYTES
    one_way_hops: int = DEFAULT_ONE_WAY_HOPS
    extra_start_time: int = 0

    @property
    def link_speed_mbps(self):
        return self.link_speed_gbps * Gb_TO_Mb

    @property
    def mtu_bytes(self):
        return self.mss_bytes + self.header_bytes

    def get_tx_delay_ns(self, wire_bytes: int, effective_gbps: Optional[float] = None):
        if effective_gbps is None:
            effective_gbps = self.link_speed_gbps
        return get_tx_delay_ns(wire_bytes, effective_gbps)

    @property
    def base_rtt_ns(self):
        return get_base_rtt_ns(
            self.link_speed_gbps,
            self.one_way_hops,
            self.link_delay_ns,
            self.mtu_bytes,
            self.ack_wire_bytes,
        )

    @property
    def bdp_wire_bytes(self):
        return self.base_rtt_ns * self.link_speed_gbps / BYTES_TO_BITS

    @property
    def bdp_pkts(self):
        return self.bdp_wire_bytes / self.mtu_bytes

    @property
    def bdp_transport_bytes(self):
        return self.bdp_wire_bytes * self.mss_bytes / self.mtu_bytes

    @property
    def queue_size_pkts(self):
        return math.ceil(self.bdp_pkts * self.queue_size_bdp)

    @property
    def ecn_kmin_pkts(self):
        return math.ceil(self.bdp_pkts * self.ecn_kmin_bdp)

    @property
    def ecn_kmax_pkts(self):
        return math.ceil(self.bdp_pkts * self.ecn_kmax_bdp)

    @property
    def init_cwnd_pkts(self):
        return math.ceil(self.bdp_pkts * self.init_cwnd_bdp)

    @property
    def topo_file(self):
        return f"topo={self.topo}:over_sub={self.over_sub}:nodes={self.nodes}:link_speed_gbps={self.link_speed_gbps}"\
        f"{f':tiers={self.topology_tiers}' if self.topology_tiers != 3 else ''}"\
        f"{f':asymmetry={self.asymmetry}' if self.asymmetry not in ['sym', 'bg'] else ''}.topo"

    @property
    def exp_tag(self):
        # Runs with same tag are stored in the same directory
        exp_tag = (
            f"topo={self.topo}:over_sub={self.over_sub}:"
            f"nodes={self.nodes}:link_speed_gbps={self.link_speed_gbps}:"
            f"matrix={self.matrix}:msg_size_bytes={self.msg_size_bytes}:"
            f"{f'asymmetry={self.asymmetry}' if self.asymmetry is not None else 'asymmetry=sym'}"
        )
        if self.incast_degree:
            exp_tag += f":incast_degree={self.incast_degree}"
        if self.load:
            assert self.workl_duration_ms is not None
            exp_tag += f":load={self.load}:workl_duration_ms={self.workl_duration_ms}"
        return exp_tag

    @property
    def scheme_str(self):
        """
        Used for legend in plots.
        """
        if self.rss_frozen_threshold != 0:
            assert self.load_balancing_algo == "rss"
            ret = f"{self.cc_algo}+FrozenRSS:ft={self.rss_frozen_threshold}"
        elif self.rss_max_number_of_skipped_rounds != 0:
            assert self.load_balancing_algo == "rss"
            ret = f"{self.cc_algo}+SkippingRSS:sr={self.rss_max_number_of_skipped_rounds}"
        else:
            ret = f"{self.cc_algo}+{self.load_balancing_algo}"
        if self.cc_algo.startswith("smartt"):
            if self.smartt_wtd:
                ret += f"+wtd"
        # if self.load_balancing_algo == "rss":
        #     ret += f"({self.rss_worse_entropy_metric}-sf={self.rss_number_of_subflows}_ui={self.rss_update_interval})"
        # if self.load_balancing_algo == "uss":
        #     ret += f"(sf={self.uss_number_of_subflows})"
        if self.disable_trim:
            if self.low_priority_trim:
                ret += f"+low_trim"
            else:
                ret += f"+no_trim"
        else:
            ret += f"+trim"
        if self.enable_sleek:
            ret += f"+sleek"
        if (self.enable_precise_fast_loss_recovery>=0):
            # ret += f"+pfld_{self.enable_precise_fast_loss_recovery}"
            ret += f"+pfld{self.enable_precise_fast_loss_recovery}"
            if (self.enable_precise_fast_loss_recovery == 4):
                ret += f"(m={self.pflr4_pkt_per_slot}_v={1+self.pflr4_use_ev_recovery})"
            if (self.pflr_proactive_probe >= 0):
                ret += f"+probe_{self.pflr_proactive_probe}"
            if (self.pflr_proactive_rtx_probe):
                ret += f"+probe_rtx"
        if self.no_droping_low_header:
            ret += f"+no_header_drop"
        # if (self.switch_random_drop_prob > 0.):
        #     ret += f"+rand_drop_{self.switch_random_drop_prob}"
        return ret

    @property
    def scheme_tag(self):
        """
        Used for creating file names. So uses the convention in README.md.
        """
        if self.rss_frozen_threshold != 0:
            assert self.load_balancing_algo == "rss"
            ret = f"cc={self.cc_algo}:lb=FrozenRSS:ft={self.rss_frozen_threshold}"
        elif self.rss_max_number_of_skipped_rounds != 0:
            assert self.load_balancing_algo == "rss"
            ret = f"cc={self.cc_algo}:lb=SkippingRSS:sr={self.rss_max_number_of_skipped_rounds}"
        else:
            ret = f"cc={self.cc_algo}:lb={self.load_balancing_algo}"
        if self.cc_algo.startswith("smartt"):
            ret += f":wtd={self.smartt_wtd}"     
        if self.load_balancing_algo == "rss":
            ret += f":rss_metric={self.rss_worse_entropy_metric}:sf={self.rss_number_of_subflows}:ui={self.rss_update_interval}"
        if self.load_balancing_algo == "uss":
            ret += f":sf={self.uss_number_of_subflows}"
        if self.disable_trim:
            if self.low_priority_trim:
                ret += f":low_trim"
            else:
                ret += f":no_trim"
        if self.enable_sleek:
            ret += f":sleek"
        if (self.enable_precise_fast_loss_recovery>=0):
            ret += f":pfld_{self.enable_precise_fast_loss_recovery}"
            if (self.enable_precise_fast_loss_recovery == 4):
                ret += f":m={self.pflr4_pkt_per_slot}:v={1+self.pflr4_use_ev_recovery}"
            if (self.pflr_proactive_probe >= 0):
                ret += f":probe_{self.pflr_proactive_probe}"
            if (self.pflr_proactive_rtx_probe):
                ret += f":probe_rtx"
        if self.no_droping_low_header:
            ret += f":no_header_drop"
        if (self.switch_random_drop_prob > 0.):
            ret += f":rand_drop_{self.switch_random_drop_prob}"
        return ret

    def get_cm_file(self):
        if self._cm_file is None:
            self._cm_file = self.generate_and_get_cm_file()
        return self._cm_file

    def generate_and_get_cm_file(self):
        if self.matrix in ["permutation", "all_to_all"]:
            cm_file = (
                f"matrix={self.matrix}:nodes={self.nodes}:"
                f"msg_size_bytes={self.msg_size_bytes}.cm"
            )
            cm_path = os.path.join(CM_DIR, cm_file)
            if not os.path.isfile(cm_path):
                gen_cmd = (
                    f"python {HTSIM_DATACENTER_ROOT}/connection_matrices/gen_permutation.py "
                    f"{cm_path} {self.nodes} {self.nodes} "
                    f"{self.msg_size_bytes} {self.extra_start_time} {self.traffic_gen_seed}"
                )
                print(gen_cmd)
                os.system(gen_cmd)

        elif self.matrix == "incast":
            assert self.incast_degree is not None
            cm_file = (
                f"matrix={self.matrix}:nodes={self.nodes}:"
                f"msg_size_bytes={self.msg_size_bytes}:"
                f"incast_degree={self.incast_degree}.cm"
            )
            cm_path = os.path.join(CM_DIR, cm_file)
            if not os.path.isfile(cm_path):
                gen_cmd = (
                    f"python {HTSIM_DATACENTER_ROOT}/connection_matrices/gen_incast.py "
                    f"{cm_path} {self.nodes} {self.incast_degree} "
                    f"{self.msg_size_bytes} {self.extra_start_time} {self.traffic_gen_seed} "
                    f"0 "  # prefer_remote
                )
                print(gen_cmd)
                os.system(gen_cmd)

        else:
            raise NotImplementedError

        return cm_file

    def get_end_time_us(self):
        if self._end_time_us is None:
            effective_over_sub = self.over_sub
            if self.incast_degree is not None:
                effective_over_sub = effective_over_sub * self.incast_degree
            self._end_time_us = get_end_time_us_helper(self.msg_size_bytes, self.link_speed_gbps, effective_over_sub)
        return self._end_time_us

    @property
    def command(self):
        data_collection_config = f"{SCRIPTS_PATH}/metrics_collection_policies/{self.data_collection_config.corresponding_file}"
        if self.collect_all_metrics:
            data_collection_config = f"{SCRIPTS_PATH}/metrics_collection_policies/collect_all_metrics.json"

        ret = (
            f"{HTSIM_DATACENTER_ROOT}/htsim_uec "
            f"-data_collection_config {data_collection_config} "
            f"-end {self.get_end_time_us()} -seed {self.sim_seed} "
            f"-tm {CM_DIR}/{self.get_cm_file()} "
            f"-topo {TOPO_DIR}/{self.topo_file} "
            f"-q {self.queue_size_pkts} -cwnd {self.init_cwnd_pkts} "
            f"-mtu {self.mtu_bytes} "
            f"-sack_threshold 0 "
            f"-ecn {self.ecn_kmin_pkts} {self.ecn_kmax_pkts} "
            f"-load_balancing_algo {self.load_balancing_algo} "
            f"-linkspeed {self.link_speed_mbps} "
        )

        if self.asymmetry == 'bg':
            ret += f"-background_traffic {self.ecmp_nodes} "

        if self.disable_trim:
            ret += f"-disable_trim "
            if self.low_priority_trim:
                ret += f"-low_priority_trim "

        cc_command_dict = {
            "eqds": (
                f"-force_disable_oversubscribed_cc "
            ),
            "rccc": (
                f""
            ),
            "uec_mprdma": (
                f"-sender_cc_only "
                f"-sender_cc_algo dctcp "
            ),
            "nscc": (
                f"-sender_cc_only "
                f"-sender_cc_algo nscc "
            )
        }
        if self.load_balancing_algo == "rss":
            ret += (
                f"-rss_parameters {self.rss_worse_entropy_metric} {self.rss_number_of_subflows} {self.rss_update_interval} {self.rss_frozen_threshold} {self.rss_max_number_of_skipped_rounds} {self.rss_period_jitter} "
            )
        
        if self.load_balancing_algo == "uss":
            ret += (
                f"-uss_parameters {self.uss_number_of_subflows} "
            )

        if self.enable_sleek:
            ret += f"-sleek "

        if (self.enable_precise_fast_loss_recovery>=0):
            ret += f"-precisefastlossrecovery {self.enable_precise_fast_loss_recovery} "
            # ret += f"-pflr_print_debug_msg "
            # ret += f"-pflr_disable_probe "
            # ret += f"-pflr_disable_nack "
        
            if (self.enable_precise_fast_loss_recovery == 4):
                ret += f"-pflr4_pkt_per_slot {self.pflr4_pkt_per_slot} "

                if (self.pflr4_use_ev_recovery):
                    ret += "-pflr4_use_ev_recovery "

            if (self.pflr_proactive_probe >= 0):
                ret += f"-pflr_proactive_probe {self.pflr_proactive_probe} "
            
            if (self.pflr_proactive_rtx_probe):
                ret += f"-pflr_proactive_rtx_probe "
        
        if self.no_droping_low_header:
            ret += f"-no_droping_low_header "

        if self.switch_random_drop_prob > 0:
            ret += f"-switch_random_drop_prob {self.switch_random_drop_prob} "

        if self.cc_algo.startswith("smartt"):
            ret += (
                f"-sender_cc_only "
                f"-sender_cc_algo {self.cc_algo} "
                f"-use_wait_to_decrease {int(self.smartt_wtd)} "
                f"-fasti_scaling_factor {self.smartt_fasti_scaling_factor} "
                f"-pi_scaling_factor {self.smartt_pi_scaling_factor} "
                f"-fi_scaling_factor {self.smartt_fi_scaling_factor} "
                f"-fd_scaling_factor {self.smartt_fd_scaling_factor} "
                f"-md_scaling_factor {self.smartt_md_scaling_factor} "
            )
        else:
            ret += cc_command_dict[self.cc_algo]

        return ret

    def get_fig_title(self, aggregate=True):
        p_list = [
            "link_speed_gbps",
            "nodes",
            "over_sub",
            "msg_size_bytes",
            "matrix",
            "incast_degree",
            "load",
            "workl_duration_ms",
            "bdp_pkts",
            "bdp_transport_bytes",
            "bdp_wire_bytes",
            "base_rtt_ns",
            "mtu_bytes",
            # "rss_worse_entropy_metric"
        ]

        if not aggregate:
            p_list.extend(["cc_algo", "load_balancing_algo", "smartt_wtd"])

        p_vals = []
        for p in p_list:
            val = getattr(self, p)
            if val is not None:
                if isinstance(val, float):
                    p_vals.append(f"{p}={val:.2f}")
                else:
                    p_vals.append(f"{p}={val}")

        return ", ".join(p_vals)
    
    @property
    def dict(self):
        return {k: v.value if isinstance(v, Enum) else v for k, v in self.__dict__.items()}


def run_experiment(
    args,
    env_wkld_params: PartialParams,
    scheme_params: List[PartialParams] = ALL_SCHEMES,
    pool: Optional[multiprocessing.pool.Pool] = None,
    store_trace=False,
):
    outdir = args.output
    print("Number of exps:", len(PartialParams.product([env_wkld_params], scheme_params)))
    for pp in PartialParams.product([env_wkld_params], scheme_params):
        p = Params(**pp)
        # ^^ This ensures all the required parameters are set (i.e.,
        # non-optional fields in Params are set), and all parameters set in
        # PartialParams are meaningful (i.e., have a field declared in Params)

        group_dir = os.path.abspath(os.path.join(outdir, p.exp_tag))
        exp_dir = os.path.join(group_dir, f"{p.scheme_tag}.htsim_data")
        os.makedirs(exp_dir, exist_ok=True)

        log_path = os.path.join(exp_dir, f"log.dat")
        p_file = os.path.join(exp_dir, "cliParams.json")
        with open(p_file, "w") as f:
            json.dump(p.dict, f)

        cmd = (
            f"{p.command} "
            f"-data_collection_dir {exp_dir} "
            f"-o {log_path} "
            f"> {os.path.join(exp_dir, f'output.txt') if store_trace else '/dev/null'} "
            # f"> /dev/null "
        )

        if pool:
            def cb(x: Tuple[str, float]):
                cmd, dur = x
                print(f"Finished {cmd} in {dur:.2f} seconds\n")
                if dur <= 0.1:
                    print(f"Failed(?) {cmd} in {dur:.2f} seconds\n")
                    # with open("scripts/failed_commands.txt", "a") as f:
                    #     f.write(f"{cmd}\n")

            def error_cb(e):
                print(f"Got error: {e}")

            pool.apply_async(
                worker, (cmd,), callback=cb, error_callback=error_cb,
            )
        else:
            worker(cmd)


def run_combination(
    args,
    env_wkld_params,
    scheme_params,
    pool=None,
    store_trace=False,
):
    for env_wkld_param in env_wkld_params:
        run_experiment(args, env_wkld_param, scheme_params, pool, store_trace=store_trace)


DURATION_SCALING = {
    'FbHdp_distribution.txt': 5 * 0.3 * 100 / 2500,
    'GoogleRPC2008.txt': 5 * 0.3 * 100 / 100_000,
    'WebSearch_distribution.txt': 5 * 0.3 * 100 / 160,
    'AliStorage2019.txt': 5 * 0.3 * 100 / 7000,
}
"""These numbers help set the simulation duration for the different workloads.

Different workloads have different message sizes, as a result for the same load,
link speed, and simulation duration, we get different message count for
different workloads.

The duration scaling, helps set different simulation duration for the different
workloads, so that we keep roughly similar message counts for the different
workloads.

To produce these scaling factors, we ran the each workload for t_measurement=5ms
of simulated time, and measured the number of messages, from this measurement we
can derive how much time we need to run the workload to get a certain number of
messages as:

t_target = t_measurement * (msg_count_target/msg_count_measurement) *
    (load_measurement/load_target) * (link_speed_measurement/link_speed_target)

Effectively, the more messages we need implies the longer we need to run.
Likewise, on higher link speed, or higher loads, we get the same number of
messages with shorter durations. All these trends are linear so we get the
product form formula above.
"""

CDF_FILES = [
    'FbHdp_distribution.txt',
    'GoogleRPC2008.txt',
    'WebSearch_distribution.txt',
    'AliStorage2019.txt',
]


def run_datacenter_experiment(args, pool, exp_params=None):
    cdf_files = CDF_FILES
    workl_duration_ms = 5
    workl_duration_s = workl_duration_ms * MS_TO_S
    end_time_us = int(workl_duration_s * S_TO_US * 1e2)  # 1e2 adds a conservative 100x headroom for all flows to complete.

    workl_start_time = 0
    nodes = 256
    link_speed_gbps = args.link_speed_gbps

    base_params = PartialParams(
        link_speed_gbps=args.link_speed_gbps,
        nodes=nodes,
        over_sub=1,
        collect_all_metrics=False,
        topo="fat_tree",
        cc_algo="uec_mprdma",
        disable_trim=False
    )
    loads = np.linspace(0.4, 1, 4)

    cdf_params = []
    for cdf_file in cdf_files:
        if not "WebSearch_distribution" in cdf_file:
            continue
        for load in loads:
            load = 3.9 * load
            fpath = os.path.join(TRAFFIC_GEN_CDF_FILES_ROOT, cdf_file)
            fname = cdf_file.removesuffix(".txt")
            cm_file = f"matrix={fname}:nodes={nodes}:load={load}:workl_duration_ms={workl_duration_ms}:link_speed_gbps={link_speed_gbps}.cm"
            cm_path = os.path.join(CM_DIR, cm_file)

            if not os.path.exists(cm_path):
                gen_cmd = (
                    f"python {TRAFFIC_GEN_ROOT}/traffic_gen.py -n {nodes} "
                    f"-c {fpath} -l {load} -b {link_speed_gbps}G "
                    f"-t {workl_start_time} -d {workl_duration_s} "
                    f"-s {TRAFFIC_GEN_SEED} -o {cm_path}"
                )
                print(gen_cmd)
                os.system(gen_cmd)

            cdf_params.append(
                PartialParams(
                    msg_size_bytes=0,
                    matrix=fname,
                    workl_duration_ms=workl_duration_ms,
                    _cm_file=cm_file,
                    _end_time_us=end_time_us,
                    load=load
                )
            )

    env_wkld_params = PartialParams.product([base_params], cdf_params)
    run_combination(args, env_wkld_params, ALL_SCHEMES if exp_params is None else exp_params, pool)


def run_different_rtt_experiment(args, pool):
    msg_size_bytes = 32 << 20  # 256 MB
    link_speed_gbps = args.link_speed_gbps
    nodes=args.number_of_nodes
    base_params = PartialParams(
        link_speed_gbps=link_speed_gbps,
        nodes=nodes,
        over_sub=1,
        msg_size_bytes=msg_size_bytes,
        collect_all_metrics=True,
        topo="fat_tree",
    )

    matrix = "different_rtt_1l_2s"
    n_flows = 3
    cm_file = f"matrix={matrix}:nodes={nodes}:n_flows={n_flows}:msg_size_bytes={msg_size_bytes}.cm"
    assert os.path.isfile(os.path.join(CM_DIR, cm_file))
    p1 = PartialParams(
        matrix=matrix,
        _cm_file=cm_file,
        _end_time_us=get_end_time_us_helper(msg_size_bytes, link_speed_gbps, n_flows)
    )

    matrix = "different_rtt_2l_1s"
    n_flows = 3
    cm_file = f"matrix={matrix}:nodes={nodes}:n_flows={n_flows}:msg_size_bytes={msg_size_bytes}.cm"
    assert os.path.isfile(os.path.join(CM_DIR, cm_file))
    p2 = PartialParams(
        matrix=matrix,
        _cm_file=cm_file,
        _end_time_us=get_end_time_us_helper(msg_size_bytes, link_speed_gbps, n_flows)
    )

    matrix = "different_rtt"
    n_flows = 2
    cm_file = f"matrix={matrix}:nodes={nodes}:n_flows={n_flows}:msg_size_bytes={msg_size_bytes}.cm"
    assert os.path.isfile(os.path.join(CM_DIR, cm_file))
    p3 = PartialParams(
        matrix=matrix,
        _cm_file=cm_file,
        _end_time_us=get_end_time_us_helper(msg_size_bytes, link_speed_gbps, n_flows)
    )

    env_wkld_params = PartialParams.product([base_params], [p1, p2, p3])
    run_combination(
        args,
        env_wkld_params,
        ALL_SCHEMES,
        pool,
    )


def generate_convergence_cm(
    cm_path: str,
    nodes: int,
    n_flows: int,
    time_between_flows_ns: float,
    link_speed_gbps: int,
):
    """We want to see what happens when flows come in and go out. For instance,
    Figure 17 of https://www.usenix.org/system/files/nsdi24-agarwal-anup.pdf

    This setups a connection matrix where flows come in every
    `time_between_flows_ns` for a total of `n_flows` flows. The flow sizes are
    set such that when all the flows have arrived, they run for 2 *
    `time_between_flows_ns`, and then one flow completes roughly every
    `time_between_flows_ns`. "Roughly" because the throughput of flows depend on
    the congestion control/load balancing algorithm. The flows complete in
    reverse of the order in which they arrive.

    NOTE: The factor 2 in 2 * time_between_flows_ns above, is just a
    mathematical convenience. See below for explanation.

    E.g., for 3 flows, and time_between_flows_ns=100, flow 0 arrives at time 0,
    flow 1 arrives at time 100, and flow 2 arrives at time 200. Flow 2 completes
    at time 400, flow 1 completes at time 500, and flow 0 completes at time 600.

    The calculation of flow start times is trivial, i.e., kth flow starts at
    time k * time_between_flows_ns.

    The calculation of flow sizes is more involved. The first flow (i.e., flow
    0) gets throughput=link_speed_gbps, until the second flow arrives, then it
    gets throughput=link_speed_gbps/2 when the third flow arrives, then
    throughput=link_speed_gbps/3 when the fourth flow arrives, and so on. So the
    flow size calculation needs to consider how much throughput the flows get at
    different times.

    The last flow gets a throughput=link_speed_gbps/n_flows. We make it last for
    2 * `time_between_flows_ns`, and thus its flow size is 2 *
    time_between_flows_ns * link_speed_gbps/n_flows.

    For the second last flow its size needs to be the size of the last flow,
    additionally it needs to last for time_between_flows_ns both before last
    flow arrive and after last flow leaves. During that time, it gets
    throughput=link_speed_gbps/(n_flows-1). So its flow size is:
    size_of_last_flow + 2 * time_between_flows_ns * link_speed_gbps/(n_flows-1).

    Using similar recursive calculation we get the sizes of all flows.
    """
    f = open(cm_path, "w")
    f.write(f"Nodes {nodes}\n")
    f.write(f"Connections {n_flows}\n")
    start_ps: float = 0
    first_msg_size_bytes = 2 * np.sum(
        [
            time_between_flows_ns * link_speed_gbps / BYTES_TO_BITS / x
            for x in range(1, n_flows + 1)
        ]
    )
    msg_size_bytes = first_msg_size_bytes
    for i in range(n_flows):
        f.write(f"{i}->{nodes-1} id {i+1} start {int(start_ps)} size {int(msg_size_bytes)}\n")
        start_ps += time_between_flows_ns * NS_TO_PS
        msg_size_bytes -= 2 * time_between_flows_ns * link_speed_gbps / BYTES_TO_BITS / (i+1)
    f.close()
    return int(first_msg_size_bytes)


def run_convergence_experiment(args, pool):
    n_flows = 8
    time_between_flows_rtt = 100

    link_speed_gbps = args.link_speed_gbps
    nodes = 1024
    matrix = "convergence"
    base_rtt_ns = get_base_rtt_ns(link_speed_gbps)
    time_between_flows_ns = time_between_flows_rtt * base_rtt_ns
    cm_file = f"matrix={matrix}:nodes={nodes}:n_flows={n_flows}:time_between_flows_rtt={time_between_flows_rtt}.cm"
    cm_path = os.path.join(CM_DIR, cm_file)
    msg_size_bytes = generate_convergence_cm(
        cm_path,
        nodes,
        n_flows,
        time_between_flows_ns,
        link_speed_gbps,
    )
    end_time_us = int(10 * time_between_flows_ns * n_flows * NS_TO_US)
    base_params = PartialParams(
        link_speed_gbps=link_speed_gbps,
        nodes=nodes,
        over_sub=1,
        msg_size_bytes=msg_size_bytes,
        matrix=matrix,
        topo="fat_tree",
        _cm_file=cm_file,
        _end_time_us=end_time_us,
        collect_all_metrics=True,
    )

    run_experiment(
        args,
        base_params,
        ALL_SCHEMES,
        pool
    )

def generate_oneWay_cm(cm_path: Path, nodes: int, flowsize: int, extra_start_time: float=0, mode: OneWayExperimentMode=OneWayExperimentMode.pod):

    def pick_outside_tor(num_nodes):
        srcs = sample(list(range(nodes)), nodes // 2)
        dsts = [i for i in range(nodes) if i not in srcs]
        k = round((4*num_nodes)**(1/3))
        res = []
        for s in srcs:
            candidates = [d for d in dsts if s // k != d // k]
            if len(candidates) == 0:
                return False, False
            res.append(sample(candidates, 1)[0])
            dsts.remove(res[-1])
        return srcs, res

    if mode == OneWayExperimentMode.pod:
        srcs = list(range(nodes // 2))
        dsts = list(range(nodes // 2, nodes))
        shuffle(dsts)
    elif mode == OneWayExperimentMode.tor:
        tmp_s, tmp_d = pick_outside_tor(nodes)
        while not tmp_s:  # I'm sure there's more efficient...
            tmp_s, tmp_d = pick_outside_tor(nodes)
        srcs, dsts = tmp_s, tmp_d
    else:
        raise NotImplementedError()

    with open(cm_path, "w") as f:
        print("Nodes", nodes, file=f)
        print("Connections", nodes // 2, file=f)
        for n in range(nodes // 2):
            out = f"{srcs[n]}->{dsts[n]} id {n+1} start {extra_start_time} size {flowsize}"
            print(out, file=f)
    print(f"Generated {mode.value} connection matrix saved at {cm_path}")

def run_oneWay_experiment(args, pool, mode):
    msg_sizes_Mbytes = [32]
    cm_files = {k: f"matrix={mode.value}:nodes={args.number_of_nodes}:n_flows={args.number_of_nodes // 2}:msg_size_bytes={k << 20}.cm" for k in msg_sizes_Mbytes}
    cm_paths = {k: Path(CM_DIR) / v for k, v in cm_files.items()}
    for k, cm_path in cm_paths.items():
        if not cm_path.exists():
            generate_oneWay_cm(cm_path, args.number_of_nodes, k << 20, 0, mode)
        else:
            print(f"found cm matrix at {str(cm_path)}")

    base_params = PartialParams(
        link_speed_gbps=args.link_speed_gbps,
        nodes=args.number_of_nodes,
        over_sub=1,
        matrix=mode.value,
        collect_all_metrics=True,
        topo="fat_tree",
        cc_algo="uec_mprdma",
        disable_trim=False
    )
    flow_sizes = [PartialParams(_cm_file=cm_files[k], msg_size_bytes=(k << 20)) for k in msg_sizes_Mbytes]
    asymmetry = [
        # PartialParams(asymmetry="all"),
        PartialParams(asymmetry="sym")
        ]
    load_balancing_algo = PartialParams.product([PartialParams(load_balancing_algo="reps"),
                               PartialParams(load_balancing_algo="ecmp"),
                               PartialParams(load_balancing_algo="flowbender"),
                               PartialParams(load_balancing_algo="uss"),
                               PartialParams(load_balancing_algo="oblivious")], flow_sizes, asymmetry)
    rss_lb = [
              PartialParams(load_balancing_algo="rss", rss_worse_entropy_metric="mean_rtt"),
            #   PartialParams(load_balancing_algo="rss", rss_worse_entropy_metric="ecn"),
            #   PartialParams(load_balancing_algo="rss", rss_worse_entropy_metric="worse_rtt"),
              PartialParams(load_balancing_algo="rss", rss_worse_entropy_metric="mean_rtt", rss_max_number_of_skipped_rounds=5),
            #   PartialParams(load_balancing_algo="rss", rss_worse_entropy_metric="ecn", rss_max_number_of_skipped_rounds=5),
            #   PartialParams(load_balancing_algo="rss", rss_worse_entropy_metric="worse_rtt", rss_max_number_of_skipped_rounds=5),
              PartialParams(load_balancing_algo="rss", rss_worse_entropy_metric="mean_rtt", rss_frozen_threshold=0.2),
            #   PartialParams(load_balancing_algo="rss", rss_worse_entropy_metric="ecn", rss_frozen_threshold=0.4),
            #   PartialParams(load_balancing_algo="rss", rss_worse_entropy_metric="worse_rtt", rss_frozen_threshold=0.4)
              ]
    if args.rss_sweep:
        rss_subflow_counts = [PartialParams(rss_number_of_subflows=count) for count in [1 << k for k in range(1, 6)]]
        rss_update_intervals = [PartialParams(rss_update_interval=interval) for interval in np.round(np.linspace(32, 160, 5), 1)]
        rss_sweep = PartialParams.product(rss_lb, flow_sizes, rss_subflow_counts, rss_update_intervals, asymmetry)
        scheme_params = PartialParams.consolidate(load_balancing_algo, rss_sweep)
    else:
        scheme_params = PartialParams.product(asymmetry, flow_sizes, PartialParams.consolidate(load_balancing_algo, rss_lb))
    
    run_experiment(
        args,
        base_params,
        scheme_params,
        pool,
    )

def run_all_euroSys_experiments(args, pool):
    asymmetry = [
        # PartialParams(asymmetry="all"),
        PartialParams(asymmetry="sym"),
        # PartialParams(asymmetry="bg", ecmp_nodes=round(1.5 * args.number_of_nodes**.5))
        ]
    msg_size_bytes = 16 << 20
    base_params = PartialParams(
        link_speed_gbps=args.link_speed_gbps,
        nodes=args.number_of_nodes,
        collect_all_metrics=False,
        topo="fat_tree",
        cc_algo="uec_mprdma",
        disable_trim=False,
        switch_random_drop_prob=0.001,
    )
    load_balancing_algo = [#PartialParams(load_balancing_algo="reps"),
                        #    PartialParams(load_balancing_algo="ecmp"),
                        #    PartialParams(load_balancing_algo="flowbender"),
                        #    PartialParams(load_balancing_algo="uss"),
                        #    PartialParams(load_balancing_algo="uss", disable_trim=True),
                        #    PartialParams(load_balancing_algo="uss", enable_precise_fast_loss_recovery=1, disable_trim=True),
                        #    PartialParams(load_balancing_algo="oblivious"),
                        #    PartialParams(load_balancing_algo="oblivious", disable_trim=True),
                           PartialParams(load_balancing_algo="rss"),
                           PartialParams(load_balancing_algo="rss", disable_trim=True),
                           PartialParams(load_balancing_algo="rss", disable_trim=True, enable_sleek=True),
                           PartialParams(load_balancing_algo="rss", disable_trim=True, low_priority_trim=True, no_droping_low_header=True),
                           PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=0, pflr_proactive_rtx_probe=True, no_droping_low_header=True, disable_trim=True),
                           PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=1, pflr_proactive_rtx_probe=True, no_droping_low_header=True, disable_trim=True),
                           PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=4, pflr_proactive_rtx_probe=True, no_droping_low_header=True, disable_trim=True),
                        #    PartialParams(load_balancing_algo="rss", disable_trim=True, low_priority_trim=True),
                        #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=0, pflr_proactive_rtx_probe=True, disable_trim=True),
                        #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=1, pflr_proactive_rtx_probe=True, disable_trim=True),
                        #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=4, pflr_proactive_rtx_probe=True, disable_trim=True),
                        #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=2, pflr_proactive_rtx_probe=True, disable_trim=True),
                        #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=4, pflr_proactive_rtx_probe=True, disable_trim=True),
                        #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=1, pflr_proactive_rtx_probe=True, no_droping_low_header=True, disable_trim=True),
                        #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=4, pflr_proactive_rtx_probe=True, no_droping_low_header=True, disable_trim=True),
                        #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=1, no_droping_low_header=1, disable_trim=True),
                        #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=2, disable_trim=True),
                        #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=4, disable_trim=True),
                        #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=8, disable_trim=True),
                        #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=4, pflr4_pkt_per_slot=4,pflr4_use_ev_recovery=True,disable_trim=True),
                        #    PartialParams(load_balancing_algo="rss", rss_number_of_subflows = 1, enable_precise_fast_loss_recovery=4, pflr4_pkt_per_slot=16, pflr4_use_ev_recovery=True,disable_trim=True),
                        #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=4, pflr4_pkt_per_slot=8,disable_trim=True),
                        #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=4, pflr4_pkt_per_slot=16,disable_trim=True),
                        #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=4, pflr4_pkt_per_slot=32,disable_trim=True),
                        #    PartialParams(load_balancing_algo="rss", rss_frozen_threshold=1)
                           ]
    output = Path(args.output)
    run_incast, run_permutation, run_websearch, run_oneway, run_rss_params, run_ft = True, True, False, False, False, False
    # Incast
    if run_incast:
        print("----- Incast -----")
        incast_base_params = base_params.copy()
        incast_base_params.update(over_sub=1, mss_bytes=1024, incast_degree=16, matrix=ExperimentType.incast.value, data_collection_config=dataCollectionConfig.incast)
        msg_size_bytes_list = [PartialParams(msg_size_bytes=(1 << x) << 10) for x in range(2, 8)]  # 2 KiB to 4 MiB
        # msg_size_bytes_list = [PartialParams(msg_size_bytes=(16) << 10), PartialParams(msg_size_bytes=(32) << 10), PartialParams(msg_size_bytes=(64) << 10)]  # 2 KiB to 4 MiB

        setattr(args, "output", output / "incast")
        run_experiment(
            args,
            incast_base_params,
            PartialParams.product(load_balancing_algo, asymmetry, msg_size_bytes_list),
            pool,
        )

    # Websearch
    if run_websearch:
        print("----- Websearch -----")
        setattr(args, "output", output / "webSearch")
        run_datacenter_experiment(args, pool, PartialParams.product(load_balancing_algo, asymmetry))

    # Permutation
    if run_permutation:
        print("----- Permutation -----")
        permuatation_base_params = base_params.copy()
        permuatation_base_params.update(matrix=ExperimentType.permutation.value, data_collection_config=dataCollectionConfig.permutation)  # 16 MiB
        msg_size_bytes_list = [PartialParams(msg_size_bytes=(1 << x) << 10) for x in range(4, 10)]  # 2 KiB to 4 MiB
        # msg_size_bytes_list = [PartialParams(msg_size_bytes=64<<10), PartialParams(msg_size_bytes=256<<10)]
        # msg_size_bytes_list = [PartialParams(msg_size_bytes=256<<10)]
        over_sub_list = [PartialParams(over_sub=8)]
        setattr(args, "output", output / "permutation")
        run_experiment(
            args,
            permuatation_base_params,
            PartialParams.product(load_balancing_algo, asymmetry, msg_size_bytes_list, over_sub_list),
            pool,
        )

    # OneWay
    if run_oneway:
        print("----- OneWay -----")
        cm_file = f"matrix={OneWayExperimentMode.pod}:nodes={args.number_of_nodes}:n_flows={args.number_of_nodes // 2}:msg_size_bytes={msg_size_bytes}.cm"
        cm_path = Path(CM_DIR) / cm_file
        if not cm_path.exists():
            generate_oneWay_cm(cm_path, args.number_of_nodes, msg_size_bytes, 0, OneWayExperimentMode.pod)
        else:
            print(f"found cm matrix at {str(cm_path)}")
        oneWay_base_params = base_params.copy()
        oneWay_base_params.update(msg_size_bytes=msg_size_bytes, matrix=ExperimentType.oneWayPod.value, over_sub=1, _cm_file=cm_file, data_collection_config=dataCollectionConfig.oneWay)

        setattr(args, "output", output / "oneWay")
        run_experiment(
            args,
            oneWay_base_params,
            PartialParams.product(load_balancing_algo, asymmetry),
            pool,
        )


    # RSS Params
    if run_rss_params:
        print("----- RSS Params -----")
        cm_file = f"matrix={OneWayExperimentMode.pod}:nodes={args.number_of_nodes}:n_flows={args.number_of_nodes // 2}:msg_size_bytes={msg_size_bytes}.cm"
        cm_path = Path(CM_DIR) / cm_file
        if not cm_path.exists():
            generate_oneWay_cm(cm_path, args.number_of_nodes, msg_size_bytes, 0, OneWayExperimentMode.pod)
        else:
            print(f"found cm matrix at {str(cm_path)}")
        rss_base_params = base_params.copy()
        rss_base_params.update(msg_size_bytes=msg_size_bytes, matrix=ExperimentType.oneWayPod.value, over_sub=1, _cm_file=cm_file, data_collection_config=dataCollectionConfig.rss_params)  # 16 MiB
        rss_subflow_counts = [PartialParams(rss_number_of_subflows=count) for count in [1 << k for k in range(2, 6)]]
        rss_update_intervals = [PartialParams(rss_update_interval=interval) for interval in np.concatenate(([8, 16], np.round(np.linspace(32, 320, 5), 1)))]
        rss_lb = [PartialParams(load_balancing_algo="rss"),
                            #    PartialParams(load_balancing_algo="rss", rss_max_number_of_skipped_rounds=10),
                            #    PartialParams(load_balancing_algo="rss", rss_frozen_threshold=0.2)
                            ]
        rss_sweep = PartialParams.product(rss_lb, rss_subflow_counts, rss_update_intervals, asymmetry)

        setattr(args, "output", output / "rssGrid")
        run_experiment(
            args,
            rss_base_params,
            rss_sweep,
            pool,
        )

    # Frozen Threshold
    if run_ft:
        print("----- Frozen Threshold -----")
        cm_file = f"matrix={OneWayExperimentMode.pod}:nodes={args.number_of_nodes}:n_flows={args.number_of_nodes // 2}:msg_size_bytes={msg_size_bytes}.cm"
        cm_path = Path(CM_DIR) / cm_file
        if not cm_path.exists():
            generate_oneWay_cm(cm_path, args.number_of_nodes, msg_size_bytes, 0, OneWayExperimentMode.pod)
        else:
            print(f"found cm matrix at {str(cm_path)}")
        frozen_base_params = base_params.copy()
        frozen_base_params.update(msg_size_bytes=msg_size_bytes, matrix=ExperimentType.oneWayPod.value, over_sub=1, _cm_file=cm_file, data_collection_config=dataCollectionConfig.frozen_threshold)
        rss_frozen_thresholds = [PartialParams(load_balancing_algo="rss", rss_frozen_threshold=round(f, 2)) for f in np.linspace(0, 5, 21)]
        setattr(args, "output", output / "frozenThreshold")
        run_experiment(
            args,
            frozen_base_params,
            PartialParams.product(asymmetry, rss_frozen_thresholds),
            pool,
        )

    # Loss
    # TODO: TBD

def run_all_pfld_experiments(args, pool):
    print("----------------- Running all PFLD experiments -----------------")
    asymmetry = [
        # PartialParams(asymmetry="all"),
        PartialParams(asymmetry="sym"),
        # PartialParams(asymmetry="bg", ecmp_nodes=round(1.5 * args.number_of_nodes**.5))
        ]
    msg_size_bytes = 16 << 20
    link_speed_multiplier=args.link_speed_gbps//100
    base_params = PartialParams(
        link_speed_gbps=args.link_speed_gbps,
        queue_size_bdp=args.queue_size_bdp,ecn_kmin_bdp=0.25,ecn_kmax_bdp=0.75,init_cwnd_bdp=1,
        nodes=args.number_of_nodes,
        collect_all_metrics=False,
        topo="fat_tree",
        cc_algo="uec_mprdma",
        disable_trim=False,
        switch_random_drop_prob=args.switch_random_drop_prob,
    )
    pfld_exp_set = args.pfld_exp_set
    if pfld_exp_set == 1:
        # plot 1: loss detection time vs. workload
        load_balancing_algo = [ PartialParams(load_balancing_algo="rss"), # rss + trim
                                # PartialParams(load_balancing_algo="rss", disable_trim=True), # rss + rto
                                # PartialParams(load_balancing_algo="rss", disable_trim=True, low_priority_trim=True, no_droping_low_header=True), # rss + low trim
                                # PartialParams(load_balancing_algo="rss", disable_trim=True, enable_precise_fast_loss_recovery=3, pflr_proactive_probe=0, pflr_proactive_rtx_probe=True, no_droping_low_header=True), # rss + pfld
                                # PartialParams(load_balancing_algo="rss", disable_trim=True, enable_precise_fast_loss_recovery=3, pflr_proactive_probe=1, pflr_proactive_rtx_probe=True, no_droping_low_header=True), # rss + pfld
                                # PartialParams(load_balancing_algo="rss", disable_trim=True, enable_precise_fast_loss_recovery=3, pflr_proactive_probe=4, pflr_proactive_rtx_probe=True, no_droping_low_header=True), # rss + pfld
                            ]
        output = Path(args.output)
        # Incast
        print("----- Incast 16 -----")
        incast_base_params = base_params.copy()
        incast_base_params.update(over_sub=1, incast_degree=16, matrix=ExperimentType.incast.value, data_collection_config=dataCollectionConfig.incast)
        msg_size_bytes_list = [PartialParams(msg_size_bytes=link_speed_multiplier*((1 << x) << 10)) for x in range(7, 9)]  # 2 KiB to 4 MiB
        setattr(args, "output", output / "incast16")
        # run_experiment(
        #     args,
        #     incast_base_params,
        #     PartialParams.product(load_balancing_algo, asymmetry, msg_size_bytes_list),
        #     pool,
        #     True,
        # )

        print("----- Permutation -----")
        permuatation_base_params = base_params.copy()
        permuatation_base_params.update(matrix=ExperimentType.permutation.value, data_collection_config=dataCollectionConfig.permutation)  # 16 MiB
        msg_size_bytes_list = [PartialParams(msg_size_bytes=link_speed_multiplier*((1 << x) << 10)) for x in range(8, 9)]  # 2 KiB to 4 MiB
        over_sub_list = [PartialParams(over_sub=2)]
        setattr(args, "output", output / "permutation")
        run_experiment(
            args,
            permuatation_base_params,
            PartialParams.product(load_balancing_algo, asymmetry, msg_size_bytes_list, over_sub_list),
            pool,
            True,
        )
    elif pfld_exp_set == 2:
        # plot 2: headroom & overhead
        load_balancing_algo = [ #PartialParams(load_balancing_algo="rss", disable_trim=True, enable_precise_fast_loss_recovery=2, pflr_proactive_probe=0, pflr_proactive_rtx_probe=True, no_droping_low_header=True),
                                # PartialParams(load_balancing_algo="rss", disable_trim=True, enable_precise_fast_loss_recovery=2, pflr_proactive_probe=1, pflr_proactive_rtx_probe=True, no_droping_low_header=True),
                                #PartialParams(load_balancing_algo="rss", disable_trim=True, enable_precise_fast_loss_recovery=2, pflr_proactive_probe=4, pflr_proactive_rtx_probe=True, no_droping_low_header=True),
                                PartialParams(load_balancing_algo="rss", disable_trim=True, enable_precise_fast_loss_recovery=3, pflr_proactive_probe=0, pflr_proactive_rtx_probe=True, no_droping_low_header=True),
                                # PartialParams(load_balancing_algo="rss", disable_trim=True, enable_precise_fast_loss_recovery=3, pflr_proactive_probe=1, pflr_proactive_rtx_probe=True, no_droping_low_header=True),
                                # PartialParams(load_balancing_algo="rss", disable_trim=True, enable_precise_fast_loss_recovery=3, pflr_proactive_probe=4, pflr_proactive_rtx_probe=True, no_droping_low_header=True),
                            ]
        output = Path(args.output)
        # Incast
        print("----- Incast 16 -----")
        incast_base_params = base_params.copy()
        incast_base_params.update(over_sub=1, incast_degree=16, matrix=ExperimentType.incast.value, data_collection_config=dataCollectionConfig.incast)
        msg_size_bytes_list = [PartialParams(msg_size_bytes=link_speed_multiplier*((1 << x) << 10)) for x in range(7, 9)]  # 2 KiB to 4 MiB
        setattr(args, "output", output / "incast16")
        run_experiment(
            args,
            incast_base_params,
            PartialParams.product(load_balancing_algo, asymmetry, msg_size_bytes_list),
            pool,
            True,
        )

        print("----- Permutation -----")
        permuatation_base_params = base_params.copy()
        permuatation_base_params.update(matrix=ExperimentType.permutation.value, data_collection_config=dataCollectionConfig.permutation)  # 16 MiB
        msg_size_bytes_list = [PartialParams(msg_size_bytes=link_speed_multiplier*((1 << x) << 10)) for x in range(7, 9)]  # 2 KiB to 4 MiB
        over_sub_list = [PartialParams(over_sub=2)]
        setattr(args, "output", output / "permutation")
        run_experiment(
            args,
            permuatation_base_params,
            PartialParams.product(load_balancing_algo, asymmetry, msg_size_bytes_list, over_sub_list),
            pool,
            True,
        )
    elif pfld_exp_set == 3:
        load_balancing_algo = [#PartialParams(load_balancing_algo="reps"),
                        #    PartialParams(load_balancing_algo="ecmp"),
                        #    PartialParams(load_balancing_algo="flowbender"),
                        #    PartialParams(load_balancing_algo="uss"),
                        #    PartialParams(load_balancing_algo="uss", disable_trim=True),
                        #    PartialParams(load_balancing_algo="uss", enable_precise_fast_loss_recovery=1, disable_trim=True),
                        #    PartialParams(load_balancing_algo="oblivious"),
                        #    PartialParams(load_balancing_algo="oblivious", disable_trim=True),
                        #    PartialParams(load_balancing_algo="oblivious", disable_trim=True, low_priority_trim=True, no_droping_low_header=True), # rss + low trim
                           PartialParams(load_balancing_algo="rss"), # rss + trim
                        #    PartialParams(load_balancing_algo="rss", disable_trim=True), # rss + rto
                        #    PartialParams(load_balancing_algo="rss", disable_trim=True, low_priority_trim=True, no_droping_low_header=True), # rss + low trim
                        # #    PartialParams(load_balancing_algo="rss", disable_trim=True, enable_precise_fast_loss_recovery=3, pflr_proactive_probe=0, pflr_proactive_rtx_probe=True, no_droping_low_header=True), # rss + pfld
                        #    PartialParams(load_balancing_algo="rss", disable_trim=True, enable_precise_fast_loss_recovery=3, pflr_proactive_probe=1, pflr_proactive_rtx_probe=True, no_droping_low_header=True), # rss + pfld
                           ]
        output = Path(args.output)
        # Incast
        print("----- Incast 16 -----")
        incast_base_params = base_params.copy()
        incast_base_params.update(over_sub=1, incast_degree=16, matrix=ExperimentType.incast.value, data_collection_config=dataCollectionConfig.incast)
        # msg_size_bytes_list = [PartialParams(msg_size_bytes=link_speed_multiplier*((1 << x) << 10)) for x in range(4, 11)]  # 2 KiB to 4 MiB
        msg_size_bytes_list = [PartialParams(msg_size_bytes=link_speed_multiplier*((1 << x) << 10)) for x in range(6, 8)]  # 2 KiB to 4 MiB
        setattr(args, "output", output / "incast16")
        # run_experiment(
        #     args,
        #     incast_base_params,
        #     PartialParams.product(load_balancing_algo, asymmetry, msg_size_bytes_list),
        #     pool,
        #     True,
        # )

        print("----- Permutation -----")
        permuatation_base_params = base_params.copy()
        permuatation_base_params.update(matrix=ExperimentType.permutation.value, data_collection_config=dataCollectionConfig.permutation)  # 16 MiB
        # msg_size_bytes_list = [PartialParams(msg_size_bytes=link_speed_multiplier*((1 << x) << 10)) for x in range(4, 11)]  # 2 KiB to 4 MiB
        msg_size_bytes_list = [PartialParams(msg_size_bytes=link_speed_multiplier*((1 << x) << 10)) for x in range(7, 8)]  # 2 KiB to 4 MiB
        over_sub_list = [PartialParams(over_sub=2)]
        setattr(args, "output", output / "permutation")
        run_experiment(
            args,
            permuatation_base_params,
            PartialParams.product(load_balancing_algo, asymmetry, msg_size_bytes_list, over_sub_list),
            pool,
            True,
        )
    elif pfld_exp_set == 4:
        load_balancing_algo = [#PartialParams(load_balancing_algo="reps"),
                           PartialParams(load_balancing_algo="ecmp"),
                           PartialParams(load_balancing_algo="flowbender"),
                        #    PartialParams(load_balancing_algo="uss"),
                        #    PartialParams(load_balancing_algo="uss", disable_trim=True),
                        #    PartialParams(load_balancing_algo="uss", enable_precise_fast_loss_recovery=1, disable_trim=True),
                           PartialParams(load_balancing_algo="oblivious"),
                        #    PartialParams(load_balancing_algo="oblivious", disable_trim=True),
                           PartialParams(load_balancing_algo="rss"), # rss + trim
                           PartialParams(load_balancing_algo="rss", disable_trim=True), # rss + rto
                        #    PartialParams(load_balancing_algo="rss", disable_trim=True, low_priority_trim=True, no_droping_low_header=True), # rss + low trim
                           PartialParams(load_balancing_algo="rss", disable_trim=True, enable_precise_fast_loss_recovery=3, pflr_proactive_probe=0, pflr_proactive_rtx_probe=True, no_droping_low_header=True), # rss + pfld
                           ]
        output = Path(args.output)
        matrix_lst = ['serialn_alltoall_4', 'serialn_alltoall_8', 'allreduce', 'allreduce_butterfly']
        for matrix in matrix_lst:
            cm_file = f'matrix={matrix}.cm'
            ai_collectives_base_params = base_params.copy()
            ai_collectives_base_params.update(_cm_file=cm_file, over_sub=1, msg_size_bytes=2<<20, matrix=matrix)
            setattr(args, "output", output / matrix)
            run_experiment(
                args,
                ai_collectives_base_params,
                PartialParams.product(load_balancing_algo, asymmetry),
                pool,
                True,
            )
    elif pfld_exp_set == 5:
        load_balancing_algo = [
                           PartialParams(load_balancing_algo="rss", disable_trim=True, enable_precise_fast_loss_recovery=3, pflr_proactive_probe=0, pflr_proactive_rtx_probe=True, no_droping_low_header=True), # rss + pfld
                           PartialParams(load_balancing_algo="rss", disable_trim=True, enable_precise_fast_loss_recovery=3, pflr_proactive_probe=0, pflr_proactive_rtx_probe=True, no_droping_low_header=False), # rss + pfld
                           ]
        output = Path(args.output)
        # Incast
        print("----- Incast 16 -----")
        incast_base_params = base_params.copy()
        incast_base_params.update(over_sub=1, incast_degree=16, matrix=ExperimentType.incast.value, data_collection_config=dataCollectionConfig.incast)
        msg_size_bytes_list = [PartialParams(msg_size_bytes=link_speed_multiplier*((1 << x) << 10)) for x in range(7, 9)]  # 2 KiB to 4 MiB
        setattr(args, "output", output / "incast16")
        run_experiment(
            args,
            incast_base_params,
            PartialParams.product(load_balancing_algo, asymmetry, msg_size_bytes_list),
            pool,
            True,
        )

        print("----- Permutation -----")
        permuatation_base_params = base_params.copy()
        permuatation_base_params.update(matrix=ExperimentType.permutation.value, data_collection_config=dataCollectionConfig.permutation)  # 16 MiB
        msg_size_bytes_list = [PartialParams(msg_size_bytes=link_speed_multiplier*((1 << x) << 10)) for x in range(7, 9)]  # 2 KiB to 4 MiB
        over_sub_list = [PartialParams(over_sub=2)]
        setattr(args, "output", output / "permutation")
        run_experiment(
            args,
            permuatation_base_params,
            PartialParams.product(load_balancing_algo, asymmetry, msg_size_bytes_list, over_sub_list),
            pool,
            True,
        )
    elif pfld_exp_set == 6:
        load_balancing_algo = [
                           PartialParams(load_balancing_algo="rss", disable_trim=True, enable_precise_fast_loss_recovery=3, pflr_proactive_probe=0, pflr_proactive_rtx_probe=True, no_droping_low_header=True), # rss + pfld
                           PartialParams(load_balancing_algo="rss", disable_trim=True, enable_precise_fast_loss_recovery=3, pflr_proactive_probe=0, pflr_proactive_rtx_probe=True, no_droping_low_header=False), # rss + pfld
                           ]
        output = Path(args.output)
        matrix_lst = ['serialn_alltoall_4', 'serialn_alltoall_8', 'allreduce', 'allreduce_butterfly']
        for matrix in matrix_lst:
            cm_file = f'matrix={matrix}.cm'
            ai_collectives_base_params = base_params.copy()
            ai_collectives_base_params.update(_cm_file=cm_file, over_sub=1, msg_size_bytes=2<<20, matrix=matrix)
            setattr(args, "output", output / matrix)
            run_experiment(
                args,
                ai_collectives_base_params,
                PartialParams.product(load_balancing_algo, asymmetry),
                pool,
                True,
            )


        # DC trace
        # load_balancing_algo = [ PartialParams(load_balancing_algo="rss", disable_trim=True, enable_precise_fast_loss_recovery=3, pflr_proactive_probe=0, pflr_proactive_rtx_probe=True, no_droping_low_header=True)
        #                     ]
        # over_sub_list = [PartialParams(over_sub=1)]
        # exp_params = PartialParams.product(load_balancing_algo, asymmetry, over_sub_list)
        # # run_datacenter_experiment(args, pool, PartialParams.product(load_balancing_algo, asymmetry))
        # cdf_files = CDF_FILES
        # workl_duration_ms = 5
        # workl_duration_s = workl_duration_ms * MS_TO_S
        # end_time_us = int(workl_duration_s * S_TO_US * 1e2)  # 1e2 adds a conservative 100x headroom for all flows to complete.

        # workl_start_time = 0
        # nodes = args.number_of_nodes
        # link_speed_gbps = args.link_speed_gbps
        # loads = np.linspace(0.4, 1, 4)

        # cdf_params = []
        # for cdf_file in cdf_files:
        #     if not "WebSearch_distribution" in cdf_file:
        #         continue
        #     for load in loads:
        #         load = 3.9 * load
        #         fpath = os.path.join(TRAFFIC_GEN_CDF_FILES_ROOT, cdf_file)
        #         fname = cdf_file.removesuffix(".txt")
        #         cm_file = f"matrix={fname}:nodes={nodes}:load={load}:workl_duration_ms={workl_duration_ms}:link_speed_gbps={link_speed_gbps}.cm"
        #         cm_path = os.path.join(CM_DIR, cm_file)

        #         if not os.path.exists(cm_path):
        #             gen_cmd = (
        #                 f"python {TRAFFIC_GEN_ROOT}/traffic_gen.py -n {nodes} "
        #                 f"-c {fpath} -l {load} -b {link_speed_gbps}G "
        #                 f"-t {workl_start_time} -d {workl_duration_s} "
        #                 f"-s {TRAFFIC_GEN_SEED} -o {cm_path}"
        #             )
        #             print(gen_cmd)
        #             os.system(gen_cmd)

        #         cdf_params.append(
        #             PartialParams(
        #                 msg_size_bytes=0,
        #                 matrix=fname,
        #                 workl_duration_ms=workl_duration_ms,
        #                 _cm_file=cm_file,
        #                 _end_time_us=end_time_us,
        #                 load=load
        #             )
        #         )

        # env_wkld_params = PartialParams.product([base_params], cdf_params)
        # run_combination(args, env_wkld_params, ALL_SCHEMES if exp_params is None else exp_params, pool, store_trace=False)


    # LB comp
    # load_balancing_algo = [PartialParams(load_balancing_algo="reps"),
    #                        PartialParams(load_balancing_algo="flowbender"),
    #                        PartialParams(load_balancing_algo="oblivious"),
    #                        PartialParams(load_balancing_algo="rss"),
    #                        ]
    # Headroom & overhead
    # if args.pfld_run_type == 'queue_ovhd':
    #     load_balancing_algo = [PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=0, pflr_proactive_rtx_probe=True, no_droping_low_header=True, disable_trim=True),
    #                         PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=1, pflr_proactive_rtx_probe=True, no_droping_low_header=True, disable_trim=True),
    #                         PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=4, pflr_proactive_rtx_probe=True, no_droping_low_header=True, disable_trim=True),
    #                         ]
    # elif args.pfld_run_type == 'compare_fct':
    # load_balancing_algo = [ PartialParams(load_balancing_algo="rss"),
    #                         PartialParams(load_balancing_algo="rss", disable_trim=True),
    #                         PartialParams(load_balancing_algo="rss", disable_trim=True, enable_sleek=True),
    #                         PartialParams(load_balancing_algo="reps", disable_trim=True, enable_sleek=True),
    #                         PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=0, pflr_proactive_rtx_probe=True, no_droping_low_header=True, disable_trim=True),
    #                         PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=1, pflr_proactive_rtx_probe=True, no_droping_low_header=True, disable_trim=True),
    #                         PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=4, pflr_proactive_rtx_probe=True, no_droping_low_header=True, disable_trim=True),
    #                     ]
    # else:
    #         raise ValueError(f'Unknown pfld run type: {args.pfld_run_type}')
    # load_balancing_algo = [#PartialParams(load_balancing_algo="reps"),
    #                     #    PartialParams(load_balancing_algo="ecmp"),
    #                     #    PartialParams(load_balancing_algo="flowbender"),
    #                     #    PartialParams(load_balancing_algo="uss"),
    #                     #    PartialParams(load_balancing_algo="uss", disable_trim=True),
    #                     #    PartialParams(load_balancing_algo="uss", enable_precise_fast_loss_recovery=1, disable_trim=True),
    #                     #    PartialParams(load_balancing_algo="oblivious"),
    #                     #    PartialParams(load_balancing_algo="oblivious", disable_trim=True),
    #                        PartialParams(load_balancing_algo="rss"),
    #                        PartialParams(load_balancing_algo="rss", disable_trim=True),
    #                        PartialParams(load_balancing_algo="rss", disable_trim=True, enable_sleek=True),
    #                        PartialParams(load_balancing_algo="rss", disable_trim=True, low_priority_trim=True, no_droping_low_header=True),
    #                        PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=0, pflr_proactive_rtx_probe=True, no_droping_low_header=True, disable_trim=True),
    #                        PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=1, pflr_proactive_rtx_probe=True, no_droping_low_header=True, disable_trim=True),
    #                        PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=4, pflr_proactive_rtx_probe=True, no_droping_low_header=True, disable_trim=True),
    #                     #    PartialParams(load_balancing_algo="rss", disable_trim=True, low_priority_trim=True),
    #                     #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=0, pflr_proactive_rtx_probe=True, disable_trim=True),
    #                     #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=1, pflr_proactive_rtx_probe=True, disable_trim=True),
    #                     #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=4, pflr_proactive_rtx_probe=True, disable_trim=True),
    #                     #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=2, pflr_proactive_rtx_probe=True, disable_trim=True),
    #                     #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=4, pflr_proactive_rtx_probe=True, disable_trim=True),
    #                     #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=1, pflr_proactive_rtx_probe=True, no_droping_low_header=True, disable_trim=True),
    #                     #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=4, pflr_proactive_rtx_probe=True, no_droping_low_header=True, disable_trim=True),
    #                     #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=1, no_droping_low_header=1, disable_trim=True),
    #                     #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=2, disable_trim=True),
    #                     #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=4, disable_trim=True),
    #                     #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=3, pflr_proactive_probe=8, disable_trim=True),
    #                     #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=4, pflr4_pkt_per_slot=4,pflr4_use_ev_recovery=True,disable_trim=True),
    #                     #    PartialParams(load_balancing_algo="rss", rss_number_of_subflows = 1, enable_precise_fast_loss_recovery=4, pflr4_pkt_per_slot=16, pflr4_use_ev_recovery=True,disable_trim=True),
    #                     #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=4, pflr4_pkt_per_slot=8,disable_trim=True),
    #                     #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=4, pflr4_pkt_per_slot=16,disable_trim=True),
    #                     #    PartialParams(load_balancing_algo="rss", enable_precise_fast_loss_recovery=4, pflr4_pkt_per_slot=32,disable_trim=True),
    #                     #    PartialParams(load_balancing_algo="rss", rss_frozen_threshold=1)
    #                        ]
    # output = Path(args.output)
    # run_incast, run_permutation = True, True
    # # Incast
    # if run_incast:
    #     print("----- Incast 16 -----")
    #     incast_base_params = base_params.copy()
    #     incast_base_params.update(over_sub=1, incast_degree=16, matrix=ExperimentType.incast.value, data_collection_config=dataCollectionConfig.incast)
    #     msg_size_bytes_list = [PartialParams(msg_size_bytes=link_speed_multiplier*((1 << x) << 10)) for x in range(6, 8)]  # 2 KiB to 4 MiB
    #     # msg_size_bytes_list = [PartialParams(msg_size_bytes=(16) << 10), PartialParams(msg_size_bytes=(32) << 10), PartialParams(msg_size_bytes=(64) << 10)]  # 2 KiB to 4 MiB

    #     setattr(args, "output", output / "incast16")
    #     run_experiment(
    #         args,
    #         incast_base_params,
    #         PartialParams.product(load_balancing_algo, asymmetry, msg_size_bytes_list),
    #         pool,
    #     )

    #     print("----- Incast 32 -----")
    #     incast_base_params = base_params.copy()
    #     incast_base_params.update(over_sub=1, incast_degree=32, matrix=ExperimentType.incast.value, data_collection_config=dataCollectionConfig.incast)
    #     msg_size_bytes_list = [PartialParams(msg_size_bytes=link_speed_multiplier*((1 << x) << 10)) for x in range(4, 12)]  # 2 KiB to 4 MiB
    #     # msg_size_bytes_list = [PartialParams(msg_size_bytes=(16) << 10), PartialParams(msg_size_bytes=(32) << 10), PartialParams(msg_size_bytes=(64) << 10)]  # 2 KiB to 4 MiB

    #     setattr(args, "output", output / "incast32")
    #     run_experiment(
    #         args,
    #         incast_base_params,
    #         PartialParams.product(load_balancing_algo, asymmetry, msg_size_bytes_list),
    #         pool,
    #     )

    # # Permutation
    # if run_permutation:
    #     print("----- Permutation -----")
    #     permuatation_base_params = base_params.copy()
    #     permuatation_base_params.update(matrix=ExperimentType.permutation.value, data_collection_config=dataCollectionConfig.permutation)  # 16 MiB
    #     msg_size_bytes_list = [PartialParams(msg_size_bytes=link_speed_multiplier*((1 << x) << 10)) for x in range(4, 12)]  # 2 KiB to 4 MiB
    #     # msg_size_bytes_list = [PartialParams(msg_size_bytes=64<<10), PartialParams(msg_size_bytes=256<<10)]
    #     # msg_size_bytes_list = [PartialParams(msg_size_bytes=256<<10)]
    #     over_sub_list = [PartialParams(over_sub=8)]
    #     setattr(args, "output", output / "permutation")
    #     run_experiment(
    #         args,
    #         permuatation_base_params,
    #         PartialParams.product(load_balancing_algo, asymmetry, msg_size_bytes_list, over_sub_list),
    #         pool,
    #     )

def run_parking_lot_experiment(args, pool):
    link_speed_gbps = args.link_speed_gbps
    msg_size_bytes = 32 << 20  # 32 MB
    n_flows = 3
    nodes = 16
    cm_file = f"matrix=parking_lot:nodes={nodes}:n_flows={n_flows}:msg_size_bytes={msg_size_bytes}.cm"
    assert os.path.isfile(os.path.join(CM_DIR, cm_file))
    base_params = PartialParams(
        link_speed_gbps=link_speed_gbps,
        nodes=nodes,
        over_sub=4,
        msg_size_bytes=msg_size_bytes,
        matrix="parking_lot",
        _cm_file=cm_file,
        _end_time_us=get_end_time_us_helper(
            msg_size_bytes, link_speed_gbps, 2
        ),  # 2 flows collide in parking lot topology at any given link
        collect_all_metrics=True,
        topo="fat_tree",
    )

    run_experiment(
        args,
        base_params,
        ALL_SCHEMES,
        pool,
    )


class ExperimentType(Enum):
    permutation = "permutation"
    incast = "incast"
    all_to_all = "all_to_all"
    multiflow = "multiflow"
    datacenter = "datacenter"
    parking_lot = "parking_lot"
    different_rtt = "different_rtt"
    single_flow = "single_flow"
    fasti = "fasti"
    convergence = "convergence"
    oneWayTor = "oneWayTor"
    oneWayPod = "oneWayPod"
    euroSys = "euroSys"
    pfld = "pfld"


@try_except_wrapper
def main(args):

    base_params = PartialParams(
        link_speed_gbps=args.link_speed_gbps,
        nodes=args.number_of_nodes,
        matrix=args.experiment_type.value,
        collect_all_metrics=False,
        topo="fat_tree",
    )
    scheme_params = ALL_SCHEMES
    env_wkld_params = []

    pool = None
    if args.parallel is not None:
        pool = multiprocessing.Pool(args.parallel)

    if args.experiment_type == ExperimentType.permutation:
        over_sub_list = [PartialParams(over_sub=1), PartialParams(over_sub=8)]
        msg_size_bytes_list = [PartialParams(msg_size_bytes=2 << 20), PartialParams(msg_size_bytes=32 << 20)]  # 2 and 32 MiB
        p1 = PartialParams.product([base_params], over_sub_list, msg_size_bytes_list)

        # WTD experiment
        over_sub_list = [PartialParams(over_sub=1)]
        msg_size_bytes_list = [PartialParams(msg_size_bytes=4 << 20)]  # 4
        # base_params.update(collect_all_metrics=True)
        p2 = PartialParams.product([base_params], over_sub_list, msg_size_bytes_list)

        env_wkld_params = PartialParams.consolidate(p1, p2)

    elif args.experiment_type == ExperimentType.all_to_all:
        over_sub_list = [PartialParams(over_sub=8)]
        msg_size_bytes_list = [PartialParams(msg_size_bytes=2 << 20), PartialParams(msg_size_bytes=32 << 20)]  # 2 and 32 MiB
        env_wkld_params = PartialParams.product([base_params], over_sub_list, msg_size_bytes_list)

    elif args.experiment_type == ExperimentType.incast:
        base_params.update(nodes=1024, over_sub=1, mss_bytes=1024)
        msg_size_bytes_list = [PartialParams(msg_size_bytes=(1 << x) << 10) for x in range(1, 12)]  # 2 KiB to 4 MiB
        incast_degree_list = [
            # PartialParams(incast_degree=32),
            # PartialParams(incast_degree=64),
            PartialParams(incast_degree=args.number_of_nodes),
        ]

        # # Benefit of quick adapt
        # msg_size_bytes_list = [PartialParams(msg_size_bytes=16 << 10)]  # 16 KiB
        # incast_degree_list = [PartialParams(incast_degree=args.number_of_nodes)]
        # base_params.update(collect_all_metrics=True)

        env_wkld_params = PartialParams.product(
            [base_params], msg_size_bytes_list, incast_degree_list
        )

    elif args.experiment_type == ExperimentType.multiflow:
        base_params.update(
            nodes=1024,
            over_sub=1,
            msg_size_bytes=2 << 20,
            matrix="incast",
            collect_all_metrics=True,
        )
        incast_degree_list = [PartialParams(incast_degree=x) for x in range(1, 21)]
        env_wkld_params = PartialParams.product([base_params], incast_degree_list)

    elif args.experiment_type == ExperimentType.parking_lot:
        run_parking_lot_experiment(args, pool)

    elif args.experiment_type == ExperimentType.different_rtt:
        run_different_rtt_experiment(args, pool)

    elif args.experiment_type == ExperimentType.datacenter:
        run_datacenter_experiment(args, pool)

    elif args.experiment_type == ExperimentType.single_flow:
        base_params.update(matrix="incast", over_sub=1, nodes=16, incast_degree=1)
        msg_sizes_bytes_list = [PartialParams(msg_size_bytes=x << 10) for x in range(1, 17)]  # 1 KiB to 16 KiB
        env_wkld_params = PartialParams.product([base_params], msg_sizes_bytes_list)

    elif args.experiment_type == ExperimentType.fasti:
        nodes=1024
        msg_size_bytes = 60 << 10  # 60 KiB (roughly BDP/8) for link_speed_gbps=100
        incast_degree = 8
        cm_file = f"matrix=fasti:nodes={nodes}:msg_size_bytes={msg_size_bytes}:incast_degree={incast_degree}.cm"
        assert os.path.isfile(os.path.join(CM_DIR, cm_file))
        base_params.update(
            over_sub=1, nodes=1024, incast_degree=8, msg_size_bytes=60 << 10,
            collect_all_metrics=True, _cm_file=cm_file
        )
        env_wkld_params = [base_params]

    elif args.experiment_type == ExperimentType.convergence:
        run_convergence_experiment(args, pool)

    elif args.experiment_type == ExperimentType.oneWayTor:
        run_oneWay_experiment(args, pool, OneWayExperimentMode.tor)

    elif args.experiment_type == ExperimentType.oneWayPod:
        run_oneWay_experiment(args, pool, OneWayExperimentMode.pod)

    elif args.experiment_type == ExperimentType.euroSys:
        run_all_euroSys_experiments(args, pool)
    
    elif args.experiment_type == ExperimentType.pfld:
        run_all_pfld_experiments(args, pool)
    else:
        raise NotImplementedError

    if len(env_wkld_params) > 0:
        run_combination(
            args,
            env_wkld_params,
            scheme_params,
            pool,
        )

    if pool is not None:
        pool.close()
        pool.join()

def is_valid_fat_tree_number(num_nodes: int) -> bool:
    k = round((4 * num_nodes) ** (1/3))
    return k ** 3 / 4 == num_nodes


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '-o', '--output', required=True,
        type=str, action='store',
        default=DEFAULT_EXP_ROOT,
        help='path output directory')
    parser.add_argument(
        '-t', '--experiment-type', required=True,
        type=ExperimentType, action='store',
        choices=list(ExperimentType),
    )
    parser.add_argument(
        '-p', '--parallel',
        type=int,
        default=None,
        help='run experiments in parallel, and set the size of the multiprocessing pool'
    )
    parser.add_argument(
        '--link-speed-gbps',
        action='store',
        type=int,
        default=100,
        help='link speed in gbps'
    )
    parser.add_argument(
        '--queue-size-bdp',
        action='store',
        type=int,
        default=1,
        help='queue size in bdp'
    )
    parser.add_argument(
        '--switch-random-drop-prob',
        action='store',
        type=float,
        default=0.0,
        help='switch random drop prob'
    )
    parser.add_argument(
        '-n', '--number-of-nodes',
        type=int,
        default=128,
        help='number of nodes in topology'
    )
    parser.add_argument(
        '--rss-sweep',
        action="store_true",
        help='Run sweep experiments for RSS parameters'
    )
    parser.add_argument(
        '--pfld-exp-set',
        action="store",
        type = int,
    )
    args = parser.parse_args()
    if not is_valid_fat_tree_number(args.number_of_nodes):
        raise argparse.ArgumentError(f"Can't construct a full Fat Tree topology with {args.number_of_nodes} nodes.")
    print(args)

    main(args)