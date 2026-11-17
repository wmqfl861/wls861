def clamp(value, lower, upper):
    if lower > upper:
        raise ValueError("lower exceeds upper")
    return min(max(value, lower), upper)
