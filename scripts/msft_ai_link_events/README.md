# Generate Link Down Events 

To generate link down event files based on sn_dfw and phx_sn bundle failure traces, run
```
python3 parse_link_down_events_cdf.py
```
The output files will be named by `<dc_id>_<bundle_id>_link_down_events.txt`, each file representing one link in one bundle's link down events. Currently we assume one bundle has one link. Each line of `[t_0, t_1]` stands for the link down start time and end time. 
