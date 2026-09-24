#!/usr/bin/env python3
"""Generate the blue-noise threshold matrix baked into src/dgpd_dither.c
(blue_noise16x16), via Ulichney's void-and-cluster method (toroidal
Gaussian energy field). Re-run and paste the output back in if the matrix
size or sigma ever needs to change."""
import math
import random

N = 16           # matrix size (NxN)
SIGMA = 1.5      # standard void-and-cluster sigma
RADIUS = 5       # truncate the Gaussian kernel at +/- RADIUS
random.seed(20240923)

# Precompute the truncated Gaussian kernel.
kernel = {}
for dy in range(-RADIUS, RADIUS + 1):
    for dx in range(-RADIUS, RADIUS + 1):
        kernel[(dy, dx)] = math.exp(-(dx * dx + dy * dy) / (2.0 * SIGMA * SIGMA))

def new_grid(fill=0.0):
    return [[fill for _ in range(N)] for _ in range(N)]

def add_point(energy, y, x, val):
    for (dy, dx), w in kernel.items():
        ny = (y + dy) % N
        nx = (x + dx) % N
        energy[ny][nx] += val * w

def find_max_one(pattern, energy):
    best = None
    best_e = -1.0
    for y in range(N):
        for x in range(N):
            if pattern[y][x] and energy[y][x] > best_e:
                best_e = energy[y][x]
                best = (y, x)
    return best

def find_min_zero(pattern, energy):
    best = None
    best_e = float("inf")
    for y in range(N):
        for x in range(N):
            if not pattern[y][x] and energy[y][x] < best_e:
                best_e = energy[y][x]
                best = (y, x)
    return best

# --- Phase 0: initial binary pattern (prototype) ---
total = N * N
m = max(2, total // 10)

pattern = new_grid(0)
energy = new_grid(0.0)

positions = random.sample(range(total), m)
for p in positions:
    y, x = divmod(p, N)
    pattern[y][x] = 1
    add_point(energy, y, x, 1.0)

# Relax to the prototype: swap tightest cluster for largest void until stable.
for _ in range(total * 4):
    ty, tx = find_max_one(pattern, energy)
    vy, vx = find_min_zero(pattern, energy)
    if (ty, tx) == (vy, vx):
        break
    pattern[ty][tx] = 0
    add_point(energy, ty, tx, -1.0)
    pattern[vy][vx] = 1
    add_point(energy, vy, vx, 1.0)

proto_pattern = [row[:] for row in pattern]
proto_energy = [row[:] for row in energy]

ranks = [[-1] * N for _ in range(N)]

# --- Phase 1: rank the minority (1) pixels of the prototype, high to low ---
p2 = [row[:] for row in proto_pattern]
e2 = [row[:] for row in proto_energy]
for rank in range(m - 1, -1, -1):
    y, x = find_max_one(p2, e2)
    ranks[y][x] = rank
    p2[y][x] = 0
    add_point(e2, y, x, -1.0)

# --- Phase 2: rank everything else, filling the largest void each step ---
p3 = [row[:] for row in proto_pattern]
e3 = [row[:] for row in proto_energy]
for rank in range(m, total):
    y, x = find_min_zero(p3, e3)
    ranks[y][x] = rank
    p3[y][x] = 1
    add_point(e3, y, x, 1.0)

# Sanity check: every rank 0..total-1 used exactly once.
flat = sorted(v for row in ranks for v in row)
assert flat == list(range(total)), "rank assignment is not a permutation!"

print("static const uint8_t blue_noise16x16[16][16] = {")
for row in ranks:
    print("    {" + ", ".join(f"{v:3d}" for v in row) + "},")
print("};")
