import numpy as np 
import math
import random 

# This script generates link down events for one link in composite queue
output_filename = "../../../scripts/msft_ai_link_events/msft_ai_link_down_events.txt"
max_event_time = 100 * 10e9 # 1 milliseconds in pico seconds

def exponential_dist_sample(mean: float) -> float:
    """
    Generates a random number from an exponential distribution with a given mean
    (mean = 1 / lambda).
    Args:
        mean (float): The mean of the exponential distribution (mean = 1 / lambda).
        This mean signifies the average time between events in a Poisson process.
    Returns:
        float: A random number sampled from an exponential distribution with the given mean.
    """
    return -math.log(1 - random.random()) * mean

def get_interval():
    """Generate a random interval between link down events."""
    return exponential_dist_sample(10e9)  # Mean interval of 1 ms

def get_duration():
    """Generate a random duration for a link down event."""
    return 10e8 # duration of 100 ms

if __name__ == "__main__":
    t0 = 0
    t1 = 0
    with open(output_filename, "w") as f:
        while (True):
            interval = get_interval()
            duration = get_duration()
            t0 = t1 + interval
            t1 = t0 + duration
            if t0 > max_event_time:
                break
            f.write(f"{t0} {t1}\n")

    print(f"Generated {output_filename} with random link down events.")