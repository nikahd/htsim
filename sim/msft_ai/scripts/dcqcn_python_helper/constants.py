# dcqcn_python_helper/constants.py

DCQCN_BASE_KNOBS = {
    "DCQCN_CNP_INTERVAL_US": 173,
    "DCQCN_EPOCH_US": 5648,
    "DCQCN_ALPHA_INIT": 0.07376021746153359,
    "DCQCN_G": 0.02822790251854281,
    "DCQCN_B_BYTES": 68306285,
    "DCQCN_F_EPOCHS": 10,
    "DCQCN_MD_CAP": 0.6023534106230093,
    "DCQCN_FLOOR_LINE_FRAC": 0.6251945643094746,
    "DCQCN_FLOOR_RT_FRAC": 0.9014808054671803,
    "DCQCN_HI_COOLDOWN": 1,
    "DCQCN_HI_CAP_DIV": 668,
}

# Only used if neither env nor knobs provide them
DCQCN_DEFAULTS = {
    "DCQCN_EPOCH_US": 15000,
    "DCQCN_HI_COOLDOWN": 2,
}

# SEARCH SPACE — only the 5 knobs you requested
DEFAULT_KNOBS_SPACE = {
    "DCQCN_ALPHA_INIT": (0.05, 1.0),                    # float
    "DCQCN_B_BYTES":    (8*1024*1024, 256*1024*1024),   # int
    "DCQCN_EPOCH_US":   (2000, 30000),                  # int
    "DCQCN_F_EPOCHS":   (2, 64),                        # int
    "DCQCN_G":          (0.001, 0.1),                   # float
}
