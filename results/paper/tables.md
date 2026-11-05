**Table 1.** Layout of the paper's tables (Section 3.3 of the author's PDF). Seeds: 1000-sim run 1, 100,000-sim run 2.

| γ | Strategy | Source | Avg spread | Profit | Std(Profit) | Final q | Std(Final q) |
|---|---|---|---|---|---|---|---|
| 0.1 | Inventory | paper, 1000 sims | 1.49 | 65.0 | 6.6 | 0.08 | 2.9 |
| 0.1 | Inventory | here, 1000 sims | 1.492 | 65.0 | 6.5 | 0.03 | 2.8 |
| 0.1 | Inventory | here, 100,000 sims | 1.492 | 64.9 | 6.5 | 0.01 | 2.9 |
| 0.1 | Symmetric | paper, 1000 sims | 1.49 | 68.4 | 12.7 | 0.26 | 8.4 |
| 0.1 | Symmetric | here, 1000 sims | 1.492 | 68.6 | 12.9 | 0.05 | 8.2 |
| 0.1 | Symmetric | here, 100,000 sims | 1.492 | 68.2 | 13.4 | 0.02 | 8.4 |
| 0.1 | Symmetric (time-matched) | here, 1000 sims | 1.492 | 68.4 | 12.9 | 0.04 | 8.2 |
| 0.1 | Symmetric (time-matched) | here, 100,000 sims | 1.492 | 68.0 | 13.2 | 0.02 | 8.4 |
| 0.01 | Inventory | paper, 1000 sims | 1.35 | 68.6 | 8.7 | 0.12 | 5.1 |
| 0.01 | Inventory | here, 1000 sims | 1.349 | 68.6 | 8.9 | -0.05 | 5.1 |
| 0.01 | Inventory | here, 100,000 sims | 1.349 | 68.4 | 9.0 | 0.01 | 5.2 |
| 0.01 | Symmetric | paper, 1000 sims | 1.35 | 68.8 | 12.8 | 0.09 | 8.7 |
| 0.01 | Symmetric | here, 1000 sims | 1.349 | 68.9 | 13.1 | -0.01 | 8.5 |
| 0.01 | Symmetric | here, 100,000 sims | 1.349 | 68.6 | 13.6 | 0.03 | 8.7 |
| 0.01 | Symmetric (time-matched) | here, 1000 sims | 1.349 | 68.9 | 12.9 | -0.01 | 8.5 |
| 0.01 | Symmetric (time-matched) | here, 100,000 sims | 1.349 | 68.6 | 13.6 | 0.03 | 8.7 |
| 1 | Inventory | paper, 1000 sims | 3.02 | 31.4 | 5.0 | 0.02 | 1.7 |
| 1 | Inventory | here, 1000 sims | 3.032 | 31.4 | 4.8 | 0.01 | 1.6 |
| 1 | Inventory | here, 100,000 sims | 3.032 | 31.4 | 4.9 | 0.00 | 1.6 |
| 1 | Symmetric | paper, 1000 sims | 3.02 | 44.0 | 11.0 | 0.00 | 5.1 |
| 1 | Symmetric | here, 1000 sims | 3.032 | 44.1 | 11.0 | -0.01 | 5.2 |
| 1 | Symmetric | here, 100,000 sims | 3.032 | 43.7 | 10.7 | 0.02 | 5.2 |
| 1 | Symmetric (time-matched) | here, 1000 sims | 3.032 | 44.4 | 9.6 | -0.04 | 5.6 |
| 1 | Symmetric (time-matched) | here, 100,000 sims | 3.032 | 44.1 | 9.7 | 0.02 | 5.8 |

**Table 2.** Inventory strategy relative to a symmetric benchmark: estimate and paired percentile-bootstrap 95% interval (2000 replicates for 1000 sims, 1000 for 100,000).

| γ | Benchmark | Sims | Std(Profit) ratio | Std(Final q) ratio | Mean profit, relative difference |
|---|---|---|---|---|---|
| 0.1 | Symmetric | 1000 | 0.506 [0.474, 0.541] | 0.344 [0.324, 0.364] | -5.27% [-6.19, -4.35] |
| 0.1 | Symmetric | 100,000 | 0.488 [0.484, 0.491] | 0.348 [0.346, 0.350] | -4.88% [-4.98, -4.78] |
| 0.1 | Symmetric (time-matched) | 1000 | 0.507 [0.476, 0.540] | 0.344 [0.326, 0.365] | -4.99% [-5.88, -4.06] |
| 0.1 | Symmetric (time-matched) | 100,000 | 0.495 [0.491, 0.498] | 0.348 [0.346, 0.350] | -4.61% [-4.70, -4.51] |
| 0.01 | Symmetric | 1000 | 0.680 [0.652, 0.709] | 0.601 [0.578, 0.625] | -0.42% [-1.10, +0.27] |
| 0.01 | Symmetric | 100,000 | 0.658 [0.655, 0.661] | 0.598 [0.596, 0.600] | -0.36% [-0.43, -0.28] |
| 0.01 | Symmetric (time-matched) | 1000 | 0.686 [0.660, 0.714] | 0.601 [0.578, 0.622] | -0.42% [-1.08, +0.27] |
| 0.01 | Symmetric (time-matched) | 100,000 | 0.659 [0.656, 0.662] | 0.598 [0.596, 0.601] | -0.36% [-0.43, -0.29] |
| 1 | Symmetric | 1000 | 0.435 [0.411, 0.461] | 0.315 [0.297, 0.335] | -28.82% [-29.77, -27.88] |
| 1 | Symmetric | 100,000 | 0.454 [0.452, 0.457] | 0.318 [0.316, 0.320] | -28.04% [-28.14, -27.94] |
| 1 | Symmetric (time-matched) | 1000 | 0.495 [0.469, 0.522] | 0.291 [0.275, 0.308] | -29.30% [-30.02, -28.63] |
| 1 | Symmetric (time-matched) | 100,000 | 0.504 [0.501, 0.506] | 0.282 [0.280, 0.284] | -28.70% [-28.77, -28.63] |

**Agreement with the paper.** Of the 30 paper statistics compared (means and standard deviations of profit and final inventory, and the two standard-deviation ratios, for γ = 0.1, 0.01, 1), 30 lie within 2 standard deviations of a 1000-simulation estimate from this model (|z| ≤ 2). The largest deviation is sd_profit (symmetric) at γ = 0.01 (paper 12.8, here 13.6, z = -1.9).

**Table 3.** Paper value against this model (z = (paper - large run) / SD of a 1000-sim estimate).

| γ | Statistic | Paper | Here (large run) | SD of a 1000-sim estimate | z |
|---|---|---|---|---|---|
| 0.1 | mean_profit (inventory) | 65 | 64.9 | 0.2 | +0.6 |
| 0.1 | sd_profit (inventory) | 6.6 | 6.55 | 0.14 | +0.4 |
| 0.1 | mean_final_q (inventory) | 0.08 | 0.00699 | 0.084 | +0.9 |
| 0.1 | sd_final_q (inventory) | 2.9 | 2.93 | 0.066 | -0.4 |
| 0.1 | mean_profit (symmetric) | 68.4 | 68.2 | 0.42 | +0.5 |
| 0.1 | sd_profit (symmetric) | 12.7 | 13.4 | 0.45 | -1.6 |
| 0.1 | mean_final_q (symmetric) | 0.26 | 0.0164 | 0.27 | +0.9 |
| 0.1 | sd_final_q (symmetric) | 8.4 | 8.41 | 0.2 | -0.1 |
| 0.1 | sd_profit ratio (inventory / symmetric) | 0.52 | 0.488 | 0.019 | +1.7 |
| 0.1 | sd_final_q ratio (inventory / symmetric) | 0.345 | 0.348 | 0.011 | -0.3 |
| 0.01 | mean_profit (inventory) | 68.6 | 68.4 | 0.27 | +0.8 |
| 0.01 | sd_profit (inventory) | 8.7 | 8.97 | 0.24 | -1.1 |
| 0.01 | mean_final_q (inventory) | 0.12 | 0.0141 | 0.16 | +0.6 |
| 0.01 | sd_final_q (inventory) | 5.1 | 5.21 | 0.12 | -0.9 |
| 0.01 | mean_profit (symmetric) | 68.8 | 68.6 | 0.42 | +0.4 |
| 0.01 | sd_profit (symmetric) | 12.8 | 13.6 | 0.43 | -1.9 |
| 0.01 | mean_final_q (symmetric) | 0.09 | 0.0327 | 0.28 | +0.2 |
| 0.01 | sd_final_q (symmetric) | 8.7 | 8.71 | 0.22 | -0.1 |
| 0.01 | sd_profit ratio (inventory / symmetric) | 0.68 | 0.658 | 0.016 | +1.4 |
| 0.01 | sd_final_q ratio (inventory / symmetric) | 0.586 | 0.598 | 0.011 | -1.0 |
| 1 | mean_profit (inventory) | 31.4 | 31.4 | 0.15 | -0.3 |
| 1 | sd_profit (inventory) | 5 | 4.86 | 0.11 | +1.3 |
| 1 | mean_final_q (inventory) | 0.02 | 0.00031 | 0.054 | +0.4 |
| 1 | sd_final_q (inventory) | 1.7 | 1.64 | 0.035 | +1.6 |
| 1 | mean_profit (symmetric) | 44 | 43.7 | 0.28 | +1.1 |
| 1 | sd_profit (symmetric) | 11 | 10.7 | 0.28 | +1.1 |
| 1 | mean_final_q (symmetric) | 0 | 0.0218 | 0.16 | -0.1 |
| 1 | sd_final_q (symmetric) | 5.1 | 5.17 | 0.12 | -0.6 |
| 1 | sd_profit ratio (inventory / symmetric) | 0.455 | 0.454 | 0.015 | +0.0 |
| 1 | sd_final_q ratio (inventory / symmetric) | 0.333 | 0.318 | 0.011 | +1.4 |
