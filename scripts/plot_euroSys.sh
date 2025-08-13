#!/bin/bash

root_dir=$1

python plot_experiment.py -i "$root_dir"/incast --incast-summary --euroSys
python plot_experiment.py -i "$root_dir"/webSearch --load --euroSys
python plot_experiment.py -i "$root_dir"/oneWay --euroSys
python plot_timeseries.py -i "$root_dir"/oneWay --experiments rss_subflow_balls_bins
python plot_experiment.py -i "$root_dir"/rssGrid --rss --euroSys
python plot_experiment.py -i "$root_dir"/frozenThreshold --frozen-exp --euroSys

