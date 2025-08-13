import numpy as np 

duration_suffix = "_duration_cdf_data"
interval_suffix = "_time_between_events_cdf_data"

scale_factor = 1e-3 # scale factor to convert from 1 second to 1 ms

sn_dfw_file_names = [
    "__i_ibr01___sn1__bundle_ether12__owr01___dfw34",
    "__i_ibr01___sn6__bundle_ether5__owr01___dfw33",
    "__i_ibr02___sn1__bundle_ether12__owr02___dfw34",
    "__i_ibr02___sn6__bundle_ether5__owr02___dfw33",
    "__i_sn1_0100_0003_01sw__port_channel10__owr03___dfw34",
    "__i_sn1_0100_0004_01sw__port_channel14__owr04___dfw34",
    "__i_sn6_0100_0001_01sw__port_channel5__owr03___dfw33",
    "__i_sn6_0100_0002_01sw__port_channel9__owr04___dfw33",
]

phx_sn_file_names = [
    "__i_owr01___phx10__bundle_ether7__ibr01___sn1",
    "__i_owr01___phx70__bundle_ether11__ibr01___sn6",
    "__i_owr02___phx10__bundle_ether13__ibr02___sn1",
    "__i_owr02___phx70__bundle_ether4__ibr02___sn6",
    "__i_owr03___phx10__port_channel15__sn1_0100_0003_01sw",
    "__i_owr03___phx10__port_channel15__sn1_0100_0003_01sw",
    "__i_owr03___phx70__port_channel3__sn6_0100_0001_01sw",
    "__i_owr04___phx10__port_channel15__sn1_0100_0004_01sw",
    "__i_owr04___phx70__port_channel11__sn6_0100_0002_01sw",
]

for bundle_idx in range(len(sn_dfw_file_names)):
    filename = sn_dfw_file_names[bundle_idx]
    duration_file = filename + duration_suffix + ".txt"
    interval_file = filename + interval_suffix + ".txt"
    
    duration_cdf = np.loadtxt(duration_file, delimiter=' ')
    interval_cdf = np.loadtxt(interval_file, delimiter=' ')

    output_file_name = "0_" + str(bundle_idx) + "_link_down_events.txt"
    with open(output_file_name, 'w') as output_file:
        t0 = 0
        t1 = 0
        while t1 < 50000: # 50 s simulation end time
            interval = interval_cdf[:, 0] * scale_factor 
            cdf = interval_cdf[:, 1]
            u = np.random.uniform(0, 1)
            i = np.searchsorted(cdf, u)
            sampled_interval = interval[i]
            t0 += sampled_interval

            duration = duration_cdf[:, 0] * scale_factor 
            cdf = duration_cdf[:, 1]
            u = np.random.uniform(0, 1)
            i = np.searchsorted(cdf, u)
            sampled_duration = duration[i]
            t1 = t0 + sampled_duration
            if sampled_duration > 0:
                output_file.write(f"{round(t0 * 1e12 * scale_factor)} {round(t1 * 1e12 * scale_factor)}\n") # picoseconds
    print(f"Generated link down events in {output_file_name}\n")

for bundle_idx in range(len(phx_sn_file_names)):
    filename = phx_sn_file_names[bundle_idx]
    duration_file = filename + duration_suffix + ".txt"
    interval_file = filename + interval_suffix + ".txt"
    
    duration_cdf = np.loadtxt(duration_file, delimiter=' ')
    interval_cdf = np.loadtxt(interval_file, delimiter=' ')

    output_file_name = "1_" + str(bundle_idx) + "_link_down_events.txt"
    with open(output_file_name, 'w') as output_file:
        t0 = 0
        t1 = 0
        while t1 < 50000: # 50 s simulation end time with scale factor
            interval = interval_cdf[:, 0] * scale_factor 
            cdf = interval_cdf[:, 1]
            u = np.random.uniform(0, 1)
            i = np.searchsorted(cdf, u)
            sampled_interval = interval[i]
            t0 += sampled_interval

            duration = duration_cdf[:, 0] * scale_factor 
            cdf = duration_cdf[:, 1]
            u = np.random.uniform(0, 1)
            i = np.searchsorted(cdf, u)
            sampled_duration = duration[i]
            t1 = t0 + sampled_duration
            if sampled_duration > 0:
                output_file.write(f"{round(t0 * 1e12 * scale_factor)} {round(t1 * 1e12 * scale_factor)}\n") # picoseconds
    print(f"Generated link down events in {output_file_name}\n")


