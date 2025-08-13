#!/bin/bash -eux

outdir=$1

python run_experiment.py -o $outdir/incast -t incast -p 40 --link-speed-gbps 100
python plot_experiment.py -i $outdir/incast --incast-summary

python run_experiment.py -o $outdir/permutation -t permutation -p 40 --link-speed-gbps 100
python plot_experiment.py -i $outdir/permutation

python run_experiment.py -o $outdir/single_flow -t single_flow -p 40 --link-speed-gbps 100
python plot_experiment.py -i $outdir/single_flow --single-flow-summary

python run_experiment.py -o $outdir/multiflow -t multiflow -p 40 --link-speed-gbps 100
python plot_timeseries.py -i $outdir/multiflow --multiflow-summary -p

python run_experiment.py -o $outdir/different_rtt -t different_rtt -p 40 --link-speed-gbps 100
python plot_timeseries.py -i $outdir/different_rtt -p

python run_experiment.py -o $outdir/fasti -t fasti -p 40 --link-speed-gbps 100
python plot_timeseries.py -i $outdir/fasti -p

python run_experiment.py -o $outdir/datacenter -t datacenter -p 40 --link-speed-gbps 100
python plot_experiment.py -i $outdir/datacenter --datacenter

python run_experiment.py -o $outdir/convergence -t convergence -p 40 --link-speed-gbps 100
python plot_timeseries.py -i $outdir/convergence -p

python run_experiment.py -o $outdir/parking_lot -t parking_lot -p 40 --link-speed-gbps 100
python plot_timeseries.py -i $outdir/parking_lot -p
