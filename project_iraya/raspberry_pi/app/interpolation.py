"""
Inverse Distance Weighting (IDW) interpolation for soil nutrient mapping.

Algorithm
---------
For an unsampled grid point p, the interpolated value is a weighted average
of all sampled values, where closer samples are weighted more heavily:

    Z(p) = sum(w_i * Z_i) / sum(w_i),   w_i = 1 / d(p, p_i)^power

`power=2` is the standard choice in precision-agriculture soil mapping
literature (it balances smoothing against over-weighting the single nearest
sample). A small epsilon avoids division by zero when the grid point
coincides with a sample location.

This is intentionally dependency-light (numpy only) so it runs comfortably
on the Pi's CPU for grids up to a few thousand cells — a resolution far
higher than needed for a field-scale nutrient map.
"""

import numpy as np


def idw_interpolate(samples, lat_min, lat_max, lon_min, lon_max,
                     grid_res=50, power=2, epsilon=1e-8):
    """
    samples: list of dicts with keys 'lat', 'lon', and one or more value keys
             e.g. [{'lat':14.998,'lon':121.000,'nitrogen':42.0}, ...]
    Returns: dict with 'grid' (2D list per nutrient), 'lats', 'lons'
    """
    if not samples:
        return {"lats": [], "lons": [], "grids": {}}

    lats = np.linspace(lat_max, lat_min, grid_res)   # north -> south, matches typical map rendering
    lons = np.linspace(lon_min, lon_max, grid_res)
    grid_lon, grid_lat = np.meshgrid(lons, lats)

    sample_lats = np.array([s["lat"] for s in samples])
    sample_lons = np.array([s["lon"] for s in samples])

    nutrient_keys = [k for k in samples[0].keys() if k not in ("lat", "lon")]
    grids = {}

    for key in nutrient_keys:
        values = np.array([s[key] for s in samples], dtype=float)
        result = np.zeros_like(grid_lat)

        for i in range(grid_res):
            for j in range(grid_res):
                d = np.sqrt(
                    (sample_lats - grid_lat[i, j]) ** 2
                    + (sample_lons - grid_lon[i, j]) ** 2
                ) + epsilon
                weights = 1.0 / (d ** power)
                result[i, j] = np.sum(weights * values) / np.sum(weights)

        grids[key] = result.round(2).tolist()

    return {
        "lats": lats.round(6).tolist(),
        "lons": lons.round(6).tolist(),
        "grids": grids,
    }


def classify_level(value, thresholds):
    """
    thresholds: (low_max, high_min) tuple.
    Returns 'low' | 'ok' | 'high'. Purely illustrative bucketing for the
    dashboard color scale — not an agronomic recommendation.
    """
    low_max, high_min = thresholds
    if value < low_max:
        return "low"
    if value > high_min:
        return "high"
    return "ok"
