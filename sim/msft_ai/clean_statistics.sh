#!/bin/bash

folder_names=("msft_ai_wan_1ec_cc" "msft_ai_wan_kec_cc" "msft_ai_wan_1ecbr" "msft_ai_wan_kecbr")
for folder_name in "${folder_names[@]}"; do
    echo "Processing folder: $folder_name"
    cd "$folder_name" || { echo "Failed to change directory to $folder_name"; continue; }
    
    # Clean up output files
    rm output_one_one_1*.txt
    rm output_one_one_2*.txt
    rm output_*.txt 
    rm statistics_one_one_1*.txt
    rm statistics_one_one_2*.txt
    rm statistics_*.txt
    
    # Clean up specific files if they exist
    rm *.log *.out
    
    echo "Cleaned up files in $folder_name"
    cd ..
done