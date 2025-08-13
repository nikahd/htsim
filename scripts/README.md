This directory hosts scripts for running experiments, and processing/plotting
experiment logs/metrics.

## Conventions
Logically there are three types of configurations: environment (think topology),
workload (think connection matrix), and algorithm (e.g., load balancing and
congestion control).

We store runs that share environment/workload parameters in the same directory.
We annotate the directory name with a subset of environment/workload parameter
key/value pairs. We also group experiments with similar environment/workloads
together, e.g., incast vs. permutation vs. parking lot. Within the directory for
the environment/workload configuration, we have directories for storing
logs/metrics for different algorithms.

For ease of communication, we shall call the directory corresponding to the
environment/workload configuration an "experiment", each algorithm sub-directory
as a "run" or "scheme", and a set of similar experiments as an "experiment
type".

Example directory structure for the experiment outputs:
```
.
├── convergence  # This is an experiment type
│   └── nodes=1024:link_speed_gbps=100:...  # This is an experiment
│       ├── scheme=eqds.htsim_data  # This is a run or scheme
│       ├── scheme=mprdma.htsim_data
│       └── scheme=nscc.htsim_data
├── datacenter
│   ├── nodes=16:link_speed_gbps=100:matrix=AliStorage2019:load=0.1
...
```

## Files
`run_experiment.py` hosts code to setup experiment input parameters including
configurations for workload (connection matrix), environment (topology), and
algorithms (load balancing/congestion control). It includes different experiment
types including incast scenarios, permutation, parking lot, etc.

`run_all.sh` shows example commands to run the suite of all default experiment
types along with plots. This is a good starting point for anyone trying to run
the scripts.

For each experiment, broadly, we produce two types of metrics: (1) flow
completion times, and (2) timeseries of internals (e.g., congestion window,
queue sizes etc.)

`plot_experiment.py` parses and plots flow completion times information.

`plot_timeseries.py` parses and plots the timeseries information.

Additionally, these two plotting scripts can also aggregate data across
experiments within an experiment type, e.g., how do flow completion times change
with varying incast degree (`plot_experiment.py --incast-summary`), or how does
queue buildup change with varying flow counts (`plot_timeseries.py
--multiflow-summary`).

For each of the scripts, see also `<script>.py --help` for more information.